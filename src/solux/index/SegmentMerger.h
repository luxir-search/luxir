#pragma once
#include "IndexWriter.h"
#include "MergeCostModel.h"
#include "OrdCollector.h"
#include "OrdColWriter.h"
#include "NormsWriter.h"
#include "BKDWriter.h"
#include "PointsWriter.h"
#include "StoredFieldsWriter.h"
#include "solux/reader/DocsEnum.h"
#include "solux/reader/NormsReader.h"
#include "solux/reader/PointsReader.h"
#include "solux/reader/StoredFieldsReader.h"
#include "solux/reader/StrColReader.h"
#include "solux/search/IndexReader.h"
#include "solux/util/geo.h"

#include <array>
#include <atomic>
#include <oneapi/tbb/task_group.h>

// This file is only included in IndexWriter.cpp

#define MERGER_DEBUG LOG_TRACE
// #define MERGER_DEBUG LOG_DEBUG

#include "solux/util/Signal.h"

namespace solux {

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

  // One field's merge work, self-contained so it can run as a task: owned copies
  // of the per-segment SegFieldInfos (their names/locations point into the open
  // source segments, not into FieldReader scratch), plus its admission price -
  // estimated peak RAM (see fieldMergeCost) and peak concurrent output streams
  // (see fieldMergeStreams; concurrently held streams cannot share a file, so the
  // stream cap is what bounds the merged segment's file count).
  struct Batch {
    int64_t cost = 0;
    int32_t streams = 0;
    std::string name;
    std::vector<MergeFieldInfo> fields;
  };

  // Runs the per-field merge batches as parallel TBB tasks under IndexRamBudget
  // admission.  Strategy: work waits, threads don't.
  //  - Batches are enumerated up front and sorted largest-cost-first, so the
  //    longest field merge starts earliest and small column merges pack around it.
  //  - admitLoop() launches every pending batch whose streams fit and whose RAM
  //    reserves; it is re-run from each task's completion path - completions are
  //    the only moments capacity grows, so admission is event-driven and no thread
  //    ever blocks on the budget.
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
    SegmentMerger& merger;
    IndexRamBudget& budget;
    oneapi::tbb::task_group_context context;
    oneapi::tbb::task_group tg;
    std::mutex mutex;
    std::vector<Batch> pending;
    int32_t inFlight = 0;
    int32_t inFlightStreams = 0;

    class BatchAdmission {
      MergeAdmissionDriver* driver = nullptr;
      IndexRamBudget::Guard ramGuard;
      int32_t streams = 0;

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

  public:
    MergeAdmissionDriver(SegmentMerger& merger, IndexRamBudget& budget, std::vector<Batch>&& batches)
      : merger(merger), budget(budget), context(), tg(context), pending(std::move(batches)) {}

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
            return admittedAny;
          }

          for (size_t i = 0; i < pending.size(); i++) {
            Batch& candidate = pending[i];
            if (inFlightStreams + candidate.streams > MergeCostModel::MAX_STREAMS) {
              continue;
            }

            auto guard = budget.tryAcquireGuard(candidate.cost);
            if (!guard) {
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

        batch = std::move(pending.front());
        pending.erase(pending.begin());
        IndexRamBudget::Guard guard = budget.forceAcquire(batch.cost);
        inFlight++;
        inFlightStreams += batch.streams;
        admission = BatchAdmission(*this, std::move(guard), batch.streams);
      }

      spawn(std::move(batch), std::move(admission));
      return true;
    }

    struct BatchTask {
      MergeAdmissionDriver* driver;
      mutable Batch batch;
      mutable BatchAdmission admission;

      void operator()() const {
        driver->merger.mergeField(batch.fields);
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

  std::span<PostingsReader *> preaders;
  std::span<LiveDocs*> liveDocs; // parallel to preaders, nullptr if no deletes
  PostingsWriter& postingsWriter;
  IndexRamBudget& ramBudget;

  std::vector<FieldReader> fieldReaders;  // todo - pool allocate (& use smart ptr on MergeSeg if destructors needed)
  std::vector<Segment> segs;
public:



  SegmentMerger(std::span<PostingsReader *> preaders, std::span<LiveDocs*> liveDocs,
                PostingsWriter& postingsWriter, IndexRamBudget& ramBudget)
  : preaders(preaders), liveDocs(liveDocs), postingsWriter(postingsWriter), ramBudget(ramBudget)
  {
    assert(preaders.size() == liveDocs.size());
  }

  void merge() {
    auto guard = MemPool::threadLocalPoolGuard();
    auto& pool = guard.pool();

    segs.reserve(preaders.size());
    fieldReaders.reserve(preaders.size());  // This is important since we take pointers to these! Pool allocate later...

    int64_t totalLive = 0;  // used to set each segment base, will be equal to total live docs after the loop
    for (size_t i = 0; i < preaders.size(); i++) {
      auto preader = preaders[i];
      auto segLiveDocs = liveDocs[i];
      
      MERGER_DEBUG("SegmentMerger: segment {}, maxDoc={}, liveDocs={}",
               i, preader->maxDoc(), segLiveDocs ? segLiveDocs->numLive() : preader->maxDoc());
      
      fieldReaders.emplace_back(pool, *preader);
      FieldReader& fieldReader = fieldReaders.back();

      // position fieldReader on first field and add to segs if it's non-empty
      if (fieldReader.readNextField()) {
        // Calculate number of live documents
        int32_t numLive = segLiveDocs ? segLiveDocs->numLive() : preader->maxDoc();
        
        segs.emplace_back(preader, &fieldReader, numLive, totalLive, (int)segs.size());
        MERGER_DEBUG("SegmentMerger: added segment to merge, base={}, numLive={}", totalLive, numLive);
        totalLive += numLive; // Count only live documents for base offset
      } else {
        LOG_ERROR("Empty fieldReader!");
      }
    }
    // Set maxDoc to total number of live documents
    postingsWriter.setMaxDoc(totalLive);

    MERGER_DEBUG("SegmentMerger: total live docs to merge: {}", totalLive);

    // Now build the docId maps based on liveDocs, then release the liveDocs.
    for (size_t i = 0; i < segs.size(); i++) {
      auto& seg = segs[i];
      buildDocIdMapping(seg, liveDocs[i], seg.remap);
      // TODO: pass the actual liveDocs shared_ptr into the merger so we can release them.
    }

    auto fnameComp = [](const Segment& a, const Segment& b){ return b.fieldReader->name() < a.fieldReader->name(); };
    std::vector<Segment*> segPtrs(segs.size());
    // IndirectPQ<Segment, decltype(fnameComp)> fieldPQ(segs, segPtrs, false);
    IndirectPQ<Segment, decltype(fnameComp)> fieldPQ(segs, segPtrs);

    std::vector<Batch> batches;
    while (fieldPQ.size() > 0) {
      Batch batch;
      batch.fields.reserve(segs.size());
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

      // If merging would break any of the segment max constraints (i.e. number of unique terms in a field)
      // we could bail early.

      batch.cost = estimateCost(batch.fields);
      batch.streams = estimateStreams(batch.fields);
      batches.push_back(std::move(batch));
    }

    std::sort(batches.begin(), batches.end(), [](const Batch& a, const Batch& b) {
      if (a.cost != b.cost) {
        return a.cost > b.cost;
      }
      return a.name < b.name;
    });

    MergeAdmissionDriver driver(*this, ramBudget, std::move(batches));
    driver.run();

    // Caller is responsible for calling postingsWriter.finish()
    // so it can collect the filenames written.
  }

private:
  static bool isStoredField(const SegFieldInfo& info) {
    return info.type == FieldType::BIN && (info.flags & FieldType::STORED) != 0;
  }

  static bool hasOneDimensionalPoints(FieldType::Type type, int32_t flags) {
    return type != FieldType::GEO_POINT && (flags & FieldType::INDEX_RANGE) != 0;
  }

  static bool hasGeoPoints(FieldType::Type type, int32_t flags) {
    return type == FieldType::GEO_POINT && (flags & FieldType::INDEX_RANGE) != 0;
  }

  static void batchTypeAndFlags(std::span<const MergeFieldInfo> fields, FieldType::Type& type, int32_t& allFlags) {
    type = FieldType::Type::NONE;
    allFlags = 0;
    for (const auto& field : fields) {
      if (type == FieldType::Type::NONE) {
        type = field.segFieldInfo.type;
      }
      allFlags |= field.segFieldInfo.flags;
    }
  }

  int64_t estimateCost(std::span<const MergeFieldInfo> fields) const {
    assert(!fields.empty());
    if (isStoredField(fields[0].segFieldInfo)) {
      int64_t maxChunkBytes = 0;
      for (const auto& field : fields) {
        maxChunkBytes = std::max(
            maxChunkBytes,
            StoredFieldsReader::peekMaxChunkBytes(*field.seg->postingsReader, field.segFieldInfo));
      }
      // Stored-field merge processes one source segment at a time.  The
      // value-scaled buffers are: source decompressed chunk, writer per-doc
      // staging, writer chunk body, writer assembled uncompressed chunk, and
      // LZ4 compression output.  LIGHT_BYTES covers fixed stream/mono scratch
      // and the small LZ4_compressBound overhead above 1x.
      return MergeCostModel::storedFieldsBytes(maxChunkBytes);
    }

    FieldType::Type type;
    int32_t allFlags;
    batchTypeAndFlags(fields, type, allFlags);

    int64_t numValues = 0;
    int64_t docsWithField = 0;
    int64_t synthesizedPointValues = 0;
    for (const auto& field : fields) {
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
          ? MergeCostModel::geoPointsBytes(numValues) : 0;

    if (type == FieldType::Type::STRING && (allFlags & FieldType::INDEX_DOCS) != 0) {
      // Ord columns hold the term-order -> doc-order transposition in RAM until the
      // postings pass finishes: OrdCollector allocates ords[mergedMaxDoc] (int32)
      // up front, plus roughly a TaggedPtr/entry of pool per value for multi-value
      // chains.  This is the heavy class the budget exists for.
      return (int64_t)postingsWriter.getMaxDoc() * 4 + numValues * 8
          + MergeCostModel::LIGHT_BYTES + pointsBytes;
    }
    if (type == FieldType::Type::TEXT && (allFlags & FieldType::INDEX_DOCS) != 0) {
      // Text postings merge streams; what accumulates is the norms build
      // (normBytes ~1 byte per doc-with-field plus its DocStream).
      return docsWithField * 2 + MergeCostModel::LIGHT_BYTES + pointsBytes;
    }
    // Int/str/vector columns and stored fields stream through fixed block buffers.
    return MergeCostModel::LIGHT_BYTES + pointsBytes;
  }

  static int32_t estimateStreams(std::span<const MergeFieldInfo> fields) {
    assert(!fields.empty());
    if (isStoredField(fields[0].segFieldInfo)) {
      return MergeCostModel::STORED_STREAMS;
    }

    FieldType::Type type;
    int32_t allFlags;
    batchTypeAndFlags(fields, type, allFlags);

    if (type == FieldType::Type::STRING && (allFlags & FieldType::INDEX_DOCS) != 0) {
      return MergeCostModel::ORD_COL_STREAMS;
    }
    if (type == FieldType::Type::TEXT && (allFlags & FieldType::INDEX_DOCS) != 0) {
      return MergeCostModel::TEXT_STREAMS;
    }
    if ((type == FieldType::Type::STRING || type == FieldType::Type::VECTOR)
        && (allFlags & FieldType::INDEX_DOCS) == 0) {
      return MergeCostModel::STR_COL_STREAMS;
    }
    return MergeCostModel::INT_COL_STREAMS
        + (hasOneDimensionalPoints(type, allFlags)
           ? MergeCostModel::POINTS_STREAMS
           : hasGeoPoints(type, allFlags)
             ? MergeCostModel::GEO_POINTS_STREAMS : 0);
  }

  // Helper to build mapping from old doc IDs to new doc IDs for a segment, accounting for deletes
  // Returns a vector where vector[oldDocId] = newDocId, or -1 if deleted.
  // This is still 0 based, so add the base in both cases.
  void buildDocIdMapping(const Segment& seg, LiveDocs* liveDocs, std::vector<int32_t>& target) {
    if (liveDocs == nullptr) {
      return; // no deletes, so nothing to do.
    }
    int32_t maxDoc = seg.postingsReader->maxDoc();
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

    assert(newDocId == seg.numLive); // Make sure we got the right number of live documents
  }

  // add docs (with positions, or just freqs for positionless fields) from the provided DocsEnum
  void addDocsPos(TextWriter& textWriter, DocsEnum& docsEnum, const Segment& seg) {
    bool hasPositions = docsEnum.indexHasPositions();
    if (!hasPositions) {
      for (;;) {
        int32_t docid = docsEnum.nextDoc();
        if (docid == INT_MAX) break;

        // predictable branch for deleted vs not
        int mappedDoc = seg.remap.empty() ? docid : seg.remap[docid];
        if (mappedDoc == -1) {
          continue; // Document is deleted
        }
        // No positions to copy; record the doc with the source term freq directly.
        // termFreq() is 1 for DOCS-only fields, the real freq for DOCS_AND_FREQS.
        textWriter.addDoc(seg.base + mappedDoc, docsEnum.termFreq());
      }
      return;
    }

    std::array<int32_t, Postings::DOCS_BLOCK_SIZE> mappedDocs;
    int32_t docid = docsEnum.nextDoc();
    while (docid != INT_MAX) {
      auto block = docsEnum.currentPositionDocBlock();
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
        docsEnum.beginPositionDeltaBatch(blockCount);
        textWriter.addDocsWithPositions(
            std::span<const int32_t>(mappedDocs.data(), (size_t) blockCount),
            block.tfreqs,
            [&](int64_t maxCount) {
              return docsEnum.nextPositionDeltaBatchSpan(maxCount);
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
          docsEnum.startPositions();
          for (;;) {
            auto deltas = docsEnum.nextPositionDeltaSpan();
            if (deltas.empty()) break;
            textWriter.appendPositionDeltas(deltas);
          }
          textWriter.endDoc(newDocid, tf);
        }
        docid = docsEnum.nextDoc();
      }
    }
  }

  // add docs and ordinals from the provided DocsEnum (for string column, record ord in docToOrd for each doc, to be written later)
  void addDocsOrds(TextWriter& textWriter, DocsEnum& docsEnum, const Segment& seg, OrdCollector& docToOrd, int32_t ord) {
    for(;;) {
      int32_t docid = docsEnum.nextDoc();
      if (docid == INT_MAX) break;

      // predictable branch for deleted vs not
      int mappedDoc = seg.remap.empty() ? docid : seg.remap[docid];
      if (mappedDoc == -1) {
        continue; // Document is deleted
      }
      int32_t newDocid = seg.base + mappedDoc;

      docToOrd.add(newDocid, ord);
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

  void mergeField(std::vector<MergeFieldInfo>& mergeFieldInfos) {
    int32_t nDocs = postingsWriter.getMaxDoc();
    auto poolGuard = MemPool::threadLocalPoolGuard();
    auto& pool = poolGuard.pool();

    // Stored-fields resources are segment-wide rather than normal per-field
    // columns.  Route each one (default or named column family) to a
    // dedicated merger.  Recognize them by the combination of type=BIN and
    // flags=STORED set by StoredFieldsWriter::finish().
    const auto& firstInfo = mergeFieldInfos[0].segFieldInfo;
    if (firstInfo.type == FieldType::BIN
        && (firstInfo.flags & FieldType::STORED) != 0) {
      mergeStoredFields(mergeFieldInfos);
      return;
    }

    // sort mergeFieldInfos so we can add docids low to high.
    // This is a simple O(n+m) sort where n=number of segments and m is number of segments with this specific field.
    // Simply slot the field into it's place and then compact.
    std::vector<MergeFieldInfo*> sortedFields(segs.size());
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

    // TODO: check if fields are compatible!
    // TODO: gather other stats to help us build the field (like if it's a dense field!)
    int32_t allFlags = 0;
    FieldType::Type type = FieldType::Type::NONE;
    for (auto* field : compactFields) {
      if (type == FieldType::Type::NONE) {
        type = field->segFieldInfo.type;
      } else if (type != field->segFieldInfo.type) {
        LOG_ERROR("Field types don't match! {} {}", (int)type, (int)field->segFieldInfo.type);
        // now what?
      }
      allFlags |= field->segFieldInfo.flags;
    }
    bool isText = type == FieldType::Type::TEXT;
    bool hasImpactNorms = isText && FieldType::hasPositions(allFlags);
    Stream normBytes;
    DocStream normDocsWithField(pool);
    int32_t numNormDocsWithField = 0;
    if (isText) {
      buildMergedNorms(compactFields, normBytes, normDocsWithField, numNormDocsWithField, pool);
    }

    // get/reserve a new fieldInfo from the postingsReader
    PostingsWriter::IndexFieldInfo& outputFieldInfo = postingsWriter.addField(compactFields[0]->segFieldInfo.fieldname);
    outputFieldInfo.type = type;
    outputFieldInfo.flags = allFlags;
    NormsWriter::PreparedNorms preparedNorms;
    if (isText) {
      preparedNorms = NormsWriter::prepare(pool, postingsWriter, outputFieldInfo, normBytes,
                                           normDocsWithField, numNormDocsWithField);
    }

    // if this is an indexed string column, we need to collect the ordinals for each doc
    bool isOrdCol = (type == FieldType::Type::STRING) && (allFlags & FieldType::INDEX_DOCS);
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

      // Collect TermsEnum for each segment.  Keep track of the index so we can visit in ascending order one at a time.
      struct TermsEnumIdx {
        TermsEnum tenum;
        size_t idx;
      };
      std::vector<TermsEnumIdx> tenums;
      tenums.reserve(compactFields.size());
      std::vector<TermsEnumIdx*> tenumPtrs;
      tenumPtrs.reserve(compactFields.size());

      for (size_t idx = 0; idx<compactFields.size(); idx++) {
        auto field = compactFields[idx];
        tenums.emplace_back(TermsEnumIdx{TermsEnum(pool, *field->seg->postingsReader, field->segFieldInfo), idx});
        // Position on the first term.  If none, don't add to the PQ
        if (tenums.back().tenum.nextTerm()) {
          tenumPtrs.push_back(&tenums.back());
        }
      }

      auto termCmp = [](const TermsEnumIdx& a, const TermsEnumIdx& b){
        int cmp = b.tenum.term() <=> a.tenum.term();
        return cmp < 0 || (cmp == 0 && b.idx < a.idx);  // tiebreak by index so we visit segments in order
      };
      // Only enums positioned on a term participate. Some indexed text
      // segments legitimately have presence/norms but zero terms.
      IndirectPQ<TermsEnumIdx, decltype(termCmp)> termPQ(tenumPtrs);

      // iterate through the terms in sorted order
      char termBuf[PackedTerm::MAX_BYTES];
      while (termPQ.size() > 0) {
        TermsEnumIdx& first = termPQ.top();
        // Copy the term to local storage: it anchors the same-term do-while comparison
        // below, and advancing the source enum overwrites the enum's term buffer.
        PackedTerm term(termBuf);
        first.tenum.term().copyTo(term);
        auto termOrd = textWriter.startTerm(term);

        do {
          TermsEnumIdx& entry = termPQ.top();
          // need to create the docsEnum while the termsEnum is still positioned on the term.
          DocsEnum docsEnum(pool, *compactFields[entry.idx]->seg->postingsReader, entry.tenum);

          if (isOrdCol) {
            // this is a string column, so keep track of the ordinals for each doc
            addDocsOrds(textWriter, docsEnum, *compactFields[entry.idx]->seg, ordCollector.value(), termOrd);
          } else {
            // text field, so add docs with positions to the textWriter
            addDocsPos(textWriter, docsEnum, *compactFields[entry.idx]->seg);
          }

          // advance that entry to the next term, removing from pq if exhausted.
          if (entry.tenum.nextTerm()) {
            termPQ.updateTop();
          } else {
            termPQ.removeTop();
          }
          // continue while more enums are positioned on the same term
        } while (termPQ.size() > 0 && termPQ.top().tenum.term() == term);

        textWriter.endTerm(term);
      }

      textWriter.endField();
    }

    if (isOrdCol) {
      // auto guard = pool.rewindScopeGuard();
      OrdColWriter ordsWriter(pool, postingsWriter, outputFieldInfo, ordCollector.value());
      ordsWriter.finish();
      // nothing is done after this in this method, so we can let the normal destructors clean up.
      // ordCollector.reset();
      // ordPool.reset();

    } else if (isText) {
      NormsWriter::writeValues(pool, postingsWriter, outputFieldInfo, preparedNorms,
                               normDocsWithField);
    } else if ((type == FieldType::Type::STRING || type == FieldType::Type::VECTOR)
               && !(allFlags & FieldType::INDEX_DOCS)) {
      // Non-indexed string/binary column (column-only storage).  Vector columns use
      // the same fixed-size binary blob format (VectorHandler extends StrColHandler).
      mergeStrCol(sortedFields, postingsWriter, outputFieldInfo);
    } else {
      // int column that is not an ord column (assume all other field types have this (currently true)
      std::vector<BKDWriter::Point> geoPoints;
      bool rebuildGeo = hasGeoPoints(type, allFlags);
      mergeIntCol2(sortedFields, postingsWriter, outputFieldInfo,
                   rebuildGeo ? &geoPoints : nullptr);
      if (rebuildGeo) {
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


  // Merge one stored-fields resource (default or named column family).
  // Decompresses each source segment's live docs in merged-docID order and
  // re-emits them via a fresh StoredFieldsWriter for the same resource name.
  // Multi-valued fields are preserved as single addValues calls so the
  // on-disk grouping survives the merge.  Verbatim chunk-copy optimization
  // is a follow-up.
  void mergeStoredFields(std::vector<MergeFieldInfo>& mergeFieldInfos) {
    // Resource name comes from the (identical across segments) field name.
    std::string_view resourceName(mergeFieldInfos[0].segFieldInfo.fieldname);

    // Build a per-segment field-info vector indexed by seg ord.  Null for
    // segments whose source had no stored-fields resource.  Readers are opened
    // one source segment at a time below so decompressed chunk scratch does not
    // accumulate across sources.
    std::vector<const SegFieldInfo*> fieldInfos(segs.size());
    for (auto& mfi : mergeFieldInfos) {
      fieldInfos[mfi.seg->ord] = &mfi.segFieldInfo;
    }

    // Config isn't threaded through the merger yet; writer uses defaults.
    // When per-family codec/chunk-size becomes meaningful, look up the
    // StoredFieldType in the current schema here.
    StoredFieldsWriter writer(postingsWriter, resourceName);

    for (auto& seg : segs) {
      const SegFieldInfo* fieldInfo = fieldInfos[seg.ord];
      if (!fieldInfo) continue;  // segment had no stored-fields; writer pads automatically
      StoredFieldsReader reader(*seg.postingsReader, *fieldInfo);
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

    writer.finish(postingsWriter.getMaxDoc());
  }

  void mergeIntCol(std::span<MergeFieldInfo*> sortedFields, PostingsWriter& postingsWriter,
                   PostingsWriter::IndexFieldInfo& outputFieldInfo) {
    auto poolGuard = MemPool::threadLocalPoolGuard();
    auto& pool = poolGuard.pool();

    auto outputPtr = postingsWriter.getOutputStream();
    IntColWriter intColWriter(*outputPtr);

    // currently all values must be written before all docs - TODO FIXME - is this still true??
    for (auto* field : sortedFields) {
      auto baseId = (int32_t)field->seg->base;
      unused(baseId);

      IntColReader reader(*field->seg->postingsReader, field->segFieldInfo);
      IntColReader::BulkValues values(reader);


      // int32_t highest = field->seg->postingsReader->numDocs();
      for (;;) {
        auto index = values.next();
        if (index == IntColReader::ENDINDEX) {
          break;
        }
        int64_t val = values.value();
        intColWriter.addInt64(val);
      }
    }

    bool full = false; // TODO: calculate if this column is dense!

    if (!full) {
      DocsWithValWriter docsWriter(pool, postingsWriter, outputFieldInfo);
      for (auto* field : sortedFields) {
        auto baseId = (int32_t)field->seg->base;

        IntColReader reader(*field->seg->postingsReader, field->segFieldInfo);
        IntColReader::Iterator colIter(reader);

        [[maybe_unused]] int32_t highest = field->seg->postingsReader->maxDoc();
        for (;;) {
          int32_t localId = colIter.next();
          if (localId == IntColReader::ENDDOC) {
            break;
          }
          assert(localId < highest);
          docsWriter.startDoc(baseId + localId);
        }
      }
      docsWriter.finish();
    }


    intColWriter.finish(outputFieldInfo);
    if (outputFieldInfo.flags & FieldType::MULTI_VALUED) {
      auto guard = pool.rewindScopeGuard();
      OutputStreamPtr out = postingsWriter.getOutputStream();
      MonoWriter endValueRankWriter(pool, *out);
      int64_t endValueRankBase = 0;

      for (auto* field : sortedFields) {
        assert(field->segFieldInfo.flags & FieldType::MULTI_VALUED);
        // open IntColReader for each segment
        IntColReader reader( *field->seg->postingsReader, field->segFieldInfo);
        MonoReader* endValueRankReader = reader.getEndValueRankReader();
        assert(endValueRankReader != nullptr);
        int64_t endValueRank;
        for (int i = 0; i < endValueRankReader->numValues(); i++) {
          endValueRank = endValueRankBase + endValueRankReader->valueAt(i);
          endValueRankWriter.addInt64(endValueRank);
        }
        endValueRankBase = endValueRank;
      }
      endValueRankWriter.finish();
      outputFieldInfo.monoLoc = endValueRankWriter.blockLoc;
      outputFieldInfo.monoMetaOff = endValueRankWriter.metaOff;
    }
  }


  // Merges non-indexed string/binary columns written by StrColHandler.
  // Rebuilds the concatenated byte stream, the per-value endOffsetReader (if values are
  // variable-size), and the per-doc endValueRankReader (if the field is multi-valued).
  void mergeStrCol(std::span<MergeFieldInfo*> sortedFields, PostingsWriter& postingsWriter,
                   PostingsWriter::IndexFieldInfo& outputFieldInfo) {
    assert(sortedFields.size() == segs.size());

    auto poolGuard = MemPool::threadLocalPoolGuard();
    auto& pool = poolGuard.pool();

    assert(outputFieldInfo.type == FieldType::Type::STRING
           || outputFieldInfo.type == FieldType::Type::VECTOR);
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
    bool isDense = true;

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
      auto& seg = segs[segnum];

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

    if (docsWithField == 0) {
      outputFieldInfo.docsWithField = 0;
      outputFieldInfo.columnMetaOff = 0;
      outputFieldInfo.numValues = 0;
      return;
    }

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
      outputFieldInfo.mono2MetaOff = (totalValues == 0) ? 0 : minSize;
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
      if (docsWriter->numAdded() == postingsWriter.getMaxDoc()) {
        docsWriter->finishDense(postingsWriter.getMaxDoc());
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

  void mergeIntCol2(std::span<MergeFieldInfo*> sortedFields,
                    PostingsWriter& postingsWriter,
                    PostingsWriter::IndexFieldInfo& outputFieldInfo,
                    std::vector<BKDWriter::Point>* geoPoints) {
    assert(sortedFields.size() == segs.size());  // expect non-compacted fields to make the code a little simpler.

    auto poolGuard = MemPool::threadLocalPoolGuard();
    auto& pool = poolGuard.pool();

    auto outputPtr = postingsWriter.getOutputStream();
    auto intColWriter = pool.make_unique_align<IntColWriter>(8, *outputPtr);

    u_ptr<DocsWithValWriter> docsWriter = nullptr;  // docs with the field, created on demand if needed

    u_ptr<MonoWriter> endValueRankWriter = nullptr; // null means single valued, otherwise multi-valued
    OutputStreamPtr monoOut;                   // output stream for the monoWriter.

    if (outputFieldInfo.flags & FieldType::MULTI_VALUED) {
      monoOut = postingsWriter.getOutputStream();
      endValueRankWriter = pool.make_unique_align<MonoWriter>(8, pool, *monoOut);
    }

    int64_t endValueRankBase = 0;  // used to calculate the endValueRank for each segment, if multivalued.
    int32_t docsWithField = 0;

    for (size_t segnum = 0; segnum < sortedFields.size(); segnum++) {
      auto* field = sortedFields[segnum];
      auto& seg = segs[segnum];
      auto base = seg.base;

      if (field == nullptr) {
        // If the field didn't exist for this segment, then output can't be dense.
        // If no docsWriter exists yet (because all full) we need to create one and fill it in up until base.
        if (docsWriter == nullptr) {
          docsWriter = pool.make_unique_align<DocsWithValWriter>(8, pool, postingsWriter, outputFieldInfo);
          for (int32_t i = 0; i < base; i++) {
            docsWriter->startDoc(i); // write all docs up to base as they were dense before this segment.
          }
        }
        continue; // no values to write, so skip to next segment.
      }

      assert(field->seg == &seg);

      IntColReader& reader = *pool.make_align<IntColReader>(8, *field->seg->postingsReader, field->segFieldInfo);
      MonoReader* endValueRankReader = reader.getEndValueRankReader();
      IntColReader::BulkValues& values = *pool.make_align<IntColReader::BulkValues>(8, reader);
      screaming::BitSet::Iterator* docsIter = nullptr; // null means all docs have values.
      if (reader.docsReader().hasBitset()) {
        docsIter = pool.make_align<screaming::BitSet::Iterator>(8, reader.docsReader().bitset());
        // not all docs have values, so we need to create a docsWriter and fill it in up until base.
        if (docsWriter == nullptr) {
          docsWriter = pool.make_unique_align<DocsWithValWriter>(8, pool, postingsWriter, outputFieldInfo);
          for (int32_t i = 0; i < base; i++) {
            docsWriter->startDoc(i); // write all docs up to base as they were dense before this segment.
          }
        }
      }

      // Strategy:
      //   - read nextDocWithValue
      //   - if multiValued, read next endValueRank and calculate number of values (use previous endValueRank)
      //   - read that many values and write that many values (1 if single valued)
      //   - write mapped docWithValue, write mapped endValueRank, write values.
      //   - if the doc is deleted, still do reads, but skip the writes.
      // variable naming: In suffix is for reading, Out suffix is for writing.

      int32_t localId = -1;
      int32_t maxDocIn = seg.postingsReader->maxDoc();

      // we don't need to check liveDocs since we have the doc mapping.
      // auto* liveBits = liveDocs[seg.ord] ? &liveDocs[seg.ord]->bitset() : nullptr;

      int64_t lastEndValueRankIn = 0;
      int32_t docRankIn = -1;  // rank of the doc we are on, faster than figuring out from docsIter.
      for (;;) {
        if (docsIter) {
          localId = docsIter->next();
        } else {
          localId++;
        }
        docRankIn++;
        if (localId >= maxDocIn) {
          break;
        }

        auto [mappedDoc, isDeleted] = seg.remapDocId(localId);

        if (isDeleted) {
          //
          // nothing needs to be adjusted for single-valued fields.
          //   - docRankIn will still be incremented and be correct for reading
          //   - mappedDoc already contains the adjustments for the docsWithValues (docsWriter)
          // for multi-valued fields, we need to read how many values there were for this deleted doc.
          //   - end rank values written need to be adjusted down by the number of values.
          //     (adjust endValueRankBase by the number of values skipped)
          //   - lastEndValueRankIn needs to be maintained correctly (that's how we tell how many values a doc has)
          if (endValueRankReader) {
            int64_t endValueRank = endValueRankReader->valueAt(docRankIn);
            auto nVals = endValueRank - lastEndValueRankIn;
            endValueRankBase -= nVals; // adjust the base down by the number of values skipped.
            lastEndValueRankIn = endValueRank;
          }
          continue;
        }


        if (docsWriter) {
          docsWriter->startDoc(mappedDoc);
        }

        docsWithField++;
        if (endValueRankReader) {
          //  multi-valued
          // TODO: OPT: use a bulk iterator for the ranks.
          int64_t endValueRank = endValueRankReader->valueAt(docRankIn);

          // write the new endValueRank
          endValueRankWriter->addInt64(endValueRankBase + endValueRank);

          // transfer the values
          for (int64_t inRank = lastEndValueRankIn; inRank < endValueRank; inRank++) {
            auto val = values.valueAt(inRank);
            intColWriter->addInt64(val);
            if (geoPoints != nullptr) {
              geoPoints->push_back({geo::unpackLatitude(val),
                                    geo::unpackLongitude(val), mappedDoc});
            }
          }

          // TODO: if doc was deleted, then adjust endValueRankBase down by nValues.
          lastEndValueRankIn = endValueRank;
        } else {
          // single-valued
          auto val = values.valueAt(docRankIn);
          intColWriter->addInt64(val);
          if (geoPoints != nullptr) {
            geoPoints->push_back({geo::unpackLatitude(val),
                                  geo::unpackLongitude(val), mappedDoc});
          }
        }
      }

      // endValueRankBase was already adjusted down for deletes, do just add the last endValueRankIn to
      // get the new base.
      endValueRankBase += lastEndValueRankIn;
    } // for-each-seg

    intColWriter->finish(outputFieldInfo);
    if (docsWriter) {
      assert(docsWriter->numAdded() == docsWithField);
      if (docsWriter->numAdded() == postingsWriter.getMaxDoc()) {
        // all docs were added.  We may have written stuff into the index,
        // but we can forget it (it will be dropped on the next merge).
        // TODO: we could look into rewinding any part of the OutputStream that was unflushed as well.
        docsWriter->finishDense(postingsWriter.getMaxDoc());
      } else {
        // some docs were not added, so we need to finish it.
        docsWriter->finish();
      }
    } else {
      outputFieldInfo.docsWithField = postingsWriter.getMaxDoc();
      outputFieldInfo.docsWithFieldEndLoc = {0, 0};
    }
    if (endValueRankWriter) {
      endValueRankWriter->finish();
      outputFieldInfo.monoLoc = endValueRankWriter->blockLoc;
      outputFieldInfo.monoMetaOff = endValueRankWriter->metaOff;
    }
  }

};


}
