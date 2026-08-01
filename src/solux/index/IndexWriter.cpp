#include "IndexWriter.h"

#include <algorithm>
#include <typeinfo>

#include <boost/sort/spreadsort/string_sort.hpp>
#include <boost/unordered/unordered_flat_set.hpp>
#include <oneapi/tbb/task_group.h>
#include "solux/api/padded_input.h"
#include "solux/store/OutputStream.h"
#include "solux/store/InputStream.h"
#include "LiveDocsWriter.h"
#include "solux/schema/Schema.h"

#include "solux/index/AuxInfo.h"
#include <memory_resource>
#include <google/protobuf/io/coded_stream.h>
#include <google/protobuf/io/zero_copy_stream_impl_lite.h>
#include "solux/util/heap.h"
#include "solux/util/Signal.h"
#include "solux/util/thread.h"
#include "SegmentMerger.h"
#include "VectorIndexBuilder.h"
#include "solux/reader/TestOverlayAuxReader.h"


namespace solux {

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

} // namespace


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
//  - When a segment is flushed, it's deletes are moved to the current CommitInfo.
//  - When a commit happens, the deletes are applied to all segments.
//    - applying deletes to segments doesn't work well with concurrent segment merges.
//      See see finishCommitBody() for how we handle this.

IndexWriter::IndexWriter(Directory& dir, std::function<std::shared_ptr<Schema>()> schemaProvider,
                         IndexRamBudget* sharedIndexRamBudget,
                         FilterCacheConfig filterCacheConfig,
                         int mergeFactor)
  : dir(dir),
    schemaProvider_(std::move(schemaProvider)),
    privateIndexRamBudget(sharedIndexRamBudget == nullptr ? std::make_unique<IndexRamBudget>() : nullptr),
    indexRamBudget(sharedIndexRamBudget == nullptr ? privateIndexRamBudget.get() : sharedIndexRamBudget),
    filterCache(std::make_shared<FilterCache>(filterCacheConfig)),
    originalFilterCacheConfig(filterCacheConfig) {
  mergePolicy = std::make_unique<MergePolicy>(*this); // defer creation until needed?
  mergePolicy->setMergeFactor(mergeFactor);
  nextCommitInfo = std::make_unique<CommitInfo>();
  std::shared_ptr<InputFile> segFile = dir.openFile(Postings::INDEX_INFO_FILE, true);
  if (segFile.get() == nullptr) {
    lastSegId = 0;
    // TODO: verify directory has no other index files? (i.e. this would tend to indicate corruption)
  }
  else {
    InputStream segmentsIs = segFile->getInputStream();

    std::pmr::monotonic_buffer_resource iiArena;  // backs the non-owning IndexInfo view
    solux::api::IndexInfo indexInfo;
    std::span<const std::byte> indexInfoBytes((const std::byte*)segmentsIs.ptr(), segmentsIs.left());
    auto padded = solux::api::copyToPaddedInput(indexInfoBytes, iiArena);
    if (!solux::api::decode(indexInfo, padded, iiArena)) {
      throw std::runtime_error("Failed to parse IndexInfo protobuf");
    }

    lastCommitTime = lastAdvertisedCommitTime = indexInfo.commit_time;
    indexGen = indexInfo.index_gen;
    coreGen = indexInfo.core_gen;
    schemaGen_ = indexInfo.schema_gen;
    updateNumber = indexInfo.update_version;
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
      seg.liveGen = segment.live_gen;
      seg.minVersion = segment.min_version;
      seg.maxVersion = segment.max_version;
      seg.liveDocs = segment.live_docs;
      seg.schemaGen = segment.schema_gen;
      seg.firstCommitTime = segment.commit_time;  // firstCommitTime is stored in the segment meta.
      seg.lastCommitTime = indexInfo.commit_time; // not stored in the segment meta, so use index meta.
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
  {
    std::lock_guard<std::mutex> lock(indexMutex);
    seedActiveVectorOverlayNamesFromManifestLocked();
  }

  // Create the updateGraph.  Start with a serial node that assigns an order to each update command
  // This could be done in a quick synchronized block instead... and could then be safely inspected by the client
  // if necessary before submitting to the actual processing graph.
  startUpdateNode = std::make_unique<UpdateMessageFunc>(updateGraph, 1,
    [this](UpdateMessage* msg) -> UpdateMessage* {
      // exceptions should be impossible here
      this->startUpdateBody(*msg);
      return msg;
    });

  // next, the node to actually process the update, indexing documents, etc.
  processUpdateNode = std::make_unique<UpdateMessageFunc>(updateGraph, tbb::flow::unlimited,
    [this](UpdateMessage* msg) -> UpdateMessage* {
      // exceptions handled in processUpdateBody()
      this->processUpdateBody(*msg);
      return msg;
    });

  // then make sure that the updates are finished in order so all updates are done before a commit is processed.
  updateSequencerNode = std::make_unique<tbb::flow::sequencer_node<UpdateMessage*>>(updateGraph,
    [](UpdateMessage* msg) -> size_t {
      INDEX_DEBUG("updateSequencerNode: msg={} updateVersion={} updateOrdinal={}",
                  (void*)msg, msg->updateVersion, msg->updateOrdinal);
      return msg->updateOrdinal;
    });

  // updates flow into the updateFinishNode in order which is single threaded and ensures that updates are finished in order.
  updateFinishNode = std::make_unique<UpdateMessageMultiFunc>(updateGraph, 1,
    [this](UpdateMessage* msg,
    UpdateMessageMultiFunc::output_ports_type& op) {
      unused(op);
      // exceptions handled in finishUpdateBody()
      this->finishUpdateBody(*msg);
      // std::get<0>(op).try_put(msg);
    });

  // connect the update nodes
  tbb::flow::make_edge(*startUpdateNode, *processUpdateNode);
  tbb::flow::make_edge(*processUpdateNode, *updateSequencerNode);
  tbb::flow::make_edge(*updateSequencerNode, *updateFinishNode);

  // now the commit nodes
  segmentFlushNode = std::make_unique<InverterMultiFunc>(updateGraph, tbb::flow::unlimited,
    [this](Inverter* inverter,
    InverterMultiFunc::output_ports_type& op) {
      unused(op);
      // TODO: handle exceptions
      this->segmentFlushBody(*inverter);
    });

  commitSequencerNode = std::make_unique<tbb::flow::sequencer_node<UpdateMessage*>>(updateGraph,
    [](UpdateMessage* msg) -> size_t {
      INDEX_DEBUG("commitSequencerNode: msg={} commitNum={}", (void*)&msg, msg->commitNum);
      return msg->commitNum;
    });

  // must be single-threaded
  commitFinishNode = std::make_unique<UpdateMessageMultiFunc>(updateGraph, 1,
    [this](UpdateMessage* msg,
    UpdateMessageMultiFunc::output_ports_type& op) {
      unused(op);
      bool dispatchCompletion = true;
      try {
        dispatchCompletion = this->finishCommitBody(*msg);
      }
      catch (std::exception& e) {
        LOG_ERROR("finishCommitBody Exception Caught: exception={}",
          e.what());
        msg->result.setException(e);
      }

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
  mergeSegmentsNode = std::make_unique<MergeMessageMultiFunc>(updateGraph, 1,
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
    });
}

IndexWriter::~IndexWriter() {
  // without this, in gcc release mode we can get a crash when the IndexWriter is destroyed, even when
  // the graph wasn't used. Presumably because the test was so fast and there was some async initialization
  // of the graph still going on?
  updateGraph.wait_for_all();

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
  }

  completeForceMergeOrigins(completedOrigins);
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

// Obtains an inverter for writing documents and sets it's updateVersion.
Inverter& IndexWriter::obtainInverter(uint64_t updateVersion) {
  // IDEA: should we prefer grabbing the inverter with the most docs?  Idea would be to
  // have a couple of really large segments that will need less merging?
  Inverter* inverter = nullptr;
  const std::lock_guard<std::mutex> lock(indexMutex);
  if (idleInverters.empty()) {
    auto newInverter = std::make_unique<Inverter>(dir, ++lastSegId, schemaProvider_);
    inverter = newInverter.get();
    busyInverters.emplace(inverter, std::move(newInverter));
  }
  else {
    auto it = idleInverters.begin();
    inverter = it->first;
    busyInverters.emplace(inverter, std::move(it->second));
    idleInverters.erase(it);
  }
  // Coercion context never leaks across update-message acquisitions. The
  // normal protobuf path immediately installs its request clock; custom/test
  // messages without one fall back to call-time DATE coercion.
  inverter->coerceContext = {};
  inverter->updateVersions(updateVersion);
  return *inverter;
}


void IndexWriter::releaseInverter(Inverter& inverter, bool flush) {
  // The undo scope is the update message that held this inverter; marks must
  // not outlive the release.
  inverter.clearUndoLog();

  const std::lock_guard<std::mutex> lock(indexMutex);
  auto it = busyInverters.find(&inverter);
  if (it == busyInverters.end()) {
    LOG_ERROR("Inverter not found in busy list.");
    assert(false); // should never happen
  }

  // TODO: update and check global statistics
  // Size-based auto-flush is driven by the caller (ProtoUpdateMessage::handle passes
  // flush=true at end of batch when inverter.shouldFlush() is true), not decided here:
  // the message handler owns the safe flush point (a closed undo scope). Phase 2's
  // global budget will add IndexWriter-side marking on top of this seam.

  // if this inverter is part of a commit, initiate a flush.
  if (inverter.commitInfo != nullptr || flush) {
    if (inverter.commitInfo) {
      INDEX_DEBUG("inverter={} message={} triggering flush.", inverter,
                  (void*)inverter.commitInfo->updateMessage);
    } else {
      INDEX_DEBUG("inverter={} flush requested.", inverter);
    }
    flushingInverters.emplace(&inverter, std::move(it->second));
    it = busyInverters.erase(it);
    segmentFlushNode->try_put(&inverter);
  }
  else {
    INDEX_DEBUG("releaseInverter: inverter={} adding back to idleInverters.", inverter);
    // return inverter to idle pool
    idleInverters.emplace(&inverter, std::move(it->second));
    busyInverters.erase(it);
  }
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


void IndexWriter::commit(UpdateMessage::CommitType commitType) {
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
      _releaseToCommitSequencer(&msg);
      commitNumber++;
      return;
    }

    // Grab the global commit info and move it to the UpdateMessage.
    msg.commitInfo = std::move(nextCommitInfo);
    nextCommitInfo = std::move(emptyCommitInfo);
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
    for (auto it = idleInverters.begin(); it != idleInverters.end(); it++) {
      if (it->second->minVersion <= msg.updateVersion) {
        if (it->second->commitInfo == nullptr) {
          INDEX_DEBUG("\tinitiateCommit: msg={} marking idle inverter={} for commit", (void*)&msg,
                      *it->second.get());
          it->second->commitInfo = &commitInfo;
          commitInfo.leftToFlush++;
        }
        else {
          // this would be a bug since we should never have an idle inverter that is part of a commit.
          LOG_ERROR("Idle inverter is part of another commit.");
        }

        // move the inverter to the flushing list
        auto [flushingIt, success] = flushingInverters.emplace(it->first, std::move(it->second));
        idleInverters.erase(it);
        // send the inverter to the segment flush node
        segmentFlushNode->try_put(flushingIt->second.get());
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
    // Normally a commit would be kicked off by the last segment flushing.  But if there are no segments to flush,
    // we need to kick it off here.
    if (commitInfo.leftToFlush == 0) {
      try {
        _releaseToCommitSequencer(&msg);
      } catch (...) {
        if (msg.waitForMerges) {
          std::erase(waitingForMerges, &msg);
        }
        nextCommitInfo = std::move(msg.commitInfo);
        throw;
      }
    }
    commitNumber++;
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

// Inverter for the segment should already be in the flushingInverters list.
// This is called in parallel.  The inverter will be deleted.
void IndexWriter::segmentFlushBody(Inverter& inverter) {
  INDEX_DEBUG("segmentFlushBody: inverter={} commitInfo={} msg.leftToFlush={}", inverter,
              (void*)inverter.commitInfo,
              inverter.commitInfo == nullptr ? -1 : inverter.commitInfo->leftToFlush);

  std::vector<std::string> flushedFiles;
  bool success = false;
  bool aborted = inverter.failed();
  if (!aborted) {
    try {
      // uncomment to serialize inverter flushing (for testing purposes)
      // const std::lock_guard<std::mutex> lock(indexMutex);
      success = inverter.flush(&flushedFiles);
    } catch (const std::exception& e) {
      LOG_ERROR("Exception caught while flushing inverter: {}", e.what());
      inverter.fail(std::current_exception());
      aborted = true;
    } catch (...) {
      LOG_ERROR("Unknown non-standard exception caught while flushing inverter");
      inverter.fail(std::current_exception());
      aborted = true;
    }
  }

  std::unique_ptr<SegInfo> segInfo;
  if (!aborted) {
    segInfo = std::make_unique<SegInfo>(inverter.getPostingsWriter().segId,
                                        inverter.getPostingsWriter().getMaxDoc());
    segInfo->unsyncedFiles = std::move(flushedFiles);
    segInfo->minVersion = inverter.minVersion;
    segInfo->maxVersion = inverter.maxVersion;
    segInfo->schemaGen = currentSchemaGen();
    // Set liveDocs + liveGen for deleted docs from errors during indexing
    if (inverter.liveGen > 0) {
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

  {
    const std::lock_guard<std::mutex> lock(indexMutex);
    INDEX_DEBUG("segmentFlushBody: inverter {} flushed. Adding {}", (void*)&inverter,
                segInfo ? format_as(*segInfo)
                        : std::string(aborted ? "(aborted segment)" : "(empty segment, dropped)"));

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

    // move any deletes from the inverter to the relevant commit info.
    if (!aborted && inverter.hasDeletions()) {
      CommitInfo& commitInfo = inverter.commitInfo ? *inverter.commitInfo : *nextCommitInfo;
      INDEX_DEBUG("segmentFlushBody: inverter={} moving deletes to commitInfo={}", inverter,
                  (void*)&commitInfo);
      commitInfo.multiDeletesData.deletes.emplace_back(std::move(inverter.sortedDeletes));
    }

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

  if (aborted) {
    // Destroy first to close any partially written file, then remove every
    // finished or temporary file belonging to this never-published segment.
    uint64_t segId = inverterPtr->getPostingsWriter().segId;
    inverterPtr.reset();
    try {
      dir.deletePrefix(Postings::getIndexFileNamePrefix(segId));
    } catch (const std::exception& e) {
      LOG_ERROR("Failed to clean files for aborted segment {}: {}", segId, e.what());
    }
  }

  // inverterPtr goes out of scope here on a successful flush.
}

// This applies deletes and writes out the new segments file.
// called from the commitFinishNode which has concurrency==1 (single-threaded)
bool IndexWriter::finishCommitBody(UpdateMessage& msg) {
  INDEX_DEBUG("finishCommitBody: msg={}", (void*)&msg);
  // TODO: if nothing actually changed, we could skip writing a new commit at this point.

  // Apply deletes from all segments that were part of this commit to all segments in the index.
  // This must be mutually exclusive with segment merging, or we must do some form of optimistic concurrency.

  // Optimistic concurrency for deletes strategy:
  // 1. With index lock held: grab list of all segments and mark them as being part of a commit.  This will
  //    prevent the SegmentInfo from being deleted.
  // 2. Apply deletes to all of the snapshot segments.
  // 3. Write the segments file with the snapshot.
  // 4. With index lock held: check if any new segments were created by merging.  If so, it's our job
  //    to apply deletes to those segments as well.  If any of the segments in our snapshot are marked
  //    as being merged, then signal the segmentMerger to transfer the deletes to those new segments.
  //    NOTE: both conditions could be true, some segments merged, some segments in process of being merged.
  //    NOTE: instead of applying deletes to the new segments, we could just wait for the
  //          next commit (which will use that new segment) to apply the deletes attached to it.

  // THOUGHT: what if a segment with personal deletes is merged?  Need to merge the personal deletes
  // of all segments as well?  Or else segment merger needs to handle that.  The latter would get
  // rid of the personal deletes faster, but would mean that it could be concurrent with applying deletes
  // in finishCommitBody().


  auto maxDeleteVersion = msg.commitInfo->multiDeletesData.getLargestVersion();
  msg.commitInfo->highestUpdateVersion = msg.updateVersion;

  std::vector<SegInfo*> segs;
  std::vector<SegInfo*> segsToApplyDeletes;
  // std::vector<std::shared_ptr<MultiDeletesData>> personalDeletes;

  {
    const std::lock_guard<std::mutex> lock(indexMutex);
    segs.reserve(segInfos.size());
    // grab all segments and mark them as being part of a commit.
    for (auto& [segId, seg] : segInfos) {
      segs.push_back(seg.get());
      if (seg->lastCommitTime == 0) {
        seg->lastCommitTime = 1; // IMPORTANT - for marking it in use to prevent its deletion.
      }
      if (!seg->personalDeletes.empty() || seg->minVersion < maxDeleteVersion) {
        // this segment has personal deletes and/or commit-level deletes that need to be applied.
        segsToApplyDeletes.push_back(seg.get());
      }
    }
  }

  // If a segmentMerge kicks off here, it could be merging segments without deletes applied yet.
  // We check for that after we finish applying deletes.

  // Collect filenames that need to be fsynced before the commit point.
  std::vector<std::string> filesToSync;

  CommitInfo& commitInfo = *msg.commitInfo;
  if (segsToApplyDeletes.empty()) {
    INDEX_DEBUG("finishCommitBody: msg={} no deletes to apply.", (void*)&msg);
  }
  else {
    // TODO: doesn't take into account personal deletes.
    INDEX_DEBUG("finishCommitBody: msg={} applying deletes to {} segments.", (void*)&msg, segs.size());
    // This is where seg.liveGen can change!
    // Can personal deletes be mutated elsewhere?
    // mergeSegmentsBody can add personalDeletes to a *new* segment, but it does it under the indexMutex lock,
    // so we will either see the new segment with its personal deletes, or not see the segment at all.
    applyDeletes(segsToApplyDeletes, commitInfo.multiDeletesData);
  }


  //
  // Now check if any of the segments were merged without necessary deletes applied.
  // There are 3 possibilities:
  // 1. An old segment liveGen is in the process of being merged.  We will add personalDeletes to the old
  //    segment and then the segment merger will copy them to the new segment.
  // 2. An old segment liveGen was merged.  We must handle putting personalDeletes on the new segment.
  //    We won't know exactly what segment, since there could have been multiple merges.
  // 3. A segment merge starts right here. No issues since it sees the latest liveGen.
  //
  {
    const std::lock_guard<std::mutex> lock(indexMutex);

    std::vector<SegInfo*> startedMergingOldVersions;
    std::vector<SegInfo*> finishedMergingOldVersions;
    std::vector<std::shared_ptr<MultiDeletesData>> personalDeletes;
    // personal deletes from segments that finished merging too early.

    // Check if any segments were merged before deletes were applied.
    for (auto& seg : segs) {
      INDEX_TRACE("postApplyDeletes CHECK seg{}", *seg);
      if (seg->mergedLiveGen >= 0) {
        // segment was merged.
        if (seg->mergedLiveGen == (int64_t)seg->liveGen) {
          // this segment was merged (or is currently merging) with the latest liveGen, so no issues. We've applied all deletes,
          // so we can drop any personal deletes to save space. Merger does not try to apply personal deletes.
          seg->personalDeletes.clear();
          INDEX_DEBUG("finishCommitBody: msg={} segment {} was merged with latest liveGen {}", (void*)&msg, seg->name(),
                      seg->liveGen);
          continue;
        }

        if (seg->merging == true) {
          INDEX_DEBUG("Detected merging segments with old deletes {}", *seg);
          startedMergingOldVersions.push_back(seg);
        }
        else {
          INDEX_DEBUG("Detected finished merge of segment with old deletes {}", *seg);

          finishedMergingOldVersions.push_back(seg);
          // since this segment was already merged, we can just move it's personal deletes off
          personalDeletes.insert(personalDeletes.end(),
                                 std::make_move_iterator(seg->personalDeletes.begin()),
                                 std::make_move_iterator(seg->personalDeletes.end()));
          seg->personalDeletes.clear();
        }
      }
      else {
        // segment was not merged and is not merging, so drop any personal deletes it had
        // because they have been applied.
        if (!seg->personalDeletes.empty()) {
          INDEX_DEBUG("finishCommitBody: segment {} dropping personal deletes.", seg->name());
          seg->personalDeletes.clear();
        }
      }
    }

    if (!startedMergingOldVersions.empty() || !finishedMergingOldVersions.empty()) {
      // add personal deletes to all new segments that could be applicable.
      // first make a shared_ptr from the commit info to it.
      CommitInfo& commitInfo = *msg.commitInfo;

      // queued (and applied) deletes for the current commit.
      std::shared_ptr<MultiDeletesData> multiDeletesDataPtr = std::make_shared<MultiDeletesData>(
        std::move(commitInfo.multiDeletesData));

      // Since only one merge can happen at once, we only need to add personal deletes to
      // one of the segments that are being merged to get them transferred to the new segment when it is done.
      if (!startedMergingOldVersions.empty()) {
        auto& seg = *startedMergingOldVersions.front();
        // no segments were merged, so we can just apply deletes to the new segments.
        INDEX_DEBUG("finishCommitBody: Added personal deletes currently merging {} deletes={}", seg,
                  (void*)multiDeletesDataPtr.get());
        seg.personalDeletes.push_back(multiDeletesDataPtr); // copy the shared_ptr

#if SPDLOG_ACTIVE_LEVEL <= SPDLOG_LEVEL_TRACE
        for (auto& s : seg.personalDeletes) {
          for (auto& d : s->deletes) {
            INDEX_TRACE("\tpersonal deletes: {}", d->toString());
          }
        }
#endif
      }


      // For segments that were already merged, look for new segments to attach personal deletes to.
      // They won't be applied immediately, but will be the next time this commit code is entered.
      // First add multiDeletesDataPtr to the personal deletes we previously collected and find the max delete version
      // since it is possible for personal deletes to be higher than commit deletes.
      personalDeletes.push_back(multiDeletesDataPtr);
      auto maxAllDeleteVersion = maxDeleteVersion; // max including those in personalDeletes.
      for (auto& deletes : personalDeletes) {
        maxAllDeleteVersion = std::max(maxAllDeleteVersion, deletes->getLargestVersion());
      }
      auto numSegmentsMissingDeletes = 0; // sanity check - we should find some.
      for (auto& [segId, seg] : segInfos) {
        // TODO: is it possible for any personal deletes to be higher than the maxDeletedVersion here?
        // Uhhh, yes! There could be *no* deletes in a commit other than the personal deletes.
        if (seg->minVersion < maxAllDeleteVersion && seg->lastCommitTime == 0) {
          // this segment has deletes that need to be applied, so append to its personal deletes.
          numSegmentsMissingDeletes++;
          // *copy* all of the collected delete sets
          seg->personalDeletes.append_range(personalDeletes);
          INDEX_DEBUG("finishCommitBody: Added personal deletes for {}", *seg);
        }
      }

      if (numSegmentsMissingDeletes == 0) {
        // One was this can happen is if a merger resulted in totally dropping a segment.
        INDEX_DEBUG(
          "A segment was merged with old deletes, but no merged segments were found that needed deletes applied. Was segment deleted?");
      }
    }
  } // end index lock

  // Check if any of the segments are now empty and remove them from the list.
  std::vector<SegInfo*> segsToKeep;
  std::vector<SegInfo*> toDelete;
  {
    const std::lock_guard<std::mutex> lock(indexMutex);
    for (auto& seg : segs) {
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

  // Decide the new index + core generations once, up front, so every downstream
  // piece (aux indexes, IndexInfo file) refers to the same values.  Stashed on
  // CommitInfo so they travel with the message.
  //
  // segsToKeep is final at this point, so we can determine whether segment
  // composition changed and mutate coreGen + lastCommittedSegIds now rather
  // than deferring to writeIndexInfoFile.
  if (msg.commitInfo) {
    msg.commitInfo->indexGen = ++indexGen;

    bool segsChanged = (segsToKeep.size() != lastCommittedSegIds.size());
    if (!segsChanged) {
      boost::unordered_flat_set<uint64_t> oldIds(lastCommittedSegIds.begin(),
                                                  lastCommittedSegIds.end());
      for (auto* s : segsToKeep) {
        if (!oldIds.contains(s->segId)) { segsChanged = true; break; }
      }
    }
    if (segsChanged) {
      coreGen++;
      lastCommittedSegIds.clear();
      lastCommittedSegIds.reserve(segsToKeep.size());
      for (auto* s : segsToKeep) lastCommittedSegIds.push_back(s->segId);
      INDEX_DEBUG("Segment composition changed, incremented coreGen to {}", coreGen);
    }
    msg.commitInfo->coreGen = coreGen;
  }

  // Build aux indexes if requested.  Vector overlays are segment-local and are
  // carried by segment liveness; index-level aux remains for future non-vector
  // kinds only.
  auto auxIndexInfos = buildAuxIndexes(msg, segsToKeep, filesToSync);
  buildSegmentOverlays(msg, segsToKeep, filesToSync);
  auto segmentOverlayInfos = flattenSegmentOverlays(segsToKeep);

  // Fsync all segment data and liveDocs files before writing the commit point.
  // "." syncs the directory to make renames durable.
  if (!filesToSync.empty()) {
    filesToSync.emplace_back(".");
    dir.sync(filesToSync);
    for (auto seg : segsToKeep) {
      seg->unsyncedFiles.clear();
    }
  }

  // write the segments file with only the segments that have live documents
  writeIndexInfoFile(segsToKeep, msg.commitInfo.get(), auxIndexInfos);

  // Aux index housekeeping: now that the new IndexInfo is durable, files
  // from the previous list that aren't in the new one are unreferenced.
  // Then publish the new list for the next commit's carry-forward.
  deleteOrphanedAuxFiles(currentAuxIndexes_, auxIndexInfos);
  currentAuxIndexes_ = std::move(auxIndexInfos);
  deleteOrphanedAuxFiles(currentSegmentOverlays_, segmentOverlayInfos);
  currentSegmentOverlays_ = std::move(segmentOverlayInfos);

  // Only move the segment to the delete list after the new IndexInfo file is written.
  // This way it should be safe for other threads to also try deletions.
  // NOTE: we did have a call to tryDeleteSegments() from the merge code as well, but
  // there was a race condition: writeIndexInfoFile() was updating the commitTime of
  // the segments before updating lastCommitTime, so a segment could be deleted in that period.
  // The race is fixable (always use "1" for a segment being committed, etc), but it's
  // easier for now to just call tryDeleteSegments() in this method.
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
  tryDeleteSegments();

  if (msg.maxSegments > 0) {
    enqueueForceMerge(msg); // completion ownership transfers here; don't access msg below.
    return false;
  }
  return true;
}

// Attempts to actually remove the files for segments in the deletion list.
// Segments that are being merged will not be deleted.
// Segments that are in the last commit will not be deleted.
void IndexWriter::tryDeleteSegments() {
  try {
    // Delete segment files only after the IndexInfo file is written.
    // Do not delete any segments that are being merged.
    std::vector<std::unique_ptr<SegInfo>> localDeleteList;
    {
      const std::lock_guard<std::mutex> lock(indexMutex);
      auto lastCommit = lastCommitTime.load(std::memory_order::relaxed);
      // grab all segments and put back ones we shouldn't delete yet.
      std::swap(localDeleteList, segmentsToDelete);
      for (auto& seg : localDeleteList) {
        // NOTE! we can observe seg->commitTime > lastCommit because the segments file
        // may be in the process of being written out, and lastCommitTime is only
        // updated *after* the IndexInfo file is written.
        // commitTime==1 means it's about to be committed (don't want try-delete call from segment merger to delete it).
        if (seg->merging || seg->lastCommitTime >= lastCommit || seg->lastCommitTime == 1) {
          segmentsToDelete.push_back(std::move(seg)); // keep it in the list, we can't delete it yet.
        }
      }
    }

    // Now delete the segments outside of the mutex.
    // In the future, if we wanted to support windows, we need a background deleter to
    // retry deletes after some time in case they are still open.
    for (auto& seg : localDeleteList) {
      if (!seg) continue; // skip null segments
      INDEX_DEBUG("Deleting files {}", *seg);
      dir.deletePrefix(Postings::getIndexFileNamePrefix(seg->segId));
      seg.reset(); // deletes the in-memory segment info
    }
  } catch (std::exception& e) {
    LOG_ERROR("Exception caught while deleting segments: {}", e.what());
  }
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
  // finishCommitBody assigns indexGen + coreGen up front so all commit-stage
  // artifacts share these values.
  assert(msg.commitInfo && msg.commitInfo->indexGen > 0);
  uint64_t newCoreGen = msg.commitInfo->coreGen;

  // Step 1: filter previous list by core gen.  Build a set of "still valid"
  // names (segment-dependent entries whose built_core_gen matches the new
  // core gen) - rebuilding those would produce identical output, so we tell
  // the builder to skip them.
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
std::shared_ptr<PostingsReader> IndexWriter::getSegmentPostingsReader(SegInfo& seg) {
  auto pr = seg.sharedPostingsReader.load();
  if (!pr) {
    pr = std::make_shared<PostingsReader>(dir, seg.segId);
    seg.sharedPostingsReader.store(pr);
  }
  return pr;
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

uint64_t IndexWriter::nextVectorOverlayGen(const SegInfo& seg, std::string_view name) const {
  uint64_t nextGen = 0;
  // Usually defense-in-depth: callers skip existing names before rebuilding, so
  // seg.auxOverlays normally cannot raise the ordinal for a selected name.
  for (const auto& overlay : seg.auxOverlays) {
    if (overlay.kind == VectorIndexBuilder::KIND && overlay.name == name) {
      nextGen = std::max(nextGen, overlay.gen + 1);
    }
  }

  // This scan is load-bearing for the current drop-then-rebuild test flow: the
  // old manifest entry may be absent from seg.auxOverlays but still present in
  // currentSegmentOverlays_ until the rebuilding commit publishes.  Future APIs
  // that publish a drop separately must preserve this ordinal another way.
  //
  // firstCommitTime keeps private merge output out of currentSegmentOverlays_:
  // merge-created segments are not published when their overlay build runs.
  if (seg.firstCommitTime != 0) {
    for (const auto& published : currentSegmentOverlays_) {
      if (published.segId == seg.segId
          && published.info.kind == VectorIndexBuilder::KIND
          && published.info.name == name) {
        nextGen = std::max(nextGen, published.info.gen + 1);
      }
    }
  }

  return nextGen;
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

    uint64_t overlayGen = nextVectorOverlayGen(seg, overlayName);
    VectorIndexBuilder vb(dir, std::span<const VectorIndexBuilder::SegInput>(&input, 1),
                          schema, overlayGen);
    std::vector<std::string> oneSelector{overlayName};
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
  assert(msg.commitInfo && msg.commitInfo->indexGen > 0);

  std::vector<std::string> vectorSelectors = collectVectorSelectors(msg.buildAuxIndexes);
  std::shared_ptr<Schema> schema;
  std::vector<std::string> stagedActiveOverlayNames;
  if (!vectorSelectors.empty() && schemaProvider_) {
    schema = schemaProvider_();
    if (schema) {
      stagedActiveOverlayNames = collectValidatedExactVectorOverlayNames(msg.buildAuxIndexes, *schema);
    }
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

        std::string fileName = Postings::getSegmentOverlayFileName(
          seg->segId, TestOverlayAuxReader::NAME, msg.commitInfo->indexGen, 0);
        {
          auto file = dir.createFile(fileName);
          OutputStream os;
          os.setFile(&*file);
          static constexpr std::string_view payload = "solux test overlay\n";
          os.write(payload.data(), payload.size());
          os.close();
          dir.finishFile(*file);
        }
        stagedFiles.push_back(fileName);

        AuxInfo info;
        info.kind = std::string(TestOverlayAuxReader::KIND);
        info.name = std::string(TestOverlayAuxReader::NAME);
        // Intentionally indexGen, not the vector rebuild ordinal: this kind is
        // existence-only (no rebuild flow), so gen only has to uniquify files.
        info.gen = msg.commitInfo->indexGen;
        info.files.push_back(fileName);
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
  for (auto& staged : stagedOverlays) {
    for (const auto& file : staged.info.files) {
      staged.seg->unsyncedFiles.push_back(file);
    }
    staged.seg->auxOverlays.push_back(std::move(staged.info));
  }
}

std::vector<IndexWriter::PublishedOverlay>
IndexWriter::flattenSegmentOverlays(std::span<SegInfo*> segs) const {
  std::vector<PublishedOverlay> out;
  size_t total = 0;
  for (auto* seg : segs) total += seg->auxOverlays.size();
  out.reserve(total);
  for (auto* seg : segs) {
    for (const auto& overlay : seg->auxOverlays) {
      out.push_back({seg->segId, overlay});
    }
  }
  return out;
}

// After a successful IndexInfo write, delete files referenced by the previous
// aux index list that aren't referenced by the new one.  Carried-forward
// entries appear in both lists, so their files survive.  Files written by a
// rebuild for a given name supersede files from the previous build of the same
// name and the old ones get cleaned up here.
namespace {
// Core of deleteOrphanedAuxFiles.  BEST-EFFORT BY CONTRACT: this runs after
// writeIndexInfoFile has published the new commit, so a cleanup failure must
// never propagate - the commit already succeeded and reporting an error now
// would lie to the client.  A leaked file is reclaimed by a later rebuild's
// diff or by the dead-segment prefix sweep.
void deleteFilesNotKept(Directory& dir,
                        const boost::unordered_flat_set<std::string>& keep,
                        const std::string& fname) {
  if (keep.contains(fname)) return;
  try {
    INDEX_DEBUG("deleteOrphanedAuxFiles: deleting {}", fname);
    dir.deleteFile(fname);
  } catch (const std::exception& e) {
    LOG_ERROR("deleteOrphanedAuxFiles: failed to delete {} (will leak until reclaimed): {}",
              fname, e.what());
  } catch (...) {
    LOG_ERROR("deleteOrphanedAuxFiles: failed to delete {} (will leak until reclaimed)", fname);
  }
}
} // namespace

// PublishedOverlay variant: same semantics, keyed file sets come from .info.
void IndexWriter::deleteOrphanedAuxFiles(const std::vector<PublishedOverlay>& oldList,
                                         const std::vector<PublishedOverlay>& newList) {
  boost::unordered_flat_set<std::string> keep;
  for (const auto& published : newList) {
    for (const auto& f : published.info.files) keep.emplace(f);
  }
  for (const auto& published : oldList) {
    for (const auto& f : published.info.files) {
      deleteFilesNotKept(dir, keep, f);
    }
  }
}

void IndexWriter::deleteOrphanedAuxFiles(const std::vector<AuxInfo>& oldList,
                                         const std::vector<AuxInfo>& newList) {
  boost::unordered_flat_set<std::string> keep;
  for (const auto& info : newList) {
    for (const auto& f : info.files) keep.emplace(f);
  }
  for (const auto& info : oldList) {
    for (const auto& f : info.files) {
      deleteFilesNotKept(dir, keep, f);
    }
  }
}


// This is only called from the finishCommit node, which has concurrency==1 (single-threaded)
// hence we only need to protect against changes in the segInfos map, not multiple invocations of this method.
// The passed span of segments may be reordered after this is finished.
void IndexWriter::writeIndexInfoFile(std::span<SegInfo*> segs, CommitInfo* commitInfo,
                                     std::span<const AuxInfo> auxIndexes) {
  // We should be able to write the segments file without holding the indexMutex,
  // as long as we access only fields that should not change on SegInfo.
  // liveDocs + liveGen won't change because we only apply deletes in finishCommitBody().

  auto indexFile = dir.createFile(Postings::INDEX_INFO_FILE);
  OutputStream indexOut;
  indexOut.setFile(&*indexFile);

  // get timestamp in microseconds and make sure it is increasing and unique.
  uint64_t now_us = std::chrono::duration_cast<std::chrono::microseconds>(
    std::chrono::system_clock::now().time_since_epoch()).count();
  if (now_us <= lastCommitTime) {
    now_us = lastCommitTime + 1;
  }

  INDEX_DEBUG("writeIndexInfoFile: now_us={} lastCommitTime={} diff={}", now_us, lastCommitTime.load(),
              now_us - lastCommitTime.load());
  uint64_t numDocs = 0;

  // Caller (finishCommitBody) sorts by segId before us so aux-index building
  // sees the same canonical order.  Defensive sort for the test-only no-commitInfo
  // path where the caller hasn't sorted.
  if (!commitInfo) {
    std::sort(segs.begin(), segs.end(),
              [](const SegInfo* a, const SegInfo* b) { return a->segId < b->segId; });
  }
  assert(std::is_sorted(segs.begin(), segs.end(),
                        [](const SegInfo* a, const SegInfo* b) { return a->segId < b->segId; }));
  
  uint64_t updateVersion = 0;
  uint64_t thisIndexGen;
  if (commitInfo) {
    // finishCommitBody assigned indexGen + coreGen up front (and updated
    // lastCommittedSegIds at the same time) so aux-index building and this
    // write share single values.
    assert(commitInfo->indexGen > 0);
    thisIndexGen = commitInfo->indexGen;
    assert(commitInfo->highestUpdateVersion >= commitInfo->updateMessage->updateVersion);
    updateVersion = commitInfo->highestUpdateVersion;
  }
  else {
    // Test-only path (no commitInfo): assign on the fly, including the
    // segsChanged check that finishCommitBody normally performs.
    bool segsChanged = (segs.size() != lastCommittedSegIds.size());
    if (!segsChanged) {
      for (size_t i = 0; i < segs.size(); i++) {
        if (segs[i]->segId != lastCommittedSegIds[i]) { segsChanged = true; break; }
      }
    }
    if (segsChanged) {
      coreGen++;
      lastCommittedSegIds.clear();
      lastCommittedSegIds.reserve(segs.size());
      for (auto seg : segs) lastCommittedSegIds.push_back(seg->segId);
    }
    thisIndexGen = ++indexGen;
    updateVersion = updateNumber;
  }

  // Build the IndexInfo message NON-OWNING into a scratch arena, then encode. Segment +
  // overlay + aux arrays are arena-allocated; AuxIndexInfo views point at the owning
  // AuxInfo state (toWire) with file-name spans also in the arena.
  std::pmr::monotonic_buffer_resource iiArena;
  solux::api::IndexInfo indexInfo;
  indexInfo.commit_time = now_us;
  indexInfo.version = 1;
  indexInfo.index_gen = thisIndexGen;
  indexInfo.update_version = updateVersion;
  indexInfo.core_gen = coreGen;
  indexInfo.schema_gen = currentSchemaGen();

  solux::api::SegmentInfo* segArr = solux::api::build::allocArray(indexInfo.segments, segs.size(), iiArena);
  size_t segIdx = 0;
  for (auto seg : segs) {
    // Update the commit time for the seg. Important to know if this seg is part of the last commit.
    // This does mean that this may be visible before the commit is done and before lastCommitTime is updated.
    // Any comparison with lastCommitTime should be done with this in mind.
    seg->lastCommitTime = now_us;
    if (seg->firstCommitTime == 0) {
      seg->firstCommitTime = now_us;
    }

    auto& segmentInfo = segArr[segIdx++];
    segmentInfo.seg_id = seg->segId;
    segmentInfo.max_doc = seg->maxDoc;
    segmentInfo.live_gen = seg->liveGen;
    segmentInfo.min_version = seg->minVersion;
    segmentInfo.max_version = seg->maxVersion;
    segmentInfo.commit_time = seg->firstCommitTime;
    segmentInfo.live_docs = seg->liveDocs;
    segmentInfo.schema_gen = seg->schemaGen;
    solux::api::AuxIndexInfo* ovArr =
        solux::api::build::allocArray(segmentInfo.overlays, seg->auxOverlays.size(), iiArena);
    for (size_t oi = 0; oi < seg->auxOverlays.size(); oi++) {
      ovArr[oi] = toWire(seg->auxOverlays[oi], iiArena);
      if (ovArr[oi].commit_time == 0) {
        ovArr[oi].commit_time = now_us;
      }
    }

    numDocs += seg->maxDoc;
    INDEX_DEBUG("\t{}", *seg);
  }

  // Carry over aux indexes built during this commit.
  solux::api::AuxIndexInfo* auxArr =
      solux::api::build::allocArray(indexInfo.aux_indexes, auxIndexes.size(), iiArena);
  for (size_t ai = 0; ai < auxIndexes.size(); ai++) {
    auxArr[ai] = toWire(auxIndexes[ai], iiArena);
    if (auxArr[ai].commit_time == 0) {
      auxArr[ai].commit_time = now_us;
    }
  }

  // Serialize the IndexInfo (changes the on-disk commit-point format).
  std::vector<std::byte> serialized;
  serialized.reserve(200 + segs.size() * 24);
  if (!solux::api::encode(indexInfo, serialized)) {
    throw std::runtime_error("Failed to serialize IndexInfo protobuf");
  }

  indexOut.write((const char*)serialized.data(), serialized.size());
  indexOut.close();
  dir.finishFile(*indexFile);

  // Fsync the commit point (INDEX_INFO_FILE) and the directory entry.
  std::array<std::string, 2> commitFiles = {std::string(Postings::INDEX_INFO_FILE), "."};
  dir.sync(commitFiles);

  INDEX_DEBUG("\twriteIndexInfoFile DONE: commitTime={} nSegs={} gen={} maxDoc={}", now_us, segs.size(), thisIndexGen,
              numDocs);

  unused(numDocs);

  // advertise this commit only after the file is closed.
  lastCommitTime = now_us;
  lastAdvertisedCommitTime = now_us;
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
      boost::unordered_flat_map<std::string, int64_t> ordTermSums;
      std::vector<SegInfo*> admitted;
      admitted.reserve(segs.size());
      for (SegInfo* seg : segs) {
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
      sourceSegIds.reserve(segs.size());
      for (auto* seg : segs) {
        sourceSegIds.push_back(seg->segId);
      }

      outputSegId = ++lastSegId;
      failure.sourceSegIds = sourceSegIds;
      failure.outputSegId = outputSegId;
      phase = "merge_start_signal";

      try {
        solux::Signal::emit("mergeStart", (void*)(int64_t)msg.mergeLevel, (void*)segs.size());

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
      preaders.emplace_back(segInfo->sharedPostingsReader.load());
      if (!preaders.back()) {
        preaders.back() = std::make_shared<PostingsReader>(dir, segInfo->segId);
        segInfo->sharedPostingsReader.store(preaders.back());
      }
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
      // Do the actual merge.
      SegmentMerger merger(preaderPtrs, liveDocsPtrs, pwriter, *indexRamBudget,
                           termPartitionMinBytes, termPartitionMinRangeBytes,
                           termPartitionMaxRanges);
      merger.merge();

      phase = "new_segment_info";
      // Create the new SegInfo for the output segment.
      auto newSegInfo = std::make_unique<SegInfo>(pwriter.getSegId(), pwriter.getMaxDoc());
      newSegInfo->minVersion = segs.front()->minVersion;
      newSegInfo->maxVersion = segs.front()->maxVersion;
      for (size_t i = 1; i < segs.size(); i++) {
        newSegInfo->minVersion = std::min(newSegInfo->minVersion, segs[i]->minVersion);
        newSegInfo->maxVersion = std::max(newSegInfo->maxVersion, segs[i]->maxVersion);
      }
      phase = "schema_generation";
      newSegInfo->schemaGen = currentSchemaGen();
      phase = "postings_finish";
      pwriter.finish(&newSegInfo->unsyncedFiles);

      phase = "active_overlay_snapshot";
      auto activeOverlayNames = snapshotActiveVectorOverlayNames();
      if (!activeOverlayNames.empty() && schemaProvider_) {
        phase = "schema_provider";
        auto schema = schemaProvider_();
        if (schema) {
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
      }

      phase = "publish_swap_prepare";
      // Move old segments to the delete list and add the new segment info.
      {
        const std::lock_guard<std::mutex> lock(indexMutex);
        segmentsToDelete.reserve(segmentsToDelete.size() + segs.size());
        if (newSegInfo->liveDocs > 0) {
          segInfos.reserve(segInfos.size() + 1);
          mergePolicy->_prepareUpdate(newSegInfo.get());
        }

        size_t personalDeleteCount = newSegInfo->personalDeletes.size();
        for (auto segInfo : segs) {
          personalDeleteCount += segInfo->personalDeletes.size();
        }
        newSegInfo->personalDeletes.reserve(personalDeleteCount);

        phase = "publish_swap";
        for (auto segInfo : segs) {
          segInfo->merging = false; // mark as no longer merging so it can be removed.
          segInfo->mergedIntoSegId = newSegInfo->segId; // mark the segment as merged into the new segment

          // If the segment had personal deletes, we need to *copy* them to the new segment.
          // The commit code could be in the process of applying deletes to this segment
          // so we don't want to move them.
          // The new segment isn't in the segInfos map yet, so this guarantees that
          // The deletes will be applied before the new segment is used in a commit.
          if (!segInfo->personalDeletes.empty()) {
            INDEX_DEBUG("Will merge personal deletes from {} to {}", *segInfo, *newSegInfo);
            newSegInfo->personalDeletes.append_range(segInfo->personalDeletes);
          }

          // remove from the index: move from segInfos to segmentsToDelete
          moveSegmentToDelete(segInfo->segId);
        }

        // add new segInfo to the index if it has any docs.  Mutations to the
        // private auxOverlays and unsyncedFiles above happen before this
        // mutex-protected transfer, so the commit thread observes them after
        // taking indexMutex.
        //
        // The empty-output case (liveDocs == 0) is not expected to occur: a
        // segment whose docs are all deleted is dropped at commit time (see
        // finishCommitBody, "if (seg->liveDocs == 0)"), so it is never selected
        // for a merge, and a merge reads liveDocs at its mergedLiveGen snapshot
        // - the gen at which the selected segments still had live docs - so the
        // output reflects those live docs.  Measured: zero empty merges across
        // heavy-churn multithreaded stress.  If that ever changes and empty
        // policy merges become possible, the successful policy path must still
        // request a commit that persists source removal and reclaims the
        // dropped source files.  Client force merges already publish even when
        // no output is selected.
        if (newSegInfo->liveDocs > 0) {
          auto* publishedSegInfo = newSegInfo.get();
          auto [iter, success] = segInfos.emplace(pwriter.getSegId(), std::move(newSegInfo));
          unused(iter);
          assert(success);
          outputPublished = true;
          mergePolicy->_update(publishedSegInfo);
        }
      } // end index lock
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
    {
      const std::lock_guard<std::mutex> lock(indexMutex);
      lastMergeFailure = failure;

      if (!outputPublished) {
        for (auto* segInfo : segs) {
          segInfo->merging = false;
          segInfo->mergedLiveGen = -1;
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

      allowSyntheticCommit = outputPublished;
      chainNextMerge = !mergeFailed;
    }
  } catch (const std::exception& e) {
    LOG_ERROR("mergeSegmentsBody escaped exception: exception={}", e.what());
    if (!outputPublished && markedSegmentCount > 0) {
      const std::lock_guard<std::mutex> lock(indexMutex);
      for (size_t i = 0; i < markedSegmentCount; i++) {
        segs[i]->merging = false;
        segs[i]->mergedLiveGen = -1;
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
    LOG_ERROR("mergeSegmentsBody escaped exception: unknown exception");
    if (!outputPublished && markedSegmentCount > 0) {
      const std::lock_guard<std::mutex> lock(indexMutex);
      for (size_t i = 0; i < markedSegmentCount; i++) {
        segs[i]->merging = false;
        segs[i]->mergedLiveGen = -1;
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



#ifdef REMOVED
  // make sure we are getting the latest index reader (wasteful!)
  indexReader.reset();
  auto reader = getIndexReader();
  std::vector<PostingsReader*> preaders; // TODO: make sure we're not trying to merge a segment that is being built!
  std::vector<LiveDocs*> liveDocsPtrs;
  preaders.reserve(reader->segments().size());
  liveDocsPtrs.reserve(reader->segments().size());
  for (auto& seg : reader->segments()) {
    preaders.push_back(&seg.postingsReader());
    liveDocsPtrs.push_back(seg.liveDocs());
  }
  // we could calc maxdoc at this point...
  uint64_t segId = ++lastSegId;
  PostingsWriter pwriter(dir, segId);

  SegmentMerger merger(preaders, liveDocsPtrs, pwriter);
  merger.merge();
  pwriter.finish();

  // update the list of segments... not safe currently
  // TODO: add unused segments to the "to be deleted" list
  segInfos.clear();
  segInfos.emplace(segId, std::make_unique<SegInfo>(segId, pwriter.getMaxDoc()));


  std::vector<SegInfo*> segs;
  // need to lock the indexMutex to get a consistent view of the segments.
  {
    const std::lock_guard<std::mutex> lock(indexMutex);
    segs.reserve(segInfos.size());
    for (auto& [segId, seg] : segInfos) {
      segs.push_back(seg.get());
    }
  }

  // Call the parameterized version w/o commit info
  writeIndexInfoFile(segs);
}
#endif


/// TEST CODE (called from tests)
bool IndexWriter::testIsEmpty() {
  const std::lock_guard<std::mutex> lock(indexMutex);
  return segInfos.empty()
         && segmentsToDelete.empty()
         && idleInverters.empty()
         && busyInverters.empty()
         && flushingInverters.empty();
}

void IndexWriter::testDeleteAllData() {
  INDEX_DEBUG("testDeleteAllData: deleting all data.");
  // wait for things in the execution graph to finish.
  updateGraph.wait_for_all();
  auto freshFilterCache = std::make_shared<FilterCache>(
      originalFilterCacheConfig);

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

    // dump the current IndexReader
    {
      const std::lock_guard<std::mutex> lock(indexReaderMutex);
      indexReader.reset();
    }
    filterCache = std::move(freshFilterCache);

    // drop all idle inverters (unflushed segments)
    idleInverters.clear();

    // drop all segments
    segInfos.clear();

    // drop segments to delete
    segmentsToDelete.clear();

    // drop all index files
    dir.clear();

    // Any operation that rewinds seg_id/coreGen/commitTime namespaces MUST
    // swap or epoch the filter cache before reusing those namespaces.
    lastSegId = 0;
    indexGen = 0;
    coreGen = 0;
    lastCommittedSegIds.clear();
    currentAuxIndexes_.clear();
    currentSegmentOverlays_.clear();
    activeVectorOverlayNames.clear();
    lastMergeFailure.reset();
    nextCommitInfo = std::make_unique<CommitInfo>();

    lastCommitTime = lastAdvertisedCommitTime = 0;
  }
  mergePolicy->refresh(); // we can't call this with lock held since it tries to acquire.




  // Don't touch commitNumber or updateOrdinal: the TBB graph relies on exact session-local ordinals.
}

// TEST HOOKS: safe only against a QUIESCED writer.  indexMutex here does not
// exclude the commit body, which mutates seg->auxOverlays and
// currentSegmentOverlays_ WITHOUT the lock (it is the single mutator by
// design rule 1).  Do not "fix" a future race by adding locking on the
// commit side; quiesce the writer in the test instead.
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

// TEST CODE
void IndexWriter::debugInfo() {
  {
    std::lock_guard<std::mutex> lock(indexMutex);
    LOG_INFO("IndexWriter: segInfos.size={} idleInverters.size={} busyInverters.size={} flushingInverters.size={}",
             segInfos.size(), idleInverters.size(), busyInverters.size(), flushingInverters.size());
    LOG_INFO("\tupdateNumber={} updateOrdinal={} commitNumber={} lastCommitTime={} lastAdvertisedCommitTime={}",
             updateNumber, updateOrdinal, commitNumber, lastCommitTime.load(), lastAdvertisedCommitTime.load());
    LOG_INFO("\tmergePolicy->outstandingMerges={}", mergePolicy->outstandingMerges);
    for (auto& [segId, seg] : segInfos) {
      LOG_INFO("\t\tsegId={} nDocs={} mergeLevel={} commitTime={}", segId, seg->maxDoc, seg->mergeLevel,
               seg->lastCommitTime);
    }
  }

  {
    std::lock_guard<std::mutex> lock(indexReaderMutex);
    LOG_INFO("IndexWriter: indexReader={} ", (void*)indexReader.get());
    if (indexReader.get()) {
      LOG_INFO("\tindexReader->commitTime={}", indexReader->commitTime());
    }
  }

  // Debug info on TBB graph?
}

/// A cursor into a sorted EntrySpan for k-way merge.
namespace {

struct DeletesCursor {
  const SortedDeletes::Entry* curr;
  const SortedDeletes::Entry* end;

  bool exhausted() const { return curr >= end; }
  void advance() { ++curr; }
  const SortedDeletes::Entry& current() const { return *curr; }
};

struct DeletesCursorGreater {
  bool operator()(const DeletesCursor& a, const DeletesCursor& b) const {
    return a.current() > b.current();  // min-heap: greater means lower priority
  }
};

/// K-way merge of pre-sorted entry spans into a single sorted, deduped vector.
/// Drops unversioned adds (version==0). Deduplicates by id, keeping highest version.
/// If there's only one input span, returns it directly (no allocation).
SortedDeletes::EntrySpan mergeDeleteSpans(
    std::span<const SortedDeletes::EntrySpan> spans,
    std::vector<SortedDeletes::Entry>& out) {

  using Entry = SortedDeletes::Entry;

  if (spans.empty()) return {};
  if (spans.size() == 1) return spans[0];

  // Build cursors for non-empty spans
  boost::container::small_vector<DeletesCursor, 8> cursors;
  size_t total = 0;
  for (auto& span : spans) {
    if (!span.empty()) {
      cursors.push_back({span.data(), span.data() + span.size()});
      total += span.size();
    }
  }

  if (cursors.empty()) return {};
  if (cursors.size() == 1) {
    return {cursors[0].curr, (size_t)(cursors[0].end - cursors[0].curr)};
  }

  out.reserve(total);

  // K-way merge using a min-heap of cursors
  DirectPQ<DeletesCursor, DeletesCursorGreater> pq(cursors, cursors.size());

  while (pq.size() > 0) {
    auto& top = pq.top();
    // Copy the entry - the reference into the span's backing memory remains valid after
    // advance/removeTop since entries live in detached TermValHash tables, not in the cursor.
    Entry entry = top.current();
    uint64_t bestVersion = entry.val().version;

    top.advance();
    if (top.exhausted()) {
      pq.removeTop();
    } else {
      pq.updateTop();
    }

    // Drain any duplicates with the same id from other cursors, keeping highest version
    while (pq.size() > 0 && (std::string_view)pq.top().current() == (std::string_view)entry) {
      uint64_t v = pq.top().current().val().version;
      if (v > bestVersion) bestVersion = v;

      pq.top().advance();
      if (pq.top().exhausted()) {
        pq.removeTop();
      } else {
        pq.updateTop();
      }
    }

    // version==0 marks ids indexed without overwrite - not deletes
    if (bestVersion > 0) {
      entry.val().version = bestVersion;
      out.push_back(entry);
    }
  }

  return out;
}

} // anonymous namespace

void IndexWriter::applyDeletes(std::span<SegInfo*> segs, MultiDeletesData& multiDeletesData) {
  // Collect commit-level spans
  boost::container::small_vector<SortedDeletes::EntrySpan, 4> commitSpans;
  for (const auto& sd : multiDeletesData.deletes) {
    for (auto& span : sd->lists()) {
      commitSpans.push_back(span);
    }
  }

  // Merge commit-level deletes once - reused across all segments
  std::vector<SortedDeletes::Entry> mergedBuf;
  SortedDeletes::EntrySpan commitDeletes = mergeDeleteSpans(commitSpans, mergedBuf);

  // Apply deletes to segments in parallel.  Each task writes new liveDocs
  // filenames into its own segment's unsyncedFiles (single owner per task, no
  // synchronization needed); the commit body drains unsyncedFiles into the
  // pre-manifest fsync set and clears them only after sync succeeds, so
  // liveDocs files get the same retry-on-failed-commit durability as segment
  // data and overlay files.
  oneapi::tbb::task_group tg;
  for (SegInfo* seg : segs) {

    tg.run([this, seg, commitDeletes]() {
      applyDeletes(*seg, commitDeletes);
    });
  }
  tg.wait();
}

void IndexWriter::applyDeletes(SegInfo& seg, SortedDeletes::EntrySpan commitDeletes) {
  if (commitDeletes.empty() && seg.personalDeletes.empty()) {
    return;
  }

  // Test hook: reports each segment selected for delete application this commit.
  // Called in parallel for different segments, so consumers should synchronize when applicable.
  solux::Signal::emit("deleteAppliedToSegment", (void*)(int64_t)seg.segId);

  // If this segment has personal deletes (rare - only during concurrent merges),
  // merge them with commit deletes into a combined span.
  SortedDeletes::EntrySpan deleteSpan = commitDeletes;
  boost::container::small_vector<SortedDeletes::EntrySpan, 4> allSpans;
  std::vector<SortedDeletes::Entry> segMergedBuf;

  if (!seg.personalDeletes.empty()) {
    for (const auto& personalDelete : seg.personalDeletes) {
      for (const auto& sd : personalDelete->deletes) {
        for (auto& span : sd->lists()) {
          allSpans.push_back(span);
        }
      }
    }
    if (!commitDeletes.empty()) {
      allSpans.push_back(commitDeletes);
    }
    deleteSpan = mergeDeleteSpans(allSpans, segMergedBuf);
  }

  if (deleteSpan.empty()) {
    return;
  }

  auto guard = MemPool::threadLocalPoolGuard();
  MemPool& pool = guard.pool();

  // Read the segment to find documents that should be deleted
  PostingsReader reader(dir, seg.segId);
  int32_t maxDocId = reader.maxDoc();

  // Load existing LiveDocs if they exist
  std::shared_ptr<LiveDocs> existingLiveDocs;
  if (seg.liveGen > 0) {
    existingLiveDocs = LiveDocs::create(dir, seg.segId, seg.liveGen, maxDocId);
  }

  // We'll allocate the FixedBitSet only when we find the first new delete
  std::unique_ptr<screaming::RAMFixedBitSet> liveBits;

  // currLiveBits points to the FixedBitSet that should be used to check for live documents.
  // it starts off pointing to the existing live documents, but switches to the new liveBits.
  const screaming::FixedBitSet* currLiveBits = existingLiveDocs ? &existingLiveDocs->bitset() : nullptr;
  int32_t numLiveDocs = existingLiveDocs ? existingLiveDocs->numLive() : maxDocId;
  int32_t newDeletesCount = 0;

  // Read the "id" field using TermsEnum
  FieldReader fieldReader(reader);
  if (!fieldReader.seek("id")) {
    LOG_WARN("applyDeletes: segment {} has no 'id' field", seg.segId);
    return;
  }

  SegFieldInfo idFieldInfo;
  fieldReader.readFieldInfo(idFieldInfo);
  TermsEnum termsEnum(pool, reader, idFieldInfo);

  // Also get the "_version_" field for version comparison
  FieldReader versionFieldReader(reader);
  SegFieldInfo versionFieldInfo;
  std::optional<IntColReader> versionColReader;
  if (versionFieldReader.seek("_version_")) {
    versionFieldReader.readFieldInfo(versionFieldInfo);
    versionColReader.emplace(reader, versionFieldInfo);
    assert(!versionColReader->multiValued());
  }

  // Phase 1: walk the merged delete span and collect the live candidate doc
  // for each delete entry.
  // The span is sorted by id, so we use seekForward() to scan the segment's terms
  // in order, avoiding redundant binary searches across blocks.
  // version==0 entries (non-overwrite adds) are filtered during merge, but can still
  // appear in the single-span fast path which skips the merge.
  struct DeleteCandidate {
    int32_t docId;
    uint64_t deleteVersion;
  };
  std::vector<DeleteCandidate> candidates;

  for (auto& entry : deleteSpan) {
    uint64_t deleteVersion = entry.val().version;
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
        candidates.push_back({docId, deleteVersion});
      }
    } // end if termsEnum.seek()
  }

  // Phase 2: version-gate the candidates in docId order so the _version_
  // column can be read with a single forward iterator.  The column can be
  // sparse - docs indexed with overwrite=false, or failed docs that never
  // reached their id field, have no version value - so values must be read by
  // rank, not docId.  A doc with no version value gates as version 0 and is
  // always deleted.
  std::sort(candidates.begin(), candidates.end(),
            [](const DeleteCandidate& a, const DeleteCandidate& b) { return a.docId < b.docId; });

  std::optional<IntColReader::SparseIterator> versionIter;
  if (versionColReader) {
    versionIter.emplace(*versionColReader);
  }

  int32_t foundDoc = -1;
  for (const auto& candidate : candidates) {
    uint64_t docVersion = 0;
    if (versionIter) {
      if (foundDoc < candidate.docId) {
        foundDoc = versionIter->advance(candidate.docId);
      }
      if (foundDoc == candidate.docId) {
        docVersion = (uint64_t)versionIter->value();
      }
      INDEX_TRACE("applyDeletes: found version {} for docId {} in segment {}",
               docVersion, candidate.docId, seg.segId);
    }

    if (docVersion < candidate.deleteVersion) {
      if (!liveBits) {
        liveBits = std::make_unique<screaming::RAMFixedBitSet>(maxDocId, true);

        if (existingLiveDocs) {
          const auto& existingBitset = existingLiveDocs->bitset();
          size_t wordsSize = screaming::FixedBitSet::sizeInWords(maxDocId) * sizeof(uint64_t);
          std::memcpy(liveBits->words, existingBitset.words, wordsSize);
        }
      }

      // a doc could appear under two delete entries if it indexed multiple id
      // values; guard the count against clearing the same bit twice.
      if (liveBits->get(candidate.docId)) {
        INDEX_TRACE("applyDeletes: marking docId {} as deleted in segment {}",
                 candidate.docId, seg.segId);
        liveBits->clear(candidate.docId);
        newDeletesCount++;
      }
    }
  }

  // If we found any new documents to delete, write a new delete generation
  if (newDeletesCount > 0) {
    auto newLiveGen = seg.liveGen + 1;
    numLiveDocs -= newDeletesCount;

    bool success = LiveDocsWriter::writeLiveDocs(dir, seg.segId, newLiveGen, *liveBits, maxDocId, numLiveDocs, seg.unsyncedFiles);
    if (!success) {
      LOG_ERROR("Failed to write liveDocs file for segment {} with liveGen {}", seg.segId, newLiveGen);
      return;
    }

    INDEX_DEBUG("Applied {} new deletes to segment {} (new delete generation: {}, total live docs: {})",
                newDeletesCount, seg.segId, newLiveGen, numLiveDocs);

    {
      const std::lock_guard<std::mutex> lock(indexMutex);
      seg.liveDocs = numLiveDocs;
      assert(seg.liveGen + 1 == newLiveGen);
      seg.liveGen = newLiveGen;
    }
  }
}

} // end namespace solux
