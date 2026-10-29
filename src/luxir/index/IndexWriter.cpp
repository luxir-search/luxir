// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include "luxir/reader/FieldReader.h"
#include "IndexWriter.h"
#include "luxir/store/Manifest.h"

#include <algorithm>
#include <chrono>
#include <typeinfo>

#include <boost/unordered/unordered_flat_set.hpp>
#include <oneapi/tbb/task_group.h>
#include "luxir/api/luxir_index.hpp"
#include "luxir/api/padded_input.h"
#include "luxir/store/OutputStream.h"
#include "luxir/store/InputStream.h"
#include "LiveDocsWriter.h"
#include "luxir/schema/Schema.h"

#include "luxir/index/AuxInfo.h"
#include <memory_resource>
#include <google/protobuf/io/coded_stream.h>
#include <google/protobuf/io/zero_copy_stream_impl_lite.h>
#include "luxir/util/Signal.h"
#include "luxir/util/Uuid.h"
#include "luxir/util/thread.h"
#include "SegmentMerger.h"
#include "VectorIndexBuilder.h"
#include "luxir/reader/TestOverlayAuxReader.h"


namespace luxir {

namespace {

void setForceMergeError(UpdateMessage& origin, std::string_view detail) {
  std::string message =
    "Data commit succeeded, but the merged layout was not durably published: ";
  message.append(detail);
  std::runtime_error error(message);
  origin.result.setException(error);
}

void setException(ErrorHolder& result, const std::exception_ptr& failure) {
  if (result.errored()) return;
  try {
    std::rethrow_exception(failure);
  } catch (const std::exception& e) {
    result.setException(e);
  } catch (...) {
    std::runtime_error error("Unknown non-standard exception while writing segment");
    result.setException(error);
  }
}

bool mayApplyDeletes(const SegInfo& seg, const SortedDeletes& packet) {
  if (seg.minVersion >= packet.getLargestVersion()) return false;
  auto range = seg.idRange.load();
  if (!range) return true;
  for (auto span : packet.lists()) {
    if (!span.empty() && range->overlaps((std::string_view)span.front(), (std::string_view)span.back())) return true;
  }
  return false;
}

bool mayApplyDeletes(const SegInfo& seg, const MultiDeletesData& deletes) {
  for (const auto& packet : deletes.deletes) if (mayApplyDeletes(seg, *packet)) return true;
  return false;
}

uint64_t deleteFacts(const SortedDeletes& packet) {
  uint64_t count = 0;
  for (auto span : packet.lists()) count += span.size();
  return count;
}

SortedDeletes::EntrySpan clipDeletes(SortedDeletes::EntrySpan span, const IdRange& range) {
  if (range.empty) return {};
  auto first = std::lower_bound(span.begin(), span.end(), std::string_view(range.min),
      [](const auto& entry, std::string_view id) { return (std::string_view)entry < id; });
  auto last = std::upper_bound(first, span.end(), std::string_view(range.max),
      [](std::string_view id, const auto& entry) { return id < (std::string_view)entry; });
  return span.subspan((size_t)(first - span.begin()), (size_t)(last - first));
}

} // namespace


class IndexWriter::DeletePassMessage final : public UpdateMessage {
public:
  DeletePassMessage() { applyOnly = true; }
  void handle(IndexWriter&) override {}
  void done(IndexWriter& writer) override {
    auto self = std::unique_ptr<DeletePassMessage>(this);
    writer.deletePassFinished(!result.errored());
  }
};

// Goals of the TBB flow graph for updates:
//   - make sure that a commit waits for all update messages to be processed (in the same stream at least)
//   - make sure that a commit waits for all inverters to flush before writing commit info
//   - make sure that commits are finished in order (i.e. indexinfo is written in-order)
// Implementation strategy:
//  - each update message is processed by a serial node that assigns a sequence number
//    - if the update has a commit, it is assigned a commit sequence number
//  - updates are processed in parallel
//  - a sequencer node makes sure that updates are finished in order, thus insuring that update messages
//    are done before a commit is processed.
// Processing a commit after the sequencer node (i.e. all update requests have processed):
//  - note all inverters currently in use.  They all must be flushed before commit info can be written.
//  - when a segment finishes flushing, it minimally decrements the number of segments left to flush on
//    the corresponding commit.
//    - this could be implemented via sending a message to a node, or a quick synchronized block.
//  - when there are no more segments left to flush, send a message to a commit sequencer node
//    to ensure commits are finished in order.
//  - Merges:
//    - assuming segment merges can be kicked off asynchronously, we need to make sure that this
//      doesn't mess up commits. Strategy?
//      - Answer: to finish a commit, we just grab the latest list of flushed/completed segments... it doesn't matter
//        if some merges have completed or some merges are ongoing.  That does mean we need the segments list
//        to be kept up-to-date in realtime.  We need this anyway when opening new readers.
// See TBBTest.cpp for a test of the TBB parts of this strategy.
//
// Failures:
//  - TODO: if something fails, we need to still flow it through the graph so the sequencers are updated.
//
// Deletes strategy:
//  - Each inverter has its own delete queue.
//  - A flush publishes its segment and immutable delete batch together.
//  - A commit snapshots both, applies the deletes, and retires batches only
//    when every request through their largest version has finished and flushed.
//    - applying deletes to segments doesn't work well with concurrent segment merges.
//      See see finishCommitBody() for how we handle this.
//
// Why applying a delete once is not enough (versions assigned at intake):
//  1. Index seed@1, then receive commit C@2, overwrite x@3, and overwrite x@4.
//  2. C pauses before selecting inverters; x@3 stalls before obtaining one.
//     x@4 runs first and reuses the seed's inverter.
//  3. C resumes and flushes that inverter because its minVersion is 1. This
//     publishes x@4 and applies its delete (ID x with version < 4), but x@3
//     has not been indexed yet, so the delete cannot reach it.
//  4. x@3 resumes into another inverter and is flushed by a later commit.
//     If C discarded x@4's delete, both docs survive: x@3's own delete only
//     removes versions < 3 and correctly leaves x@4 alone.
// Keep x@4's delete through C@2. A successful normal commit with cutoff >= 4
// flushes the older work and applies the retained delete before retiring it.
// Covered by IndexWriterTest.overwriteSurvivesCommitAheadOfOlderUpdate.

IndexWriter::IndexWriter(CommitSnapshotRegistry& snapshots, std::shared_ptr<Schema> schema,
                         IndexRamBudget* sharedIndexRamBudget, int mergeFactor, std::string initialIncarnation,
                         oneapi::tbb::task_arena* arena)
  : dir(snapshots.dir),
    privateIndexRamBudget(sharedIndexRamBudget == nullptr ? std::make_unique<IndexRamBudget>() : nullptr),
    indexRamBudget(sharedIndexRamBudget == nullptr ? privateIndexRamBudget.get() : sharedIndexRamBudget),
    snapshots(snapshots) {
  mergePolicy = std::make_unique<MergePolicy>(*this); // defer creation until needed?
  mergePolicy->setMergeFactor(mergeFactor);
  auto manifest = Manifest::load(dir);
  nextManifestGen = manifest.highestGeneration;
  manifestNames = std::move(manifest.names);
  if (!manifest.bytes) {
    incarnation = initialIncarnation.empty() ? CommitId::newIncarnation() : std::move(initialIncarnation);
    lastSegId = 0;
  } else {
    std::pmr::monotonic_buffer_resource iiArena;
    auto indexInfo = Manifest::decode(manifest.bytes, iiArena);
    if (!indexInfo.schema || indexInfo.incarnation.empty()) throw std::runtime_error("Incomplete commit manifest");
    if (schema) throw std::invalid_argument("Initial schema supplied when reopening an index");
    incarnation = indexInfo.incarnation;
    lastSegId = indexInfo.last_seg_id;
    lastCommitTime = indexInfo.commit_time;
    lastSnapshot.store(std::make_shared<const CommitSnapshot>(manifest.bytes,
        Schema::fromStored(*indexInfo.schema, indexInfo.schema_gen),
        CommitId{incarnation, indexInfo.index_gen}, indexInfo.commit_time, filesOf(indexInfo),
        CommitSnapshot::populatedOf(indexInfo), CommitSnapshot::digestOf(*manifest.bytes)));
    snapshots.publish(lastSnapshot.load());
    indexGen = indexInfo.index_gen;
    coreGen = indexInfo.core_gen;
    schemaGen_ = indexInfo.schema_gen;
    updateNumber.store(indexInfo.update_version, std::memory_order_relaxed);
    completedUpdateVersion = indexInfo.update_version;
    durableUpdateVersion = indexInfo.update_version;
    segInfos.reserve(indexInfo.segments.size());
    lastCommittedSegIds.reserve(indexInfo.segments.size());

    // TODO: maybe maintain segment order by recording ord in segments file.
    for (const auto& segment : indexInfo.segments) {
      auto segId = segment.seg_id;
      lastSegId = std::max(lastSegId.load(std::memory_order::relaxed), segId);
      int32_t nDocs = segment.max_doc;
      // having ndocs in the list of segments is redundant with info in the segment itself and may be removed later.
      // for now it makes it easy to populate nDocs for merge decisions.
      auto [iter, success] = segInfos.emplace(segId, std::make_unique<SegInfo>(segId, nDocs));
      assert(success); // should be no repeated segments
      auto& seg = *iter->second;
      seg.liveGen = seg.publishedLiveGen = segment.live_gen;
      seg.minVersion = segment.min_version;
      seg.maxVersion = segment.max_version;
      seg.liveDocs = segment.live_docs;
      seg.schemaGen = segment.schema_gen;
      seg.files = fromWire(segment.files);
      seg.firstCommitTime = segment.commit_time;  // firstCommitTime is stored in the segment meta.
      seg.lastCommitTime = indexInfo.commit_time; // not stored in the segment meta, so use index meta.
      seg.nextOverlayGen = segment.next_overlay_gen;
      seg.auxOverlays.reserve(segment.overlays.size());
      for (const auto& overlay : segment.overlays) {
        seg.auxOverlays.push_back(fromWire(overlay));
        currentSegmentOverlays_.push_back({segId, fromWire(overlay)});
      }
      mergePolicy->_update(&seg);
      lastCommittedSegIds.push_back(segId);
    }
    // Sort to ensure consistent ordering for comparison (currently not needed since we sort when doing commit)
    // std::sort(lastCommittedSegIds.begin(), lastCommittedSegIds.end());

    // Pull aux indexes forward so the next commit can carry them and so we
    // know which files the previous commit referenced (for orphan cleanup).
    currentAuxIndexes_.reserve(indexInfo.aux_indexes.size());
    for (const auto& aux : indexInfo.aux_indexes) {
      currentAuxIndexes_.push_back(fromWire(aux));
    }
  }
  if (!manifest.bytes) {
    api::IndexInfo initial;
    std::pmr::monotonic_buffer_resource arena;
    publish(initial, arena, schema ? schema : Schema::createDefaultSchema());
  } else if (manifest.generation != manifest.highestGeneration) {
    // A recovered older root must not advertise a regressed generation under
    // the old identity. Collection storage also switches incarnation directories.
    incarnation = CommitId::newIncarnation();
    std::pmr::monotonic_buffer_resource arena;
    auto recovered = Manifest::decode(manifest.bytes, arena);
    publish(recovered, arena);
  }
  snapshots.sweepOrphans(manifest);
  storedDictionaries.setSchema(StoredDictionaryRegistry::prepareSchema(*getSchema()));
  // Only manifest-live segments can seed a resource. Own the winning bytes;
  // later retirement of its segment must not affect inverter/merge writers.
  for (const auto& [name, type] : getSchema()->fieldTypeMap) {
    auto* stored = dynamic_cast<const StoredFieldType*>(type.get());
    if (!stored || !stored->format_.dictionaryBytes) continue;
    std::string_view bestDictionary;
    int64_t bestBytes = -1;
    uint64_t bestSegId = 0;
    for (const auto& [id, seg] : segInfos) {
      auto reader = getSegmentPostingsReader(*seg);
      const auto* field = reader->fieldInfo(name);
      if (!field || !field->stored || field->stored->dictionary.empty()) continue;
      auto bytes = field->columnMetaOff;
      if (bytes > bestBytes || (bytes == bestBytes && id < bestSegId)) {
        bestBytes = bytes;
        bestSegId = id;
        bestDictionary = field->stored->dictionary;
      }
    }
    auto token = storedDictionaries.token(name, stored->format_);
    if (!bestDictionary.empty() && token) {
      storedDictionaries.publish(name, *token,
          std::make_shared<const StoredDictionary>(std::string(bestDictionary)));
    }
  }
  {
    std::lock_guard<std::mutex> lock(indexMutex);
    seedActiveVectorOverlayNamesFromManifestLocked();
  }

  if (arena) arena->execute([this] { updateGraph.emplace(); });
  else updateGraph.emplace();

  // Create the updateGraph.  Start with a serial node that assigns an order to each update command
  // This could be done in a quick synchronized block instead... and could then be safely inspected by the client
  // if necessary before submitting to the actual processing graph.
  startUpdateNode = std::make_unique<UpdateMessageMultiFunc>(*updateGraph, 1,
    [this](UpdateMessage* msg,
    UpdateMessageMultiFunc::output_ports_type& op) {
      try {
        this->startUpdateBody(*msg);
      } catch (const std::exception& e) {
        // Only a closed writer throws here.  The message took no sequence number, so
        // it must complete here rather than flow on to the sequencer nodes.
        msg->result.setException(e);
        msg->done(*this);
        return;
      }
      std::get<0>(op).try_put(msg);
    });

  // next, the node to actually process the update, indexing documents, etc.
  processUpdateNode = std::make_unique<UpdateMessageFunc>(*updateGraph, tbb::flow::unlimited,
    [this](UpdateMessage* msg) -> UpdateMessage* {
      // exceptions handled in processUpdateBody()
      this->processUpdateBody(*msg);
      return msg;
    });

  // then make sure that the updates are finished in order so all updates are done before a commit is processed.
  updateSequencerNode = std::make_unique<tbb::flow::sequencer_node<UpdateMessage*>>(*updateGraph,
    [](UpdateMessage* msg) -> size_t {
      INDEX_DEBUG("updateSequencerNode: msg={} updateVersion={} updateOrdinal={}",
                  (void*)msg, msg->updateVersion, msg->updateOrdinal);
      return msg->updateOrdinal;
    });

  // updates flow into the updateFinishNode in order which is single threaded and ensures that updates are finished in order.
  updateFinishNode = std::make_unique<UpdateMessageMultiFunc>(*updateGraph, 1,
    [this](UpdateMessage* msg,
    UpdateMessageMultiFunc::output_ports_type& op) {
      unused(op);
      // exceptions handled in finishUpdateBody()
      this->finishUpdateBody(*msg);
      // std::get<0>(op).try_put(msg);
    });

  // connect the update nodes
  tbb::flow::make_edge(tbb::flow::output_port<0>(*startUpdateNode), *processUpdateNode);
  tbb::flow::make_edge(*processUpdateNode, *updateSequencerNode);
  tbb::flow::make_edge(*updateSequencerNode, *updateFinishNode);

  // now the commit nodes.  Flushes and merges run at a higher graph priority
  // than update batches: a prioritized graph task is submitted as a critical
  // task in the graph's arena, so workers take it ahead of any queued batch.
  // A flush frees inverter RAM while every queued batch adds to it, so under a
  // firehose (hundreds of batches in flight) a flush that waited its turn
  // behind them would let the budget overshoot by the whole queue; a merge
  // that waited would let segments pile up for the commit to pay for.
  segmentFlushNode = std::make_unique<InverterMultiFunc>(*updateGraph, tbb::flow::unlimited,
    [this](Inverter* inverter,
    InverterMultiFunc::output_ports_type& op) {
      unused(op);
      // TODO: handle exceptions
      this->segmentFlushBody(*inverter);
    }, FLUSH_PRIORITY);

  // Budget callbacks never take indexMutex or flush inline.  They coalesce on
  // this elevated-priority graph node, whose body rechecks global pressure and
  // claims victims against the budget before changing writer state.
  pressureCheckNode = std::make_unique<ContinueMultiFunc>(*updateGraph, 1,
    [this](tbb::flow::continue_msg,
           ContinueMultiFunc::output_ports_type& op) {
      unused(op);
      this->pressureCheckBody();
    }, FLUSH_PRIORITY);

  segmentReleaseNode = std::make_unique<ContinueMultiFunc>(*updateGraph, 1,
    [this](tbb::flow::continue_msg,
           ContinueMultiFunc::output_ports_type& op) {
      unused(op);
      std::vector<std::unique_ptr<SegInfo>> released;
      {
        const std::lock_guard lock(retiredMutex);
        released.swap(retiredSegments);
      }
    });

  commitSequencerNode = std::make_unique<tbb::flow::sequencer_node<UpdateMessage*>>(*updateGraph,
    [](UpdateMessage* msg) -> size_t {
      INDEX_DEBUG("commitSequencerNode: msg={} commitNum={}", (void*)&msg, msg->commitNum);
      return msg->commitNum;
    });

  // must be single-threaded
  commitFinishNode = std::make_unique<UpdateMessageMultiFunc>(*updateGraph, 1,
    [this](UpdateMessage* msg,
    UpdateMessageMultiFunc::output_ports_type& op) {
      unused(op);
      bool dispatchCompletion = true;
      // Delete passes share this serial node with commits, not their metrics.
      MetricScope metric(*metrics, metricActivity(msg->applyOnly ? MetricStage::DELETE_PASS : MetricStage::COMMIT));
      // Read before the body: a max_segments commit hands msg to the force
      // merge, which may complete and free it concurrently.
      bool forceIntent = !msg->applyOnly && !msg->publishOnly && msg->maxSegments > 0;
      try {
        if (msg->applyOnly) this->deletePassBody();
        else dispatchCompletion = this->finishCommitBody(*msg);
      }
      catch (std::exception& e) {
        LOG_ERROR("finishCommitBody Exception Caught: exception={}",
          e.what());
        msg->result.setException(e);
      }
      catch (...) {
        // Escaping the node would cancel the graph and strand this commit.
        std::runtime_error error("Unknown non-standard exception while committing");
        LOG_ERROR("finishCommitBody Exception Caught: exception={}", error.what());
        msg->result.setException(error);
      }
      if (forceIntent) {
        const std::lock_guard<std::mutex> lock(indexMutex);
        forceMergeIntents--;
        _resumePolicyMergesLocked();
      }

      metric.finish(dispatchCompletion && msg->result.errored() ? MetricOutcome::ERROR : MetricOutcome::OK);
      if (dispatchCompletion) {
        try {
          msg->done(*this);
        }
        catch (std::exception& e2) {
          LOG_ERROR("finishCommitBody completion threw: exception={}",
            e2.what());
        }
      }
    });

  tbb::flow::make_edge(*commitSequencerNode, *commitFinishNode);

  // must be single-threaded
  mergeSegmentsNode = std::make_unique<MergeMessageMultiFunc>(*updateGraph, 1,
    [this](MergeMessage* msg,
    MergeMessageMultiFunc::output_ports_type& op) {
      unused(op);
      INDEX_DEBUG("mergeSegmentsNode: msg={}", (void*)&msg,  msg->commitNum);
      bool dispatchCompletion = this->mergeSegmentsBody(*msg);
      if (dispatchCompletion) {
        try {
          msg->done(*this);
        } catch (std::exception& e2) {
          LOG_ERROR("mergeSegmentsNode completion threw: exception={}", e2.what());
        }
      }
    }, MERGE_PRIORITY);

  pressureRegistration = indexRamBudget->registerPressureListener([this]() noexcept {
    requestPressureCheck();
  });
  openingCommit = lastSnapshot.load()->id;
}

IndexWriter::~IndexWriter() {
  close();
  // Detach the deferred-commit slot BEFORE the final drain: after detach() no
  // timer callback can start, and a submit a racing callback already made is a
  // graph task covered by the wait below.  (Completion-side re-arms are guarded
  // by `closed`, which close() published before draining the graph.)
  DeadlineScheduler::global().detach(autoCommitSlot);
  // A submit racing close() is rejected by startUpdateNode, but that rejection is
  // still a graph task; wait for it before the nodes are destroyed.
  updateGraph->wait_for_all();
  idleInverters.clear();
  busyInverters.clear();
  flushingInverters.clear();
}

void IndexWriter::fail(std::string reason) {
  std::lock_guard publicationLock(publicationMutex);
  if (failed.load(std::memory_order_acquire)) return;
  failureReason = std::make_shared<const std::string>(std::move(reason));
  failed.store(true, std::memory_order_release);
  snapshots.fail();
}

void IndexWriter::close() {
  // No delete-pass retry fires once this returns; detach waits out a running one.
  DeadlineScheduler::global().detach(deletePassRetrySlot);
  // Fatal publication can happen inside a graph task. Drain here even when
  // already marked, never recursively from the publishing task itself.
  std::lock_guard closeLock(closeMutex);
  {
    std::lock_guard publicationLock(publicationMutex);
    std::lock_guard lock(indexMutex);
    closed.store(true, std::memory_order_release);
  }

  // Stop new budget callbacks before draining the graph.  Registration reset
  // synchronizes with a callback already copied out of the budget; any task it
  // queued before reset is included in wait_for_all below.
  pressureRegistration.reset();

  // without this, in gcc release mode we can get a crash when the IndexWriter is destroyed, even when
  // the graph wasn't used. Presumably because the test was so fast and there was some async initialization
  // of the graph still going on?
  updateGraph->wait_for_all();

  std::vector<UpdateMessage*> abandoned;
  {
    const std::lock_guard<std::mutex> lock(indexMutex);
    abandoned.reserve(pendingForceMerges.size() + (activeForceMerge == nullptr ? 0 : 1));
    if (activeForceMerge != nullptr) {
      abandoned.push_back(activeForceMerge);
      activeForceMerge = nullptr;
    }
    while (!pendingForceMerges.empty()) {
      abandoned.push_back(pendingForceMerges.front());
      pendingForceMerges.pop_front();
    }
  }

  for (auto* origin : abandoned) {
    setForceMergeError(*origin, "IndexWriter shut down before publication");
  }
  completeForceMergeOrigins(abandoned);
}

void IndexWriter::ForceMergeMessage::done(IndexWriter& iw) {
  iw.completeForceMerge(*this);
  delete this;
}

void IndexWriter::MergeCommitMessage::done(IndexWriter& iw) {
  MergeMessage* mergeMessage = origMessage;
  if (result.errored() && mergeMessage->forcedBy != nullptr) {
    std::string detail = "publish commit failed: ";
    detail.append(result.what());
    setForceMergeError(*mergeMessage->forcedBy, detail);
  }
  if (mergeMessage->forcedBy && resultingCommit) {
    mergeMessage->forcedBy->resultingCommit = resultingCommit;
  }
  delete this;
  mergeMessage->done(iw);
}

// Caller must hold indexMutex.
void IndexWriter::_addMergeWaiterHoldsLocked() {
  for (auto* waitingMsg : waitingForMerges) {
    assert(waitingMsg->commitInfo);
    waitingMsg->commitInfo->leftToFlush++;
  }
}

// Caller must hold indexMutex.
void IndexWriter::_releaseMergeWaiterHoldsLocked() {
  std::vector<UpdateMessage*> toRelease;
  toRelease.reserve(waitingForMerges.size());
  for (auto* waitingMsg : waitingForMerges) {
    assert(waitingMsg->commitInfo);
    assert(waitingMsg->commitInfo->leftToFlush > 0);
    if (--waitingMsg->commitInfo->leftToFlush == 0) {
      toRelease.push_back(waitingMsg);
    }
  }
  for (auto* releasedMsg : toRelease) {
    _releaseToCommitSequencer(releasedMsg);
  }
}

// Caller must hold indexMutex.  Admission and waiter accounting are one
// transaction so a rejected graph submission cannot strand a wait_for_merges
// commit or leave the policy throttle engaged.
bool IndexWriter::_submitMergeLocked(MergeMessage* msg) {
  mergePolicy->outstandingMerges++;
  _addMergeWaiterHoldsLocked();

  auto rollback = [&]() {
    assert(mergePolicy->outstandingMerges > 0);
    mergePolicy->outstandingMerges--;
    _releaseMergeWaiterHoldsLocked();
  };

  bool accepted;
  try {
    accepted = mergeSegmentsNode->try_put(msg);
  } catch (...) {
    rollback();
    throw;
  }
  if (!accepted) {
    rollback();
  }
  return accepted;
}

// Caller must hold indexMutex.  Rejected origins are returned to the caller so
// their potentially re-entrant done() callbacks run after releasing the lock.
void IndexWriter::_activateNextForceMergeLocked(std::vector<UpdateMessage*>& rejectedOrigins) {
  while (activeForceMerge == nullptr && !pendingForceMerges.empty()) {
    UpdateMessage* origin = pendingForceMerges.front();
    pendingForceMerges.pop_front();
    activeForceMerge = origin;

    auto failAdmission = [&](std::string_view detail) {
      std::string message = "merge admission failed: ";
      message.append(detail);
      setForceMergeError(*origin, message);
      activeForceMerge = nullptr;
      rejectedOrigins.push_back(origin);
    };

    std::unique_ptr<ForceMergeMessage> mergeMessage;
    try {
      mergeMessage = std::make_unique<ForceMergeMessage>();
      mergeMessage->maxSegments = origin->maxSegments;
      mergeMessage->forcedBy = origin;
      if (!_submitMergeLocked(mergeMessage.get())) {
        failAdmission("merge graph rejected the request");
        continue;
      }
    } catch (const std::exception& e) {
      failAdmission(e.what());
      continue;
    }

    mergeMessage.release();
  }
}

void IndexWriter::completeForceMergeOrigins(const std::vector<UpdateMessage*>& origins) {
  for (auto* origin : origins) {
    try {
      origin->done(*this);
    } catch (const std::exception& e) {
      LOG_ERROR("Force-merge origin completion threw: exception={}", e.what());
    } catch (...) {
      LOG_ERROR("Force-merge origin completion threw: unknown exception");
    }
  }
}

void IndexWriter::enqueueForceMerge(UpdateMessage& origin) {
  std::vector<UpdateMessage*> rejectedOrigins;
  {
    const std::lock_guard<std::mutex> lock(indexMutex);
    rejectedOrigins.reserve(pendingForceMerges.size() + 1);
    pendingForceMerges.push_back(&origin);
    _activateNextForceMergeLocked(rejectedOrigins);
  }

  completeForceMergeOrigins(rejectedOrigins);
}

void IndexWriter::completeForceMerge(MergeMessage& msg) {
  UpdateMessage* origin = msg.forcedBy;
  assert(origin != nullptr);

  std::vector<UpdateMessage*> completedOrigins;
  {
    const std::lock_guard<std::mutex> lock(indexMutex);
    completedOrigins.reserve(pendingForceMerges.size() + 1);
    completedOrigins.push_back(origin);
    assert(activeForceMerge == origin);
    activeForceMerge = nullptr;
    _activateNextForceMergeLocked(completedOrigins);
    _resumePolicyMergesLocked();
  }

  completeForceMergeOrigins(completedOrigins);
}

// Caller must hold indexMutex.  Flushes that landed while a pending force
// merge held policy merges off submitted none, so check every level after.
void IndexWriter::_resumePolicyMergesLocked() {
  if (_forceMergePendingLocked()) return;
  try {
    mergePolicy->_maybeMergeSegments(nullptr);
  } catch (const std::exception& e) {
    LOG_ERROR("Policy merge resume failed: exception={}", e.what());
  }
}

// Returns true when the merge message itself should be completed by the merge
// node.  An accepted commit takes over that completion and calls it from done().
bool IndexWriter::submitMergeCommit(MergeMessage& msg, bool publishOnly) {
  std::unique_ptr<MergeCommitMessage> commitMessage;
  try {
    commitMessage = std::make_unique<MergeCommitMessage>();
  } catch (const std::exception& e) {
    if (msg.forcedBy != nullptr) {
      std::string detail = "publish commit allocation failed: ";
      detail.append(e.what());
      setForceMergeError(*msg.forcedBy, detail);
    } else {
      msg.result.setException(e);
    }
    return true;
  }
  commitMessage->commit = UpdateMessage::COMMIT;
  commitMessage->publishOnly = publishOnly;
  commitMessage->origMessage = &msg;
  INDEX_DEBUG("mergeSegmentsBody: requesting commit. msg={}", (void*)commitMessage.get());
  if (submitUpdate(commitMessage.get())) {
    commitMessage.release();
    return false;
  }

  if (msg.forcedBy != nullptr) {
    setForceMergeError(*msg.forcedBy, "publish commit admission failed");
  }
  return true;
}

void IndexWriter::setSchema(std::shared_ptr<Schema> schema) {
  updateSchema([schema = std::move(schema)](const Schema*) { return schema; });
}

std::shared_ptr<const CommitSnapshot> IndexWriter::updateSchema(
    std::function<std::shared_ptr<Schema>(const Schema*)> change) {
  std::unique_lock lock(publicationMutex);
  if (isClosed()) throw IndexWriterClosedError("index writer is closed");
  auto previous = getSchema();
  auto schema = change(previous.get());
  if (!schema) throw std::invalid_argument("Schema change returned no schema");
  auto before = previous->signatures(true);
  for (const auto& [name, signature] : schema->signatures(true)) {
    auto old = before.find(name);
    if (old == before.end()) continue;
    // Templates are patterns, not fields: a template change only affects names resolved from it
    // later, and the schema history tracks it per generation.
    if (!name.empty() && name[0] == '_') continue;
    // Mirrors what merge enforces (SegFieldInfo::requireCompatibleType): anything that
    // changes the persisted type or its parameters would fail later at merge time.
    // Only long term handling and write-time vector normalization may change freely.
    const auto& oldProps = old->second.properties;
    const auto& newProps = signature.properties;
    for (const char* key : {"type", "multi", "index", "column", "posting_flags", "unit", "dims",
                            "truncate_dims", "metric", "normalized", "analyzer", "normalizer"}) {
      // Vector dims of 0 mean "not configured" (a bare template default): no vectors can be
      // written under it, so moving to or from it is not a data-affecting change.
      bool vectorKey = std::string_view(key) == "dims" || std::string_view(key) == "truncate_dims"
          || std::string_view(key) == "metric" || std::string_view(key) == "normalized";
      if (vectorKey) {
        auto oldDims = oldProps.find("dims");
        auto newDims = newProps.find("dims");
        if ((oldDims != oldProps.end() && oldDims->second == "0")
            || (newDims != newProps.end() && newDims->second == "0")) continue;
      }
      auto o = oldProps.find(key);
      auto n = newProps.find(key);
      std::string_view ov = o == oldProps.end() ? "none" : std::string_view(o->second);
      std::string_view nv = n == newProps.end() ? "none" : std::string_view(n->second);
      if (ov == nv) continue;
      throw SchemaError(std::format("Cannot change {} of existing field '{}': {} -> {}",
          key, name, ov, nv));
    }
  }
  std::pmr::monotonic_buffer_resource arena;
  api::IndexInfo info;
  if (!api::decode(info, api::copyToPaddedInput(*lastSnapshot.load()->bytes, arena), arena)) {
    throw std::runtime_error("Failed to decode published manifest");
  }
  publish(info, arena, std::move(schema));
  auto published = lastSnapshot.load();
  lock.unlock();
  tryDeleteSegments();
  return published;
}

Inverter& IndexWriter::obtainInverter(uint64_t updateVersion, std::shared_ptr<Schema> pinned) {
  // Reuse the idle inverter with the lowest minVersion, so the oldest work fills
  // and flushes. A stable pick (map order) would leave the others idle, holding
  // their minVersion and the delete retention it implies indefinitely.
  Inverter* inverter = nullptr;
  std::lock_guard<std::mutex> lock(indexMutex);
  auto current = getSchema();
  if (!pinned) pinned = current;
  for (auto it = idleInverters.begin(); it != idleInverters.end();) {
    auto* idle = (it++)->first;
    if (idle->schema != current) {
      if (!startIdleFlushLocked(*idle, false)) {
        throw std::runtime_error("Segment flush node rejected a stale inverter");
      }
    } else if (idle->schema == pinned && (!inverter || idle->minVersion < inverter->minVersion)) {
      inverter = idle;
    }
  }
  if (!inverter) {
    auto newInverter = std::make_unique<Inverter>(
        dir, ++lastSegId, pinned, indexRamBudget,
        [this](std::string_view resource, const StoredChunkFormat& format) {
          return storedDictionaries.lookup(resource, format);
        });
    inverter = newInverter.get();
    inverter->updateVersions(updateVersion);
    busyInverters.emplace(inverter, std::move(newInverter));
  }
  else {
    inverter->updateVersions(updateVersion);
    auto it = idleInverters.find(inverter);
    busyInverters.emplace(inverter, std::move(it->second));
    idleInverters.erase(it);
  }
  // Coercion context never leaks across update-message acquisitions. The
  // normal protobuf path immediately installs its request clock; custom/test
  // messages without one fall back to call-time DATE coercion.
  inverter->coerceContext = {};
  inverter->acquisitionMaxDoc = inverter->getMaxDoc();
  return *inverter;
}


void IndexWriter::requestPressureCheck() noexcept {
  bool expected = false;
  if (!pressureCheckQueued.compare_exchange_strong(
          expected, true, std::memory_order_acq_rel)) {
    return;
  }
  try {
    if (!pressureCheckNode->try_put(tbb::flow::continue_msg{})) {
      pressureCheckQueued.store(false, std::memory_order_release);
    }
  } catch (...) {
    pressureCheckQueued.store(false, std::memory_order_release);
  }
}

void IndexWriter::pressureCheckBody() {
  // Clear at entry: a notification concurrent with this check queues one more
  // pass, while a notification before entry is covered by this pass.  This is
  // the coalescing boundary that avoids both lost wakeups and self-requeue loops
  // when this writer has no eligible victim.
  pressureCheckQueued.store(false, std::memory_order_release);
  if (isClosed()) return;

  const std::lock_guard<std::mutex> lock(indexMutex);
  flushPinningInverterLocked();
  pressureShedIdleLocked();
}

// Caller holds indexMutex.  A pressure claim and the budget's global draining
// increment are one transaction, so concurrent writers may all inspect local
// victims but only still-needed victims transition to flushing.
bool IndexWriter::startIdleFlushLocked(Inverter& inverter, bool pressure) {
  auto it = idleInverters.find(&inverter);
  assert(it != idleInverters.end());

  if (pressure) {
    if (!inverter.ramGuard.tryMarkDrainingForPressure((int64_t)inverter.retainedDeleteBytes())) return false;
  } else {
    inverter.ramGuard.markDraining((int64_t)inverter.retainedDeleteBytes());
  }

  decltype(flushingInverters)::iterator flushingIt;
  try {
    auto [inserted, success] =
        flushingInverters.emplace(&inverter, std::move(it->second));
    assert(success);
    flushingIt = inserted;
    idleInverters.erase(it);
  } catch (...) {
    inverter.ramGuard.clearDraining();
    throw;
  }

  bool accepted = false;
  try {
    accepted = segmentFlushNode->try_put(&inverter);
  } catch (...) {
  }
  if (accepted) return true;

  auto inverterPtr = std::move(flushingIt->second);
  flushingInverters.erase(flushingIt);
  idleInverters.emplace(&inverter, std::move(inverterPtr));
  inverter.ramGuard.clearDraining();
  return false;
}

// Caller holds indexMutex. Largest-relief-first avoids flushing retained RAM; the
// budget-side claim prevents broadcasts to multiple writers from over-shedding
// once the global pressure target is met.
//
// Pressure that only merge demand creates sheds an inverter only once it holds
// a fair share of half the pool (budget / (2 * this writer's active
// inverters)): the demand is waiting for merges that shrink the segment count,
// and a stream of floor-sized flushes would grow it faster than they can.
void IndexWriter::pressureShedIdleLocked() {
  if (!indexRamBudget->pressurePossible()) return;
  size_t floor = pressureFlushFloorBytes;
  if (!idleInverters.empty() && !indexRamBudget->liveOverBudget()) {
    size_t active = idleInverters.size() + busyInverters.size();
    floor = std::max(floor, (size_t)indexRamBudget->totalBytes() / (2 * active));
  }
  for (;;) {
    Inverter* victim = nullptr;
    size_t largestRelief = 0;
    for (auto& entry : idleInverters) {
      size_t retained = entry.first->retainedDeleteBytes();
      assert(entry.first->memSize() >= retained);
      size_t relief = entry.first->memSize() - retained;
      if (relief == 0 || relief < floor) continue;
      if (relief > largestRelief) {
        victim = entry.first;
        largestRelief = relief;
      }
    }
    if (victim == nullptr || !startIdleFlushLocked(*victim, true)) return;
    INDEX_DEBUG("indexing RAM pressure, flushing idle inverter={}", *victim);
  }
}

void IndexWriter::releaseInverter(Inverter& inverter, bool flush) {
  // The undo scope is the update message that held this inverter; marks must
  // not outlive the release.
  inverter.clearUndoLog();

  // This is the first quiescent point where the output size proxy is known.
  // Spill candidates as soon as an inverter proves large, including when it
  // returns idle without flushing, so only expected-small segments retain
  // complete filesystem output in RAM.
  inverter.postingsWriter.configureRamDelegation(
      inverter.memSize() < PostingsWriter::SMALL_SEGMENT_BYTES);

  const std::lock_guard<std::mutex> lock(indexMutex);
  auto it = busyInverters.find(&inverter);
  if (it == busyInverters.end()) {
    LOG_ERROR("Inverter not found in busy list.");
    assert(false); // should never happen
  }

  // Resync this inverter's share of the global indexing RAM budget - once per
  // batch, not per doc. The
  // reservation is held until the inverter is destroyed (after flush), so
  // flushing inverters stay counted until their RAM is actually freed.
  inverter.ramGuard.forceResize((int64_t)inverter.memSize(), (int64_t)inverter.retainedDeleteBytes());
  if (inverter.getMaxDoc() > inverter.acquisitionMaxDoc) {
    inverter.minDocVersion = std::min(inverter.minDocVersion, inverter.currVersion);
    minInverterDocVersion = std::min(minInverterDocVersion, inverter.minDocVersion);
  }
  // A delete-only buffer flushes at the RAM cap or the id dictionary's locality
  // knee, whichever comes first: past the knee, larger packets buy no per-id
  // saving. Below the floor, the knee's sum over segments is not needed.
  if (!flush && inverter.commitInfo == nullptr && inverter.getMaxDoc() == 0
      && inverter.deleteCount() >= deleteOnlyFlushFloor
      && inverter.deleteCount() >= deleteOnlyFlushCapLocked()) {
    flush = true;
    deleteOnlyKneeFlushes++;
  }

  // Size-based auto-flush is driven by the caller (ProtoUpdateMessage::handle passes
  // flush=true at end of batch when inverter.shouldFlush() is true), not decided here:
  // the message handler owns the safe flush point (a closed undo scope). The global
  // budget check below only ever flushes idle inverters.

  // if this inverter is part of a commit, initiate a flush.
  if (inverter.commitInfo != nullptr || flush || inverter.schema != getSchema()) {
    if (inverter.commitInfo) {
      INDEX_DEBUG("inverter={} message={} triggering flush.", inverter,
                  (void*)inverter.commitInfo->updateMessage);
    } else {
      INDEX_DEBUG("inverter={} flush requested.", inverter);
    }
    auto [flushingIt, success] =
        flushingInverters.emplace(&inverter, std::move(it->second));
    assert(success);
    flushingIt->second->ramGuard.markDraining((int64_t)inverter.retainedDeleteBytes());
    it = busyInverters.erase(it);
    if (!segmentFlushNode->try_put(&inverter)) {
      throw std::runtime_error("Segment flush node rejected a released inverter");
    }
  }
  else {
    INDEX_DEBUG("releaseInverter: inverter={} adding back to idleInverters.", inverter);
    // return inverter to idle pool
    idleInverters.emplace(&inverter, std::move(it->second));
    busyInverters.erase(it);
    // Only the frontier holder going idle can make it worth flushing.
    if (inverter.minDocVersion == minInverterDocVersion) flushPinningInverterLocked();
  }

  // Re-evaluate even when raw reservations fit: pending merge demand may need
  // headroom.  Already-draining guards are discounted globally by the budget,
  // suppressing floor-sized respawn flushes while the current victims drain.
  pressureShedIdleLocked();
}


void IndexWriter::commit(std::function<void()>&& callback, UpdateMessage::CommitType commitType) {
  class UpdateMessageWithCallback : public UpdateMessage {
  public:
    std::function<void()> callback;

    void handle(IndexWriter& iw) override {
      unused(iw);
    }

    void done(IndexWriter& iw) override {
      unused(iw);
      std::unique_ptr<UpdateMessageWithCallback> self(this);
      callback();
    }
  };

  auto updateMessage = std::make_unique<UpdateMessageWithCallback>();
  updateMessage->commit = commitType;
  updateMessage->callback = std::move(callback);
  if (!submitUpdate(updateMessage.get())) {
    throw std::runtime_error("Commit admission failed");
  }
  updateMessage.release();
}


// Deferred-commit machinery.  Invariants (all under autoCommitMutex):
//  - autoCommitPending means data sequenced since the last covering commit carries
//    a deadline (the min of its windows).  Set/cleared only from startUpdateBody
//    (serial), so it is exactly ordered against the commits that satisfy it.
//  - The scheduler slot is armed only while no auto-commit is in flight; the
//    completion callback re-arms if new deadline data arrived meanwhile.  A commit
//    slower than the window therefore degrades to back-to-back auto-commits, never
//    a growing queue of them.
void IndexWriter::deferCommitWithin(int64_t withinMs, uint64_t updateVersion) {
  const auto now = DeadlineScheduler::Clock::now();
  // Saturate: a window past the clock's range would overflow the addition (UB,
  // typically wrapping the deadline into the past).  Stay below the slot's
  // time_point::max() disarmed sentinel.
  const int64_t maxMs = std::chrono::duration_cast<std::chrono::milliseconds>(
      DeadlineScheduler::Clock::time_point::max() - now).count() - 1000;
  auto due = now + std::chrono::milliseconds(std::min(withinMs, maxMs));
  bool arm;
  {
    const std::lock_guard<std::mutex> lock(autoCommitMutex);
    // Reach the (process-global) scheduler only when this update TIGHTENED the
    // window: in a steady stream all carrying the same T, the first update
    // after each commit arms the timer and every later one is just this
    // writer-local lock + compare.
    const bool tightened = !autoCommitPending || due < autoCommitDeadline;
    if (tightened) autoCommitDeadline = due;
    autoCommitPending = true;
    autoCommitMaxVersion = updateVersion;  // monotonic: only the serial entry node calls this
    arm = tightened && !autoCommitInFlight;
    due = autoCommitDeadline;
  }
  // Arm outside the lock: the scheduler callback takes autoCommitMutex.  arm()
  // only tightens, so a racing stale arm can at worst fire early; the callback
  // re-checks the deadline and re-arms.
  if (arm) DeadlineScheduler::global().arm(autoCommitSlot, due);
}

// Called after a commit is durably published, with its watermark.  Clearing on
// publication (not graph entry) means a FAILED commit leaves the pending
// deadline in place: the synthetic auto-commit retries (paced by
// autoCommitFinished), and deferred data behind a failed explicit commit is
// still covered by its own timer.
void IndexWriter::deferredCommitPublished(uint64_t highestUpdateVersion) {
  const std::lock_guard<std::mutex> lock(autoCommitMutex);
  if (autoCommitPending && autoCommitMaxVersion <= highestUpdateVersion) {
    autoCommitPending = false;
    // The slot may still fire at the stale deadline; the callback no-ops on
    // !autoCommitPending.
  }
}

// Runs on the DeadlineScheduler thread.
void IndexWriter::autoCommitTimerFired() {
  DeadlineScheduler::Clock::time_point rearm{};
  {
    const std::lock_guard<std::mutex> lock(autoCommitMutex);
    if (!autoCommitPending || autoCommitInFlight) return;
    if (DeadlineScheduler::Clock::now() < autoCommitDeadline) {
      rearm = autoCommitDeadline;  // spurious early fire (stale tighter arm)
    } else {
      autoCommitInFlight = true;
    }
  }
  if (rearm != DeadlineScheduler::Clock::time_point{}) {
    // Must not throw on the scheduler thread; the slot is already registered
    // here so arm() cannot allocate, but stay defensive.
    try {
      DeadlineScheduler::global().arm(autoCommitSlot, rearm);
    } catch (const std::exception& e) {
      LOG_ERROR("deferred-commit re-arm failed: {}", e.what());
    }
    return;
  }
  // Submit outside the lock: try_put may run startUpdateBody inline on this
  // thread, and that takes autoCommitMutex.  On a closed writer the message
  // is rejected at the graph entry and the callback still runs (with closed
  // observed true, so it will not re-arm).
  try {
    commit([this] { autoCommitFinished(); }, UpdateMessage::COMMIT);
  } catch (const std::exception& e) {
    LOG_ERROR("deferred auto-commit submit failed: {}", e.what());
    autoCommitFinished();
  }
}

void IndexWriter::autoCommitFinished() {
  bool arm = false;
  DeadlineScheduler::Clock::time_point due{};
  {
    const std::lock_guard<std::mutex> lock(autoCommitMutex);
    autoCommitInFlight = false;
    // Pending here means either new deadline data arrived while this commit ran
    // (future deadline - resume its timer exactly), or the commit FAILED and
    // publication never cleared it (deadline now stale - floor the retry so a
    // persistently failing commit pipeline is paced, not spun).
    if (autoCommitPending && !isClosed()) {
      const auto now = DeadlineScheduler::Clock::now();
      if (autoCommitDeadline <= now) autoCommitDeadline = now + std::chrono::seconds(1);
      arm = true;
      due = autoCommitDeadline;
    }
  }
  if (arm) {
    try {
      DeadlineScheduler::global().arm(autoCommitSlot, due);
    } catch (const std::exception& e) {
      LOG_ERROR("deferred-commit re-arm failed: {}", e.what());
    }
  }
}

std::optional<CommitId> IndexWriter::commit(UpdateMessage::CommitType commitType) {
  class BlockingUpdateMessage : public UpdateMessage {
  public:
    Blocker blocker;

    void handle(IndexWriter& iw) override {
      unused(iw);
    }

    void done(IndexWriter& iw) override {
      unused(iw);
      blocker.notify();
    }
  };

  // all stack allocated since we will be waiting for completion.
  BlockingUpdateMessage updateMessage;
  updateMessage.commit = commitType;

  INDEX_DEBUG("SYNC_COMMIT_START: msg={}", (void*)&updateMessage);
  if (!submitUpdate(&updateMessage)) {
    throw std::runtime_error("Commit admission failed");
  }

  updateMessage.blocker.wait();
  INDEX_DEBUG("SYNC_COMMIT_END: msg={}", (void*)&updateMessage);
  if (updateMessage.result.errored()) std::rethrow_exception(updateMessage.result.getError()->eptr);
  return updateMessage.resultingCommit;
}


void IndexWriter::initiateCommit(UpdateMessage& msg) {
  INDEX_DEBUG("initiateCommit: msg={} STARTING", (void*)&msg);
  auto emptyCommitInfo = std::make_unique<CommitInfo>();
  Signal::emit("initiateCommit", &msg);

  {
    const std::lock_guard<std::mutex> lock(indexMutex);

    // Finish container growth before transferring commit state.  Once the
    // transfer starts, only graph admission can fail.
    if (!msg.publishOnly) {
      flushingInverters.reserve(flushingInverters.size() + idleInverters.size());
      if (msg.waitForMerges) {
        waitingForMerges.reserve(waitingForMerges.size() + 1);
      }
    }

    // A merge publication must not flush or consume state from later client
    // updates.  It only needs a fresh CommitInfo for generation assignment and
    // durable publication of the segment layout already in segInfos.
    if (msg.publishOnly) {
      msg.commitInfo = std::move(emptyCommitInfo);
      msg.commitInfo->updateMessage = &msg;
      msg.commitNum = commitNumber;
    if (msg.waitForMerges) Signal::emit("commitWaitingForMerges", &msg);
      _releaseToCommitSequencer(&msg);
      commitNumber++;
      return;
    }

    msg.commitInfo = std::move(emptyCommitInfo);
    auto& commitInfo = *msg.commitInfo;
    commitInfo.updateMessage = &msg; // set the update message that triggered this commit

    // Register with one hold for every merge already submitted but not tailed.
    // A later merge submission adds its own hold before entering the graph.
    if (msg.waitForMerges) {
      waitingForMerges.push_back(&msg);
      commitInfo.leftToFlush += (uint32_t)mergePolicy->outstandingMerges;
    }

    // first look at any flushing inverters that are not marked for a commit yet
    // and mark them if necessary.
    for (auto it = flushingInverters.begin(); it != flushingInverters.end(); it++) {
      if (it->second->commitInfo == nullptr && it->second->minVersion <= msg.updateVersion) {
        INDEX_DEBUG("\tinitiateCommit: msg={} marking flushing inverter={} for commit", (void*)&msg,
                    *it->second.get());
        it->second->commitInfo = &commitInfo;
        commitInfo.leftToFlush++;
      }
    }

    // now look at all idle inverters and initiate a flush if necessary.
    for (auto it = idleInverters.begin(); it != idleInverters.end();) {
      auto current = it++;
      if (current->second->minVersion <= msg.updateVersion) {
        if (current->second->commitInfo == nullptr) {
          INDEX_DEBUG("\tinitiateCommit: msg={} marking idle inverter={} for commit", (void*)&msg,
                      *current->second.get());
          current->second->commitInfo = &commitInfo;
          commitInfo.leftToFlush++;
        }
        else {
          // this would be a bug since we should never have an idle inverter that is part of a commit.
          LOG_ERROR("Idle inverter is part of another commit.");
        }

        if (!startIdleFlushLocked(*current->second, false)) {
          throw std::runtime_error("Segment flush node rejected a commit inverter");
        }
      }
    }

    // Any inverters that are busy should be marked so that when they are released they can be flushed.
    for (auto it = busyInverters.begin(); it != busyInverters.end(); it++) {
      if (it->second->commitInfo == nullptr && it->second->minVersion <= msg.updateVersion) {
        INDEX_DEBUG("\tinitiateCommit: msg={} marking busy inverter={} for commit", (void*)&msg,
                    *it->second.get());
        it->second->commitInfo = &commitInfo;
        commitInfo.leftToFlush++;
      }
    }

    INDEX_DEBUG("initiateCommit: msg={} leftToFlush={}", (void*)&msg, msg.commitInfo->leftToFlush);

    // updateFinishNode is serial and segment-flush completion needs indexMutex,
    // so this is the commit's single admission point.  No ordinal is consumed
    // until all state above is installed.  An immediate commit increments the
    // counter only after the sequencer accepts it.
    msg.commitNum = commitNumber;
    if (msg.waitForMerges) Signal::emit("commitWaitingForMerges", &msg);
    // Normally a commit would be kicked off by the last segment flushing.  But if there are no segments to flush,
    // we need to kick it off here.
    if (commitInfo.leftToFlush == 0) {
      try {
        _releaseToCommitSequencer(&msg);
      } catch (...) {
        if (msg.waitForMerges) {
          std::erase(waitingForMerges, &msg);
        }
        throw;
      }
    }
    commitNumber++;
    // Flushes this commit started complete under indexMutex, so none can
    // reach the merge policy before the intent is visible.
    if (msg.maxSegments > 0) forceMergeIntents++;
  } // end mutex protected section
}

// Caller must hold indexMutex.
void IndexWriter::_releaseToCommitSequencer(UpdateMessage* msg) {
  if (!commitSequencerNode->try_put(msg)) {
    throw std::runtime_error("Commit sequencer rejected an admitted commit");
  }
  if (!msg->waitForMerges) return;

  auto it = std::find(waitingForMerges.begin(), waitingForMerges.end(), msg);
  if (it != waitingForMerges.end()) {
    waitingForMerges.erase(it);
  }
}

uint64_t IndexWriter::minDocumentVersionLocked() const {
  uint64_t minimum = UINT64_MAX;
  auto include = [&](const auto& inverters) {
    for (const auto& [inverter, _] : inverters) minimum = std::min(minimum, inverter->minDocVersion);
  };
  include(busyInverters);
  include(idleInverters);
  include(flushingInverters);
  return minimum;
}

// The delete frontier. A packet whose largest version V is at or below it has
// no live inverter holding a document born before V and no older request in
// flight. Completion is ordered: a request's documents are recorded under
// indexMutex at release before completedUpdateVersion can pass its version.
// Only request completion and flush registration raise the frontier, so
// passes are requested there; a release cannot, since it lowers
// minInverterDocVersion only to a request still in flight. Cached inputs are
// lowered eagerly and raised only by a recompute, so a stale one can delay a
// pass but never retire a packet early.
uint64_t IndexWriter::deleteFrontierLocked() const {
  return std::min(completedUpdateVersion, minInverterDocVersion);
}

uint64_t IndexWriter::deleteOnlyFlushCapLocked() const {
  uint64_t blocks = 0;
  for (const auto& [_, seg] : segInfos) {
    auto count = seg->idDictionaryBlocks.load();
    // Reopened segments and merge outputs without a finalized id dictionary
    // count slots until a reader supplies the exact count (including partial
    // blocks at partition boundaries).
    if (count == UINT64_MAX) count = ((uint64_t)seg->maxDoc + Postings::TERMS_BLOCK_SIZE - 1) / Postings::TERMS_BLOCK_SIZE;
    blocks += count;
  }
  return std::max(deleteOnlyFlushFloor, blocks);
}

// The oldest inverter holding documents pins the frontier; flush it once idle
// if the pending batches it holds back are at least its size. Releases of other
// inverters cannot make that true, so it is checked when a batch registers,
// when the holder goes idle, and on a pressure check.
void IndexWriter::flushPinningInverterLocked() noexcept {
  // Only a finite budget runs passes. A pin below completedUpdateVersion must
  // hold back a pending batch.
  if (indexRamBudget->totalBytes() == 0 || minInverterDocVersion >= completedUpdateVersion
      || pendingDeletes.between(minInverterDocVersion, completedUpdateVersion).empty()) return;
  Inverter* oldest = nullptr;
  uint64_t first = UINT64_MAX, next = completedUpdateVersion;
  auto include = [&](const auto& inverters) {
    for (const auto& [inverter, _] : inverters) {
      auto version = inverter->minDocVersion;
      if (version == UINT64_MAX) continue;
      if (version < first) {
        next = std::min(next, first);
        first = version;
        oldest = inverter;
      } else next = std::min(next, version);
    }
  };
  include(busyInverters);
  include(idleInverters);
  include(flushingInverters);
  if (!oldest || next <= first || !idleInverters.contains(oldest)) return;
  uint64_t released = 0;
  for (const auto& batch : pendingDeletes.between(first, next)) released += batch->memSize();
  try {
    if (released >= oldest->memSize() && startIdleFlushLocked(*oldest, false)) {
      pinningInverterFlushes++;
      Signal::emit("pinningInverterFlush", oldest, (void*)released);
    }
  } catch (...) {
    // A flush that fails to start leaves the inverter idle for the next check.
  }
}

// A pass takes a packet once every request through its largest version has
// flushed and no outstanding commit has claimed it.
bool IndexWriter::passMayRetireLocked(const SortedDeletes& packet, uint64_t frontier) const {
  return packet.getLargestVersion() <= frontier && packet.commitClaim <= completedCommitVersion;
}

// Visits each packet a pass would take now, pending batches first, as
// visit(packet, fromPending); stops and returns false once visit does. The
// pass trigger and the pass's capture both select here. Pending batches are
// ordered by largest version, so the walk stops at the first past the frontier.
template <class Visit>
bool IndexWriter::forEachPassPacketLocked(uint64_t frontier, Visit&& visit) {
  for (const auto& packet : pendingDeletes.deletes) {
    if (packet->getLargestVersion() > frontier) break;
    if (passMayRetireLocked(*packet, frontier) && !visit(packet, true)) return false;
  }
  if (!carriedDeletes) return true;
  bool carried = false;
  for (const auto& [_, seg] : segInfos) {
    for (const auto& packet : seg->personalDeletes.deletes) {
      carried = true;
      if (passMayRetireLocked(*packet, frontier) && !visit(packet, false)) return false;
    }
  }
  carriedDeletes = carried;
  return true;
}

// Any packet a pass may take starts one, so the final commit inherits only the
// last generation. One outstanding message keeps a commit behind at most one
// pass; its finish asks again. This runs for every request, so it reads only
// pending batches at or below the frontier, and carried packets while a merge
// output holds some.
void IndexWriter::requestDeletePassLocked() noexcept {
  if (deletePassQueued || deletePassHeld || isClosed() || indexRamBudget->totalBytes() == 0) return;
  try {
    if (forEachPassPacketLocked(deleteFrontierLocked(), [](const auto&, bool) { return false; })) return;
    auto message = std::make_unique<DeletePassMessage>();
    deletePassQueued = true;
    if (commitFinishNode->try_put(message.get())) message.release();
    else deletePassQueued = false;
  } catch (...) {
    deletePassQueued = false;
  }
}

// A batch the frontier has passed leaves pressure: a pass, or the commit that
// claimed it, reclaims it. Runs where the frontier can move, and marks only the
// batches between the last frontier it saw and this one.
void IndexWriter::markRetirableLocked() {
  uint64_t frontier = deleteFrontierLocked();
  if (frontier <= retirableThrough) return;
  for (const auto& batch : pendingDeletes.between(retirableThrough, frontier)) {
    if (batch->ramGuard) batch->ramGuard.markRetirable();
  }
  retirableThrough = frontier;
}

void IndexWriter::deletePassFinished(bool succeeded) {
  uint64_t prepared;
  std::optional<DeadlineScheduler::Clock::time_point> retry;
  {
    const std::lock_guard lock(indexMutex);
    prepared = preparedDeleteVersion;
    deletePassQueued = false;
    if (succeeded) {
      deletePassBackoff = {};
    } else if (!isClosed()) {
      // Retry without waiting for traffic, paced so a persistent fault cannot spin.
      deletePassBackoff = deletePassBackoff.count() == 0 ? DELETE_PASS_RETRY_MIN
                                                         : std::min(2 * deletePassBackoff, DELETE_PASS_RETRY_MAX);
      deletePassHeld = true;
      retry = DeadlineScheduler::Clock::now() + deletePassBackoff;
    }
    requestDeletePassLocked();
  }
  if (retry) {
    try {
      DeadlineScheduler::global().arm(deletePassRetrySlot, *retry);
    } catch (const std::exception& e) {
      LOG_ERROR("delete pass retry arm failed: {}", e.what());
    }
  }
  Signal::emit("deletePassFinished", this, (void*)prepared);
}

// Runs on the DeadlineScheduler thread; close() detaches the slot.
void IndexWriter::deletePassRetryFired() {
  const std::lock_guard lock(indexMutex);
  deletePassHeld = false;
  requestDeletePassLocked();
}

// Inverter for the segment should already be in the flushingInverters list.
// This is called in parallel.  The inverter will be deleted.
void IndexWriter::segmentFlushBody(Inverter& inverter) {
  MetricWork metric(*metrics, MetricStage::FLUSH);
  INDEX_DEBUG("segmentFlushBody: inverter={} commitInfo={} msg.leftToFlush={}", inverter,
              (void*)inverter.commitInfo,
              inverter.commitInfo == nullptr ? -1 : inverter.commitInfo->leftToFlush);
  // Test seam; emitted before any lock so a blocking listener can hold flushes
  // in flight without stalling releaseInverter.
  Signal::emit("segmentFlushBody", &inverter);

  std::vector<std::string> flushedFiles;
  std::vector<FileDescriptor> descriptors;
  bool success = false;
  bool aborted = inverter.failed();
  if (!aborted) {
    try {
      // uncomment to serialize inverter flushing (for testing purposes)
      // const std::lock_guard<std::mutex> lock(indexMutex);
      success = inverter.flush(&flushedFiles, &descriptors);
    } catch (const std::exception& e) {
      LOG_ERROR("Exception caught while flushing inverter: {}", e.what());
      inverter.fail(std::current_exception());
      aborted = true;
      success = false;
    } catch (...) {
      LOG_ERROR("Unknown non-standard exception caught while flushing inverter");
      inverter.fail(std::current_exception());
      aborted = true;
      success = false;
    }
  }

  metric.failed = aborted;
  std::unique_ptr<SegInfo> segInfo;
  if (!aborted) {
    segInfo = std::make_unique<SegInfo>(inverter.getPostingsWriter().segId,
                                        inverter.getPostingsWriter().getMaxDoc());
    segInfo->unsyncedFiles = std::move(flushedFiles);
    segInfo->files = std::move(descriptors);
    segInfo->minVersion = inverter.minVersion;
    segInfo->maxVersion = inverter.maxVersion;
    segInfo->schemaGen = inverter.schema->gen_;
    auto& postings = inverter.getPostingsWriter();
    if (auto& range = postings.idRange) segInfo->idRange.store(std::make_shared<const IdRange>(std::move(*range)));
    segInfo->idDictionaryBlocks.store(postings.idDictionaryBlocks.value_or(0));
    // Set liveDocs + liveGen for deleted docs from errors during indexing
    if (inverter.liveGen > 0) {
      liveDocsBytesWritten += LiveDocsWriter::fileBytes(segInfo->maxDoc);
      segInfo->liveGen = inverter.liveGen;
      segInfo->liveDocs = inverter.liveDocs;
      assert(segInfo->liveDocs <= segInfo->maxDoc);
      // We still need to carry over deletes-by-string-id even if liveDocs == 0.
    } else {
      segInfo->liveDocs = segInfo->maxDoc;
    }
  }

  // A segment born with no live docs (every doc failed and was marked deleted)
  // never becomes visible: don't register it as a live segment.  Registering it
  // would feed the merge policy a level count that merge selection can never
  // reduce (mergeSegmentsBody skips liveDocs==0 sources), which sustains an
  // endless self-chaining merge loop.  Route it straight to segmentsToDelete so
  // its files get removed after the commit.
  if (success && segInfo->liveDocs == 0) {
    INDEX_DEBUG("segmentFlushBody: inverter {} produced empty segment {}; dropping",
                (void*)&inverter, *segInfo);
    success = false;
    const std::lock_guard<std::mutex> lock(indexMutex);
    segmentsToDelete.push_back(std::move(segInfo));
  }

  std::unique_ptr<Inverter> inverterPtr;
  std::shared_ptr<SortedDeletes> deletes;
  if (!aborted && inverter.hasDeletions()) {
    auto& batch = *inverter.sortedDeletes;
    int64_t actual = (int64_t)batch.memSize();
    assert(actual == inverter.ramGuard.nonDrainingSize());
    batch.ramGuard = inverter.ramGuard.transferToRetainedDeletes(actual);
    deletes = std::move(inverter.sortedDeletes);
  }

  {
    const std::lock_guard<std::mutex> lock(indexMutex);
    INDEX_DEBUG("segmentFlushBody: inverter {} flushed. Adding {}", (void*)&inverter,
                segInfo ? format_as(*segInfo)
                        : std::string(aborted ? "(aborted segment)" : "(empty segment, dropped)"));

    // Reserve the segment map, then register deletes before exposing the segment.
    // The delete-set insertion may allocate. Holding indexMutex across both
    // registrations keeps segment snapshots consistent with their deletes.
    if (success) segInfos.reserve(segInfos.size() + 1);
    if (deletes) {
      if (segInfo) deletes->sourceSegId = segInfo->segId;
      // An adopting commit retires the packet unless it holds a newer request,
      // and must not race a pass for it. No commit adopts the inverter once it
      // leaves flushingInverters below. If initiateCommit throws after adopting
      // inverters, nothing completes that commit, so their packets keep the
      // claim until a later commit completes: a delay only.
      if (auto* commit = inverter.commitInfo) {
        uint64_t version = commit->updateMessage->updateVersion;
        if (deletes->getLargestVersion() <= version) deletes->commitClaim = version;
      }
      if (deletes->getLargestVersion() <= retirableThrough && deletes->ramGuard) deletes->ramGuard.markRetirable();
      pendingDeletes.deletes.insert(std::move(deletes));
    }

    // segments are flushed in parallel, so the segids are not in order... (or in the completed order.) should be fine.
    std::pair<SegMap::iterator, bool> iter;
    if (success) {
      iter = segInfos.emplace(segInfo->segId, std::move(segInfo));
      assert(iter.second); // should never already exist
    }

    // remove the inverter from the flushingInverters set, but remember it until the end of this function.
    auto it = flushingInverters.find(&inverter);
    if (it == flushingInverters.end()) {
      assert(false);
    }
    inverterPtr = std::move(it->second);
    flushingInverters.erase(it);
    // The cached minimum rises only when the inverter holding it leaves.
    if (inverterPtr->minDocVersion != UINT64_MAX && inverterPtr->minDocVersion == minInverterDocVersion) {
      minInverterDocVersion = minDocumentVersionLocked();
    }
    markRetirableLocked();
    requestDeletePassLocked();
    flushPinningInverterLocked();

    // Check if we should merge anything.  Must happen *before* the leftToFlush
    // decrement: if this flush triggers a merge and the inverter's commit is
    // a member of waitingForMerges, _maybeMergeSegments will bump its
    // leftToFlush.  Doing the bump first ensures the subsequent decrement
    // doesn't prematurely fire triggerCommit (and release the message into
    // commitSequencerNode while a merge still holds it as a waiter).
    // We do this with the lock held since segInfo could go away otherwise.
    if (success) {
      mergePolicy->_maybeMergeSegments(iter.first->second.get());
    }

    // The mutex protects against races in the setting of inverter.updateMessage as well as leftToFlush.
    // Higher level logical races protected against would be kicking off a segment flush and then that completing and
    // kicking off a commit before the next segment flush is kicked off.
    // Release inside the same lock block so a concurrent _maybeMergeSegments
    // can't bump leftToFlush back up between our 0-check and the try_put.
    if (inverter.commitInfo != nullptr) {
      if (aborted) {
        setException(inverter.commitInfo->updateMessage->result, inverter.failure());
      }
      if (--inverter.commitInfo->leftToFlush == 0) {
        _releaseToCommitSequencer(inverter.commitInfo->updateMessage);
      }
    }
  }

  // Destroy before removing an aborted segment's files.
  uint64_t segId = inverterPtr->getPostingsWriter().segId;
  inverterPtr.reset();
  if (aborted) {
    try {
      dir.deletePrefix(Postings::getIndexFileNamePrefix(segId));
    } catch (const std::exception& e) {
      LOG_ERROR("Failed to clean files for aborted segment {}: {}", segId, e.what());
    }
  }
}

void IndexWriter::DeleteApplication::select(const std::shared_ptr<const SortedDeletes>& packet, bool fromPending) {
  selected.deletes.insert(packet);
  if (fromPending) pending.deletes.insert(packet);
}

void IndexWriter::DeleteApplication::unpin() {
  if (!pinned) return;
  const std::lock_guard lock(writer.indexMutex);
  for (auto* seg : segs) --seg->preparationPins;
  pinned = false;
}

// Caller holds indexMutex. Pins every registered segment across application
// and concurrent merge replacement, and picks those a selected packet may touch.
void IndexWriter::pinSnapshotLocked(DeleteApplication& app) {
  app.segs.reserve(segInfos.size());
  app.targets.reserve(segInfos.size());
  app.pinned = true;
  for (auto& [segId, seg] : segInfos) {
    app.segs.push_back(seg.get());
    ++seg->preparationPins;
    bool carried = std::ranges::any_of(seg->personalDeletes.deletes, [&](const auto& packet) {
      return app.selected.deletes.contains(packet) && mayApplyDeletes(*seg, *packet);
    });
    if (carried || mayApplyDeletes(*seg, app.pending)) app.targets.push_back(seg.get());
  }
}

// A running merge carries completed applications by liveDocs diff. Only an
// application finishing after the swap needs a packet carry to the output.
void IndexWriter::reconcileDeletes(const DeleteApplication& app) {
  const std::lock_guard<std::mutex> lock(indexMutex);
  for (auto* seg : app.segs) {
    if (!seg->merging && seg->mergedLiveGen >= 0 && seg->mergedLiveGen != (int64_t)seg->liveGen) {
      // Swaps keep mergedIntoSegId on the latest output. An output that is
      // no longer live has nothing left to delete.
      if (auto it = segInfos.find(seg->mergedIntoSegId); it != segInfos.end()) {
        auto& target = *it->second;
        // The output is newer than this snapshot. Attach before clearing
        // the source so an allocation failure keeps the packets.
        assert(std::ranges::find(app.segs, &target) == app.segs.end());
        uint64_t carriedBytes = 0;
        auto carry = [&](const auto& packet) {
          if (target.personalDeletes.deletes.insert(packet).second) carriedBytes += packet->memSize();
          carriedDeletes = true;
        };
        for (const auto& packet : seg->personalDeletes.deletes) {
          if (app.selected.deletes.contains(packet)) carry(packet);
        }
        // Batches this application does not retire reach the output from pending.
        for (const auto& batch : app.pending.deletes) {
          if (batch->getLargestVersion() <= app.retireThrough) carry(batch);
        }
        Signal::emit("mergeDeleteCarry", this, (void*)carriedBytes, seg);
      }
    }
    if (!seg->personalDeletes.empty()) {
      boost::container::erase_if(seg->personalDeletes.deletes,
                                 [&](const auto& packet) { return app.selected.deletes.contains(packet); });
    }
  }
}

// Retires the application's pending batches through retireThrough; merge
// carries keep their own references. Returns the bytes removed from pending.
uint64_t IndexWriter::retirePendingLocked(const DeleteApplication& app) {
  uint64_t bytes = 0;
  for (const auto& batch : app.pending.deletes) {
    if (batch->getLargestVersion() > app.retireThrough || !pendingDeletes.deletes.erase(batch)) continue;
    preparedDeleteVersion = std::max(preparedDeleteVersion, batch->getLargestVersion());
    bytes += batch->memSize();
  }
  return bytes;
}

// This applies deletes and writes out the new segments file.
// called from the commitFinishNode which has concurrency==1 (single-threaded)
bool IndexWriter::finishCommitBody(UpdateMessage& msg) {
  INDEX_DEBUG("finishCommitBody: msg={}", (void*)&msg);
  // A commit's claims end with it on every path; packets it did not retire
  // return to passes.
  auto endClaims = scope_guard([this, version = msg.updateVersion] {
    const std::lock_guard lock(indexMutex);
    completedCommitVersion = std::max(completedCommitVersion, version);
    requestDeletePassLocked();
  });
  if (failed.load(std::memory_order_acquire)) throw IndexWriterClosedError("index writer has failed");
  // TODO: if nothing actually changed, we could skip writing a new commit at this point.
  // Not for a commit that waits for replicas: that wait relies on a generation
  // newer than any follower has acknowledged, even after a follower restart.

  // Pin the application snapshot through publication. A concurrent merge
  // diffs source liveDocs before swapping; applications finishing after that
  // swap forward their packets to its output for the next application.
  DeleteApplication app(*this);
  {
    const std::lock_guard<std::mutex> lock(indexMutex);
    // A commit applies every pending batch and carried packet. publishOnly
    // commits retire nothing; their batches stay pending.
    for (const auto& batch : pendingDeletes.deletes) app.select(batch, true);
    for (const auto& [_, seg] : segInfos) {
      for (const auto& packet : seg->personalDeletes.deletes) app.select(packet, false);
    }
    app.retireThrough = msg.publishOnly ? 0 : msg.updateVersion;
    msg.commitInfo->highestUpdateVersion =
      std::max({durableUpdateVersion, preparedDeleteVersion, msg.updateVersion, app.pending.getLargestVersion()});
    pinSnapshotLocked(app);
  }

  // If a segmentMerge kicks off here, it could be merging segments without deletes applied yet.
  // We check for that after we finish applying deletes.
  INDEX_DEBUG("finishCommitBody: msg={} applying deletes to {} of {} segments.", (void*)&msg,
              app.targets.size(), app.segs.size());
  applyDeletes(app);
  reconcileDeletes(app);

  // Collect filenames that need to be fsynced before the commit point.
  std::vector<std::string> filesToSync;

  CommitInfo& commitInfo = *msg.commitInfo;
  // Check if any of the segments are now empty and remove them from the list.
  std::vector<SegInfo*> segsToKeep;
  std::vector<SegInfo*> toDelete;
  {
    const std::lock_guard<std::mutex> lock(indexMutex);
    for (auto* seg : app.segs) {
      if (seg->liveDocs == 0) {
        toDelete.push_back(seg);
      }
      else {
        segsToKeep.push_back(seg);
      }
    }
  }
  // The manifest makes every retained segment durable, including a segment
  // auto-flushed by a later update while this commit was in flight.
  for (auto* seg : segsToKeep) {
    commitInfo.highestUpdateVersion =
      std::max(commitInfo.highestUpdateVersion, seg->maxVersion);
  }
  // Sort segsToKeep by segId now (rather than later in writeIndexInfoFile) so
  // every commit-stage step - buildAuxIndexes, IndexInfo serialization, and
  // future query-time derivation of FAISS-id -> segment mapping - sees the
  // same canonical segment order.
  std::sort(segsToKeep.begin(), segsToKeep.end(),
            [](const SegInfo* a, const SegInfo* b) { return a->segId < b->segId; });

  // Collect unsynced segment data, liveDocs, and overlay files.  Keep them on
  // the segment until sync succeeds so a failed commit (sync failure or a
  // build exception later in this function) can retry durability on a later
  // commit attempt.  applyDeletes above routes liveDocs filenames through
  // seg->unsyncedFiles for exactly this reason.
  for (auto seg : segsToKeep) {
    if (!seg->unsyncedFiles.empty()) {
      filesToSync.insert(filesToSync.end(),
                         seg->unsyncedFiles.begin(), seg->unsyncedFiles.end());
    }
  }

  // Decide physical composition before aux preparation. Snapshot identity and
  // query schema are selected later, under the publication mutex.
  assert(msg.commitInfo);
  {
    const std::lock_guard<std::mutex> lock(indexMutex);
    bool segsChanged = (segsToKeep.size() != lastCommittedSegIds.size());
    if (!segsChanged) {
      boost::unordered_flat_set<uint64_t> oldIds(lastCommittedSegIds.begin(),
                                                 lastCommittedSegIds.end());
      for (auto* s : segsToKeep) {
        if (!oldIds.contains(s->segId)) {
          segsChanged = true;
          break;
        }
      }
    }
    msg.commitInfo->coreGen = coreGen + (segsChanged ? 1 : 0);
    if (segsChanged) {
      INDEX_DEBUG("Segment composition changed, next coreGen={}", msg.commitInfo->coreGen);
    }
  }

  // Build aux indexes if requested.  Vector overlays are segment-local and are
  // carried by segment liveness; index-level aux remains for future non-vector
  // kinds only.
  auto auxIndexInfos = buildAuxIndexes(msg, segsToKeep, filesToSync);
  buildSegmentOverlays(msg, segsToKeep, filesToSync);

  // Make all dependencies durable before a root can reference them. Keep data
  // I/O outside publicationMutex so schema updates can publish independently.
  Signal::emit("commitDataSync", this);
  if (!filesToSync.empty()) {
    dir.sync(filesToSync);
    std::array<std::string, 1> syncDirectory = {"."};
    dir.sync(syncDirectory);
  }
  // Only manifest assembly and publication serialize with schema changes.
  // The schema may have advanced while these segments/overlays were built.
  std::unique_lock publicationLock(publicationMutex);
  CommitId result{incarnation, 0};
  writeIndexInfoFile(segsToKeep, *msg.commitInfo, auxIndexInfos);
  for (auto* seg : segsToKeep) seg->unsyncedFiles.clear();
  result.index_gen = indexGen;
  msg.resultingCommit = std::move(result);
  {
    const std::lock_guard<std::mutex> lock(indexMutex);
    // Retire only batches in this snapshot. Later flushes may have registered
    // more while deletes were applied.
    retirePendingLocked(app);
    // A published application lifts a failed pass's hold.
    deletePassBackoff = {};
    deletePassHeld = false;
  }
  publicationLock.unlock();
  // publishOnly commits publish a merge layout without flushing later-arrived
  // inverter state, so they must not satisfy deferred-commit deadlines.
  if (!msg.publishOnly) deferredCommitPublished(msg.updateVersion);

  // Only move the segment to the delete list after the new IndexInfo file is written.
  // This way it should be safe for other threads to also try deletions.
  // NOTE: we did have a call to tryDeleteSegments() from the merge code as well, but
  // there was a race condition: writeIndexInfoFile() was updating the commitTime of
  // the segments before updating lastCommitTime, so a segment could be deleted in that period.
  // Preparation pins now protect that interval; cleanup still follows publication.
  if (!toDelete.empty()) {
    const std::lock_guard<std::mutex> lock(indexMutex);
    for (auto& seg : toDelete) {
      moveSegmentToDelete(seg->segId);
      seg->lastCommitTime = 0; // mark as not being part of the last commit so it may be deleted immediately.
    }
  }

  // handling deletions should probably be done asynchronously elsewhere,
  // but we'll just do it here for now.

  // Delete segment files only after the IndexInfo file is written.
  app.unpin();
  tryDeleteSegments();

  if (msg.maxSegments > 0) {
    enqueueForceMerge(msg); // completion ownership transfers here; don't access msg below.
    return false;
  }
  return true;
}

// A delete pass applies and retires the packets every request through the
// frontier has flushed, without flushing, syncing or publishing; the next
// commit publishes what it prepared. Capture, application and merge
// reconciliation are a commit's.
void IndexWriter::deletePassBody() {
  if (failed.load(std::memory_order_acquire)) throw IndexWriterClosedError("index writer has failed");
  auto start = std::chrono::steady_clock::now();
  DeleteApplication app(*this);
  {
    const std::lock_guard<std::mutex> lock(indexMutex);
    app.retireThrough = deleteFrontierLocked();
    forEachPassPacketLocked(app.retireThrough, [&](const auto& packet, bool fromPending) {
      app.select(packet, fromPending);
      return true;
    });
    // A commit ahead of this message may have retired what triggered it.
    if (app.selected.empty()) return;
    pinSnapshotLocked(app);
    deletePassStats.deletePassCount++;
    for (const auto& packet : app.selected.deletes) deletePassStats.deletePassFacts += deleteFacts(*packet);
    for (const auto& packet : app.pending.deletes) deletePassStats.deletePassNewFacts += deleteFacts(*packet);
  }
  auto timing = scope_guard([&] {
    uint64_t nanos = nanosSince(start);
    const std::lock_guard lock(indexMutex);
    deletePassStats.deletePassNanos += nanos;
    deletePassStats.deletePassMaxNanos = std::max(deletePassStats.deletePassMaxNanos, nanos);
  });
  Signal::emit("deletePassStart", this, (void*)app.retireThrough);
  uint64_t probes = applyDeletes(app);
  reconcileDeletes(app);

  {
    const std::lock_guard lock(indexMutex);
    deletePassStats.deletePassProbes += probes;
    deletePassStats.deletePassMaxProbes = std::max(deletePassStats.deletePassMaxProbes, probes);
    deletePassStats.deletePassRetiredBytes += retirePendingLocked(app);
    // The next publication makes the smaller layout visible.
    for (auto* seg : app.segs) {
      if (seg->liveDocs == 0 && !seg->merging) moveSegmentToDelete(seg->segId);
    }
  }
  app.unpin();
  // Reclaims the generations this pass superseded and the segments it emptied.
  tryDeleteSegments();
}

// Collects seg's obsolete liveDocs generations for retirement, keeping the
// current, published and merge-held ones. No sync list holds the others:
// applyDeletes drops a generation from unsyncedFiles when it supersedes it.
void IndexWriter::collectObsoleteLiveDocsLocked(SegInfo& seg, std::vector<std::string>& names) {
  std::erase_if(seg.obsoleteLiveGens, [&](uint64_t gen) {
    if (gen == seg.liveGen || gen == seg.publishedLiveGen
        || (seg.mergedLiveGen > 0 && gen == (uint64_t)seg.mergedLiveGen)
        || (seg.mergeDiffLiveGen > 0 && gen == (uint64_t)seg.mergeDiffLiveGen)) return false;
    names.push_back(Postings::getLiveDocsFileName(Postings::getSortableString(seg.segId), gen));
    return true;
  });
}

// Attempts to actually remove the files for segments in the deletion list.
// Segments that are being merged will not be deleted.
// Segments that are in the last commit will not be deleted.
void IndexWriter::tryDeleteSegments() {
  try {
    // Delete segment files only after the IndexInfo file is written.
    // Do not delete any segments that are being merged.
    std::vector<std::unique_ptr<SegInfo>> localDeleteList;
    std::vector<std::string> liveDocsToDelete;
    {
      std::lock_guard publicationLock(publicationMutex);
      const std::lock_guard<std::mutex> lock(indexMutex);
      liveDocsToDelete.swap(pendingRetirement);
      localDeleteList = takeDeletableSegmentsLocked();
      for (auto& [id, seg] : segInfos) collectObsoleteLiveDocsLocked(*seg, liveDocsToDelete);
      for (auto& seg : segmentsToDelete) collectObsoleteLiveDocsLocked(*seg, liveDocsToDelete);
    }

    // No publication or index lock is held during filesystem cleanup.
    snapshots.retire(liveDocsToDelete);
    retireSegments(localDeleteList);
  } catch (std::exception& e) {
    LOG_ERROR("Exception caught while deleting segments: {}", e.what());
  }
}

std::vector<std::unique_ptr<SegInfo>> IndexWriter::takeDeletableSegmentsLocked() {
  std::vector<std::unique_ptr<SegInfo>> result;
  result.swap(segmentsToDelete);
  for (auto& seg : result) {
    bool published = std::ranges::find(lastCommittedSegIds, seg->segId) != lastCommittedSegIds.end();
    if (seg->merging || published || seg->preparationPins != 0) {
      segmentsToDelete.push_back(std::move(seg));
    }
  }
  return result;
}

void IndexWriter::retireSegments(std::vector<std::unique_ptr<SegInfo>>& segments) {
  Signal::emit("segmentRetirement", this);
  for (auto& seg : segments) {
    if (!seg) continue;
    INDEX_DEBUG("Deleting files {}", *seg);
    snapshots.retirePrefix(Postings::getIndexFileNamePrefix(seg->segId));
  }
  releaseSegments(segments);
}

// Hands segments to segmentReleaseNode, which drops their readers off the
// caller's path.  Once closed, the graph is draining or drained, so they are
// dropped here.
void IndexWriter::releaseSegments(std::vector<std::unique_ptr<SegInfo>>& segments) {
  if (!closed.load(std::memory_order_acquire)) {
    {
      const std::lock_guard lock(retiredMutex);
      for (auto& seg : segments) {
        if (seg) retiredSegments.push_back(std::move(seg));
      }
    }
    segmentReleaseNode->try_put(tbb::flow::continue_msg{});
  }
  segments.clear();
}

// Move a segment from segInfos to segmentsToDelete for deferred deletion
// Call with indexMutex already locked.
void IndexWriter::moveSegmentToDelete(uint64_t segId) {
  auto it = segInfos.find(segId);
  // It's not an error if not found since someone else may have deleted it already.
  if (it != segInfos.end()) {
    SegInfo* seg = it->second.get();
    INDEX_DEBUG("ToDelete += {}", *seg);
    // Remove from merge policy
    mergePolicy->_remove(seg);
    // Move the segment to deletion list instead of just erasing it
    segmentsToDelete.push_back(std::move(it->second));
    segInfos.erase(it);
  }
}


// Produce the index-level aux list to publish in this commit's IndexInfo.
// Vectors are not built here; they are segment overlays handled by
// buildSegmentOverlays.  The old built_core_gen filter remains only for
// future index-level aux kinds that choose to use it.
std::vector<AuxInfo> IndexWriter::buildAuxIndexes(
    const UpdateMessage& msg,
    std::span<SegInfo*> segsToKeep,
    std::vector<std::string>& outFilesToSync) {
  unused(segsToKeep, outFilesToSync);
  // Physical composition is fixed before aux preparation.
  assert(msg.commitInfo);
  uint64_t newCoreGen = msg.commitInfo->coreGen;

  // Step 1: filter previous list by core gen.  Build a set of "still valid"
  // names (segment-dependent entries whose built_core_gen matches the new
  // core gen) - rebuilding those would produce identical output, so we tell
  // the builder to skip them.
  std::lock_guard lock(indexMutex);
  std::vector<AuxInfo> carried;
  carried.reserve(currentAuxIndexes_.size());
  for (const auto& prev : currentAuxIndexes_) {
    if (prev.kind == VectorIndexBuilder::KIND) {
      continue;
    }
    if (prev.built_core_gen == 0 || prev.built_core_gen == newCoreGen) {
      carried.push_back(prev);
    }
  }

  return carried;
}

namespace {

std::vector<std::string> collectVectorSelectors(const std::vector<std::string>& buildAuxIndexes) {
  std::vector<std::string> selectors;
  boost::unordered_flat_set<std::string> seen;
  for (const auto& selector : buildAuxIndexes) {
    if (selector == "*" || selector.starts_with(VectorIndexBuilder::NAME_PREFIX)) {
      if (seen.emplace(selector).second) {
        selectors.push_back(selector);
      }
    }
  }
  return selectors;
}

bool eligibleVectorOverlayName(const Schema& schema, std::string_view overlayName) {
  if (!overlayName.starts_with(VectorIndexBuilder::NAME_PREFIX)) return false;
  overlayName.remove_prefix(VectorIndexBuilder::NAME_PREFIX.size());
  auto* ft = schema.getFieldTypePtr(overlayName);
  if (ft == nullptr || ft->type() != FieldType::VECTOR) return false;
  auto* vft = (const VectorFieldType*)ft;
  return vft->knnSearchable();
}

std::vector<std::string> collectValidatedExactVectorOverlayNames(
    const std::vector<std::string>& buildAuxIndexes,
    const Schema& schema) {
  std::vector<std::string> names;
  boost::unordered_flat_set<std::string> seen;
  for (const auto& selector : buildAuxIndexes) {
    if (selector.starts_with(VectorIndexBuilder::NAME_PREFIX)) {
      if (!seen.emplace(selector).second) continue;
      if (eligibleVectorOverlayName(schema, selector)) {
        names.push_back(selector);
      } else {
        LOG_WARN("Ignoring vector aux selector {} because it does not resolve to an eligible vector field",
                 selector);
      }
    }
  }
  return names;
}

} // namespace

// Benign load-then-store race: the commit thread and a merge thread can both
// miss and each create a PostingsReader for a source segment - both are valid,
// one is wasted, last store wins.  Deliberately NOT a lock or CAS; do not
// "fix" this into synchronization.
//
std::shared_ptr<PostingsReader> IndexWriter::getSegmentPostingsReader(SegInfo& seg) {
  auto pr = seg.sharedPostingsReader.load();
  if (!pr) {
    std::shared_ptr<IndexReader> reader;
    if (const auto* published = publishedSegment(seg, reader)) pr = published->postingsReaderShared();
    if (!pr) pr = std::make_shared<PostingsReader>(dir, seg.segId);
    seg.sharedPostingsReader.store(pr);
  }
  bool rangeKnown = seg.idRange.load() != nullptr;
  if (!rangeKnown || seg.idDictionaryBlocks.load() == UINT64_MAX) {
    IdRange range;
    uint64_t blocks = 0;
    if (auto* info = pr->fieldInfo("id"); info && info->nTerms > 0) {
      auto guard = MemPool::threadLocalPoolGuard();
      TermsEnum terms(guard.pool(), *pr, *info);
      blocks = terms.numBlocks();
      if (!rangeKnown) {
        terms.seekOrd(0);
        range.min = (std::string_view)terms.term();
        terms.seekOrd(info->nTerms - 1);
        range.max = (std::string_view)terms.term();
        range.empty = false;
      }
    }
    seg.idDictionaryBlocks.store(blocks);
    // A populated range is never replaced.
    if (!rangeKnown) seg.idRange.store(std::make_shared<const IdRange>(std::move(range)));
  }
  return pr;
}

// The published reader's segment of the same id is the same physical segment:
// segment ids are never reused within an incarnation (lastSegId persists in
// the manifest, and rewinding it starts a new incarnation and a new reader
// lineage). The reader may lag this writer, never lead it.
const Segment* IndexWriter::publishedSegment(const SegInfo& seg, std::shared_ptr<IndexReader>& reader) {
  reader = snapshots.readers.indexReader.load(std::memory_order_acquire);
  if (!reader || reader->incarnation() != lastSnapshot.load()->id.incarnation) return nullptr;
  const auto* segment = reader->findSegment(seg.segId);
  return segment != nullptr && segment->maxDoc() == seg.maxDoc ? segment : nullptr;
}

void IndexWriter::seedActiveVectorOverlayNamesFromManifestLocked() {
  activeVectorOverlayNames.clear();
  for (const auto& published : currentSegmentOverlays_) {
    if (published.info.kind == VectorIndexBuilder::KIND) {
      activeVectorOverlayNames.emplace(published.info.name);
    }
  }
}

// Exact vector names are activated only after schema validation.  That
// validation gate is the "pass validation" part of the process-local intent
// contract; "*" still activates only concrete names found on eligible segment
// fields below.
void IndexWriter::activateVectorOverlayNames(std::span<const std::string> names) {
  if (names.empty()) return;
  std::lock_guard<std::mutex> lock(indexMutex);
  for (const auto& name : names) {
    activeVectorOverlayNames.emplace(name);
  }
}

std::vector<std::string> IndexWriter::snapshotActiveVectorOverlayNames() {
  std::vector<std::string> names;
  {
    std::lock_guard<std::mutex> lock(indexMutex);
    names.reserve(activeVectorOverlayNames.size());
    for (const auto& name : activeVectorOverlayNames) {
      names.push_back(name);
    }
  }
  std::sort(names.begin(), names.end());
  return names;
}

uint64_t IndexWriter::nextSegmentOverlayGen(SegInfo& seg) {
  std::lock_guard lock(indexMutex);
  return seg.nextOverlayGen++;
}

std::vector<AuxInfo> IndexWriter::buildConcreteVectorOverlays(
    SegInfo& seg,
    PostingsReader& postingsReader,
    std::span<const std::string> overlayNames,
    const Schema& schema,
    VectorIndexBuilder::BuildSite buildSite,
    std::vector<std::string>& outFiles) {
  std::vector<AuxInfo> built;
  if (overlayNames.empty()) return built;

  boost::unordered_flat_set<std::string> existingNames;
  for (const auto& overlay : seg.auxOverlays) {
    if (overlay.kind == VectorIndexBuilder::KIND) {
      existingNames.emplace(overlay.name);
    }
  }

  VectorIndexBuilder::SegInput input{seg.segId, &postingsReader};
  for (const auto& overlayName : overlayNames) {
    if (existingNames.contains(overlayName)) {
      continue;
    }

    uint64_t overlayGen = nextSegmentOverlayGen(seg);
    VectorIndexBuilder vb(dir, std::span<const VectorIndexBuilder::SegInput>(&input, 1),
                          schema, overlayGen);
    std::vector<std::string> oneSelector{overlayName};
    MetricWork metric(*metrics, MetricStage::VECTOR_BUILD);
    auto newlyBuilt = vb.build(oneSelector, existingNames, outFiles, buildSite);
    for (auto& info : newlyBuilt) {
      existingNames.emplace(info.name);
      built.push_back(std::move(info));
    }
  }
  return built;
}

void IndexWriter::deleteStagedOverlayFiles(std::span<const std::string> files,
                                           std::string_view context) noexcept {
  for (const auto& file : files) {
    try {
      dir.deleteFile(file);
    } catch (const std::exception& e) {
      LOG_ERROR("Failed to delete staged vector overlay file {} after {}: {}",
                file, context, e.what());
    } catch (...) {
      LOG_ERROR("Failed to delete staged vector overlay file {} after {}: unknown exception",
                file, context);
    }
  }
}

void IndexWriter::buildSegmentOverlays(const UpdateMessage& msg,
                                       std::span<SegInfo*> segsToKeep,
                                       std::vector<std::string>& outFilesToSync) {
  assert(msg.commitInfo);

  std::vector<std::string> vectorSelectors = collectVectorSelectors(msg.buildAuxIndexes);
  std::shared_ptr<Schema> schema;
  std::vector<std::string> stagedActiveOverlayNames;
  if (!vectorSelectors.empty()) {
    schema = getSchema();
    stagedActiveOverlayNames = collectValidatedExactVectorOverlayNames(msg.buildAuxIndexes, *schema);
  }

  if (segsToKeep.empty()) {
    activateVectorOverlayNames(stagedActiveOverlayNames);
    return;
  }

  auto selectorMatches = [](const std::vector<std::string>& selectors, std::string_view name) {
    for (const auto& s : selectors) {
      if (s == name) return true;
    }
    return false;
  };

  struct StagedOverlay {
    SegInfo* seg;
    AuxInfo info;
  };
  std::vector<StagedOverlay> stagedOverlays;
  std::vector<std::string> stagedFiles;

  try {
    if (TestOverlayAuxReader::enabledForTests
        && selectorMatches(msg.buildAuxIndexes, TestOverlayAuxReader::NAME)) {
      for (size_t i = 0; i < segsToKeep.size(); i++) {
        auto* seg = segsToKeep[i];
        bool exists = false;
        for (const auto& overlay : seg->auxOverlays) {
          if (overlay.kind == TestOverlayAuxReader::KIND
              && overlay.name == TestOverlayAuxReader::NAME) {
            exists = true;
            break;
          }
        }
        if (exists) continue;

        auto gen = nextSegmentOverlayGen(*seg);
        std::string fileName = Postings::getSegmentOverlayFileName(
          seg->segId, TestOverlayAuxReader::NAME, gen, 0);
        FileDescriptor descriptor;
        {
          auto file = dir.createFile(fileName);
          OutputStream os;
          os.setFile(&*file);
          static constexpr std::string_view payload = "luxir test overlay\n";
          os.write(payload.data(), payload.size());
          os.close();
          dir.finishFile(*file);
          descriptor = file->descriptor();
        }
        stagedFiles.push_back(fileName);

        AuxInfo info;
        info.kind = std::string(TestOverlayAuxReader::KIND);
        info.name = std::string(TestOverlayAuxReader::NAME);
        info.gen = gen;
        info.files.push_back(std::move(descriptor));
        static constexpr std::string_view testMeta = "test";
        info.opaque_meta.assign((const std::byte*)testMeta.data(),
                                (const std::byte*)testMeta.data() + testMeta.size());
        stagedOverlays.push_back({seg, std::move(info)});
      }
    }

    if (!vectorSelectors.empty() && schema) {
      for (size_t i = 0; i < segsToKeep.size(); i++) {
        auto* seg = segsToKeep[i];
        auto pr = getSegmentPostingsReader(*seg);
        VectorIndexBuilder::SegInput input{seg->segId, pr.get()};
        VectorIndexBuilder matcher(dir, std::span<const VectorIndexBuilder::SegInput>(&input, 1),
                                   *schema, 0);
        auto overlayNames = matcher.matchingOverlayNames(vectorSelectors);
        stagedActiveOverlayNames.insert(stagedActiveOverlayNames.end(),
                                        overlayNames.begin(), overlayNames.end());
        auto built = buildConcreteVectorOverlays(*seg, *pr, overlayNames, *schema,
                                                 VectorIndexBuilder::BuildSite::COMMIT, stagedFiles);
        for (auto& info : built) {
          stagedOverlays.push_back({seg, std::move(info)});
        }
      }
    }
  } catch (...) {
    deleteStagedOverlayFiles(stagedFiles, "segment overlay build failure");
    throw;
  }

  for (auto& file : stagedFiles) {
    outFilesToSync.push_back(file);
  }
  activateVectorOverlayNames(stagedActiveOverlayNames);
  {
    const std::lock_guard<std::mutex> lock(indexMutex);
    for (auto& staged : stagedOverlays) {
      for (const auto& file : staged.info.files) {
        staged.seg->unsyncedFiles.push_back(file.name);
      }
      staged.seg->auxOverlays.push_back(std::move(staged.info));
    }
  }
}

// Caller holds publicationMutex (or is constructing this writer).
void IndexWriter::publish(api::IndexInfo& info, std::pmr::memory_resource& arena, std::shared_ptr<Schema> schema) {
  if (failed.load(std::memory_order_acquire)) throw IndexWriterClosedError("index writer has failed");
  auto previous = lastSnapshot.load();
  auto previousSchema = previous ? previous->schema : nullptr;
  schema = schema ? schema->withHistory(previousSchema ? previousSchema->gen_ + 1 : 1,
      previousSchema.get()) : previousSchema;
  std::optional<StoredDictionaryRegistry::SchemaState> dictionarySlots;
  if (schema != previousSchema) dictionarySlots = StoredDictionaryRegistry::prepareSchema(*schema);
  info.version = 1;
  info.index_gen = ++nextManifestGen;
  info.schema_gen = schema->gen_;
  info.schema = schema->storedInfo(arena);
  info.incarnation = incarnation;
  info.last_seg_id = lastSegId.load(std::memory_order_relaxed);
  auto now = (uint64_t)std::chrono::duration_cast<std::chrono::microseconds>(
      std::chrono::system_clock::now().time_since_epoch()).count();
  info.commit_time = std::max(now, lastCommitTime.load() + 1);

  auto stampAux = [&](std::span<const api::AuxIndexInfo>& entries) {
    auto previous = entries;
    auto* copies = api::build::allocArray(entries, previous.size(), arena);
    for (size_t i = 0; i < previous.size(); i++) {
      copies[i] = previous[i];
      if (copies[i].commit_time == 0) copies[i].commit_time = info.commit_time;
    }
  };
  auto previousSegments = info.segments;
  auto* segments = api::build::allocArray(info.segments, previousSegments.size(), arena);
  for (size_t i = 0; i < previousSegments.size(); i++) {
    segments[i] = previousSegments[i];
    if (segments[i].commit_time == 0) segments[i].commit_time = info.commit_time;
    stampAux(segments[i].overlays);
  }
  stampAux(info.aux_indexes);

  // Allocate owning published state before writing the candidate. Commit timestamps
  // are assigned once and carried forward in both wire and in-memory metadata.
  std::vector<uint64_t> segmentIds;
  std::vector<AuxInfo> auxIndexes;
  std::vector<PublishedOverlay> overlays;
  for (const auto& segment : info.segments) {
    segmentIds.push_back(segment.seg_id);
    for (const auto& overlay : segment.overlays) overlays.push_back({segment.seg_id, fromWire(overlay)});
  }
  for (const auto& aux : info.aux_indexes) auxIndexes.push_back(fromWire(aux));
  std::vector<std::byte> serialized;
  if (!api::encode(info, serialized)) throw std::runtime_error("Failed to serialize IndexInfo");
  auto digest = CommitSnapshot::digestOf(serialized);

  // The segments this writer owns, current or awaiting retirement, and the
  // ones of this snapshot it holds open, for the reader that opens it.
  boost::unordered_flat_map<uint64_t, SegInfo*> ownedSegments;
  std::vector<std::weak_ptr<PostingsReader>> openReaders(info.segments.size());
  {
    std::lock_guard lock(indexMutex);
    for (const auto& [id, seg] : segInfos) ownedSegments.emplace(id, seg.get());
    for (const auto& seg : segmentsToDelete) ownedSegments.emplace(seg->segId, seg.get());
    for (size_t i = 0; i < info.segments.size(); i++) {
      auto owned = ownedSegments.find(info.segments[i].seg_id);
      if (owned != ownedSegments.end()) openReaders[i] = owned->second->sharedPostingsReader.load();
    }
  }
  auto snapshot = std::make_shared<const CommitSnapshot>(
      std::make_shared<const std::vector<std::byte>>(std::move(serialized)), schema,
      CommitId{incarnation, info.index_gen}, info.commit_time, filesOf(info), CommitSnapshot::populatedOf(info), digest,
      std::move(openReaders));
  std::pmr::monotonic_buffer_resource retirementArena;
  api::IndexInfo obsolete;
  if (previous) obsolete = Manifest::decode(previous->bytes, retirementArena);
  boost::unordered_flat_set<std::string> retainedFiles;
  if (previous) {
    // A merge may have captured an older liveGen without opening it yet.
    // Segment retirement handles data and liveDocs separately, preserving only
    // published/current liveDocs and the generation a merge actually captured.
    for (const auto& segment : obsolete.segments) {
      if (ownedSegments.contains(segment.seg_id)) for (const auto& file : segment.files) retainedFiles.emplace(file.name);
    }
  }
  manifestNames.push_back(Manifest::name(info.index_gen));
  auto failure = std::make_shared<const std::string>("snapshot publication failed");
  bool durable = false;
  try {
    Manifest::commit(dir, info.index_gen, *snapshot->bytes, this);
    durable = true;
    Signal::emit("manifestDurable", this);
    {
      std::lock_guard lock(indexMutex);
      for (const auto& segment : info.segments) {
        SegInfo* seg = nullptr;
        if (auto it = segInfos.find(segment.seg_id); it != segInfos.end()) {
          seg = it->second.get();
        } else {
          // A concurrent merge can move a still-published source to retirement.
          auto retired = std::ranges::find_if(segmentsToDelete,
              [&](const auto& candidate) { return candidate->segId == segment.seg_id; });
          if (retired != segmentsToDelete.end()) seg = retired->get();
        }
        if (!seg) continue; // initial/reopen publication has no mutable segment state
        seg->lastCommitTime = info.commit_time;
        seg->firstCommitTime = segment.commit_time;
        seg->publishedLiveGen = segment.live_gen;
        for (const auto& overlay : segment.overlays) {
          auto owning = std::ranges::find_if(seg->auxOverlays, [&](const auto& candidate) {
            return candidate.name == overlay.name && candidate.gen == overlay.gen;
          });
          if (owning != seg->auxOverlays.end()) owning->commit_time = overlay.commit_time;
        }
      }
      indexGen = info.index_gen;
      coreGen = info.core_gen;
      schemaGen_ = info.schema_gen;
      durableUpdateVersion = info.update_version;
      lastCommittedSegIds.swap(segmentIds);
      currentAuxIndexes_.swap(auxIndexes);
      currentSegmentOverlays_.swap(overlays);
      lastCommitTime = info.commit_time;
      commitCount.fetch_add(1, std::memory_order_relaxed);
    }
    // The remaining stores/notification do not throw. Advance registry formats
    // first, so observing the new schema cannot admit an old-format CAS.
    if (dictionarySlots) storedDictionaries.setSchema(std::move(*dictionarySlots));
    lastSnapshot.store(snapshot);
    snapshots.publish(snapshot);
  } catch (...) {
    if (durable) {
      failureReason = std::move(failure);
      failed.store(true, std::memory_order_release);
      snapshots.fail();
    } else {
      // A failed candidate can be complete but unacknowledged. Its dependencies
      // are durable, so even a crash during cleanup is recoverable. Never reuse
      // its generation; a later successful publication also cleans its names.
      try {
        dir.deleteFile(Manifest::name(info.index_gen));
        dir.deleteFile(Manifest::name(info.index_gen) + ".tmp");
        std::array<std::string, 1> syncDirectory = {"."};
        dir.sync(syncDirectory);
      } catch (const std::exception& e) {
        LOG_WARN("Failed snapshot cleanup: {}", e.what());
      } catch (...) {
        LOG_WARN("Failed snapshot cleanup: unknown exception");
      }
    }
    throw;
  }
  // Retirement is best-effort after durability. Reappearing obsolete names
  // after a crash are harmless; the newest root never references them.
  try {
    if (previous) {
      auto names = CommitSnapshotRegistry::obsoleteFiles(*previous, *snapshot, std::move(retainedFiles));
      pendingRetirement.insert(pendingRetirement.end(),
          std::make_move_iterator(names.begin()), std::make_move_iterator(names.end()));
    }
    std::erase_if(manifestNames, [&](const auto& name) {
      auto gen = Manifest::generationOf(name);
      if (gen == info.index_gen) return false;
      pendingRetirement.push_back(name);
      return true;
    });
  } catch (const std::exception& e) {
    LOG_WARN("Snapshot cleanup failed: {}", e.what());
  } catch (...) {
    LOG_WARN("Snapshot cleanup failed: unknown exception");
  }
}

uint64_t IndexWriter::writeIndexInfoFile(std::span<SegInfo*> segs, CommitInfo& commitInfo,
                                         std::span<AuxInfo> auxIndexes) {
  assert(std::is_sorted(segs.begin(), segs.end(),
                        [](const SegInfo* a, const SegInfo* b) { return a->segId < b->segId; }));
  assert(commitInfo.highestUpdateVersion >= commitInfo.updateMessage->updateVersion);
  std::pmr::monotonic_buffer_resource arena;
  api::IndexInfo info;
  info.update_version = commitInfo.highestUpdateVersion;
  info.core_gen = commitInfo.coreGen;
  auto* segments = api::build::allocArray(info.segments, segs.size(), arena);
  // publish() owns the publication timestamp. New physical objects use the
  // same timestamp, patched there before encoding.
  for (size_t i = 0; i < segs.size(); i++) {
    const auto& seg = *segs[i];
    auto& wire = segments[i];
    wire.seg_id = seg.segId;
    wire.max_doc = seg.maxDoc;
    wire.live_gen = seg.liveGen;
    wire.min_version = seg.minVersion;
    wire.max_version = seg.maxVersion;
    wire.commit_time = seg.firstCommitTime;
    wire.live_docs = seg.liveDocs;
    wire.schema_gen = seg.schemaGen;
    wire.next_overlay_gen = seg.nextOverlayGen;
    wire.files = toWire(seg.files, arena);
    auto* overlays = api::build::allocArray(wire.overlays, seg.auxOverlays.size(), arena);
    for (size_t j = 0; j < seg.auxOverlays.size(); j++) overlays[j] = toWire(seg.auxOverlays[j], arena);
  }
  auto* aux = api::build::allocArray(info.aux_indexes, auxIndexes.size(), arena);
  for (size_t i = 0; i < auxIndexes.size(); i++) aux[i] = toWire(auxIndexes[i], arena);
  publish(info, arena);
  return info.commit_time;
}

bool IndexWriter::finishMergeTail(bool allowSyntheticCommit, bool chainNextMerge) {
  bool triggerCommit = false;
  {
    const std::lock_guard<std::mutex> lock(indexMutex);

    // Tail ordering is intentional.  Remove this merge from the outstanding
    // count first, let a policy chain submit (and add its waiter holds), then
    // remove this merge's waiter holds, and only then release zero-count
    // waiters.
    assert(mergePolicy->outstandingMerges > 0);
    mergePolicy->outstandingMerges--;

    // On a contained merge FAILURE the caller passes chainNextMerge=false: the
    // sources were restored unchanged, so _maybeMergeSegments would immediately
    // re-select the same set and spin the single-concurrency merge node.  The
    // next segment flush re-triggers _maybeMergeSegments naturally
    // (event-paced), so a transient failure still self-heals without a busy
    // loop.  A cause-aware retry/quarantine policy is future work.
    bool anotherMerge = false;
    if (chainNextMerge) {
      try {
        anotherMerge = mergePolicy->_maybeMergeSegments(nullptr);
      } catch (const std::exception& e) {
        LOG_ERROR("Policy merge chaining failed: exception={}", e.what());
      } catch (...) {
        LOG_ERROR("Policy merge chaining failed: unknown exception");
      }
    }

    _releaseMergeWaiterHoldsLocked();

    if (allowSyntheticCommit && !anotherMerge && mergePolicy->outstandingMerges == 0
        && waitingForMerges.empty()) {
      if (busyInverters.empty() && flushingInverters.empty() && idleInverters.empty()) {
        // no indexing activity, so let's trigger a commit.
        triggerCommit = true;
      }
    }
  }
  return triggerCommit;
}


// called from the mergeSegmentsNode which has concurrency==1 (single-threaded)
// Only one merge will be running at a time.
// We do run concurrently with everything else, including commits and segment deletions.
// So we don't delete segments that the commit code is about to use, the commit code marks
// those segments.
// We also mark segments that are going to be merged, so the commit code knows about them.
bool IndexWriter::mergeSegmentsBody(MergeMessage& msg) {
  // Started once there are sources to merge (or on a failure), so a pass that
  // finds nothing to merge does not count as a merge.
  std::optional<MetricWork> metric;
  auto metricFailed = [&] {
    if (!metric) metric.emplace(*metrics, MetricStage::MERGE);
    metric->failed = true;
  };
  std::vector<SegInfo*> segs;
  std::vector<uint64_t> sourceSegIds;
  bool clientOrigin = msg.forcedBy != nullptr;
  bool allowSingleSourceRewrite = false;
  bool allowSyntheticCommit = false;
  bool chainNextMerge = false;
  bool outputPublished = false;
  bool mergeFailed = false;
  size_t markedSegmentCount = 0;
  uint64_t outputSegId = 0;
  MergeFailureInfo failure;
  const char* phase = "select_sources";

  auto sourceSegIdsString = [&]() {
    std::string out;
    for (size_t i = 0; i < sourceSegIds.size(); i++) {
      if (i > 0) out += ',';
      out += std::to_string(sourceSegIds[i]);
    }
    return out;
  };

  try {

    // We grab the list of segments to merge with the lock held, but use them outside of the lock.
    // This is safe since the only place where segments are deleted is in a merge, and this has concurrency==1
    {
      const std::lock_guard<std::mutex> lock(indexMutex);
      if (clientOrigin) {
        assert(msg.maxSegments > 0);
        segs.reserve(segInfos.size());
        for (auto& [segId, seg] : segInfos) {
          unused(segId);
          if (seg->liveDocs == 0) {
            continue;
          }
          segs.push_back(seg.get());
        }

        size_t target = (size_t)msg.maxSegments;
        if (segs.size() > target) {
          std::sort(segs.begin(), segs.end(), [](const SegInfo* a, const SegInfo* b) {
            if (a->liveDocs != b->liveDocs) {
              return a->liveDocs < b->liveDocs;
            }
            return a->segId < b->segId;
          });
          segs.resize(segs.size() - target + 1);
        } else if (target == 1 && segs.size() == 1
                   && segs.front()->liveDocs < segs.front()->maxDoc) {
          allowSingleSourceRewrite = true;
        } else {
          segs.clear();
        }
      } else {
        segs.reserve(mergePolicy->mergeFactor * 2);
        for (auto& [segId, seg] : segInfos) {
          unused(segId);
          if (seg->mergeLevel == msg.mergeLevel) {
            if (seg->liveDocs == 0) {
              // This can occur after deletes are applied but before commit-time removal.
              continue;
            }
            segs.push_back(seg.get());
          }
        }
      }

      // Segment-local ords are signed int32. Admit only sources whose summed
      // per-field term dictionaries remain below the flush-side limit. Text
      // dictionaries are exempt because they do not produce an ord column.
      // Doc ids carry the same shape of limit: the merged segment's maxDoc
      // (the live docs the merger copies) must stay below MAX_SEGMENT_DOCS.
      int64_t mergedDocs = 0;
      boost::unordered_flat_map<std::string, int64_t> ordTermSums;
      std::vector<SegInfo*> admitted;
      admitted.reserve(segs.size());
      for (SegInfo* seg : segs) {
        if (!MergeCostModel::docsFit(mergedDocs, seg->liveDocs)) {
          INDEX_DEBUG("mergeSegmentsBody: skipping segment {} because the merged segment would exceed MAX_SEGMENT_DOCS",
                      seg->segId);
          continue;
        }
        auto postingsReader = getSegmentPostingsReader(*seg);
        MemPool metadataPool;
        FieldReader fields(*postingsReader);
        boost::unordered_flat_map<std::string, int64_t> additions;
        bool fits = true;
        while (fields.readNextField()) {
          SegFieldInfo info;
          fields.readFieldInfo(info);
          if (info.type != FieldType::STRING ||
              (info.flags & FieldType::INDEX_DOCS) == 0 ||
              (info.flags & FieldType::MULTI_VALUED) == 0) {
            continue;
          }
          std::string name((std::string_view)info.fieldname);
          int64_t next = additions[name] + info.nTerms;
          if (!MergeCostModel::ordTermsFit(ordTermSums[name], next)) {
            fits = false;
            break;
          }
          additions[name] = next;
        }
        if (!fits) {
          INDEX_DEBUG("mergeSegmentsBody: skipping segment {} because a merged ord dictionary would exceed the safety limit",
                      seg->segId);
          continue;
        }
        for (const auto& [field, terms] : additions) ordTermSums[field] += terms;
        mergedDocs += seg->liveDocs;
        admitted.push_back(seg);
      }
      segs.swap(admitted);

      // Selection must be complete before changing segment state.  Marking an
      // unselected candidate would permanently block its deletion.
      for (auto* seg : segs) {
        INDEX_DEBUG("mergeSegmentsBody: will merge {}", *seg);
        seg->merging = true;
        seg->mergedLiveGen = seg->liveGen;
        markedSegmentCount++;
      }

      // Sanity check this merge. a bug in testDeleteAllData led to merge accounting getting out-of-sync
      // with actual segments and resulted in a merge loop.
      mergePolicy->_sanityCheck();
    }

    if (segs.empty()) {
      INDEX_DEBUG("mergeSegmentsBody: no segments to merge for msg={}", (void*)&msg);
      // A client no-op may expose a newly eligible policy merge.  A policy
      // merge that gathered nothing must wait for the next flush instead of
      // immediately reselecting the same empty level.
      chainNextMerge = clientOrigin;
    } else if (segs.size() == 1 && !allowSingleSourceRewrite) {
      // A single-segment gather means the level counts are out of sync with the
      // mergeable segments at this level (e.g. liveDocs==0 segments awaiting their
      // commit-time drop still hold counts).  Merging one segment into an
      // equivalent same-level output makes no progress; chaining on it spins the
      // merge node forever.  Bail out and let the next flush re-trigger merging.
      {
        const std::lock_guard<std::mutex> lock(indexMutex);
        segs[0]->merging = false;
        segs[0]->mergedLiveGen = -1;
        markedSegmentCount = 0;
      }
      INDEX_DEBUG("mergeSegmentsBody: only one mergeable segment at level {}; skipping", msg.mergeLevel);
    } else {
      metric.emplace(*metrics, MetricStage::MERGE);
      sourceSegIds.reserve(segs.size());
      for (auto* seg : segs) {
        sourceSegIds.push_back(seg->segId);
      }

      outputSegId = ++lastSegId;
      failure.sourceSegIds = sourceSegIds;
      failure.outputSegId = outputSegId;
      phase = "merge_start_signal";

      try {
        luxir::Signal::emit("mergeStart", (void*)(int64_t)msg.mergeLevel, (void*)segs.size());

    phase = "sort_sources";
    // Sort the list of segments by the segId.
    // Some tests rely on not reordering segments.
    std::sort(segs.begin(), segs.end(), [](const SegInfo* a, const SegInfo* b) {
      return a->segId < b->segId;
    });

    phase = "open_source_readers";
    // grab or open all the postings readers
    std::vector<std::shared_ptr<PostingsReader>> preaders;
    preaders.reserve(segs.size());
    for (auto segInfo : segs) {
      preaders.emplace_back(getSegmentPostingsReader(*segInfo));
    }

    phase = "load_live_docs";
    // Load liveDocs (the version we got a snapshot for) each segment to be merged
    std::vector<std::shared_ptr<LiveDocs>> liveDocsVec;
    std::vector<LiveDocs*> liveDocsPtrs;
    liveDocsVec.reserve(segs.size());
    liveDocsPtrs.reserve(segs.size());

    for (auto segInfo : segs) {
      if (segInfo->mergedLiveGen > 0) {
        // Load liveDocs for this segment
        auto liveDocs = LiveDocs::create(dir, segInfo->segId, segInfo->mergedLiveGen, segInfo->maxDoc);
        if (!liveDocs) {
          throw std::runtime_error("Failed to load liveDocs for segment " + std::to_string(segInfo->segId) +
                                   " gen=" + std::to_string(segInfo->mergedLiveGen));
        }
        liveDocsVec.push_back(liveDocs);
        liveDocsPtrs.push_back(liveDocs.get());
      } else {
        // No deletes for this segment
        liveDocsVec.push_back(nullptr);
        liveDocsPtrs.push_back(nullptr);
      }
    }

    phase = "prepare_merge_inputs";
    // Copy the preaders to a vector of pointers for our underlying merge code.
    std::vector<PostingsReader*> preaderPtrs;
    preaderPtrs.reserve(preaders.size());
    for (auto& preader : preaders) {
      preaderPtrs.push_back(preader.get());
    }

    {
      PostingsWriter pwriter(dir, outputSegId);

      phase = "segment_merge";
      // Do the actual merge.  The current schema chooses each stored-fields
      // resource's output format.
      auto mergeSchema = getSchema();
      SegmentMerger merger(preaderPtrs, liveDocsPtrs, pwriter, *indexRamBudget,
                           termPartitionMinBytes, termPartitionMinRangeBytes,
                           termPartitionMaxRanges, *mergeSchema, storedDictionaries);
      merger.merge();

      phase = "new_segment_info";
      // Create the new SegInfo for the output segment.
      auto newSegInfo = std::make_unique<SegInfo>(pwriter.getSegId(), pwriter.getMaxDoc());
      newSegInfo->minVersion = segs.front()->minVersion;
      newSegInfo->maxVersion = segs.front()->maxVersion;
      newSegInfo->schemaGen = segs.front()->schemaGen;
      for (size_t i = 1; i < segs.size(); i++) {
        newSegInfo->minVersion = std::min(newSegInfo->minVersion, segs[i]->minVersion);
        newSegInfo->maxVersion = std::max(newSegInfo->maxVersion, segs[i]->maxVersion);
        newSegInfo->schemaGen = std::min(newSegInfo->schemaGen, segs[i]->schemaGen);
      }
      phase = "postings_finish";
      pwriter.finish(&newSegInfo->unsyncedFiles, &newSegInfo->files);
      if (pwriter.idRange) newSegInfo->idRange.store(std::make_shared<const IdRange>(std::move(*pwriter.idRange)));
      if (pwriter.idDictionaryBlocks) newSegInfo->idDictionaryBlocks.store(*pwriter.idDictionaryBlocks);

      phase = "active_overlay_snapshot";
      auto activeOverlayNames = snapshotActiveVectorOverlayNames();
      if (!activeOverlayNames.empty()) {
        phase = "schema_snapshot";
        auto schema = getSchema();
        // The merged segment is private until the swap below.  Building here
        // avoids reading live source overlays and publishes segment plus
        // overlay entries atomically at the later commit.
        phase = "merged_postings_reader";
        // Test hook: a listener may throw to exercise containment of a
        // post-merge, pre-swap failure.
        Signal::emit("mergedPostingsReader", newSegInfo.get());
        auto pr = getSegmentPostingsReader(*newSegInfo);
        #ifndef NDEBUG
        {
          std::lock_guard<std::mutex> lock(indexMutex);
          assert(!segInfos.contains(newSegInfo->segId));
        }
        #endif
        std::vector<std::string> stagedOverlayFiles;
        try {
          phase = "merge_vector_overlay";
          auto built = buildConcreteVectorOverlays(*newSegInfo, *pr, activeOverlayNames,
                                                   *schema, VectorIndexBuilder::BuildSite::MERGE,
                                                   stagedOverlayFiles);
          for (auto& file : stagedOverlayFiles) {
            newSegInfo->unsyncedFiles.push_back(file);
          }
          for (auto& info : built) {
            newSegInfo->auxOverlays.push_back(std::move(info));
          }
        } catch (const std::exception& e) {
          deleteStagedOverlayFiles(stagedOverlayFiles, "merge vector overlay build failure");
          LOG_ERROR("Merge vector overlay build failed for seg={}; publishing flat fallback: {}",
                    newSegInfo->segId, e.what());
        } catch (...) {
          deleteStagedOverlayFiles(stagedOverlayFiles, "merge vector overlay build failure");
          LOG_ERROR("Merge vector overlay build failed for seg={}; publishing flat fallback: unknown exception",
                    newSegInfo->segId);
        }
        phase = "after_merge_vector_overlay";
      }

      std::vector<uint64_t> coveredGens;
      coveredGens.reserve(segs.size());
      for (auto* seg : segs) coveredGens.push_back(seg->mergedLiveGen);
      auto diffGuard = indexRamBudget->forceAcquire(0);
      std::unique_ptr<screaming::RAMFixedBitSet> outputLiveBits;
      for (;;) {
        std::vector<uint64_t> currentGens;
        currentGens.reserve(segs.size());
        {
          const std::lock_guard<std::mutex> lock(indexMutex);
          for (auto* seg : segs) {
            currentGens.push_back(seg->liveGen);
            // Pin the generation until it has been loaded outside the lock.
            seg->mergeDiffLiveGen = seg->liveGen;
          }
        }
        phase = "merge_live_docs_diff";
        bool changed = false;
        for (size_t i = 0; i < segs.size(); i++) {
          if (currentGens[i] == coveredGens[i]) continue;
          auto* seg = segs[i];
          // Rescan from the merged generation on every retry. Clearing an
          // already-clear output bit is a no-op, so only the generation the
          // diff covered needs to be remembered.
          auto current = LiveDocs::create(dir, seg->segId, currentGens[i], seg->maxDoc, false);
          const auto* merged = liveDocsVec[i] ? &liveDocsVec[i]->bitset() : nullptr;
          const auto& now = current->bitset();
          for (int32_t word = 0; word < (int32_t)screaming::FixedBitSet::sizeInWords(seg->maxDoc); word++) {
            uint64_t removed = (merged ? merged->words[word] : ~uint64_t(0)) & ~now.words[word];
            while (removed != 0) {
              int32_t doc = word * 64 + (int32_t)std::countr_zero(removed);
              removed &= removed - 1;
              if (doc >= seg->maxDoc) break;
              int32_t mapped = merger.mapDocId(i, doc);
              assert(mapped >= 0);
              if (!outputLiveBits) {
                diffGuard.forceResize(screaming::FixedBitSet::sizeInWords(newSegInfo->maxDoc) * sizeof(uint64_t));
                outputLiveBits = std::make_unique<screaming::RAMFixedBitSet>(newSegInfo->maxDoc, true);
              }
              if (!outputLiveBits->get(mapped)) continue;
              outputLiveBits->clear(mapped);
              newSegInfo->liveDocs--;
              changed = true;
            }
          }
          coveredGens[i] = currentGens[i];
        }
        if (changed) {
          // The output is private. Replace its first generation on a retry;
          // no reader or manifest can observe an intermediate bitmap.
          newSegInfo->unsyncedFiles.reserve(newSegInfo->unsyncedFiles.size() + 1);
          newSegInfo->files.reserve(newSegInfo->files.size() + 1);
          auto name = Postings::getLiveDocsFileName(Postings::getSortableString(outputSegId), 1);
          std::erase(newSegInfo->unsyncedFiles, name);
          std::erase_if(newSegInfo->files, [&](const auto& file) { return file.name == name; });
          liveDocsBytesWritten += LiveDocsWriter::writeLiveDocs(dir, outputSegId, 1, *outputLiveBits,
                                                                newSegInfo->maxDoc, newSegInfo->liveDocs,
                                                                newSegInfo->unsyncedFiles, &newSegInfo->files);
          newSegInfo->liveGen = 1;
        }
        Signal::emit("mergeLiveDocsDiff", newSegInfo.get());
        phase = "publish_swap_prepare";
        const std::lock_guard<std::mutex> lock(indexMutex);
        bool retry = false;
        for (size_t i = 0; i < segs.size(); i++) {
          if (segs[i]->liveGen != coveredGens[i]) retry = true;
        }
        if (retry) continue;
        segmentsToDelete.reserve(segmentsToDelete.size() + segs.size());
        if (newSegInfo->liveDocs > 0) {
          segInfos.reserve(segInfos.size() + 1);
          mergePolicy->_prepareUpdate(newSegInfo.get());
        }

        size_t personalDeleteCount = newSegInfo->personalDeletes.size();
        for (auto segInfo : segs) {
          personalDeleteCount += segInfo->personalDeletes.size();
        }
        newSegInfo->personalDeletes.deletes.reserve(personalDeleteCount);

        phase = "publish_swap";
        for (size_t i = 0; i < segs.size(); i++) {
          auto* segInfo = segs[i];
          segInfo->mergedLiveGen = coveredGens[i];
          segInfo->mergeDiffLiveGen = -1;
          segInfo->merging = false; // mark as no longer merging so it can be removed.
          segInfo->mergedIntoSegId = newSegInfo->segId; // mark the segment as merged into the new segment

          // If the segment had personal deletes, we need to *copy* them to the new segment.
          // The commit code could be in the process of applying deletes to this segment
          // so we don't want to move them.
          // The new segment isn't in the segInfos map yet, so this guarantees that
          // The deletes will be applied before the new segment is used in a commit.
          if (!segInfo->personalDeletes.empty()) {
            INDEX_DEBUG("Will merge personal deletes from {} to {}", *segInfo, *newSegInfo);
            newSegInfo->personalDeletes.addAll(segInfo->personalDeletes);
            carriedDeletes = true;
          }

          // remove from the index: move from segInfos to segmentsToDelete
          moveSegmentToDelete(segInfo->segId);
        }

        // add new segInfo to the index if it has any docs.  Mutations to the
        // private auxOverlays and unsyncedFiles above happen before this
        // mutex-protected transfer, so the commit thread observes them after
        // taking indexMutex.
        //
        // Concurrent applications can delete the entire output during the
        // merge. Still publish source removal when no output survives.
        if (newSegInfo->liveDocs > 0) {
          auto* publishedSegInfo = newSegInfo.get();
          auto [iter, success] = segInfos.emplace(pwriter.getSegId(), std::move(newSegInfo));
          unused(iter);
          assert(success);
          outputPublished = true;
          mergePolicy->_update(publishedSegInfo);
        }
        // Keep pinned ancestors pointing at the latest output even if an
        // intermediate output is reclaimed before their application finishes.
        for (auto& retired : segmentsToDelete) {
          auto it = std::lower_bound(segs.begin(), segs.end(), retired->mergedIntoSegId,
                                    [](const SegInfo* seg, uint64_t id) { return seg->segId < id; });
          if (it != segs.end() && (*it)->segId == retired->mergedIntoSegId) {
            retired->mergedIntoSegId = outputSegId;
          }
        }
        break;
      } // end swap retry
      Signal::emit("mergeSwap", (void*)outputSegId);
    }
  } catch (const std::exception& e) {
    mergeFailed = true;
    failure.phase = phase;
    failure.exceptionType = typeid(e).name();
    failure.message = e.what();
    failure.outputPublished = outputPublished;
  } catch (...) {
    mergeFailed = true;
    failure.phase = phase;
    failure.exceptionType = "unknown";
    failure.message = "unknown exception";
    failure.outputPublished = outputPublished;
  }

  if (mergeFailed) {
    metricFailed();
    {
      const std::lock_guard<std::mutex> lock(indexMutex);
      lastMergeFailure = failure;

      if (!outputPublished) {
        for (auto* segInfo : segs) {
          segInfo->merging = false;
          segInfo->mergedLiveGen = -1;
          segInfo->mergeDiffLiveGen = -1;
          segInfo->mergedIntoSegId = 0;

          if (!segInfos.contains(segInfo->segId)) {
            // The source is not in the live set.  The publish swap is
            // effectively noexcept (allocations reserved up front), so a
            // pre-swap failure moved nothing to segmentsToDelete - a source
            // found there was condemned by a CONCURRENT COMMIT that deleted all
            // its docs (liveDocs==0).  Respect that: restore only a source that
            // still has live docs (a genuine partial-swap victim), never
            // resurrect one a commit already emptied - doing so reseeds the
            // level with a zero-live segment that mergeSegmentsBody skips,
            // scheduling endless no-op merges.
            auto it = std::find_if(segmentsToDelete.begin(), segmentsToDelete.end(),
                                   [&](const std::unique_ptr<SegInfo>& candidate) {
                                     return candidate.get() == segInfo;
                                   });
            if (it != segmentsToDelete.end() && (*it)->liveDocs > 0) {
              auto restored = std::move(*it);
              segmentsToDelete.erase(it);
              mergePolicy->_update(restored.get());
              segInfos.emplace(restored->segId, std::move(restored));
            }
          }
        }
      }
    }

    if (!outputPublished) {
      try {
        dir.deletePrefix(Postings::getIndexFileNamePrefix(outputSegId));
      } catch (const std::exception& e) {
        LOG_ERROR("Failed to delete merged segment files for segId={} after merge failure: {}",
                  outputSegId, e.what());
      } catch (...) {
        LOG_ERROR("Failed to delete merged segment files for segId={} after merge failure: unknown exception",
                  outputSegId);
      }
    }

    LOG_ERROR(
      "Merge failed and was contained: sources=[{}] outputSegId={} outputPublished={} phase={} exceptionType={} message={}",
      sourceSegIdsString(), outputSegId, outputPublished, failure.phase,
      failure.exceptionType, failure.message);
    if (clientOrigin) {
      std::string detail = "merge failed during ";
      detail.append(failure.phase);
      detail.append(": ");
      detail.append(failure.message);
      setForceMergeError(*msg.forcedBy, detail);
    }
  } else if (!outputPublished) {
    try {
      dir.deletePrefix(Postings::getIndexFileNamePrefix(outputSegId));
    } catch (const std::exception& e) {
      LOG_ERROR("Failed to delete unused merged segment files for segId={}: {}",
                outputSegId, e.what());
    } catch (...) {
      LOG_ERROR("Failed to delete unused merged segment files for segId={}: unknown exception",
                outputSegId);
    }
  }

      allowSyntheticCommit = !mergeFailed;
      chainNextMerge = !mergeFailed;
    }
  } catch (const std::exception& e) {
    metricFailed();
    LOG_ERROR("mergeSegmentsBody escaped exception: exception={}", e.what());
    if (!outputPublished && markedSegmentCount > 0) {
      const std::lock_guard<std::mutex> lock(indexMutex);
      for (size_t i = 0; i < markedSegmentCount; i++) {
        segs[i]->merging = false;
        segs[i]->mergedLiveGen = -1;
        segs[i]->mergeDiffLiveGen = -1;
        segs[i]->mergedIntoSegId = 0;
      }
    }
    if (clientOrigin) {
      std::string detail = "merge failed during ";
      detail.append(phase);
      detail.append(": ");
      detail.append(e.what());
      setForceMergeError(*msg.forcedBy, detail);
    } else {
      msg.result.setException(e);
    }
    allowSyntheticCommit = outputPublished;
    chainNextMerge = false;
  } catch (...) {
    metricFailed();
    LOG_ERROR("mergeSegmentsBody escaped exception: unknown exception");
    if (!outputPublished && markedSegmentCount > 0) {
      const std::lock_guard<std::mutex> lock(indexMutex);
      for (size_t i = 0; i < markedSegmentCount; i++) {
        segs[i]->merging = false;
        segs[i]->mergedLiveGen = -1;
        segs[i]->mergeDiffLiveGen = -1;
        segs[i]->mergedIntoSegId = 0;
      }
    }
    std::runtime_error error("merge failed: unknown exception");
    if (clientOrigin) {
      setForceMergeError(*msg.forcedBy, error.what());
    } else {
      msg.result.setException(error);
    }
    allowSyntheticCommit = outputPublished;
    chainNextMerge = false;
  }

  // If a future throw site appears after outputPublished is set, the output is
  // already live and the sources are already condemned.  In that case the
  // failure path above skips source/file restoration and this tail may still
  // request the synthetic publish commit.
  if (clientOrigin) {
    finishMergeTail(allowSyntheticCommit, chainNextMerge);
    return submitMergeCommit(msg, /*publishOnly=*/true);
  }
  bool triggerCommit = finishMergeTail(allowSyntheticCommit, chainNextMerge);
  if (triggerCommit) {
    return submitMergeCommit(msg, /*publishOnly=*/false);
  } else {
    INDEX_DEBUG(
      "mergeSegmentsBody: merge done, but not triggering commit since there are busy, flushing, or idle inverters.");
  }
  return true;
}

// Test-only blocking forceMerge(1), implemented through the client commit path.
void IndexWriter::mergeSegments() {
  class BlockingCommitMessage : public UpdateMessage {
  public:
    Blocker blocker;

    void handle(IndexWriter& iw) override {
      unused(iw);
    }

    void done(IndexWriter& iw) override {
      unused(iw);
      blocker.notify();
    }
  };

  BlockingCommitMessage commitMessage;
  commitMessage.commit = UpdateMessage::COMMIT;
  commitMessage.maxSegments = 1;
  if (!submitUpdate(&commitMessage)) {
    throw std::runtime_error("Force-merge commit admission failed");
  }
  commitMessage.blocker.wait();
}






/// TEST CODE (called from tests)
bool IndexWriter::testIsEmpty() {
  const std::lock_guard<std::mutex> lock(indexMutex);
  return segInfos.empty()
         && pendingDeletes.empty()
         && segmentsToDelete.empty()
         && idleInverters.empty()
         && busyInverters.empty()
         && flushingInverters.empty();
}

void IndexWriter::testDeleteAllData() {
  INDEX_DEBUG("testDeleteAllData: deleting all data.");
  // wait for things in the execution graph to finish.
  updateGraph->wait_for_all();
  std::unique_lock publicationLock(publicationMutex);

  {
    const std::lock_guard<std::mutex> lock(indexMutex);
    // check if there are any unflushed segments
    // TODO: just dropping the segments may not be safe in the future, it may leave stuff around in the directory
    // (or even open files in the future).  We should probably do a commit first before we drop?
    if (!busyInverters.empty() || !flushingInverters.empty()) {
      LOG_ERROR("Error trying to clear index. There are busy or flushing inverters!");
      return;
    }
    if (mergePolicy && mergePolicy->outstandingMerges > 0) {
      LOG_ERROR("Error trying to clear index. There is a merge running!");
      return;
    }

    // drop all idle inverters (unflushed segments)
    idleInverters.clear();

    // drop all segments
    segInfos.clear();

    // drop segments to delete
    segmentsToDelete.clear();

    // Revoke snapshot pins before reusing the directory namespace.
    snapshots.testReset();
    pendingRetirement.clear();
    // drop all index files
    dir.clear();

    // Any operation that rewinds seg_id/coreGen/commitTime namespaces MUST
    // swap or epoch the filter cache before reusing those namespaces.
    lastSegId = 0;
    indexGen = 0;
    incarnation = CommitId::newIncarnation();
    coreGen = 0;
    lastCommittedSegIds.clear();
    currentAuxIndexes_.clear();
    currentSegmentOverlays_.clear();
    activeVectorOverlayNames.clear();
    lastMergeFailure.reset();
    pendingDeletes.clear();
    retirableThrough = 0;
    carriedDeletes = false;
    minInverterDocVersion = UINT64_MAX;
    durableUpdateVersion = 0;
    preparedDeleteVersion = 0;
    completedCommitVersion = 0;
    deletePassBackoff = {};
    deletePassHeld = false;
    nextManifestGen = 0;
    manifestNames.clear();

    lastCommitTime = 0;
  }
  storedDictionaries.setSchema({});
  storedDictionaries.setSchema(StoredDictionaryRegistry::prepareSchema(*getSchema()));
  std::pmr::monotonic_buffer_resource arena;
  api::IndexInfo initial;
  publish(initial, arena);
  publicationLock.unlock();
  tryDeleteSegments();
  mergePolicy->refresh(); // we can't call this with lock held since it tries to acquire.




  // Don't touch commitNumber or updateOrdinal: the TBB graph relies on exact session-local ordinals.
}

// TEST HOOKS: safe only against a QUIESCED writer. indexMutex excludes overlay
// publication, but the commit body also reads overlay state outside the lock;
// quiesce the writer in the test rather than racing that read.
bool IndexWriter::testDropSegmentOverlay(std::string_view name, size_t segmentOrd) {
  std::lock_guard<std::mutex> lock(indexMutex);
  if (segmentOrd >= segInfos.size()) return false;

  std::vector<SegInfo*> ordered;
  ordered.reserve(segInfos.size());
  for (auto& [segId, seg] : segInfos) {
    unused(segId);
    ordered.push_back(seg.get());
  }
  std::sort(ordered.begin(), ordered.end(), [](const SegInfo* a, const SegInfo* b) {
    if (a->firstCommitTime != b->firstCommitTime) {
      return a->firstCommitTime < b->firstCommitTime;
    }
    return a->segId < b->segId;
  });

  auto& overlays = ordered[segmentOrd]->auxOverlays;
  auto oldSize = overlays.size();
  std::erase_if(overlays, [&](const AuxInfo& info) {
    return info.name == name;
  });
  if (overlays.size() == oldSize) return false;
  return true;
}

bool IndexWriter::testActiveVectorOverlayName(std::string_view name) {
  std::lock_guard<std::mutex> lock(indexMutex);
  return activeVectorOverlayNames.contains(std::string(name));
}

void IndexWriter::testReseedActiveVectorOverlayNamesFromManifest() {
  std::lock_guard<std::mutex> lock(indexMutex);
  seedActiveVectorOverlayNamesFromManifestLocked();
}

bool IndexWriter::testMergeRunning() {
  std::lock_guard<std::mutex> lock(indexMutex);
  return mergePolicy && mergePolicy->outstandingMerges > 0;
}

std::optional<IndexWriter::MergeFailureInfo> IndexWriter::testLastMergeFailure() {
  std::lock_guard<std::mutex> lock(indexMutex);
  return lastMergeFailure;
}

IndexWriter::Stats IndexWriter::stats(bool includeSegments, bool includeFiles) {
  Stats out;
  std::vector<uint64_t> committedIds;
  std::vector<uint64_t> writerSegmentIds;
  auto copyAux = [](const AuxInfo& src) {
    AuxStats dst;
    dst.kind = src.kind;
    dst.field = src.field;
    dst.name = src.name;
    dst.gen = src.gen;
    dst.commitTime = src.commit_time;
    dst.builtCoreGen = src.built_core_gen;
    for (const auto& file : src.files) dst.files.push_back(file.name);
    return dst;
  };
  {
    const std::lock_guard<std::mutex> lock(indexMutex);
    out.commitTime = lastCommitTime.load(std::memory_order_relaxed);
    out.commits = commitCount.load(std::memory_order_relaxed);
    out.indexGen = indexGen;
    out.coreGen = coreGen;
    out.updateVersion = updateNumber.load(std::memory_order_relaxed);
    out.schemaGen = schemaGen_;
    out.activeMerges = mergePolicy ? (uint64_t)mergePolicy->outstandingMerges : 0;
    static_cast<DeletePassStats&>(out) = deletePassStats;
    out.completedUpdateVersion = completedUpdateVersion;
    out.minInverterDocVersion = minInverterDocVersion;
    uint64_t frontier = deleteFrontierLocked();
    out.liveDocsBytesWritten = liveDocsBytesWritten.load(std::memory_order_relaxed);
    out.pinningInverterFlushes = pinningInverterFlushes;
    out.deleteOnlyKneeFlushes = deleteOnlyKneeFlushes;
    out.deleteOnlyFlushCap = deleteOnlyFlushCapLocked();
    boost::unordered_flat_set<const SortedDeletes*> retained;
    for (const auto& batch : pendingDeletes.deletes) {
      retained.insert(batch.get());
      if (batch->getLargestVersion() <= frontier) out.pendingRetirableDeleteBytes += batch->memSize();
      else out.pendingHeldDeleteBytes += batch->memSize();
    }
    auto countCarry = [&](const MultiDeletesData& deletes) {
      for (const auto& batch : deletes.deletes) {
        if (retained.insert(batch.get()).second) out.mergeCarryDeleteBytes += batch->memSize();
      }
    };
    for (const auto& [_, seg] : segInfos) countCarry(seg->personalDeletes);
    for (const auto& seg : segmentsToDelete) countCarry(seg->personalDeletes);
    out.frontierHeldDeleteBytes = out.pendingHeldDeleteBytes;
    out.retainedDeleteBytes = out.pendingRetirableDeleteBytes + out.pendingHeldDeleteBytes + out.mergeCarryDeleteBytes;
    out.segments = segInfos.size();
    out.flushing = flushingInverters.size();
    out.idle = idleInverters.size();
    out.indexing = busyInverters.size();
    out.auxIndexes.reserve(currentAuxIndexes_.size());
    std::ranges::transform(currentAuxIndexes_, std::back_inserter(out.auxIndexes), copyAux);

    committedIds = lastCommittedSegIds;
    if (!includeSegments) writerSegmentIds.reserve(segInfos.size());
    if (includeSegments) out.segmentStats.reserve(segInfos.size());
    for (const auto& [segId, seg] : segInfos) {
      unused(segId);
      if (!includeSegments) writerSegmentIds.push_back(seg->segId);
      out.maxDocs += (uint64_t)seg->maxDoc;
      out.liveDocs += (uint64_t)seg->liveDocs;
      if (!includeSegments) continue;

      auto& dst = out.segmentStats.emplace_back();
      dst.segId = seg->segId;
      dst.liveGen = seg->liveGen;
      dst.minUpdateVersion = seg->minVersion;
      dst.maxUpdateVersion = seg->maxVersion;
      dst.firstCommitTime = seg->firstCommitTime;
      dst.schemaGen = seg->schemaGen;
      dst.overlays.reserve(seg->auxOverlays.size());
      std::ranges::transform(seg->auxOverlays, std::back_inserter(dst.overlays), copyAux);
      dst.maxDoc = seg->maxDoc;
      dst.liveDocs = seg->liveDocs;
      dst.mergeLevel = std::max(0, seg->mergeLevel);
      dst.merging = seg->merging;
    }
  }

  boost::unordered_flat_set<uint64_t> committedSet(committedIds.begin(), committedIds.end());
  for (uint64_t segId : writerSegmentIds) {
    if (committedSet.contains(segId)) out.committedSegments++;
  }
  for (auto& segment : out.segmentStats) {
    segment.committed = committedSet.contains(segment.segId);
    if (segment.committed) out.committedSegments++;
  }

  // On-disk bytes, from a single directory listing taken outside indexMutex
  // (never hold indexMutex across statx).  A file that disappeared between the
  // segment snapshot above and this listing simply contributes 0.
  std::vector<Directory::FileInfo> files;
  if (includeFiles) dir.listFiles(files);
  for (const auto& file : files) {
    out.totalBytes += file.size;
  }
  auto fileSize = [&files](const std::string& name) -> uint64_t {
    auto it = std::lower_bound(files.begin(), files.end(), name,
                               [](const Directory::FileInfo& f, const std::string& n) { return f.name < n; });
    return (it != files.end() && it->name == name) ? it->size : 0;
  };
  auto fillAuxBytes = [&fileSize](AuxStats& aux) {
    for (const auto& name : aux.files) {
      aux.bytes += fileSize(name);
    }
  };
  for (auto& aux : out.auxIndexes) fillAuxBytes(aux);
  for (auto& segment : out.segmentStats) {
    // Prefix attribution is unambiguous: the length-prefixed base36 segId
    // encoding means no segId string is a prefix of another, and '_' is not
    // an id character.
    auto prefix = Postings::getIndexFileNamePrefix(segment.segId);
    auto it = std::lower_bound(files.begin(), files.end(), prefix,
                               [](const Directory::FileInfo& f, const std::string& p) { return f.name < p; });
    for (; it != files.end() && it->name.starts_with(prefix); ++it) {
      segment.bytes += it->size;
    }
    for (auto& overlay : segment.overlays) fillAuxBytes(overlay);
  }

  snapshots.readers.cacheStats(out);

  std::sort(out.segmentStats.begin(), out.segmentStats.end(),
            [](const SegmentStats& a, const SegmentStats& b) { return a.segId < b.segId; });
  return out;
}

// This is where seg.liveGen can change. A snapshot segment's personal deletes
// stay put meanwhile: a merge attaches them to a new output under indexMutex,
// and the snapshot holds that output whole or not at all. Returns the clipped
// facts applied, summed over segments.
uint64_t IndexWriter::applyDeletes(const DeleteApplication& app) {
  std::atomic<uint64_t> applications = 0;
  // Apply deletes to segments in parallel.  Each task writes new liveDocs
  // filenames into its own segment's unsyncedFiles (single owner per task, no
  // synchronization needed); the commit body drains unsyncedFiles into the
  // pre-manifest fsync set and clears them only after sync succeeds, so
  // liveDocs files get the same retry-on-failed-commit durability as segment
  // data and overlay files.
  // The waiting commit thread must not execute an unrelated merge or flush.
  oneapi::tbb::this_task_arena::isolate([&] {
    oneapi::tbb::task_group tg;
    for (SegInfo* seg : app.targets) {
      tg.run([this, seg, &app, &applications]() {
        // Packets apply span by span, unmerged: a document goes when any fact for
        // its id outranks its birth, which is what a merged maximum would decide.
        boost::container::small_vector<SortedDeletes::EntrySpan, 8> spans;
        auto add = [&](const SortedDeletes& packet) {
          if (packet.sourceSegId == seg->segId || !mayApplyDeletes(*seg, packet)) return;
          for (auto& span : packet.lists()) spans.push_back(span);
        };
        for (const auto& packet : app.pending.deletes) add(*packet);
        for (const auto& packet : seg->personalDeletes.deletes) {
          if (!app.pending.deletes.contains(packet) && app.selected.deletes.contains(packet)) add(*packet);
        }
        applications.fetch_add(applyDeletes(*seg, spans), std::memory_order_relaxed);
      });
    }
    tg.wait();
  });
  return applications.load(std::memory_order_relaxed);
}

uint64_t IndexWriter::applyDeletes(SegInfo& seg, std::span<const SortedDeletes::EntrySpan> spans) {
  if (spans.empty()) {
    return 0;
  }

  // Test hook: reports each segment selected for delete application this commit.
  // Called in parallel for different segments, so consumers should synchronize when applicable.
  luxir::Signal::emit("deleteAppliedToSegment", (void*)(int64_t)seg.segId);

  boost::container::small_vector<SortedDeletes::EntrySpan, 8> clipped;
  auto clip = [&](const IdRange& range) {
    clipped.clear();
    for (auto span : spans) {
      span = clipDeletes(span, range);
      if (!span.empty()) clipped.push_back(span);
    }
  };
  auto range = seg.idRange.load();
  if (range) {
    clip(*range);
    if (clipped.empty()) return 0;
  }

  // Read the segment to find documents that should be deleted
  auto sharedReader = getSegmentPostingsReader(seg);
  PostingsReader& reader = *sharedReader;
  int32_t maxDocId = reader.maxDoc();

  if (!range) {
    clip(*seg.idRange.load());
    if (clipped.empty()) return 0;
  }

  // Load existing LiveDocs if they exist: the published reader's when it
  // holds this generation
  std::shared_ptr<LiveDocs> existingLiveDocs;
  if (seg.liveGen > 0) {
    std::shared_ptr<IndexReader> publishedReader;
    const auto* published = publishedSegment(seg, publishedReader);
    if (published != nullptr && published->segInfo.live_gen == seg.liveGen) {
      existingLiveDocs = published->liveDocsShared();
    }
    if (!existingLiveDocs) existingLiveDocs = LiveDocs::create(dir, seg.segId, seg.liveGen, maxDocId);
  }

  // The new generation's bits: copied from the existing liveDocs at the first
  // delete, or before chunk tasks start so they only clear bits.
  std::unique_ptr<screaming::RAMFixedBitSet> liveBits;
  auto ensureLiveBits = [&] {
    if (liveBits) return;
    liveBits = std::make_unique<screaming::RAMFixedBitSet>(maxDocId, true);
    if (existingLiveDocs) {
      const auto& existingBitset = existingLiveDocs->bitset();
      size_t wordsSize = screaming::FixedBitSet::sizeInWords(maxDocId) * sizeof(uint64_t);
      std::memcpy(liveBits->words, existingBitset.words, wordsSize);
    }
  };

  // currLiveBits is the existing generation, immutable while chunks collect.
  const screaming::FixedBitSet* currLiveBits = existingLiveDocs ? &existingLiveDocs->bitset() : nullptr;
  int32_t numLiveDocs = existingLiveDocs ? existingLiveDocs->numLive() : maxDocId;
  std::atomic<int32_t> newDeletesCount = 0;

  // Read the "id" field using TermsEnum
  auto* idFieldInfo = reader.fieldInfo("id");
  if (!idFieldInfo) {
    LOG_WARN("applyDeletes: segment {} has no 'id' field", seg.segId);
    return 0;
  }

  // Each chunk walks its id-sorted piece with seekForward() and collects the
  // live doc for each hit, then gates hits in docId order against request
  // births and clears them. Hits drain whenever they reach the chunk size, so
  // scratch is bounded by the chunk rather than by overlapping packets or
  // duplicate postings. Zero watermarks are inert.
  struct DeleteCandidate {
    int32_t docId;
    uint64_t deleteVersion;
  };
  const size_t chunk = deleteApplyChunkEntries;
  assert(chunk > 0);
  auto drain = [&](std::vector<DeleteCandidate>& hits, IndexRamBudget::Guard& scratch) {
    if (hits.empty()) return;
    scratch.forceResize((int64_t)(hits.capacity() * sizeof(DeleteCandidate)));
    std::sort(hits.begin(), hits.end(),
              [](const DeleteCandidate& a, const DeleteCandidate& b) { return a.docId < b.docId; });
    VersionRuns::Cursor versions(reader.versionRuns());
    int32_t cleared = 0;
    for (const auto& hit : hits) {
      // A doc goes when any fact outranks its birth; ties survive.
      if (versions.birth(hit.docId) >= hit.deleteVersion) continue;
      ensureLiveBits();
      // A doc can be hit from several chunks or packets (or by several id values);
      // only the clear that finds its bit set counts it.
      std::atomic_ref<uint64_t> word(liveBits->words[hit.docId >> 6]);
      uint64_t mask = (uint64_t)1 << (hit.docId & 63);
      if (word.fetch_and(~mask, std::memory_order_relaxed) & mask) {
        INDEX_TRACE("applyDeletes: marking docId {} as deleted in segment {}", hit.docId, seg.segId);
        cleared++;
      }
    }
    newDeletesCount.fetch_add(cleared, std::memory_order_relaxed);
    hits.clear();
  };
  auto apply = [&](SortedDeletes::EntrySpan span) {
    auto guard = MemPool::threadLocalPoolGuard();
    TermsEnum termsEnum(guard.pool(), reader, *idFieldInfo);
    std::vector<DeleteCandidate> hits;
    auto scratch = indexRamBudget->forceAcquire(0);
    for (auto& entry : span) {
      uint64_t deleteVersion = entry.val().deleteWatermark;
      if (deleteVersion == 0) continue;
      std::string_view deleteId = (std::string_view)entry;

      INDEX_TRACE("applyDeletes: looking up term '{}' with version {} in segment {}",
               deleteId, deleteVersion, seg.segId);

      if (termsEnum.seekForward(deleteId)) {
        // Found the ID term, now get documents containing this ID
        DocsOnlyEnum docsEnum(termsEnum);

        for (int32_t docId = docsEnum.next(); docId != DocsEnumMeta::END; docId = docsEnum.next()) {
          if (currLiveBits && !currLiveBits->get(docId)) {
            continue;
          }
          hits.push_back({docId, deleteVersion});
          if (hits.size() >= chunk) drain(hits, scratch);
        }
      } // end if termsEnum.seek()
    }
    drain(hits, scratch);
  };
  // Split large spans into id-ordered chunks so a few big segments still use every
  // worker; each chunk seeks forward from its own enum.
  size_t applied = 0;
  boost::container::small_vector<SortedDeletes::EntrySpan, 8> pieces;
  for (auto span : clipped) {
    applied += span.size();
    for (size_t at = 0; at < span.size(); at += chunk) {
      pieces.push_back(span.subspan(at, std::min(chunk, span.size() - at)));
    }
  }
  if (pieces.size() == 1) {
    apply(pieces[0]);
  } else {
    ensureLiveBits();
    oneapi::tbb::task_group tg;
    for (auto piece : pieces) {
      tg.run([&, piece] { apply(piece); });
    }
    tg.wait();
  }

  // If we found any new documents to delete, write a new delete generation
  if (int32_t deleted = newDeletesCount.load(std::memory_order_relaxed); deleted > 0) {
    auto newLiveGen = seg.liveGen + 1;
    numLiveDocs -= deleted;

    std::string oldLiveFile = Postings::getLiveDocsFileName(Postings::getSortableString(seg.segId), seg.liveGen);
    liveDocsBytesWritten += LiveDocsWriter::writeLiveDocs(dir, seg.segId, newLiveGen, *liveBits, maxDocId, numLiveDocs,
                                                          seg.unsyncedFiles, &seg.files);
    // The superseded generation needs no sync: it is published or never will be.
    std::erase_if(seg.files, [&](const auto& file) { return file.name == oldLiveFile; });
    std::erase(seg.unsyncedFiles, oldLiveFile);

    INDEX_DEBUG("Applied {} new deletes to segment {} (new delete generation: {}, total live docs: {})",
                deleted, seg.segId, newLiveGen, numLiveDocs);

    {
      const std::lock_guard<std::mutex> lock(indexMutex);
      seg.liveDocs = numLiveDocs;
      assert(seg.liveGen + 1 == newLiveGen);
      if (seg.liveGen > 0) seg.obsoleteLiveGens.push_back(seg.liveGen);
      seg.liveGen = newLiveGen;
    }
  }
  return applied;
}

} // end namespace luxir
