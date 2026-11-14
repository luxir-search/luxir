// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once
#include "luxir/reader/SegmentCore.h"
#include "luxir/reader/OwnershipShape.h"
#include <map>
#include <set>
#include "luxir/reader/FieldReader.h"
#include "IndexWriter.h"
#include "OwnershipWriter.h"
#include "MergeCostModel.h"
#include "OrdCollector.h"
#include "OrdColWriter.h"
#include "NormsWriter.h"
#include "BKDWriter.h"
#include "PointsWriter.h"
#include "StoredFieldsWriter.h"
#include "StoredDictionaryRegistry.h"
#include "luxir/reader/DocsEnum.h"
#include "luxir/reader/PosEnum.h"
#include "luxir/reader/NormsReader.h"
#include "luxir/reader/PointsReader.h"
#include "luxir/reader/StoredFieldsReader.h"
#include "luxir/reader/StrColReader.h"
#include "luxir/reader/OrdColReader.h"
#include "luxir/schema/Schema.h"
#include "luxir/search/IndexReader.h"
#include "luxir/util/geo.h"
#include "luxir/util/heap.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <exception>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <oneapi/tbb/task_group.h>

// This file is only included in IndexWriter.cpp

#define MERGER_DEBUG LOG_TRACE
// #define MERGER_DEBUG LOG_DEBUG

#include "luxir/util/Signal.h"
#include "luxir/util/ScratchFile.h"
#include "luxir/reader/DocsReader.h"

namespace luxir {

class SegmentMerger {

  struct Segment {
    PostingsReader* postingsReader;
    FieldReader* fieldReader;
    int32_t numLive; // number of live documents in this segment
    int32_t base; // docId base for this segment in the merged segment, accounting for deletions
    int ord; // 0-based ordinal (index in original merge list)
    std::vector<int32_t> remap;  // 0 based (segment local) old to new id mapping, -1 means deleted

    bool hasDeletes() {
      return !remap.empty();
    }

    // returns a pair of (mappedDocId, isDeleted)
    std::pair<int32_t, bool> remapDocId(int32_t localId) const {
      bool isDeleted = false;
      int32_t mappedDoc;
      if (!remap.empty()) {
        mappedDoc = remap[localId];
        if (mappedDoc == -1) {
          isDeleted = true;
        }
      } else {
        mappedDoc = localId;
      }
      mappedDoc += base;
      return {mappedDoc, isDeleted};
    }


  };

  struct Space {
    SpaceOrdinal ordinal;
    std::vector<PostingsReader*> preaders;
    std::span<LiveDocs*> liveDocs; // parallel to preaders, nullptr if no deletes
    std::vector<FieldReader> fieldReaders;  // todo - pool allocate (& use smart ptr on MergeSeg if destructors needed)
    std::vector<Segment> segs;
    int32_t maxDoc = 0;

    explicit Space(SpaceOrdinal ordinal, std::span<LiveDocs*> liveDocs = {})
        : ordinal(ordinal), liveDocs(liveDocs) {}
  };

  struct MergeFieldInfo {
    SegFieldInfo segFieldInfo;
    Segment* seg;  // points to the segment that produced this.
  };

  struct MergePoint {
    int64_t value;
    int32_t docid;
  };

  static_assert(sizeof(MergePoint) == MergeCostModel::SYNTHESIZED_POINT_BYTES);
  static_assert(PointsWriter::DEFAULT_MAX_POINTS_PER_LEAF
                == MergeCostModel::POINTS_PER_LEAF);

  class PointRun {
    Segment* segment;
    PointsReader* reader = nullptr;
    std::span<int64_t> leafValues;
    std::span<uint32_t> leafDocids;
    std::span<uint32_t> leafResiduals;
    std::vector<MergePoint> synthesized;
    uint32_t leafIndex = 0;
    uint16_t pointIndex = 0;
    uint16_t pointsInLeaf = 0;
    size_t synthesizedIndex = 0;
    MergePoint currentPoint{};

  public:
    PointRun(Segment& segment, PointsReader& reader, MemPool& pool)
        : segment(&segment), reader(&reader),
          leafValues(pool.make_span<int64_t>(reader.maxPointsPerLeaf())),
          leafDocids(pool.make_span<uint32_t>(reader.maxPointsPerLeaf())),
          leafResiduals(pool.make_span<uint32_t>(reader.maxPointsPerLeaf())) {}

    PointRun(Segment& segment, std::vector<MergePoint>&& synthesized)
        : segment(&segment), synthesized(std::move(synthesized)) {}

    int sourceOrd() const { return segment->ord; }
    const MergePoint& point() const { return currentPoint; }

    bool next() {
      if (reader == nullptr) {
        if (synthesizedIndex >= synthesized.size()) return false;
        currentPoint = synthesized[synthesizedIndex++];
        return true;
      }

      for (;;) {
        if (pointIndex >= pointsInLeaf) {
          if (leafIndex >= reader->leafCount()) return false;
          pointsInLeaf = reader->decodeLeafInto(
              leafIndex++, leafValues, leafDocids, leafResiduals);
          pointIndex = 0;
        }
        uint16_t index = pointIndex++;
        // Live-doc compaction is monotone, so filtering/remapping cannot
        // disturb this source run's (value, docid) order.
        auto [mappedDoc, isDeleted] = segment->remapDocId(
            (int32_t)leafDocids[index]);
        if (isDeleted) continue;
        currentPoint = {leafValues[index], mappedDoc};
        return true;
      }
    }
  };

  struct PointRunCompare {
    bool operator()(const PointRun& lhs, const PointRun& rhs) const {
      const MergePoint& a = lhs.point();
      const MergePoint& b = rhs.point();
      if (a.value != b.value) return a.value > b.value;
      if (a.docid != b.docid) return a.docid > b.docid;
      return lhs.sourceOrd() > rhs.sourceOrd();
    }
  };

  // Runs the per-field merge batches as parallel TBB tasks under IndexRamBudget
  // admission.  Strategy: work waits, threads don't.
  //  - Batches are enumerated up front and sorted longest-work-first, so the
  //    longest field merge starts earliest and small column merges pack around it.
  //    RAM cost is a poor proxy for length: a text field's merge holds only its
  //    norms, while a short low-cardinality string column holds an ord per doc.
  //  - admitLoop() launches every pending batch whose streams fit and whose RAM
  //    reserves.  Failed RAM admissions publish merge demand, and budget capacity
  //    releases ping the driver to retry, so admission is event-driven and no
  //    thread ever blocks on the budget.
  //  - The driver's only wait is tg.wait() on its own running tasks.  If nothing
  //    is in flight and nothing fits, it force-admits the head batch (bounded
  //    budget overdraft): the merge always makes progress, an oversized field
  //    still runs (alone), and a zero-parallelism outcome equals today's serial
  //    merge rather than a stall.
  //  - The budget/stream reservation travels INSIDE the task object (moved into
  //    the capture), because a canceled task_group can destroy queued tasks
  //    without running them - destruction must still release.  Lock order is
  //    driver mutex -> budget mutex, never inverted (releases drop the budget
  //    guard before taking the driver mutex).
  // Failure containment is unchanged: a throwing batch cancels the group, wait()
  // rethrows the first exception, and the caller's merge-failure handling applies.
  class MergeAdmissionDriver {
  public:
    class HeldStreams {
      MergeAdmissionDriver* driver = nullptr;
      int32_t streams = 0;

    public:
      HeldStreams() = default;
      HeldStreams(MergeAdmissionDriver& driver, int32_t streams)
        : driver(&driver), streams(streams) {}
      HeldStreams(const HeldStreams&) = delete;
      HeldStreams& operator=(const HeldStreams&) = delete;
      HeldStreams(HeldStreams&& other) noexcept
        : driver(other.driver), streams(other.streams) {
        other.driver = nullptr;
        other.streams = 0;
      }
      HeldStreams& operator=(HeldStreams&& other) noexcept {
        if (this != &other) {
          release();
          driver = other.driver;
          streams = other.streams;
          other.driver = nullptr;
          other.streams = 0;
        }
        return *this;
      }
      ~HeldStreams() { release(); }

      void release() {
        MergeAdmissionDriver* target = driver;
        if (target == nullptr) return;
        int32_t released = streams;
        driver = nullptr;
        streams = 0;
        {
          const std::lock_guard<std::mutex> lock(target->mutex);
          assert(target->inFlightStreams >= released);
          target->inFlightStreams -= released;
        }
        target->admitLoop();
      }
    };

    class BatchAdmission {
      MergeAdmissionDriver* driver = nullptr;
      IndexRamBudget::Guard ramGuard;
      int32_t streams = 0;

      friend class MergeAdmissionDriver;

    public:
      BatchAdmission() = default;
      BatchAdmission(MergeAdmissionDriver& driver, IndexRamBudget::Guard&& ramGuard, int32_t streams)
        : driver(&driver), ramGuard(std::move(ramGuard)), streams(streams) {}
      BatchAdmission(const BatchAdmission&) = delete;
      BatchAdmission& operator=(const BatchAdmission&) = delete;

      BatchAdmission(BatchAdmission&& other) noexcept
        : driver(other.driver), ramGuard(std::move(other.ramGuard)), streams(other.streams) {
        other.driver = nullptr;
        other.streams = 0;
      }

      BatchAdmission& operator=(BatchAdmission&& other) noexcept {
        if (this != &other) {
          releaseResources(false);
          driver = other.driver;
          ramGuard = std::move(other.ramGuard);
          streams = other.streams;
          other.driver = nullptr;
          other.streams = 0;
        }
        return *this;
      }

      ~BatchAdmission() {
        releaseResources(false);
      }

      IndexRamBudget::Guard takeRamGuard() {
        return std::move(ramGuard);
      }

      bool repriceRam(int64_t bytes) {
        int64_t oldBytes = ramGuard.size();
        if (!ramGuard.tryResize(bytes)) return false;
        if (bytes < oldBytes) driver->admitLoop();
        return true;
      }

      void forceRepriceRam(int64_t bytes) {
        assert(bytes >= ramGuard.size());
        ramGuard.forceResize(bytes);
      }

      HeldStreams takeStreams() {
        assert(driver != nullptr);
        HeldStreams held(*driver, streams);
        streams = 0;
        return held;
      }

      void completeAndAdmit() {
        releaseResources(true);
      }

    private:
      void releaseResources(bool runAdmit) {
        MergeAdmissionDriver* target = driver;
        if (target == nullptr) {
          return;
        }
        int32_t releasedStreams = streams;
        driver = nullptr;
        streams = 0;

        ramGuard.release();

        {
          const std::lock_guard<std::mutex> lock(target->mutex);
          assert(target->inFlight > 0);
          assert(target->inFlightStreams >= releasedStreams);
          target->inFlight--;
          target->inFlightStreams -= releasedStreams;
        }

        if (runAdmit) {
          target->admitLoop();
        }
      }
    };

    struct Batch {
      int64_t cost = 0;
      // Estimated merge work, for ordering only (postings and values processed).
      int64_t work = 0;
      int32_t streams = 0;
      std::string name;
      std::vector<MergeFieldInfo> fields;
      Space* space = nullptr;
      bool bootstrapStoredDictionary = false;
      std::function<void(MergeAdmissionDriver&, Batch&, BatchAdmission&)> body;
    };

  private:
    SegmentMerger& merger;
    IndexRamBudget& budget;
    const int32_t maxStreams;
    oneapi::tbb::task_group_context context;
    oneapi::tbb::task_group tg;
    std::mutex mutex;
    std::vector<Batch> pending;
    int32_t inFlight = 0;
    int32_t inFlightStreams = 0;
    std::exception_ptr callbackFailure;
    IndexRamBudget::MergeDemandRegistration demandRegistration;

  public:
    MergeAdmissionDriver(SegmentMerger& merger, IndexRamBudget& budget,
                         int32_t maxStreams, std::vector<Batch>&& batches)
      : merger(merger), budget(budget), maxStreams(maxStreams), context(), tg(context),
        pending(std::move(batches)),
        demandRegistration(budget.registerMergeDemand([this]() noexcept {
          admissionCallback();
        })) {
      assert(maxStreams >= MergeCostModel::STREAM_BUDGET_FLOOR);
    }

    void run() {
      for (;;) {
        bool admitted = admitLoop();
        bool force = false;
        {
          const std::lock_guard<std::mutex> lock(mutex);
          if (context.is_group_execution_cancelled()) {
            break;
          }
          if (pending.empty() && inFlight == 0) {
            break;
          }
          force = inFlight == 0 && !pending.empty() && !admitted;
        }

        if (force) {
          forceAdmitFirst();
        }

        tg.wait();
      }
      tg.wait();
      rethrowCallbackFailure();
    }

    // Atomically replaces a running parent batch's stream reservation with a
    // smaller coordinator lease while making its child batches visible.  If
    // the slots were released first, many text fields could accumulate idle
    // coordinators and leave no two-stream slot for any range to make progress.
    void replaceWithBatches(BatchAdmission& parent, int32_t retainedStreams,
                            HeldStreams& heldStreams, std::vector<Batch>&& batches) {
      bool accepted = false;
      {
        const std::lock_guard<std::mutex> lock(mutex);
        if (!context.is_group_execution_cancelled()) {
          assert(parent.driver == this);
          assert(retainedStreams >= 0 && retainedStreams <= parent.streams);
          pending.reserve(pending.size() + batches.size());
          for (auto& batch : batches) {
            pending.push_back(std::move(batch));
          }
          std::sort(pending.begin(), pending.end(), longestFirst);
          int32_t releasedStreams = parent.streams - retainedStreams;
          assert(inFlightStreams >= releasedStreams);
          inFlightStreams -= releasedStreams;
          parent.streams = 0;
          heldStreams = HeldStreams(*this, retainedStreams);
          accepted = true;
        }
      }
      if (accepted) {
        admitLoop();
      }
    }

    static bool longestFirst(const Batch& a, const Batch& b) {
      if (a.work != b.work) return a.work > b.work;
      return a.name < b.name;
    }

  private:
    bool admitLoop() {
      bool admittedAny = false;
      for (;;) {
        Batch batch;
        BatchAdmission admission;
        bool haveBatch = false;

        {
          const std::lock_guard<std::mutex> lock(mutex);
          if (context.is_group_execution_cancelled()) {
            demandRegistration.publish(0);
            return admittedAny;
          }

          int32_t hypotheticalStreams = inFlightStreams;
          int32_t hypotheticalBatches = 0;
          int64_t pendingDemand = 0;
          for (size_t i = 0; i < pending.size(); i++) {
            Batch& candidate = pending[i];
            if (hypotheticalStreams + candidate.streams > maxStreams) {
              continue;
            }
            hypotheticalStreams += candidate.streams;
            hypotheticalBatches++;

            auto guard = budget.tryAcquireGuard(candidate.cost);
            if (!guard) {
              // Pending is longest-work-first.  A RAM-starved candidate consumes
              // virtual stream slots just as if it had admitted, so a cheaper
              // later batch cannot steal the slots being reclaimed for the head
              // work.  This makes demand describe the same ordered schedule we
              // would launch if RAM were infinite.  The budget caps aggregate
              // demand at one pool before applying it to pressure.
              int64_t available = std::numeric_limits<int64_t>::max() - pendingDemand;
              pendingDemand = candidate.cost >= available
                  ? std::numeric_limits<int64_t>::max()
                  : pendingDemand + candidate.cost;
              continue;
            }

            batch = std::move(candidate);
            pending.erase(pending.begin() + (int64_t)i);
            inFlight++;
            inFlightStreams += batch.streams;
            admission = BatchAdmission(*this, std::move(*guard), batch.streams);
            haveBatch = true;
            admittedAny = true;
            break;
          }

          // Publish only a stable scan.  If a real batch admitted, the next
          // iteration rescans against its streams and reservation before exposing
          // demand, avoiding a transient request for work that just became live.
          if (!haveBatch) {
            Signal::emit("mergeAdmissionDemand", &pendingDemand, &hypotheticalStreams,
                         &hypotheticalBatches);
            demandRegistration.publish(pendingDemand);
          }
        }

        if (!haveBatch) {
          return admittedAny;
        }

        spawn(std::move(batch), std::move(admission));
      }
    }

    // TODO: overdrafts are currently uncoordinated across writers - every starved
    // merge force-admits independently, so concurrent collections can overdraw the
    // budget by one (possibly huge) batch EACH.  See the overdraft-token TODO in
    // IndexRamBudget.h.
    bool forceAdmitFirst() {
      Batch batch;
      BatchAdmission admission;
      {
        const std::lock_guard<std::mutex> lock(mutex);
        if (context.is_group_execution_cancelled() || pending.empty() || inFlight != 0) {
          return false;
        }

        // Only force-admit a batch whose streams fit: unlike RAM, stream slots
        // must never overdraw (they bound the segment's file count).  Returning
        // false with pending work is safe from livelock only because held
        // streams (a partition coordinator's) imply pending two-stream range
        // work.  replaceWithBatches publishes that work before reducing the
        // parent to its one-stream lease, so at least one range remains able to
        // run whenever only coordinator leases remain.
        size_t selected = pending.size();
        for (size_t i = 0; i < pending.size(); i++) {
          if (inFlightStreams + pending[i].streams <= maxStreams) {
            selected = i;
            break;
          }
        }
        if (selected == pending.size()) return false;

        batch = std::move(pending[selected]);
        pending.erase(pending.begin() + (int64_t) selected);
        IndexRamBudget::Guard guard = budget.forceAcquire(batch.cost);
        inFlight++;
        inFlightStreams += batch.streams;
        admission = BatchAdmission(*this, std::move(guard), batch.streams);
      }

      spawn(std::move(batch), std::move(admission));
      // The forced batch is now represented by reserved RAM rather than pending
      // demand.  Recompute immediately so pressure reclaim targets only the work
      // that can coexist with its stream reservation.
      admitLoop();
      return true;
    }

    void admissionCallback() noexcept {
      try {
        admitLoop();
      } catch (...) {
        {
          const std::lock_guard<std::mutex> lock(mutex);
          if (callbackFailure == nullptr) callbackFailure = std::current_exception();
        }
        context.cancel_group_execution();
      }
    }

    void rethrowCallbackFailure() {
      std::exception_ptr failure;
      {
        const std::lock_guard<std::mutex> lock(mutex);
        failure = callbackFailure;
      }
      if (failure != nullptr) std::rethrow_exception(failure);
    }

    struct BatchTask {
      MergeAdmissionDriver* driver;
      mutable Batch batch;
      mutable BatchAdmission admission;

      void operator()() const {
        batch.body(*driver, batch, admission);
        Signal::emit("segmentMergeBody");
        admission.completeAndAdmit();
      }
    };

    void spawn(Batch&& batch, BatchAdmission&& admission) {
      if (context.is_group_execution_cancelled()) {
        return;
      }
      tg.run(BatchTask{this, std::move(batch), std::move(admission)});
    }
  };

  std::span<const std::shared_ptr<SegmentCore>> cores;
  IndexRamBudget::Guard rowMapsRam;
  Space root;
  std::vector<std::unique_ptr<Space>> children;
  PostingsWriter& postingsWriter;
  IndexRamBudget& ramBudget;
  // Current schema, for per-resource stored-fields formats.
  const Schema& schema;
  StoredDictionaryRegistry& storedDictionaries;
  const uint64_t estimatedOutputBytes;
  const int32_t streamBudget;
  int64_t termPartitionMinBytes;
  int64_t termPartitionMinRangeBytes;
  int32_t termPartitionMaxRanges;
  // Replay merges whose per-source term ord maps (replayOrdMapBytes) reach
  // this write them to scratch files instead of the heap.
  int64_t ordMapSpillMinBytes;

public:

  size_t rowMapBytes() const {
    size_t bytes = 0;
    auto count = [&](const Space& space) {
      for (const auto& seg : space.segs) bytes += seg.remap.capacity() * sizeof(int32_t);
    };
    count(root);
    for (const auto& child : children) count(*child);
    return bytes;
  }

  int32_t mapDocId(size_t sourceOrd, int32_t localDocId) const {
    auto [docId, deleted] = root.segs[sourceOrd].remapDocId(localDocId);
    return deleted ? -1 : docId;
  }



  SegmentMerger(std::span<const std::shared_ptr<SegmentCore>> cores, std::span<LiveDocs*> liveDocs,
                PostingsWriter& postingsWriter, IndexRamBudget& ramBudget,
                int64_t termPartitionMinBytes, int64_t termPartitionMinRangeBytes,
                int32_t termPartitionMaxRanges, const Schema& schema,
                StoredDictionaryRegistry& storedDictionaries,
                int64_t ordMapSpillMinBytes = MergeCostModel::ORD_MAP_SPILL_MIN_BYTES)
  : cores(cores), root(SpaceOrdinal::ROOT, liveDocs), postingsWriter(postingsWriter), ramBudget(ramBudget),
    schema(schema), storedDictionaries(storedDictionaries),
    estimatedOutputBytes(estimateOutputBytes(cores)),
    streamBudget(MergeCostModel::streamBudgetForBytes(estimatedOutputBytes)),
    termPartitionMinBytes(termPartitionMinBytes),
    termPartitionMinRangeBytes(termPartitionMinRangeBytes),
    termPartitionMaxRanges(termPartitionMaxRanges),
    ordMapSpillMinBytes(ordMapSpillMinBytes)
  {
    assert(cores.size() == liveDocs.size());
    root.preaders.reserve(cores.size());
    for (const auto& core : cores) root.preaders.push_back(&core->rootReader());
    assert(termPartitionMinBytes >= 0 && termPartitionMinRangeBytes > 0);
    assert(termPartitionMaxRanges == MergeCostModel::DERIVED_TERM_RANGES
           || (termPartitionMaxRanges >= 2
               && termPartitionMaxRanges <= MergeCostModel::MAX_TERM_RANGES_CAP));
    // File-count cost scales with output size, but retaining complete output
    // streams in RAM does not. Only estimated-small merges request delegation;
    // each skewed stream still has its own safety spill threshold.
    postingsWriter.configureRamDelegation(
        estimatedOutputBytes < PostingsWriter::SMALL_SEGMENT_BYTES, &ramBudget);
  }

  void merge() {
    auto guard = MemPool::threadLocalPoolGuard();
    prepareSpace(root);
    // Set maxDoc to total number of live documents
    postingsWriter.setMaxDoc(root.maxDoc);

    MERGER_DEBUG("SegmentMerger: total live docs to merge: {}, estimated bytes: {}, stream budget: {}",
                 root.maxDoc, estimatedOutputBytes, streamBudget);
    writeRootVersions();
    prepareChildren();
    assert(rowMapsRam.size() == (int64_t)rowMapBytes());
    Signal::emit("mergeRowMaps", this);
    std::vector<MergeAdmissionDriver::Batch> batches;
    planFields(root, batches);
    for (auto& child : children) planFields(*child, batches);
    std::sort(batches.begin(), batches.end(), MergeAdmissionDriver::longestFirst);

    MergeAdmissionDriver driver(*this, ramBudget, streamBudget, std::move(batches));
    driver.run();

    // Caller is responsible for calling postingsWriter.finish()
    // so it can collect the filenames written.
  }

private:
  void reserveRowMap(std::vector<int32_t>& map, int32_t count) {
    assert(map.empty() && map.capacity() == 0);
    // One reservation grows before each allocation. Parent maps determine
    // child sizes, and both stay charged until the merger releases its maps.
    int64_t bytes = (int64_t)count * sizeof(int32_t);
    if (rowMapsRam) rowMapsRam.forceResize(rowMapsRam.size() + bytes);
    else rowMapsRam = ramBudget.forceAcquire(bytes);
    map.reserve(count);
    if (map.capacity() != (size_t)count) {
      rowMapsRam.forceResize(rowMapsRam.size() + (int64_t)(map.capacity() - count) * sizeof(int32_t));
    }
  }

  void prepareSpace(Space& space) {
    space.segs.reserve(space.preaders.size());
    space.fieldReaders.reserve(space.preaders.size());  // This is important since we take pointers to these! Pool allocate later...

    int64_t totalLive = 0;  // used to set each segment base, will be equal to total live docs after the loop
    for (size_t i = 0; i < space.preaders.size(); i++) {
      auto preader = space.preaders[i];
      auto segLiveDocs = space.liveDocs[i];
      
      MERGER_DEBUG("SegmentMerger: segment {}, maxDoc={}, liveDocs={}",
               i, preader->maxDoc(), segLiveDocs ? segLiveDocs->numLive() : preader->maxDoc());
      
      space.fieldReaders.emplace_back(*preader);
      FieldReader& fieldReader = space.fieldReaders.back();

      int32_t numLive = segLiveDocs ? segLiveDocs->numLive() : preader->maxDoc();
      space.segs.emplace_back(preader, &fieldReader, numLive, totalLive, (int)i);
      totalLive += numLive;
    }
    space.maxDoc = (int32_t)totalLive;
    // Now build the docId maps based on liveDocs, then release the liveDocs.
    for (size_t i = 0; i < space.segs.size(); i++) {
      auto& seg = space.segs[i];
      auto* live = space.liveDocs[i];
      if (live && live->numLive() != seg.postingsReader->maxDoc()) {
        reserveRowMap(seg.remap, seg.postingsReader->maxDoc());
        buildDocIdMapping(seg.postingsReader->maxDoc(), live, seg.remap);
      }
      // TODO: pass the actual liveDocs shared_ptr into the merger so we can release them.
    }
  }

  void prepareChildren() {
    std::set<std::string> paths;
    for (const auto& core : cores) {
      for (size_t i = 1; i < core->spaces().size(); i++) paths.insert(core->spaces()[i].path);
    }
    std::map<std::string, Space*, std::less<>> spaces;
    for (const auto& path : paths) {
      auto parentPath = spaceParentPath(path);
      auto found = spaces.find(parentPath);
      Space* parent = parentPath.empty() ? &root : found == spaces.end() ? nullptr : found->second;
      if (!parent) continue;
      auto space = std::make_unique<Space>(SpaceOrdinal::ROOT);
      space->preaders.reserve(cores.size());
      space->segs.reserve(cores.size());
      space->fieldReaders.reserve(cores.size());
      int64_t total = 0;
      for (size_t i = 0; i < cores.size(); i++) {
        auto* reader = cores[i]->spaceReader(path);
        const auto& owner = parent->segs[i];
        int32_t count = 0;
        if (reader && owner.numLive) {
          if (owner.numLive == owner.postingsReader->maxDoc()) count = reader->maxDoc();
          else {
            OwnershipShape::Cursor cursor(*cores[i]->ownership(path));
            for (int32_t p = 0; p < owner.postingsReader->maxDoc(); p++) {
              if (owner.remapDocId(p).second) continue;
              auto family = cursor.seek(p);
              count += family.end - family.first;
            }
          }
        }
        space->preaders.push_back(reader);
        FieldReader* fields = nullptr;
        if (count) {
          space->fieldReaders.emplace_back(*reader);
          fields = &space->fieldReaders.back();
        }
        space->segs.emplace_back(count ? reader : nullptr, fields, count, (int32_t)total, (int)i);
        total += count;
      }
      if (!total) continue;
      if (!MergeCostModel::docsFit(0, total)) throw std::runtime_error("merged row space exceeds row limit: " + path);
      space->maxDoc = (int32_t)total;
      space->ordinal = postingsWriter.addSpace(path);
      const auto* type = dynamic_cast<const NestedFieldType*>(schema.getFieldTypePtr(path));
      bool reverseMap = !type || type->reverseMap;
      auto shapeRam = ramBudget.forceAcquire(OwnershipWriter::workspaceBytes(parent->maxDoc, space->maxDoc, reverseMap));
      OwnershipWriter shape(postingsWriter, reverseMap);
      int32_t end = 0, owners = 0;
      for (size_t i = 0; i < cores.size(); i++) {
        const auto& owner = parent->segs[i];
        auto& child = space->segs[i];
        auto* reader = space->preaders[i];
        if (child.numLive && child.numLive != reader->maxDoc()) {
          reserveRowMap(child.remap, reader->maxDoc());
          child.remap.assign(reader->maxDoc(), -1);
        }
        int32_t local = 0;
        std::optional<OwnershipShape::Cursor> cursor;
        if (reader) cursor.emplace(*cores[i]->ownership(path));
        if (!owner.numLive) continue;
        for (int32_t p = 0; p < owner.postingsReader->maxDoc(); p++) {
          if (owner.remapDocId(p).second) continue;
          auto family = cursor ? cursor->seek(p) : OwnershipShape::Family{p, 0, 0};
          if (!child.remap.empty()) {
            for (int32_t row = family.first; row < family.end; row++) child.remap[row] = local++;
          } else local += family.end - family.first;
          end += family.end - family.first;
          shape.addEnd(end);
          owners++;
        }
        assert(local == child.numLive);
      }
      unused(owners);
      assert(owners == parent->maxDoc && end == space->maxDoc);
      postingsWriter.finishSpace(space->ordinal, space->maxDoc, shape.finish());
      spaces.emplace(path, space.get());
      children.push_back(std::move(space));
    }
  }

  void planFields(Space& space, std::vector<MergeAdmissionDriver::Batch>& batches) {
    auto fnameComp = [](const Segment& a, const Segment& b){ return b.fieldReader->name() < a.fieldReader->name(); };
    std::vector<Segment*> segPtrs;
    segPtrs.reserve(space.segs.size());
    for (auto& seg : space.segs) {
      if (seg.fieldReader && seg.fieldReader->readNextField()) segPtrs.push_back(&seg);
    }
    IndirectPQ<Segment, decltype(fnameComp)> fieldPQ(segPtrs);

    while (fieldPQ.size() > 0) {
      MergeAdmissionDriver::Batch batch;
      batch.space = &space;
      batch.fields.reserve(space.segs.size());
      auto currField = fieldPQ.top().fieldReader->name();
      batch.name = std::string((std::string_view)currField);

      // a redundant compare the first time through here, but simpler code.
      while (fieldPQ.size() > 0 && currField == fieldPQ.top().fieldReader->name()) {
        MergeFieldInfo& segField = batch.fields.emplace_back();
        fieldPQ.top().fieldReader->readFieldInfo(segField.segFieldInfo);
        segField.seg = &fieldPQ.top();  // point to which segment produced the segFieldInfo

        // increment to next field name and fix up heap
        if (!fieldPQ.top().fieldReader->readNextField()) {
          fieldPQ.removeTop();
        } else {
          fieldPQ.updateTop();
        }
      }

      // Check before route selection, cost estimation, or dispatching field work.
      for (const auto& field : batch.fields) {
        batch.fields.front().segFieldInfo.requireCompatibleType(field.segFieldInfo);
      }

      // If merging would break any of the segment max constraints (i.e. number of unique terms in a field)
      // we could bail early.

      batch.cost = estimateCost(space, batch.fields);
      batch.work = estimateWork(batch.fields);
      if (isStoredField(batch.fields[0].segFieldInfo)) {
        auto format = storedFormat(batch.name);
        batch.bootstrapStoredDictionary = format.codec == StoredChunkFormat::ZSTD && format.dictionaryBytes
            && !storedDictionaries.hasDictionary(batch.name, format);
        if (batch.bootstrapStoredDictionary) batch.cost += StoredChunkCompressor::dictionaryTrainingBytes(format);
        std::string_view resource = batch.name;
        Signal::emit("storedFieldsMergePlan", &resource, &batch.cost, &batch.bootstrapStoredDictionary);
      }
      batch.streams = estimateStreams(batch.fields);
      batch.body = [this](MergeAdmissionDriver& driver, MergeAdmissionDriver::Batch& current,
                          MergeAdmissionDriver::BatchAdmission& admission) {
        mergeField(*current.space, current, driver, admission);
      };
      batches.push_back(std::move(batch));
    }
  }

  static uint64_t estimateOutputBytes(std::span<const std::shared_ptr<SegmentCore>> cores) {
    uint64_t total = 0;
    for (const auto& core : cores) {
      uint64_t bytes = core->sizeInBytes();
      total = bytes > std::numeric_limits<uint64_t>::max() - total
          ? std::numeric_limits<uint64_t>::max() : total + bytes;
    }
    return total;
  }

  static bool isStoredField(const SegFieldInfo& info) {
    return FieldType::storage(info.type) == FieldType::Storage::BIN && (info.flags & FieldType::STORED) != 0;
  }

  static bool hasOneDimensionalPoints(FieldType::Type type, int32_t flags) {
    return FieldType::isNumericColumn(type) && (flags & FieldType::INDEX_RANGE) != 0;
  }

  static bool hasGeoPoints(FieldType::Type type, int32_t flags) {
    return FieldType::usesGeoPoints(type) && (flags & FieldType::INDEX_RANGE) != 0;
  }

  static bool hasRangePoints(FieldType::Type type, int32_t flags) {
    return type == FieldType::INTERVAL && (flags & FieldType::INDEX_RANGE) != 0;
  }

  static bool hasLiveFieldValues(const MergeFieldInfo& field) {
    const auto& seg = *field.seg;
    // Indexed terms without a column also write row presence. Text records it
    // with the norms, including rows whose analysis produced no terms.
    int32_t count = field.segFieldInfo.docsWithField;
    if (count == 0 || seg.numLive == 0) return false;
    // If there are more populated rows than deletes, at least one survives.
    if (count > seg.postingsReader->maxDoc() - seg.numLive) return true;

    DocsReader docs(*seg.postingsReader, field.segFieldInfo);
    screaming::BitSet::Iterator rows(docs.bitset());
    for (int32_t row = rows.next(); row != screaming::BitSet::END; row = rows.next()) {
      if (!seg.remapDocId(row).second) return true;
    }
    return false;
  }

  static void batchTypeAndFlags(std::span<const MergeFieldInfo> fields, FieldType::Type& type, int32_t& allFlags) {
    type = FieldType::Type::NONE;
    allFlags = 0;
    bool allIndexed = true;
    for (const auto& field : fields) {
      if (type == FieldType::Type::NONE) {
        type = field.segFieldInfo.type;
      }
      allFlags |= field.segFieldInfo.flags;
      if (field.segFieldInfo.numValues != 0
          && !(field.segFieldInfo.flags & FieldType::INDEX_DOCS)) allIndexed = false;
    }
    // Numeric postings are an optional complete acceleration of the column.
    // A mode change can leave inputs without postings; their values must not
    // disappear behind a partial dictionary. Keep the merged column instead.
    // TODO: take the merged index structures from the current schema instead
    // of the inputs' flags. Today postings drop on mixed inputs (none -> match
    // never takes effect for merged data) while points are synthesized from
    // the column (range -> none never takes effect). Building postings from
    // the column as synthesizePointRun does for points would let index mode
    // changes converge in both directions.
    if (FieldType::storage(type) == FieldType::Storage::INT64 && !allIndexed) {
      allFlags &= ~FieldType::INDEX_DOCS_FREQS_POSITIONS;
    }
  }

  int64_t estimateCost(const Space& space, std::span<const MergeFieldInfo> fields) const {
    assert(!fields.empty());
    if (isStoredField(fields[0].segFieldInfo)) {
      StoredChunkFormat format = storedFormat(std::string_view(fields[0].segFieldInfo.fieldname));
      int64_t maxChunkBytes = format.chunkTarget;
      for (const auto& field : fields) {
        maxChunkBytes = std::max(
            maxChunkBytes,
            StoredFieldsReader::peekMaxChunkBytes(*field.seg->postingsReader, field.segFieldInfo));
      }
      // Stored-field merge processes one source segment at a time.  The
      // value-scaled buffers are: source decompressed chunk, writer per-doc
      // staging, writer chunk body, writer assembled uncompressed chunk, and
      // compression output.  LIGHT_BYTES covers fixed stream/mono scratch,
      // the small compress-bound overhead above 1x and a ZSTD source's
      // decompression context; a ZSTD output adds its compression context.
      return MergeCostModel::storedFieldsBytes(maxChunkBytes)
          + StoredChunkCompressor::contextBytes(format, maxChunkBytes);
    }

    FieldType::Type type;
    int32_t allFlags;
    batchTypeAndFlags(fields, type, allFlags);
    auto storage = FieldType::storage(type);

    int64_t numValues = 0;
    int64_t docsWithField = 0;
    int64_t synthesizedPointValues = 0;
    int64_t sourceTerms = 0;
    bool ordColumn = true;
    bool replay = false;
    for (const auto& field : fields) {
      sourceTerms += field.segFieldInfo.nTerms;
      ordColumn = ordColumn && mergesOrdColumn(field.segFieldInfo);
      replay = replay || replaysOrdColumn(field.segFieldInfo);
      numValues += field.segFieldInfo.numValues;
      docsWithField += field.segFieldInfo.docsWithField;
      if (field.segFieldInfo.pointsMetaOff == 0) {
        synthesizedPointValues += field.segFieldInfo.numValues;
      }
    }
    int64_t pointsBytes = hasOneDimensionalPoints(type, allFlags)
        ? MergeCostModel::pointsBytes((int64_t)fields.size(), numValues,
                                      synthesizedPointValues)
        : hasGeoPoints(type, allFlags)
          ? MergeCostModel::geoPointsBytes(numValues)
          : hasRangePoints(type, allFlags)
            ? MergeCostModel::rangePointsBytes(numValues) : 0;
    int64_t termIndexBytes = (allFlags & FieldType::INDEX_DOCS) != 0
        ? MergeCostModel::termIndexBytes(sourceTerms, Postings::TERMS_BLOCK_SIZE) : 0;

    if (storage == FieldType::Storage::TERMS && (allFlags & FieldType::INDEX_DOCS) != 0
        && ordColumn) {
      // Multi-valued columns are replayed from the source columns and hold
      // only per-source term ord maps.
      if (replay) {
        int64_t mapBytes = 0;
        for (const auto& field : fields) mapBytes += (field.segFieldInfo.nTerms + 1) * sizeof(uint32_t);
        // Spilled maps are mapped page cache; the heap keeps a write buffer each.
        if (mapBytes >= ordMapSpillMinBytes) {
          mapBytes = (int64_t)fields.size() * (int64_t)ScratchFile::DEFAULT_BUFFER_BYTES;
        }
        // Mapped source runs stream directly; only maps and codec plans grow.
        int64_t blocks = (numValues + OrdColumnFormat::BLOCK_SIZE - 1) / OrdColumnFormat::BLOCK_SIZE;
        return mapBytes + (int64_t)fields.size() * sizeof(std::vector<uint32_t>)
            + blocks * (2 * sizeof(OrdColumnFormat::PredictedBlockPlan))
            + MergeCostModel::LIGHT_BYTES + pointsBytes + termIndexBytes;
      }
      // Single-valued columns hold the term-order -> doc-order transposition
      // in RAM until the postings pass finishes: OrdCollector's
      // ords[mergedMaxDoc] (int32).
      return (int64_t)space.maxDoc * 4
          + MergeCostModel::LIGHT_BYTES + pointsBytes + termIndexBytes;
    }
    if (storage == FieldType::Storage::TEXT && (allFlags & FieldType::INDEX_DOCS) != 0) {
      // Text postings stream. What accumulates is the norms build (roughly one
      // byte per doc-with-field plus its DocStream).
      return docsWithField * 2 + MergeCostModel::LIGHT_BYTES + pointsBytes + termIndexBytes;
    }
    // Int/str/vector columns and stored fields stream through fixed block buffers.
    return MergeCostModel::LIGHT_BYTES + pointsBytes + termIndexBytes;
  }

  // In postings and values processed. A merged term is a heap step over the
  // sources plus per-term encoding, worth many postings.
  static int64_t estimateWork(std::span<const MergeFieldInfo> fields) {
    constexpr int64_t TERM_WORK = 16;
    int64_t work = 0;
    for (const auto& field : fields) {
      const auto& info = field.segFieldInfo;
      work += std::max<int64_t>(0, info.nTerms) * TERM_WORK
          + std::max({info.sumTotalTermFreq, info.sumDocFreq, info.numValues,
                      (int64_t) info.docsWithField, (int64_t) 0});
    }
    return work;
  }

  static int32_t estimateStreams(std::span<const MergeFieldInfo> fields) {
    assert(!fields.empty());
    if (isStoredField(fields[0].segFieldInfo)) {
      return MergeCostModel::STORED_STREAMS;
    }

    FieldType::Type type;
    int32_t allFlags;
    batchTypeAndFlags(fields, type, allFlags);
    auto storage = FieldType::storage(type);

    if (storage == FieldType::Storage::TERMS && (allFlags & FieldType::INDEX_DOCS) != 0) {
      return MergeCostModel::ORD_COL_STREAMS;
    }
    if (storage == FieldType::Storage::TEXT && (allFlags & FieldType::INDEX_DOCS) != 0) {
      return MergeCostModel::TEXT_STREAMS;
    }
    if ((storage == FieldType::Storage::TERMS || storage == FieldType::Storage::VECTOR)
        && (allFlags & FieldType::INDEX_DOCS) == 0) {
      return MergeCostModel::STR_COL_STREAMS;
    }
    return MergeCostModel::INT_COL_STREAMS
        + (hasOneDimensionalPoints(type, allFlags)
           ? MergeCostModel::POINTS_STREAMS
           : hasGeoPoints(type, allFlags) || hasRangePoints(type, allFlags)
             ? MergeCostModel::GEO_POINTS_STREAMS : 0);
  }

  // Helper to build mapping from old doc IDs to new doc IDs for a segment, accounting for deletes
  // Returns a vector where vector[oldDocId] = newDocId, or -1 if deleted.
  // This is still 0 based, so add the base in both cases.
  static void buildDocIdMapping(int32_t maxDoc, const LiveDocs* liveDocs, std::vector<int32_t>& target) {
    if (liveDocs == nullptr) return;
    assert(liveDocs->size() == maxDoc); // make sure this is the right liveDocs for the segment.
    target.resize(maxDoc);
    int32_t newDocId = 0;
    const FixedBitSet& bitset = liveDocs->bitset();
    for (int32_t oldDocId = 0; oldDocId < maxDoc; oldDocId++) {
      if (bitset.get(oldDocId)) {
        target[oldDocId] = newDocId++;
      } else {
        target[oldDocId] = -1; // Document is deleted
      }
    }
    assert(newDocId == liveDocs->numLive()); // Make sure we got the right number of live documents
  }

  void writeRootVersions() {
    PostingsWriter::VersionRunsWriter versions(postingsWriter);
    for (size_t i = 0; i < root.segs.size(); i++) {
      const auto& seg = root.segs[i];
      VersionRuns::Iterator runs(seg.postingsReader->core().versionRuns());
      VersionRun current, next;
      if (seg.remap.empty()) {
        while (runs.next(current)) versions.add((uint32_t)seg.base + current.firstDocId, current.version);
        continue;
      }
      if (!runs.next(current)) continue;
      for (;;) {
        bool haveNext = runs.next(next);
        int32_t end = haveNext ? (int32_t)next.firstDocId : seg.postingsReader->maxDoc();
        int32_t first = (int32_t)current.firstDocId;
        if (seg.remap[first] == -1) first = root.liveDocs[i]->bitset().nextSetBit(first + 1, end);
        if (first < end) versions.add((uint32_t)(seg.base + seg.remap[first]), current.version);
        if (!haveNext) break;
        current = next;
      }
    }
    versions.finish();
  }

  void addDocsFreq(TextWriter& textWriter, DocsFreqEnum& docsEnum, const Segment& seg) {
    for (;;) {
      int32_t docid = docsEnum.nextDoc();
      if (docid == INT_MAX) break;

      int mappedDoc = seg.remap.empty() ? docid : seg.remap[docid];
      if (mappedDoc == -1) {
        continue;
      }
      // termFreq() is 1 for DOCS-only fields, the real freq for DOCS_AND_FREQS.
      textWriter.addDoc(seg.base + mappedDoc, docsEnum.termFreq());
    }
  }

  void addDocsPos(TextWriter& textWriter, DocsPosEnum& docsEnum, const Segment& seg) {
    PosEnum positions(docsEnum);
    std::array<int32_t, Postings::DOCS_BLOCK_SIZE> mappedDocs;
    int32_t docid = docsEnum.nextDoc();
    while (docid != INT_MAX) {
      auto block = docsEnum.currentDocFreqBlock();
      int32_t blockCount = (int32_t) block.docs.size();
      assert(blockCount > 0);
      assert(block.tfreqs.size() == block.docs.size());

      bool allLive = true;
      if (seg.remap.empty()) {
        for (int32_t i = 0; i < blockCount; i++) {
          mappedDocs[(size_t) i] = seg.base + block.docs[(size_t) i];
        }
      } else {
        for (int32_t i = 0; i < blockCount; i++) {
          int32_t sourceDoc = block.docs[(size_t) i];
          int32_t mappedDoc = seg.remap[sourceDoc];
          if (mappedDoc == -1) {
            allLive = false;
            break;
          }
          mappedDocs[(size_t) i] = seg.base + mappedDoc;
        }
      }

      if (allLive) {
        positions.beginPositionDeltaBatch(blockCount);
        textWriter.addDocsWithPositions(
            std::span<const int32_t>(mappedDocs.data(), (size_t) blockCount),
            block.tfreqs,
            [&](int64_t maxCount) {
              return positions.nextPositionDeltaBatchSpan(maxCount);
            });
        docid = docsEnum.nextDoc();
        continue;
      }

      // A block containing any delete retains the doc-scoped path so skipped
      // tfs continue through the existing deferred position repair.
      for (int32_t i = 0; i < blockCount; i++) {
        assert(docid == block.docs[(size_t) i]);
        int32_t mappedDoc = seg.remap[docid];
        if (mappedDoc != -1) {
          int32_t newDocid = seg.base + mappedDoc;
          int32_t tf = docsEnum.termFreq();
          textWriter.startDoc(newDocid);
          positions.startPositions();
          for (;;) {
            auto deltas = positions.nextPositionDeltaSpan();
            if (deltas.empty()) break;
            textWriter.appendPositionDeltas(deltas);
          }
          textWriter.endDoc(newDocid, tf);
        }
        docid = docsEnum.nextDoc();
      }
    }
  }

  // Add docs and ordinals from the provided postings enum (for string columns,
  // record ord in docToOrd for each doc, to be written later).
  void addDocsOrds(TextWriter& textWriter, DocsOnlyEnum& docsEnum, const Segment& seg,
                   OrdCollector& docToOrd, int64_t ord) {
    assert(ord > 0 && ord <= INT32_MAX);
    for(;;) {
      int32_t docid = docsEnum.nextDoc();
      if (docid == INT_MAX) break;

      // predictable branch for deleted vs not
      int mappedDoc = seg.remap.empty() ? docid : seg.remap[docid];
      if (mappedDoc == -1) {
        continue; // Document is deleted
      }
      int32_t newDocid = seg.base + mappedDoc;

      docToOrd.add(newDocid, (uint32_t)ord);
      textWriter.addDoc(newDocid, 1);  // DOCS-only string column: no positions to copy
    }
  }

  void buildMergedNorms(std::span<MergeFieldInfo*> compactFields, Stream& normBytes,
                        DocStream& normDocsWithField, int32_t& numDocsWithField, MemPool& pool) {
    numDocsWithField = 0;
    for (auto* field : compactFields) {
      auto& seg = *field->seg;
      NormsReader reader(*seg.postingsReader, field->segFieldInfo);
      NormsReader::Iterator iter(reader);
      for (int32_t localId = iter.next(); localId != NormsReader::ENDDOC; localId = iter.next()) {
        auto [mappedDoc, isDeleted] = seg.remapDocId(localId);
        if (isDeleted) {
          continue;
        }
        normBytes.writeByte(pool, iter.value());
        normDocsWithField.addDoc(pool, mappedDoc);
        numDocsWithField++;
      }
    }
  }

  struct TermRangePlan {
    std::optional<std::string> lower;
    std::optional<std::string> upper;
    uint64_t bytes = 0;  // source postings bytes of the range's terms
  };

  // The coordinator holds the merged norms until stitch. Range term blocks are
  // charged by the ranges as they grow, then handed to the coordinator.
  static int64_t partitionCoordinatorCost(std::span<MergeFieldInfo*> fields) {
    int64_t docsWithField = 0;
    for (const auto* field : fields) docsWithField += field->segFieldInfo.docsWithField;
    return docsWithField * 2;
  }

  static int64_t sourceTermBlocks(std::span<MergeFieldInfo*> fields) {
    int64_t blocks = 0;
    for (const auto* field : fields) {
      blocks += (field->segFieldInfo.nTerms + Postings::TERMS_BLOCK_SIZE - 1)
          / Postings::TERMS_BLOCK_SIZE;
    }
    return blocks;
  }

  // Planning reads every stride-th source term block.
  static int32_t termPlanStride(std::span<MergeFieldInfo*> fields) {
    int64_t blocks = sourceTermBlocks(fields);
    return (int32_t) std::max<int64_t>(
        1, (blocks + MergeCostModel::MAX_TERM_PLAN_SAMPLES - 1)
               / MergeCostModel::MAX_TERM_PLAN_SAMPLES);
  }

  // Planning copies each sampled block's first term: about one term in
  // TERMS_BLOCK_SIZE * stride of the source dictionaries.
  static int64_t termPlanCost(std::span<MergeFieldInfo*> fields) {
    int64_t stride = termPlanStride(fields);
    int64_t samples = 0;
    int64_t dictionaryBytes = 0;
    for (const auto* field : fields) {
      const auto& info = field->segFieldInfo;
      if (info.nTerms == 0) continue;
      int64_t blocks = (info.nTerms + Postings::TERMS_BLOCK_SIZE - 1) / Postings::TERMS_BLOCK_SIZE;
      samples += (blocks + stride - 1) / stride;
      assert(info.termBlockIndexLoc.filenum() == info.termsLoc.filenum());
      assert(info.termBlockIndexLoc.offset() >= info.termsLoc.offset());
      dictionaryBytes += (int64_t) (info.termBlockIndexLoc.offset() - info.termsLoc.offset());
    }
    return MergeCostModel::termPlanBytes(samples)
        + dictionaryBytes / Postings::TERMS_BLOCK_SIZE / stride;
  }

  // Range count follows source posting bytes, not the indexing RAM cap, with a
  // ceiling derived from this merge's size-scaled stream budget (an explicit
  // setting can only lower it).  Range docs and positions stream directly to
  // checked-out segment files; only term blocks remain buffered so stitch()
  // can preserve the reader invariant that every range's termsBase shares the
  // block-index stream's file.
  std::optional<std::vector<TermRangePlan>> planTermRanges(
      std::span<MergeFieldInfo*> compactFields, MemPool& pool) {
    struct WeightedTerm {
      size_t block;
      uint64_t bytes;
    };

    int64_t sourceTerms = 0;
    for (auto* field : compactFields) {
      sourceTerms += field->segFieldInfo.nTerms;
    }
    int64_t minimumPartitionBlocks =
        (sourceTerms + Postings::TERMS_BLOCK_SIZE - 1) / Postings::TERMS_BLOCK_SIZE + 1;
    if (minimumPartitionBlocks > (int64_t) TrieBuilder::MAX_BLOCKS) return std::nullopt;

    int32_t stride = termPlanStride(compactFields);
    std::vector<TermsEnum::BlockEstimate> sourceBlocks;
    sourceBlocks.reserve((size_t) (minimumPartitionBlocks / stride) + compactFields.size());
    for (auto* field : compactFields) {
      if (field->segFieldInfo.nTerms == 0) continue;
      TermsEnum terms(pool, *field->seg->postingsReader, field->segFieldInfo);
      for (auto& block : terms.estimateTermBlocks(stride)) sourceBlocks.push_back(std::move(block));
    }
    if (sourceBlocks.empty()) return std::nullopt;

    std::vector<WeightedTerm> weighted;
    weighted.reserve(sourceBlocks.size());
    for (size_t i = 0; i < sourceBlocks.size(); i++) {
      weighted.push_back({i, sourceBlocks[i].bytes});
    }
    auto termFor = [&](const WeightedTerm& entry) -> const std::string& {
      return sourceBlocks[entry.block].firstTerm;
    };

    std::sort(weighted.begin(), weighted.end(), [&](const WeightedTerm& a, const WeightedTerm& b) {
      return termFor(a) < termFor(b);
    });
    size_t out = 0;
    for (size_t i = 0; i < weighted.size(); i++) {
      WeightedTerm& entry = weighted[i];
      if (out != 0 && termFor(weighted[out - 1]) == termFor(entry)) {
        uint64_t& bytes = weighted[out - 1].bytes;
        bytes = UINT64_MAX - bytes < entry.bytes ? UINT64_MAX : bytes + entry.bytes;
      } else {
        if (out != i) weighted[out] = entry;
        out++;
      }
    }
    weighted.resize(out);

    uint64_t totalBytes = 0;
    for (const auto& entry : weighted) {
      totalBytes = UINT64_MAX - totalBytes < entry.bytes
          ? UINT64_MAX : totalBytes + entry.bytes;
    }
    if (totalBytes < (uint64_t) termPartitionMinBytes || totalBytes == 0) {
      return std::nullopt;
    }

    int32_t maxRanges = MergeCostModel::maxTermRangesForStreams(streamBudget);
    if (termPartitionMaxRanges != MergeCostModel::DERIVED_TERM_RANGES) {
      maxRanges = std::min(maxRanges, termPartitionMaxRanges);
    }
    uint64_t quotient = totalBytes / (uint64_t) termPartitionMinRangeBytes;
    int32_t desiredRanges = (int32_t) std::min<uint64_t>(
        (uint64_t) maxRanges, std::max<uint64_t>(2, quotient));

    auto buildPlan = [&](int32_t rangeTarget) {
      std::vector<std::string> splits;
      splits.reserve((size_t) rangeTarget - 1);
      uint64_t accumulated = 0;
      int32_t nextCut = 1;
      for (const auto& entry : weighted) {
        const std::string& term = termFor(entry);
        while (nextCut < rangeTarget
               && accumulated >= (totalBytes / (uint64_t) rangeTarget) * (uint64_t) nextCut) {
          if (splits.empty() || splits.back() != term) {
            splits.push_back(term);
          }
          nextCut++;
        }
        accumulated = UINT64_MAX - accumulated < entry.bytes
            ? UINT64_MAX : accumulated + entry.bytes;
      }
      if (splits.empty()) return std::vector<TermRangePlan>{};

      std::vector<TermRangePlan> plan(splits.size() + 1);
      for (size_t i = 0; i < plan.size(); i++) {
        if (i != 0) plan[i].lower = splits[i - 1];
        if (i < splits.size()) plan[i].upper = splits[i];
      }
      for (const auto& entry : weighted) {
        const std::string& term = termFor(entry);
        size_t range = (size_t) (std::upper_bound(splits.begin(), splits.end(), term)
            - splits.begin());
        uint64_t& bytes = plan[range].bytes;
        bytes = UINT64_MAX - bytes < entry.bytes ? UINT64_MAX : bytes + entry.bytes;
      }
      return plan;
    };

    std::vector<TermRangePlan> plan = buildPlan(desiredRanges);
    if (plan.size() < 2) return std::nullopt;

    int64_t worstBlocks = (sourceTerms + Postings::TERMS_BLOCK_SIZE - 1)
        / Postings::TERMS_BLOCK_SIZE + (int64_t) plan.size() - 1;
    if (worstBlocks > (int64_t) TrieBuilder::MAX_BLOCKS) return std::nullopt;
    return plan;
  }

  struct NoTermHook {
    void operator()() const {}
  };

  // Bytes of the per-source term ord maps a replay merge builds.
  static int64_t replayOrdMapBytes(std::span<MergeFieldInfo*> fields) {
    int64_t bytes = 0;
    for (auto* field : fields) bytes += (field->segFieldInfo.nTerms + 1) * (int64_t)sizeof(uint32_t);
    return bytes;
  }

  // Per-source maps from a source term ord (slot ord + 1; slot 0 is unused)
  // to the merged term ord, for a replay merge. The term walk visits each
  // source's terms in increasing ord, so a map is written append-only and,
  // when spilled, streams to a scratch file that is mapped for the column
  // write: the heap then holds only a write buffer per source.
  class ReplayOrdMaps {
    std::vector<std::vector<uint32_t>> heap;
    std::vector<std::unique_ptr<ScratchFile>> files;
    std::vector<int64_t> slots;  // per source: map length (nTerms + 1)
    std::vector<int64_t> next;   // per source: next slot to write (spilled)
    std::vector<std::span<const uint32_t>> maps;

    void appendZeros(size_t idx, int64_t upTo) {
      static constexpr uint32_t ZEROS[256] = {};
      while (next[idx] < upTo) {
        int64_t n = std::min<int64_t>(upTo - next[idx], 256);
        files[idx]->append(ZEROS, (size_t)n * sizeof(uint32_t));
        next[idx] += n;
      }
    }

  public:
    ReplayOrdMaps(std::span<MergeFieldInfo*> fields, bool spill) {
      for (auto* field : fields) slots.push_back(field->segFieldInfo.nTerms + 1);
      if (!spill) {
        for (int64_t n : slots) heap.emplace_back((size_t)n);
        return;
      }
      for (size_t i = 0; i < slots.size(); i++) {
        files.push_back(std::make_unique<ScratchFile>());
        next.push_back(0);
      }
    }

    bool spilled() const { return !files.empty(); }

    void set(size_t idx, int64_t sourceOrd, uint32_t termOrd) {
      int64_t slot = sourceOrd + 1;
      assert(slot > 0 && slot < slots[idx]);
      if (!spilled()) {
        heap[idx][(size_t)slot] = termOrd;
        return;
      }
      assert(slot >= next[idx]);
      appendZeros(idx, slot);
      files[idx]->append(&termOrd, sizeof(termOrd));
      next[idx] = slot + 1;
    }

    // Completes every map; call once the term walk is done.
    void finish() {
      maps.clear();
      for (size_t i = 0; i < slots.size(); i++) {
        if (!spilled()) {
          maps.emplace_back(heap[i]);
          continue;
        }
        appendZeros(i, slots[i]);
        auto bytes = files[i]->map();
        maps.emplace_back((const uint32_t*)bytes.data(), bytes.size() / sizeof(uint32_t));
      }
    }

    std::span<const uint32_t> operator[](size_t idx) const { return maps[idx]; }
  };

  // onTerm runs after each merged term is written.
  template<DocsEnumTier Tier, bool Array = false, typename OnTerm = NoTermHook>
  void mergeTermsTier(TextWriter& textWriter, std::span<MergeFieldInfo*> compactFields,
                      OrdCollector* ordCollector, const std::optional<std::string>& lower,
                      const std::optional<std::string>& upper, MemPool& pool,
                      ReplayOrdMaps* ordMaps = nullptr,
                      OnTerm onTerm = {}) {
    struct TermsEnumIdx {
      TermsEnum tenum;
      size_t idx;
      // tenum.termPrefixKey(), refreshed whenever tenum moves: most queue
      // comparisons are decided by it without touching the terms.
      unsigned __int128 key = 0;
    };
    std::vector<TermsEnumIdx> tenums;
    tenums.reserve(compactFields.size());
    std::vector<TermsEnumIdx*> tenumPtrs;
    tenumPtrs.reserve(compactFields.size());

    for (size_t idx = 0; idx < compactFields.size(); idx++) {
      auto* field = compactFields[idx];
      tenums.emplace_back(TermsEnumIdx{
          TermsEnum(pool, *field->seg->postingsReader, field->segFieldInfo), idx});
      bool positioned = lower.has_value()
          ? tenums.back().tenum.seekCeil(*lower) : tenums.back().tenum.nextTerm();
      if (positioned && (!upper.has_value() || tenums.back().tenum.term() < *upper)) {
        tenums.back().key = tenums.back().tenum.termPrefixKey();
        tenumPtrs.push_back(&tenums.back());
      }
    }

    auto termCmp = [](const TermsEnumIdx& a, const TermsEnumIdx& b) {
      if (a.key != b.key) return b.key < a.key;
      int cmp = b.tenum.term() <=> a.tenum.term();
      return cmp < 0 || (cmp == 0 && b.idx < a.idx);
    };
    IndirectPQ<TermsEnumIdx, decltype(termCmp)> termPQ(tenumPtrs);

    char termBuf[PackedTerm::MAX_BYTES];
    while (termPQ.size() > 0) {
      TermsEnumIdx& first = termPQ.top();
      PackedTerm term(termBuf);
      first.tenum.term().copyTo(term);
      auto termKey = first.key;
      int64_t termOrd = textWriter.startTerm(term);

      do {
        TermsEnumIdx& entry = termPQ.top();
        BasicDocsEnum<Tier> docsEnum(entry.tenum);
        if constexpr (Tier == DocsEnumTier::DOCS) {
          if constexpr (Array) {
            ordMaps->set(entry.idx, entry.tenum.ord(), (uint32_t)termOrd);
            const auto& seg = *compactFields[entry.idx]->seg;
            for (int32_t doc = docsEnum.nextDoc(); doc != INT_MAX; doc = docsEnum.nextDoc()) {
              auto [mapped, deleted] = seg.remapDocId(doc);
              if (!deleted) textWriter.addDoc(mapped, 1);
            }
          } else {
            assert(ordCollector != nullptr);
            addDocsOrds(textWriter, docsEnum, *compactFields[entry.idx]->seg,
                        *ordCollector, termOrd);
          }
        } else if constexpr (Tier == DocsEnumTier::FREQS) {
          addDocsFreq(textWriter, docsEnum, *compactFields[entry.idx]->seg);
        } else {
          addDocsPos(textWriter, docsEnum, *compactFields[entry.idx]->seg);
        }

        bool next = entry.tenum.nextTerm();
        if (next && (!upper.has_value() || entry.tenum.term() < *upper)) {
          entry.key = entry.tenum.termPrefixKey();
          termPQ.updateTop();
        } else {
          termPQ.removeTop();
        }
      } while (termPQ.size() > 0 && termPQ.top().key == termKey &&
               termPQ.top().tenum.term() == term);

      textWriter.endTerm(term);
      onTerm();
    }
  }

  template<typename OnTerm = NoTermHook>
  void mergeTerms(TextWriter& textWriter, std::span<MergeFieldInfo*> compactFields,
                  bool isOrdCol, bool hasPositions, OrdCollector* ordCollector,
                  const std::optional<std::string>& lower,
                  const std::optional<std::string>& upper, MemPool& pool,
                  OnTerm onTerm = {}) {
    if (isOrdCol) {
      mergeTermsTier<DocsEnumTier::DOCS, false, OnTerm>(
          textWriter, compactFields, ordCollector, lower, upper, pool, nullptr, onTerm);
    } else if (hasPositions) {
      mergeTermsTier<DocsEnumTier::POSITIONS, false, OnTerm>(
          textWriter, compactFields, ordCollector, lower, upper, pool, nullptr, onTerm);
    } else {
      // This non-ord path also covers ID fields, not only positionless text.
      mergeTermsTier<DocsEnumTier::FREQS, false, OnTerm>(
          textWriter, compactFields, ordCollector, lower, upper, pool, nullptr, onTerm);
    }
  }

  // A replayable, doc-ordered source for the writer's count/plan/encode passes.
  // Only one source column/cursor is live at a time. Sequence payloads stay on
  // disk rather than being copied into another merge-time collector.
  class MergedSourceOrds {
    std::span<MergeFieldInfo*> fields;
    const ReplayOrdMaps& maps;
    int32_t maxDoc;
    int32_t docCount = 0;
    bool multi = false;
    mutable size_t fieldIndex = 0;
    mutable std::optional<OrdColReader> column;
    mutable std::optional<OrdColReader::Iterator> iter;
    mutable int32_t nextDoc = -1;
    mutable int32_t requested = -1;

    void advance() const {
      while (fieldIndex < fields.size()) {
        auto& field = *fields[fieldIndex];
        if (!column) {
          column.emplace(*field.seg->postingsReader, field.segFieldInfo);
          iter.emplace(*column);
        }
        for (int32_t doc = iter->next(); doc != OrdColReader::ENDDOC; doc = iter->next()) {
          auto [mapped, deleted] = field.seg->remapDocId(doc);
          if (!deleted) { nextDoc = mapped; return; }
        }
        iter.reset();
        column.reset();
        fieldIndex++;
      }
      nextDoc = OrdColReader::ENDDOC;
    }

    bool seek(int32_t doc) const {
      if (requested < 0 || doc < requested) {
        iter.reset();
        column.reset();
        fieldIndex = 0;
        nextDoc = -1;
      }
      requested = doc;
      while (nextDoc < doc) advance();
      return nextDoc == doc;
    }

  public:
    MergedSourceOrds(std::span<MergeFieldInfo*> fields,
                     const ReplayOrdMaps& maps, int32_t maxDoc)
        : fields(fields), maps(maps), maxDoc(maxDoc) {
      for (auto* field : fields) {
        OrdColReader source(*field->seg->postingsReader, field->segFieldInfo);
        OrdColReader::Iterator docs(source);
        for (int32_t doc = docs.next(); doc != OrdColReader::ENDDOC; doc = docs.next()) {
          if (field->seg->remapDocId(doc).second) continue;
          docCount++;
          if (source.multiValued() && !multi) {
            auto [start, end] = source.getStartEndValueRank(docs.rank());
            multi = end - start > 1;
          }
        }
      }
    }
    int32_t size() const { return maxDoc; }
    int32_t docsWithValue() const { return docCount; }
    bool multiValued() const { return multi; }
    bool hasValues(int32_t doc) const { return seek(doc); }
    void pushValues(int32_t doc, auto&& acceptor) const {
      if (!seek(doc)) return;
      const auto& map = maps[fieldIndex];
      if (!column->multiValued()) {
        acceptor(map[iter->value()]);
      } else {
        auto [start, end] = column->getStartEndValueRank(iter->rank());
        for (int64_t rank = start; rank < end; rank++) {
          acceptor(map[iter->values().valueAt(rank)]);
        }
      }
    }
  };

  // Writes the merged docs-with-field set of an indexed field without a
  // column from each source's set, in merged doc order.
  void mergeDocsWithField(std::span<MergeFieldInfo*> compactFields,
                          PostingsWriter::IndexFieldInfo& output, MemPool& pool) {
    auto forEachDoc = [&](auto&& accept) {
      for (auto* field : compactFields) {
        const Segment& seg = *field->seg;
        DocsReader docs(*seg.postingsReader, field->segFieldInfo);
        auto emit = [&](int32_t doc) {
          auto [mapped, deleted] = seg.remapDocId(doc);
          if (!deleted) accept(mapped);
        };
        if (!docs.hasBitset()) {
          for (int32_t doc = 0; doc < docs.numDocs(); doc++) emit(doc);
          continue;
        }
        screaming::BitSet::Iterator it(docs.bitset());
        for (int32_t doc = it.next(); doc != screaming::BitSet::END; doc = it.next()) emit(doc);
      }
    };
    int32_t numDocs = 0;
    forEachDoc([&](int32_t) { numDocs++; });
    output.numValues = output.sumDocFreq;
    DocsWithValWriter::write(pool, postingsWriter, output, numDocs, forEachDoc);
  }

  // An indexed TERMS field keeps an ord column only when every source has one
  // and declares it (COLUMN_STORED): column: false drops it, and a schema
  // change never needs a column rebuilt for sources written without one.
  static bool mergesOrdColumn(const SegFieldInfo& info) {
    return info.hasOrdColumn() && (info.flags & FieldType::COLUMN_STORED) != 0;
  }

  // A source with a multi-valued ord column (ARRAY, or SET holding a
  // multi-valued doc) sends the merge to mergeOrdColumnByReplay.
  static bool replaysOrdColumn(const SegFieldInfo& info) {
    return (info.flags & FieldType::MULTI_ARRAY) != 0 || info.numValues > info.docsWithField;
  }

  // Rebuilds a multi-valued ord column doc-major from the source ord columns
  // through per-source term ord maps, holding one map entry per source term
  // rather than every doc's ords. ARRAY sequences cannot be reconstructed from
  // DOCS-only postings at all; SET columns could be uninverted from them, but
  // the sources already hold the doc-major order, so replaying them is faster
  // and its workspace does not grow with the values.
  void LUXIR_NOINLINE mergeOrdColumnByReplay(
      std::span<MergeFieldInfo*> fields, PostingsWriter::IndexFieldInfo& output,
      MemPool& pool) {
    ReplayOrdMaps ordMaps(fields, replayOrdMapBytes(fields) >= ordMapSpillMinBytes);
    {
      // Scoped so its term index is freed before the column write.
      TextWriter textWriter(postingsWriter);
      textWriter.startField(&output);
      mergeTermsTier<DocsEnumTier::DOCS, true>(textWriter, fields, nullptr,
          std::nullopt, std::nullopt, pool, &ordMaps);
      textWriter.endField();
    }

    ordMaps.finish();
    bool spilled = ordMaps.spilled();
    Signal::emit("mergeOrdColumnByReplay", &output, &spilled);
    MergedSourceOrds ords(fields, ordMaps, postingsWriter.getMaxDoc(output.space));
    BasicOrdColWriter<MergedSourceOrds> writer(pool, postingsWriter, output, ords);
    writer.finish();
  }

  class PartitionCoordinator {
    struct RangeOutput {
      TermRangePlan plan;
      RAMFile termsFile;
      // termsFile's bytes from range completion until stitch appends them.
      IndexRamBudget::Guard termsGuard;
      TextWriter::RangeResult result;
      seg_location docsBase;
      seg_location posBase;

      RangeOutput(TermRangePlan&& plan, int32_t ord)
        : plan(std::move(plan)),
          termsFile("merge-range-terms-" + std::to_string(ord)) {}
    };

    SegmentMerger& merger;
    Space& space;
    MergeAdmissionDriver& driver;
    IndexRamBudget::Guard ramGuard;
    MergeAdmissionDriver::HeldStreams heldStreams;
    std::vector<MergeFieldInfo> fields;
    std::vector<MergeFieldInfo*> compactFields;
    std::vector<TermRangePlan> plans;
    MemPool pool;
    Stream normBytes;
    DocStream normDocsWithField;
    NormsWriter::PreparedNorms preparedNorms;
    PostingsWriter::IndexFieldInfo* outputFieldInfo = nullptr;
    OutputStreamPtr termsStream;
    std::vector<std::unique_ptr<RangeOutput>> outputs;
    std::atomic<int32_t> completed = 0;
    int32_t flags;

  public:
    PartitionCoordinator(SegmentMerger& merger, Space& space, MergeAdmissionDriver& driver,
                         std::vector<MergeFieldInfo>&& fields,
                         std::vector<TermRangePlan>&& plans, int32_t flags)
      : merger(merger), space(space), driver(driver), fields(std::move(fields)), plans(std::move(plans)),
        normDocsWithField(pool), flags(flags) {
      std::vector<MergeFieldInfo*> bySegment(space.segs.size());
      for (auto& field : this->fields) bySegment[(size_t) field.seg->ord] = &field;
      compactFields.reserve(this->fields.size());
      for (auto* field : bySegment) {
        if (field != nullptr) compactFields.push_back(field);
      }
    }

    void start(const std::shared_ptr<PartitionCoordinator>& self,
               MergeAdmissionDriver::BatchAdmission& admission, int64_t fieldWork) {
      int32_t numNormDocsWithField = 0;
      merger.buildMergedNorms(compactFields, normBytes, normDocsWithField,
                              numNormDocsWithField, pool);

      outputFieldInfo = &merger.postingsWriter.addField(space.ordinal, compactFields[0]->segFieldInfo.fieldname);
      outputFieldInfo->type = FieldType::TEXT;
      outputFieldInfo->flags = flags;
      preparedNorms = NormsWriter::prepare(pool, merger.postingsWriter, *outputFieldInfo,
                                           normBytes, normDocsWithField,
                                           numNormDocsWithField);
      // Terms are stitched into one shared stream because TermsEnum reuses a
      // single termsIS across range rows.  docsBase and posBase encode their
      // own file numbers, so range postings need no equivalent coordinator file.
      termsStream = merger.postingsWriter.getOutputStream();

      ramGuard = admission.takeRamGuard();

      outputs.reserve(plans.size());
      for (size_t i = 0; i < plans.size(); i++) {
        outputs.push_back(std::make_unique<RangeOutput>(std::move(plans[i]), (int32_t) i));
      }

      std::vector<MergeAdmissionDriver::Batch> batches;
      batches.reserve(outputs.size());
      uint64_t planBytes = 0;
      for (const auto& output : outputs) planBytes += output->plan.bytes;
      int64_t rangeCost =
          MergeCostModel::termRangeWorkingBytes((int64_t) compactFields.size());
      for (size_t i = 0; i < outputs.size(); i++) {
        MergeAdmissionDriver::Batch batch;
        batch.cost = rangeCost;
        // Cuts land on block-leading terms, so a range holding a heavy term
        // runs long: queue ranges by their share of the source bytes.
        batch.work = planBytes == 0 ? fieldWork / (int64_t) outputs.size()
            : (int64_t) ((__int128) fieldWork * outputs[i]->plan.bytes / planBytes);
        batch.streams = MergeCostModel::TERM_RANGE_STREAMS;
        batch.name = std::string((std::string_view) outputFieldInfo->fieldname)
            + "#" + std::to_string(i);
        batch.body = [self, i](MergeAdmissionDriver&, MergeAdmissionDriver::Batch& current,
                               MergeAdmissionDriver::BatchAdmission& rangeAdmission) {
          self->mergeRange(i, current.cost, rangeAdmission);
        };
        batches.push_back(std::move(batch));
      }
      driver.replaceWithBatches(admission, MergeCostModel::TERM_COORDINATOR_STREAMS,
                                heldStreams, std::move(batches));
    }

  private:
    void mergeRange(size_t rangeOrd, int64_t workingBytes,
                    MergeAdmissionDriver::BatchAdmission& admission) {
      RangeOutput& output = *outputs[rangeOrd];
      auto poolGuard = MemPool::threadLocalPoolGuard();
      MemPool& rangePool = poolGuard.pool();

      OutputStream termsOut(&output.termsFile);
      auto rangeStreams = merger.postingsWriter.getOutputStreams<2>();
      OutputStream& docsOut = *rangeStreams[0];
      OutputStream& posOut = *rangeStreams[1];
      output.docsBase = docsOut.slocation();
      output.posBase = posOut.slocation();
      PostingsWriter::IndexFieldInfo rangeFieldInfo{};
      rangeFieldInfo.space = space.ordinal;
      rangeFieldInfo.type = FieldType::TEXT;
      rangeFieldInfo.flags = flags;

      TextWriter writer(merger.postingsWriter, termsOut, docsOut, posOut);
      writer.startField(&rangeFieldInfo);
      if (FieldType::hasPositions(flags)) {
        writer.setNorms(preparedNorms.textView());
      }
      // Term blocks stay in RAM until stitch, so charge them as they grow
      // rather than reserving an upper bound for the whole field up front.
      int64_t chargedTerms = 0;
      auto chargeTerms = [&] {
        int64_t bytes = (int64_t) termsOut.size();
        if (bytes - chargedTerms < MergeCostModel::TERM_RANGE_RESYNC_BYTES) return;
        chargedTerms = bytes;
        admission.forceRepriceRam(workingBytes + chargedTerms);
      };
      merger.mergeTerms(writer, compactFields, false, FieldType::hasPositions(flags), nullptr,
                        output.plan.lower, output.plan.upper, rangePool, chargeTerms);
      output.result = writer.finishTermRun();
      termsOut.flush(true);
      // Release the working set, keeping the term blocks and their block index
      // charged until stitch.
      int64_t retained = (int64_t) termsOut.size()
          + (int64_t) (output.result.termBlockOffsets.capacity() * sizeof(uint64_t));
      for (const auto& key : output.result.separatorKeys) {
        retained += (int64_t) (sizeof(std::string) + key.capacity());
      }
      output.termsGuard = admission.takeRamGuard();
      output.termsGuard.forceResize(retained);
      // A nonempty row embeds these file numbers in the terms stream, so make
      // their per-range files durable before release. Empty ranges produce no
      // row and should leave header-only streams buffered for reuse/collapse.
      if (output.result.nTerms != 0) {
        docsOut.flush(true);
        posOut.flush(true);
      }

      Signal::emit("termRangeStreamsCheckedOut", &output.docsBase, &output.posBase,
                   &rangeOrd);
      for (auto& stream : rangeStreams) stream.reset();

      bool last = completed.fetch_add(1, std::memory_order_acq_rel) + 1
          == (int32_t) outputs.size();
      if (last) stitch();
    }

    void stitch() {
      OutputStream& termsOut = *termsStream;
      std::vector<uint64_t> blockOffsets;
      std::vector<TermRangeRow> rows;
      TrieBuilder trieBuilder;
      std::string previousLastTerm;
      int64_t termOrd = 0;
      int32_t blockOrd = 0;
      int64_t sumDocFreq = 0;
      int64_t sumTotalTermFreq = 0;
      bool mayHaveFitted = false;

      for (auto& outputPtr : outputs) {
        RangeOutput& output = *outputPtr;
        auto& result = output.result;
        if (result.nTerms == 0) {
          output.termsGuard.release();
          continue;
        }

        seg_location termsBase = termsOut.slocation();
        if (blockOrd != 0) {
          trieBuilder.add(TextWriter::separatorKey(previousLastTerm, result.firstTerm),
                          (uint32_t) blockOrd);
        }
        assert(result.separatorKeys.size() == (size_t) result.nBlocks - 1);
        for (size_t i = 0; i < result.separatorKeys.size(); i++) {
          trieBuilder.add(result.separatorKeys[i], (uint32_t) blockOrd + (uint32_t) i + 1);
        }

        rows.push_back(TermRangeRow{
            (uint64_t) termOrd, (uint32_t) blockOrd, 0, output.docsBase, output.posBase,
            termsBase, result.docsBytes, result.posBytes});
        // Absolute bases normally live only in IndexFieldInfo and are patched
        // before serialization. TermRangeRow is the one append-only exception;
        // sparse survivor filenums let us pin its files instead of rewriting it.
        std::array<uint32_t, 3> rowFiles = {
            termsBase.filenum(), output.docsBase.filenum(), output.posBase.filenum()};
        merger.postingsWriter.protectOutputFiles(rowFiles);
        termsOut.appendFile(output.termsFile);
        output.termsGuard.release();
        blockOffsets.insert(blockOffsets.end(), result.termBlockOffsets.begin(),
                            result.termBlockOffsets.end());
        termOrd += result.nTerms;
        blockOrd += result.nBlocks;
        sumDocFreq += result.sumDocFreq;
        sumTotalTermFreq += result.sumTotalTermFreq;
        mayHaveFitted |= result.mayHaveFitted;
        previousLastTerm = result.lastTerm;
      }

      outputFieldInfo->nTerms = termOrd;
      outputFieldInfo->sumDocFreq = sumDocFreq;
      outputFieldInfo->sumTotalTermFreq = sumTotalTermFreq;
      if (mayHaveFitted) outputFieldInfo->flags |= FieldType::MAY_HAVE_FITTED;
      if (termOrd == 0) {
        outputFieldInfo->flags &= ~FieldType::TERM_RANGES;
        outputFieldInfo->termBlockIndexLoc = {0, 0};
        outputFieldInfo->termsLoc = {0, 0};
        outputFieldInfo->docsLoc = {0, 0};
        outputFieldInfo->posLoc = {0, 0};
        outputFieldInfo->trieLoc = {0, 0};
        outputFieldInfo->trieRootOff = 0;
        outputFieldInfo->rangeTableLoc = {0, 0};
      } else {
        assert(rows.size() >= 1);
        assert(blockOrd == (int32_t) blockOffsets.size());
        outputFieldInfo->termsLoc = rows[0].termsBase;
        outputFieldInfo->docsLoc = rows[0].docsBase;
        outputFieldInfo->posLoc = rows[0].posBase;
        // 8-aligned: the reader reads this table as a uint64_t array in place.
        termsOut.align(MAX_ALIGN);
        outputFieldInfo->termBlockIndexLoc = termsOut.slocation();
        termsOut.write(blockOffsets.data(), blockOffsets.size() * sizeof(blockOffsets[0]));

        outputFieldInfo->trieRootOff = (int64_t) trieBuilder.finish();
        const std::vector<char>& trieBytes = trieBuilder.bytes();
        assert(blockOffsets.size() * sizeof(blockOffsets[0]) + trieBytes.size()
               >= SVB_OVERREAD_PAD);
        outputFieldInfo->trieLoc = termsOut.slocation();
        termsOut.write(trieBytes.data(), trieBytes.size());

        termsOut.align(MAX_ALIGN);
        outputFieldInfo->rangeTableLoc = termsOut.slocation();
        termsOut.writeInt((int32_t) rows.size());
        termsOut.writeInt(blockOrd);
        for (const auto& range : rows) {
          termsOut.writeLong((int64_t) range.firstTermOrd);
          termsOut.writeInt((int32_t) range.firstBlockOrd);
          termsOut.writeInt((int32_t) range.padding);
          termsOut.writeLong((int64_t) range.docsBase.raw());
          termsOut.writeLong((int64_t) range.posBase.raw());
          termsOut.writeLong((int64_t) range.termsBase.raw());
          termsOut.writeLong((int64_t) range.docsBytes);
          termsOut.writeLong((int64_t) range.posBytes);
        }
        outputFieldInfo->flags |= FieldType::TERM_RANGES;
      }

      termsStream.reset();
      NormsWriter::writeValues(pool, merger.postingsWriter, *outputFieldInfo,
                               preparedNorms, normDocsWithField);
      heldStreams.release();
      ramGuard.release();
    }
  };

  void mergeField(Space& space, MergeAdmissionDriver::Batch& batch,
                  MergeAdmissionDriver& admissionDriver,
                  MergeAdmissionDriver::BatchAdmission& admission) {
    auto& mergeFieldInfos = batch.fields;

    // Stored-fields resources are segment-wide rather than normal per-field
    // columns.  Route each one (default or named column family) to a
    // dedicated merger.  Recognize them by the combination of type=BIN and
    // flags=STORED set by StoredFieldsWriter::finish().
    const auto& firstInfo = mergeFieldInfos[0].segFieldInfo;
    if (isStoredField(firstInfo)) {
      mergeStoredFields(space, mergeFieldInfos, batch.bootstrapStoredDictionary);
      return;
    }

    // Drop the field before any route can register it or write its indexes.
    if (std::ranges::none_of(mergeFieldInfos, hasLiveFieldValues)) return;

    int32_t nDocs = space.maxDoc;
    auto poolGuard = MemPool::threadLocalPoolGuard();
    auto& pool = poolGuard.pool();

    // sort mergeFieldInfos so we can add docids low to high.
    // This is a simple O(n+m) sort where n=number of segments and m is number of segments with this specific field.
    // Simply slot the field into it's place and then compact.
    std::vector<MergeFieldInfo*> sortedFields(space.segs.size());
    for (auto& field : mergeFieldInfos) {
      sortedFields[field.seg->ord] = &field;
    }

    // compactFields are sorted with nulls removed.
    std::vector<MergeFieldInfo*> compactFields;
    compactFields.reserve(mergeFieldInfos.size());
    for (auto* field : sortedFields) {
      if (field) {
        compactFields.push_back(field);
      }
    }

    // TODO: gather other stats to help us build the field (like if it's a dense field!)
    int32_t allFlags;
    FieldType::Type type;
    batchTypeAndFlags(mergeFieldInfos, type, allFlags);
    allFlags &= ~FieldType::SEGMENT_MARKERS;
    auto storage = FieldType::storage(type);
    bool isText = storage == FieldType::Storage::TEXT;
    bool isIndexedTerms = storage == FieldType::Storage::TERMS && (allFlags & FieldType::INDEX_DOCS);
    bool isOrdCol = isIndexedTerms
        && std::ranges::all_of(compactFields, [](auto* f) { return mergesOrdColumn(f->segFieldInfo); });
    if (isIndexedTerms && !isOrdCol) allFlags &= ~FieldType::COLUMN_STORED;
    bool hasImpactNorms = isText && FieldType::hasPositions(allFlags);
    if (isText && (allFlags & FieldType::INDEX_DOCS) != 0) {
      int64_t serialCost = estimateCost(space, mergeFieldInfos);
      int64_t coordinatorCost = partitionCoordinatorCost(compactFields);
      // Partitioning is an on-disk plan decision, so it must not depend on
      // transient pool occupancy after this batch has started.  Planning holds
      // one estimate per source term block; afterwards the coordinator keeps
      // only the merged norms.  Forced growth notifies budget pressure
      // immediately, allowing inverter shedding where reclaimable RAM exists
      // without blocking or retrying in this batch.
      admission.forceRepriceRam(
          std::max(serialCost, coordinatorCost + termPlanCost(compactFields)));
      auto rangePlan = planTermRanges(compactFields, pool);
      if (rangePlan.has_value()) {
        if (!admission.repriceRam(coordinatorCost)) std::unreachable();
        auto coordinator = std::make_shared<PartitionCoordinator>(
            *this, space, admissionDriver, std::move(mergeFieldInfos),
            std::move(*rangePlan), allFlags);
        coordinator->start(coordinator, admission, batch.work);
        return;
      }
      if (!admission.repriceRam(serialCost)) std::unreachable();
    }
    Stream normBytes;
    DocStream normDocsWithField(pool);
    int32_t numNormDocsWithField = 0;
    if (isText) {
      buildMergedNorms(compactFields, normBytes, normDocsWithField, numNormDocsWithField, pool);
    }

    // get/reserve a new fieldInfo from the postingsReader
    PostingsWriter::IndexFieldInfo& outputFieldInfo = postingsWriter.addField(space.ordinal, compactFields[0]->segFieldInfo.fieldname);
    outputFieldInfo.type = type;
    outputFieldInfo.flags = allFlags;
    outputFieldInfo.typeFlags = FieldType::typeParameters(type, firstInfo.typeFlags);
    NormsWriter::PreparedNorms preparedNorms;
    if (isText) {
      preparedNorms = NormsWriter::prepare(pool, postingsWriter, outputFieldInfo, normBytes,
                                           normDocsWithField, numNormDocsWithField);
    }

    if (isOrdCol && std::ranges::any_of(compactFields, [](auto* f) {
          return replaysOrdColumn(f->segFieldInfo); })) {
      mergeOrdColumnByReplay(compactFields, outputFieldInfo, pool);
      return;
    }
    std::optional<OrdCollector> ordCollector;
    // use a separate pool for the ordCollector since the ords will be built at the same time as the postings are read/written,
    // and we want to roll back much of that allocation, but preserve the ords.
    std::optional<MemPool> ordPool;
    if (isOrdCol) {
      ordPool.emplace();
      ordCollector.emplace(ordPool.value(), nDocs);
    }

    if (allFlags & FieldType::INDEX_DOCS) {
      // nocommit outputFieldInfo.flags |= 0x01;
      TextWriter textWriter(postingsWriter);
      textWriter.startField(&outputFieldInfo);
      if (hasImpactNorms) {
        textWriter.setNorms(preparedNorms.textView());
      }

      mergeTerms(textWriter, compactFields, isOrdCol, FieldType::hasPositions(allFlags),
                 isOrdCol ? &ordCollector.value() : nullptr,
                 std::nullopt, std::nullopt, pool);
      textWriter.endField();
    }

    if (isOrdCol) {
      // auto guard = pool.rewindScopeGuard();
      OrdColWriter ordsWriter(pool, postingsWriter, outputFieldInfo, ordCollector.value());
      ordsWriter.finish();
      // nothing is done after this in this method, so we can let the normal destructors clean up.
      // ordCollector.reset();
      // ordPool.reset();

    } else if (isIndexedTerms) {
      mergeDocsWithField(compactFields, outputFieldInfo, pool);
    } else if (isText) {
      NormsWriter::writeValues(pool, postingsWriter, outputFieldInfo, preparedNorms,
                               normDocsWithField);
    } else if ((storage == FieldType::Storage::TERMS || storage == FieldType::Storage::VECTOR)
               && !(allFlags & FieldType::INDEX_DOCS)) {
      // Non-indexed string/binary column (column-only storage).  Vector columns use
      // the same fixed-size binary blob format (VectorHandler extends StrColHandler).
      mergeStrCol(space, sortedFields, postingsWriter, outputFieldInfo);
    } else {
      // int column that is not an ord column (assume all other field types have this (currently true)
      std::vector<BKDWriter::Point> geoPoints;
      std::vector<BasicBKDWriter<BKDInt64Traits>::Point> rangePoints;
      bool rebuildGeo = hasGeoPoints(type, allFlags);
      bool rebuildRange = hasRangePoints(type, allFlags);
      if (rebuildGeo || rebuildRange) {
        // Upper bound (deleted docs drop out): no growth doubling. A range
        // point is a (min, max) value pair.
        int64_t values = 0;
        for (auto* field : compactFields) values += field->segFieldInfo.numValues;
        if (rebuildGeo) geoPoints.reserve((size_t)values);
        else rangePoints.reserve((size_t)(values / 2));
      }
      mergeIntCol(space, sortedFields, postingsWriter, outputFieldInfo,
                  rebuildGeo ? &geoPoints : nullptr,
                  rebuildRange ? &rangePoints : nullptr);
      if (rebuildRange) {
        if (!rangePoints.empty()) {
          auto output = postingsWriter.getOutputStream();
          BasicBKDWriter<BKDInt64Traits> writer(*output);
          auto data = writer.write(rangePoints);
          assert(data.pointCount * 2 == (uint64_t)outputFieldInfo.numValues);
          outputFieldInfo.pointsLoc = data.pointsLoc;
          outputFieldInfo.pointsMetaOff = data.pointsMetaOff;
        }
      } else if (rebuildGeo) {
        if (!geoPoints.empty()) {
          auto output = postingsWriter.getOutputStream();
          BKDWriter writer(*output);
          auto data = writer.write(geoPoints);
          assert(data.pointCount == (uint64_t)outputFieldInfo.numValues);
          outputFieldInfo.pointsLoc = data.pointsLoc;
          outputFieldInfo.pointsMetaOff = data.pointsMetaOff;
        }
      } else if (hasOneDimensionalPoints(type, allFlags)) {
        mergePoints(sortedFields, postingsWriter, outputFieldInfo);
      }
    }
  }


  // Output format for a stored-fields resource: the schema's current setting,
  // else the defaults.
  StoredChunkFormat storedFormat(std::string_view resourceName) const {
    auto* type = dynamic_cast<const StoredFieldType*>(schema.getFieldTypePtr(resourceName));
    return type ? type->format_ : StoredChunkFormat{};
  }

  std::vector<char> sampleStoredFields(const Space& space, std::span<const PostingsReader::Field* const> fields,
                                       size_t budget) {
    uint64_t chunks = 0;
    for (auto* field : fields) if (field) chunks += (uint64_t)field->numValues;
    std::vector<char> samples;
    if (!chunks) return samples;
    samples.reserve(budget);
    std::unique_ptr<StoredFieldsReader> reader;
    int source = -1;
    // Bit-reversed order visits 0, half, quarter, three quarters, ... across
    // the concatenated chunk ordinals. Skip excess ordinals for non-powers of
    // two. This spreads the sample without a per-chunk permutation/directory.
    uint64_t limit = std::bit_ceil(chunks);
    uint64_t position = 0;
    for (uint64_t visited = 0; visited < limit && samples.size() < budget; visited++) {
      if (position < chunks) {
        uint64_t chunk = position;
        for (auto& seg : space.segs) {
          auto* field = fields[seg.ord];
          if (!field) continue;
          if (chunk >= (uint64_t)field->numValues) {
            chunk -= (uint64_t)field->numValues;
            continue;
          }
          if (source != seg.ord) {
            reader = std::make_unique<StoredFieldsReader>(*seg.postingsReader, *field);
            source = seg.ord;
          }
          reader->readChunkDocs((int32_t)chunk, [&](int32_t doc, std::string_view encoded) {
            if (seg.remapDocId(doc).second || encoded.empty() || encoded[0] == 0) return true;
            size_t bytes = std::min(encoded.size(), budget - samples.size());
            samples.insert(samples.end(), encoded.data(), encoded.data() + bytes);
            return samples.size() < budget;
          });
          break;
        }
      }
      uint64_t bit = limit >> 1;
      while (position & bit) { position ^= bit; bit >>= 1; }
      position ^= bit;
    }
    return samples;
  }

  std::shared_ptr<const StoredCompressionDictionary> storedDictionary(
      const Space& space, std::string_view resource, const StoredChunkFormat& format,
      std::span<const PostingsReader::Field* const> fields, bool bootstrap) {
    auto dictionary = storedDictionaries.lookup(resource, format);
    if (!bootstrap || dictionary) return dictionary;
    auto token = storedDictionaries.token(resource, format);
    if (!token) return {};
    size_t budget = StoredChunkCompressor::dictionarySampleBudget(format);
    auto samples = sampleStoredFields(space, fields, budget);
    // Group encoded bytes into bounded training blocks. Document boundaries
    // need no side array, even when millions of tiny docs fill the budget.
    std::vector<size_t> sizes;
    if (samples.size() == budget) {
      size_t block = std::max(StoredChunkCompressor::DICTIONARY_SAMPLE_BYTES,
          (samples.size() + StoredChunkCompressor::MAX_DICTIONARY_SAMPLES - 1)
              / StoredChunkCompressor::MAX_DICTIONARY_SAMPLES);
      sizes.reserve((samples.size() + block - 1) / block);
      for (size_t offset = 0; offset < samples.size(); offset += block) {
        sizes.push_back(std::min(block, samples.size() - offset));
      }
    }
    std::string_view sample(samples.data(), samples.size());
    std::span<const size_t> sampleSizes(sizes);
    Signal::emit("storedFieldsDictionarySample", &resource, &sample, &sampleSizes);
    if (!sizes.empty() && !storedDictionaries.hasDictionary(resource, format)) {
      auto current = storedDictionaries.token(resource, format);
      if (current && current->epoch == token->epoch) {
        size_t capacity = std::max((size_t)1024,
            std::min((size_t)format.dictionaryBytes, samples.size() / 100));
        auto trained = StoredChunkCompressor::trainDictionary(sample, sizes, capacity);
        std::string_view bytes(trained);
        Signal::emit("storedFieldsDictionaryTrained", &resource, &bytes);
        if (!trained.empty()) {
          storedDictionaries.publish(resource, *token,
              std::make_shared<const StoredDictionary>(std::move(trained)));
        }
      }
    }
    return storedDictionaries.lookup(resource, format);
  }

  // Merge one stored-fields resource (default or named column family).
  // Writes a fresh StoredFieldsWriter for the same resource name in the
  // resource's current format. Sources in that layout within the dirt threshold
  // can be copied as compressed chunks; the rest have their live docs re-added
  // in merged-docID order. Adopt the largest eligible chunk region's field table
  // first so earlier re-added fields cannot prevent its copy. Re-added docs pack
  // together, but a copy boundary can seal another partial chunk. Each partial
  // chunk a merge leaves costs about 100 chunks of later recompression when its
  // segment crosses 1%; this is an amortization rule, not an output invariant.
  // Multi-valued fields keep their grouping through single addValues calls.
  void mergeStoredFields(Space& space, std::vector<MergeFieldInfo>& mergeFieldInfos, bool bootstrap) {
    // Resource name comes from the (identical across segments) field name.
    std::string_view resourceName(mergeFieldInfos[0].segFieldInfo.fieldname);

    // Build a per-segment field-info vector indexed by seg ord.  Null for
    // segments whose source had no stored-fields resource.  Readers are opened
    // one source segment at a time below so decompressed chunk scratch does not
    // accumulate across sources.
    std::vector<const PostingsReader::Field*> fieldInfos(space.segs.size());
    for (auto& mfi : mergeFieldInfos) {
      fieldInfos[mfi.seg->ord] = mfi.seg->postingsReader->fieldInfo(resourceName);
    }

    auto format = storedFormat(resourceName);
    StoredFieldsWriter writer(postingsWriter, space.ordinal, resourceName, format,
                              storedDictionary(space, resourceName, format, fieldInfos, bootstrap));
    std::vector<int64_t> srcChunkFirstDocs, srcChunkOffsets;

    auto copyable = [&](Segment& seg, const StoredFieldsReader& reader) {
      return !seg.hasDeletes() && reader.format().sameLayout(writer.format())
          && !StoredFieldsWriter::tooDirty(reader.numChunks(), reader.numDirtyChunks());
    };
    int bestSource = -1;
    int64_t bestBytes = 0;
    for (auto& seg : space.segs) {
      const auto* fieldInfo = fieldInfos[seg.ord];
      if (!fieldInfo) continue;
      StoredFieldsReader reader(*seg.postingsReader, *fieldInfo);
      if (copyable(seg, reader) && writer.canCopyChunks(reader.format(), reader.dictionary())
          && reader.chunkRegionBytes() > bestBytes) {
        bestSource = seg.ord;
        bestBytes = reader.chunkRegionBytes();
      }
    }
    if (bestSource >= 0) {
      StoredFieldsReader reader(*space.segs[bestSource].postingsReader, *fieldInfos[bestSource]);
      writer.adoptFieldTable(reader.fieldNames());  // writer's table is still empty
    }

    for (auto& seg : space.segs) {
      const auto* fieldInfo = fieldInfos[seg.ord];
      if (!fieldInfo) continue;  // segment had no stored-fields; writer pads automatically
      StoredFieldsReader reader(*seg.postingsReader, *fieldInfo);

      // Verbatim chunk copy: with no deletions every local doc maps to
      // base + localId, and compressed chunk bodies are position-independent,
      // so the whole source region is copied without decompression - the
      // dominant cost of a large merge is otherwise this resource's serial
      // recompression.  The source must have the output's codec and chunk
      // limits, the same dictionary and few enough dirty (partially filled)
      // chunks, and bodies reference its field-id table, so it must adopt into
      // ours; deletions, another layout or dictionary, too many dirty chunks or
      // a table mismatch fall back to the doc-by-doc re-add below, which packs
      // these docs with adjacent re-added sources.
      bool copy = copyable(seg, reader) && writer.canCopyChunks(reader.format(), reader.dictionary())
          && writer.adoptFieldTable(reader.fieldNames());
      // Test seam: resource name, source ordinal, and copy versus re-add.
      Signal::emit("storedFieldsMergeSource", &resourceName, &seg.ord, &copy);
      if (copy) {
        writer.alignForRawAppend(seg.base);
        reader.readChunkDirectory(srcChunkFirstDocs, srcChunkOffsets);
        writer.appendRawChunkRegion(seg.base, reader.chunkRegionPtr(),
                                    reader.chunkRegionBytes(),
                                    srcChunkFirstDocs, srcChunkOffsets,
                                    reader.maxDoc(), reader.maxChunkBytes(),
                                    reader.numDirtyChunks());
        continue;
      }

      int32_t maxDocIn = seg.postingsReader->maxDoc();
      for (int32_t localId = 0; localId < maxDocIn; localId++) {
        auto [mappedDoc, isDeleted] = seg.remapDocId(localId);
        if (isDeleted) continue;
        reader.readDoc(localId,
            [&](std::string_view name, std::span<const std::string_view> values) {
          if (values.size() == 1) {
            writer.addValue(mappedDoc, name, values[0]);
          } else {
            writer.addValues(mappedDoc, name, values);
          }
        });
      }
    }

    writer.finish(space.maxDoc);
  }

  // Merges non-indexed string/binary columns written by StrColHandler.
  // Rebuilds the concatenated byte stream, the per-value endOffsetReader (if values are
  // variable-size), and the per-doc endValueRankReader (if the field is multi-valued).
  void mergeStrCol(const Space& space, std::span<MergeFieldInfo*> sortedFields, PostingsWriter& postingsWriter,
                   PostingsWriter::IndexFieldInfo& outputFieldInfo) {
    assert(sortedFields.size() == space.segs.size());

    auto poolGuard = MemPool::threadLocalPoolGuard();
    auto& pool = poolGuard.pool();

    assert(FieldType::storage(outputFieldInfo.type) == FieldType::Storage::TERMS
           || FieldType::storage(outputFieldInfo.type) == FieldType::Storage::VECTOR);
    assert(!(outputFieldInfo.flags & FieldType::INDEX_DOCS));

    bool multiValued = (outputFieldInfo.flags & FieldType::MULTI_VALUED) != 0;

    // Output stream for the concatenated value bytes.
    OutputStreamPtr valuesOut = postingsWriter.getOutputStream();
    outputFieldInfo.columnLoc = valuesOut->slocation();

    u_ptr<DocsWithValWriter> docsWriter = nullptr;

    // endOffsetWriter is created lazily the first time we see a value whose size differs
    // from the first value's size.  Until then we track the uniform size and can skip
    // writing the mono column.
    u_ptr<MonoWriter> endOffsetWriter = nullptr;
    OutputStreamPtr endOffsetOut;

    // endValueRankWriter is created up-front when the field is multi-valued.
    u_ptr<MonoWriter> endValueRankWriter = nullptr;
    OutputStreamPtr endValueRankOut;
    if (multiValued) {
      endValueRankOut = postingsWriter.getOutputStream();
      endValueRankWriter = pool.make_unique_align<MonoWriter>(8, pool, *endValueRankOut);
    }

    // valDoc map (per-value rank -> merged docId): regenerated when any source segment
    // carried one (multi-valued vector columns).  Like the source map it is monotonic
    // non-decreasing, since merged doc ids are assigned in increasing order.
    bool hasValDoc = false;
    for (auto* f : sortedFields) {
      if (f != nullptr && !(f->segFieldInfo.valDocLoc.offset() == 0 &&
                            f->segFieldInfo.valDocLoc.filenum() == 0)) {
        hasValDoc = true;
        break;
      }
    }
    u_ptr<MonoWriter> valDocWriter = nullptr;
    OutputStreamPtr valDocOut;
    if (multiValued && hasValDoc) {
      valDocOut = postingsWriter.getOutputStream();
      valDocWriter = pool.make_unique_align<MonoWriter>(8, pool, *valDocOut);
    }

    int32_t docsWithField = 0;
    int64_t totalValues = 0;
    int32_t minSize = std::numeric_limits<int32_t>::max();
    int32_t maxSize = std::numeric_limits<int32_t>::min();
    int64_t cumulativeBytes = 0;
    [[maybe_unused]] bool isDense = true;

    // Emits one value into the merged output: appends its bytes, updates size tracking,
    // and writes the corresponding endOffset entry (lazily starting the mono column if
    // a size mismatch appears).
    auto emitValue = [&](std::string_view value) {
      int32_t valueSize = (int32_t)value.size();
      valuesOut->write(value.data(), valueSize);
      cumulativeBytes += valueSize;
      int32_t prevMin = minSize;
      minSize = std::min(minSize, valueSize);
      maxSize = std::max(maxSize, valueSize);

      if (endOffsetWriter) {
        endOffsetWriter->addInt64(cumulativeBytes);
      } else if (totalValues > 0 && valueSize != prevMin) {
        // First size mismatch: start writing the mono column and backfill prior values.
        endOffsetOut = postingsWriter.getOutputStream();
        endOffsetWriter = pool.make_unique_align<MonoWriter>(8, pool, *endOffsetOut);
        for (int64_t i = 1; i <= totalValues; i++) {
          endOffsetWriter->addInt64((int64_t)prevMin * i);
        }
        endOffsetWriter->addInt64(cumulativeBytes);
      }
      totalValues++;
    };

    // Process segments in order - since doc remapping is monotonic,
    // we can simply iterate through each segment sequentially.
    for (size_t segnum = 0; segnum < sortedFields.size(); segnum++) {
      auto* field = sortedFields[segnum];
      auto& seg = space.segs[segnum];

      if (field == nullptr) {
        // Field didn't exist for this segment.  If segment has live docs, the merged
        // field becomes sparse: create docsWriter and backfill.
        if (seg.numLive > 0) {
          isDense = false;
          if (!docsWriter) {
            docsWriter = pool.make_unique_align<DocsWithValWriter>(8, pool, postingsWriter, outputFieldInfo);
            for (int32_t i = 0; i < docsWithField; i++) {
              docsWriter->startDoc(i);
            }
          }
        }
        continue;
      }

      assert(field->seg == &seg);

      StrColReader reader(*field->seg->postingsReader, field->segFieldInfo);
      StrColReader::Iterator iter(reader);

      if (reader.docsReader().hasBitset() && !docsWriter) {
        isDense = false;
        docsWriter = pool.make_unique_align<DocsWithValWriter>(8, pool, postingsWriter, outputFieldInfo);
        for (int32_t i = 0; i < docsWithField; i++) {
          docsWriter->startDoc(i);
        }
      }

      for (int32_t localId = iter.advance(0); localId != StrColReader::Iterator::ENDDOC; localId = iter.next()) {
        auto [mappedDoc, isDeleted] = seg.remapDocId(localId);
        if (isDeleted) {
          continue;
        }

        assert(!isDense || mappedDoc == docsWithField);

        if (multiValued) {
          auto [startValueRank, endValueRank] = iter.valueRange();
          for (int64_t r = startValueRank; r < endValueRank; r++) {
            emitValue(reader.valueAt(r));
            if (valDocWriter) valDocWriter->addInt64(mappedDoc);
          }
          endValueRankWriter->addInt64(totalValues);
        } else {
          emitValue(iter.value());
        }

        if (docsWriter) {
          docsWriter->startDoc(mappedDoc);
        }

        docsWithField++;
      }
    }

    assert(docsWithField > 0);

    valuesOut->flush(true);
    outputFieldInfo.columnMetaOff = valuesOut->size() - outputFieldInfo.columnLoc.offset();
    outputFieldInfo.numValues = totalValues;

    // endOffsetReader (mono2)
    if (endOffsetWriter) {
      endOffsetWriter->finish();
      outputFieldInfo.mono2Loc = endOffsetWriter->blockLoc;
      outputFieldInfo.mono2MetaOff = endOffsetWriter->metaOff;
    } else {
      // All values are the same size - store size in mono2MetaOff, leave mono2Loc zero.
      outputFieldInfo.mono2Loc = {0, 0};
      outputFieldInfo.mono2MetaOff = minSize;
    }

    // endValueRankReader (mono)
    if (endValueRankWriter) {
      endValueRankWriter->finish();
      outputFieldInfo.monoLoc = endValueRankWriter->blockLoc;
      outputFieldInfo.monoMetaOff = endValueRankWriter->metaOff;
    }

    // valDoc map (valueRank -> docId)
    if (valDocWriter) {
      valDocWriter->finish();
      outputFieldInfo.valDocLoc = valDocWriter->blockLoc;
      outputFieldInfo.valDocMetaOff = valDocWriter->metaOff;
    }

    outputFieldInfo.docsWithField = docsWithField;

    if (docsWriter) {
      assert(docsWriter->numAdded() == docsWithField);
      if (docsWriter->numAdded() == space.maxDoc) {
        docsWriter->finishDense(space.maxDoc);
      } else {
        docsWriter->finish();
      }
    } else {
      outputFieldInfo.docsWithFieldEndLoc = {0, 0};
    }
  }

  std::vector<MergePoint> synthesizePointRun(MergeFieldInfo& field) {
    Segment& segment = *field.seg;
    IntColReader reader(*segment.postingsReader, field.segFieldInfo);
    IntColReader::Iterator docs(reader);
    std::vector<MergePoint> run;
    run.reserve((size_t)field.segFieldInfo.numValues);
    for (int32_t localDoc = docs.next(); localDoc != IntColReader::ENDDOC;
         localDoc = docs.next()) {
      auto [mappedDoc, isDeleted] = segment.remapDocId(localDoc);
      if (!reader.multiValued()) {
        if (!isDeleted) run.push_back({docs.value(), mappedDoc});
        continue;
      }
      auto [start, end] = reader.getStartEndValueRank(docs.rank());
      if (isDeleted) continue;
      for (int64_t rank = start; rank < end; rank++) {
        run.push_back({docs.values().valueAt(rank), mappedDoc});
      }
    }
    sortPointsByValueDocid(std::span<MergePoint>(run));
    return run;
  }

  void mergePoints(std::span<MergeFieldInfo*> sortedFields,
                   PostingsWriter& postingsWriter,
                   PostingsWriter::IndexFieldInfo& outputFieldInfo) {
    auto poolGuard = MemPool::threadLocalPoolGuard();
    MemPool& pool = poolGuard.pool();
    std::vector<PointRun> runs;
    runs.reserve(sortedFields.size());
    for (MergeFieldInfo* field : sortedFields) {
      if (field == nullptr) continue;
      if (field->segFieldInfo.pointsMetaOff != 0) {
        auto* reader = pool.make<PointsReader>(
            *field->seg->postingsReader, field->segFieldInfo);
        runs.emplace_back(*field->seg, *reader, pool);
      } else {
        runs.emplace_back(*field->seg, synthesizePointRun(*field));
      }
    }

    std::vector<PointRun*> active;
    active.reserve(runs.size());
    for (PointRun& run : runs) {
      if (run.next()) active.push_back(&run);
    }
    if (active.empty()) return;

    IndirectPQ<PointRun, PointRunCompare> queue(active);
    auto output = postingsWriter.getOutputStream();
    PointsWriter writer(*output, PointsWriter::Options{});
    while (queue.size() != 0) {
      PointRun& run = queue.top();
      writer.addPoint(run.point().value, run.point().docid);
      if (run.next()) queue.updateTop();
      else queue.removeTop();
    }
    auto data = writer.finish();
    assert(data.pointCount == (uint64_t)outputFieldInfo.numValues);
    outputFieldInfo.pointsLoc = data.pointsLoc;
    outputFieldInfo.pointsMetaOff = data.pointsMetaOff;
  }

  // Merges int64 columns. The output is dense (no docs-with-field bitset, so
  // value rank == doc id for single-valued readers) whenever every surviving
  // doc has a value: all sources dense and present, or a sparse source whose
  // docs without a value were all deleted.
  void mergeIntCol(const Space& space, std::span<MergeFieldInfo*> sortedFields,
                   PostingsWriter& postingsWriter,
                   PostingsWriter::IndexFieldInfo& outputFieldInfo,
                   std::vector<BKDWriter::Point>* geoPoints,
                   std::vector<BasicBKDWriter<BKDInt64Traits>::Point>* rangePoints) {
    assert(sortedFields.size() == space.segs.size());  // expect non-compacted fields to make the code a little simpler.

    auto poolGuard = MemPool::threadLocalPoolGuard();
    auto& pool = poolGuard.pool();

    bool hasEndValueRanks = outputFieldInfo.type == FieldType::INTERVAL
        || std::ranges::any_of(sortedFields, [](const auto* field) {
          return field && field->segFieldInfo.hasEndValueRanks();
        });
    auto valuesOut = postingsWriter.getOutputStream();
    IntColWriter valuesWriter(*valuesOut,
        !hasEndValueRanks && FieldType::isNumericColumn(outputFieldInfo.type));

    OutputStreamPtr monoOut;
    std::optional<MonoWriter> endRanks;
    if (hasEndValueRanks) {
      monoOut = postingsWriter.getOutputStream();
      endRanks.emplace(pool, *monoOut);
    }

    std::optional<DocsWithValWriter> docsWriter;
    int32_t docsWithField = 0;
    int64_t endOut = 0;
    for (size_t segnum = 0; segnum < sortedFields.size(); segnum++) {
      auto* field = sortedFields[segnum];
      const auto& seg = space.segs[segnum];
      if (seg.numLive == 0) continue;

      int32_t sourceDocs = field ? field->segFieldInfo.docsWithField : 0;
      if (!docsWriter && sourceDocs != seg.postingsReader->maxDoc()) {
        docsWriter.emplace(pool, postingsWriter, outputFieldInfo);
        for (int32_t doc = 0; doc < seg.base; doc++) {
          docsWriter->startDoc(doc);
        }
      }
      if (sourceDocs == 0) continue;

      assert(field->seg == &seg);
      IntColReader reader(*seg.postingsReader, field->segFieldInfo);
      IntColReader::Iterator docs(reader);
      auto* sourceEnds = reader.getEndValueRankReader();
      assert(outputFieldInfo.type != FieldType::INTERVAL || sourceEnds);

      int64_t endIn = 0;
      for (int32_t localId = docs.next(); localId != IntColReader::ENDDOC; localId = docs.next()) {
        int64_t startIn = endIn;
        endIn = sourceEnds ? sourceEnds->valueAt(docs.rank()) : (int64_t)docs.rank() + 1;
        auto [mappedDoc, deleted] = seg.remapDocId(localId);
        if (deleted) continue;

        if (docsWriter) docsWriter->startDoc(mappedDoc);
        docsWithField++;
        endOut += endIn - startIn;
        if (endRanks) endRanks->addInt64(endOut);

        int64_t rangeMin = 0;
        assert(!rangePoints || (endIn - startIn) % 2 == 0);
        for (int64_t rank = startIn; rank < endIn; rank++) {
          int64_t value = docs.values().valueAt(rank);
          valuesWriter.addInt64(value);
          if (geoPoints) {
            geoPoints->push_back({geo::unpackLatitude(value),
                                  geo::unpackLongitude(value), mappedDoc});
          }
          if (rangePoints) {
            if ((rank - startIn) % 2 == 0) rangeMin = value;
            else rangePoints->push_back({rangeMin, value, mappedDoc});
          }
        }
      }
    }

    valuesWriter.finish(outputFieldInfo);
    assert(docsWithField > 0 && endOut == outputFieldInfo.numValues);
    assert(docsWriter || docsWithField == space.maxDoc);
    if (docsWriter && docsWithField != space.maxDoc) {
      assert(docsWriter->numAdded() == docsWithField);
      docsWriter->finish();
    } else {
      outputFieldInfo.docsWithField = docsWithField;
      outputFieldInfo.docsWithFieldEndLoc = {};
    }
    if (endRanks) {
      [[maybe_unused]] size_t count = endRanks->finish();
      assert(count == (size_t)docsWithField);
      outputFieldInfo.monoLoc = endRanks->blockLoc;
      outputFieldInfo.monoMetaOff = endRanks->metaOff;
    } else {
      assert(endOut == docsWithField);
    }
  }

};


}
