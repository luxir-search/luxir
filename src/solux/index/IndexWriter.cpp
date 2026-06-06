#include "IndexWriter.h"

#include <algorithm>

#include <boost/sort/spreadsort/string_sort.hpp>
#include <boost/unordered/unordered_flat_set.hpp>
#include <oneapi/tbb/task_group.h>
#include "solux/store/OutputStream.h"
#include "solux/store/InputStream.h"
#include "LiveDocsWriter.h"
#include "solux/schema/Schema.h"

#include "protos/solux_types.pb.h"
#include <google/protobuf/io/coded_stream.h>
#include <google/protobuf/io/zero_copy_stream_impl_lite.h>
#include "solux/util/heap.h"
#include "solux/util/Signal.h"
#include "solux/util/thread.h"
#include "SegmentMerger.h"
#include "VectorIndexBuilder.h"
#include "solux/reader/TestOverlayAuxReader.h"


namespace solux {


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

IndexWriter::IndexWriter(Directory& dir, std::function<std::shared_ptr<Schema>()> schemaProvider)
  : dir(dir), schemaProvider_(std::move(schemaProvider)) {
  mergePolicy = std::make_unique<MergePolicy>(*this); // defer creation until needed?
  nextCommitInfo = std::make_unique<CommitInfo>();
  std::shared_ptr<InputFile> segFile = dir.openFile(Postings::INDEX_INFO_FILE, true);
  if (segFile.get() == nullptr) {
    lastSegId = 0;
    // TODO: verify directory has no other index files? (i.e. this would tend to indicate corruption)
  }
  else {
    InputStream segmentsIs = segFile->getInputStream();

    google::protobuf::Arena arena;
    proto::IndexInfo& indexInfo = *google::protobuf::Arena::Create<proto::IndexInfo>(&arena);
    google::protobuf::io::ArrayInputStream arrayStream(segmentsIs.ptr(), segmentsIs.left());
    google::protobuf::io::CodedInputStream codedStream(&arrayStream);

    if (!indexInfo.ParseFromCodedStream(&codedStream)) {
      throw std::runtime_error("Failed to parse IndexInfo protobuf");
    }

    lastCommitTime = lastAdvertisedCommitTime = indexInfo.commit_time();
    indexGen = indexInfo.index_gen();
    coreGen = indexInfo.core_gen();
    schemaGen_ = indexInfo.schema_gen();
    updateBase = indexInfo.update_version() + 1;
    segInfos.reserve(indexInfo.segments_size());
    lastCommittedSegIds.reserve(indexInfo.segments_size());

    // TODO: maybe maintain segment order by recording ord in segments file.
    for (int i = 0; i < indexInfo.segments_size(); i++) {
      const auto& segment = indexInfo.segments(i);
      auto segId = segment.seg_id();
      lastSegId = std::max(lastSegId.load(std::memory_order::relaxed), segId);
      int32_t nDocs = segment.max_doc();
      // having ndocs in the list of segments is redundant with info in the segment itself and may be removed later.
      // for now it makes it easy to populate nDocs for merge decisions.
      auto [iter, success] = segInfos.emplace(segId, std::make_unique<SegInfo>(segId, nDocs));
      assert(success); // should be no repeated segments
      auto& seg = *iter->second;
      seg.liveGen = segment.live_gen();
      seg.minVersion = segment.min_version();
      seg.maxVersion = segment.max_version();
      seg.liveDocs = segment.live_docs();
      seg.schemaGen = segment.schema_gen();
      seg.firstCommitTime = segment.commit_time();  // firstCommitTime is stored in the segment meta.
      seg.lastCommitTime = indexInfo.commit_time(); // not stored in the segment meta, so use index meta.
      seg.auxOverlays.reserve(segment.overlays_size());
      for (int j = 0; j < segment.overlays_size(); j++) {
        seg.auxOverlays.push_back(segment.overlays(j));
        currentSegmentOverlays_.push_back(segment.overlays(j));
      }
      mergePolicy->_update(&seg);
      lastCommittedSegIds.push_back(segId);
    }
    // Sort to ensure consistent ordering for comparison (currently not needed since we sort when doing commit)
    // std::sort(lastCommittedSegIds.begin(), lastCommittedSegIds.end());

    // Pull aux indexes forward so the next commit can carry them and so we
    // know which files the previous commit referenced (for orphan cleanup).
    currentAuxIndexes_.reserve(indexInfo.aux_indexes_size());
    for (int i = 0; i < indexInfo.aux_indexes_size(); i++) {
      currentAuxIndexes_.push_back(indexInfo.aux_indexes(i));
    }
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
    [this](UpdateMessage* msg) -> size_t {
      INDEX_DEBUG("updateSequencerNode: msg={} updateVersion={}", (void*)msg, msg->updateVersion);
      return msg->updateVersion - this->updateBase - 1; // get a 0 based sequence number for the sequencer node;
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
      try {
        this->finishCommitBody(*msg);
      }
      catch (std::exception& e) {
        LOG_ERROR("finishCommitBody Exception Caught: exception={}",
          e.what());
        try {
          msg->result.setException(e);
          msg->done(*this);
        }
        catch (std::exception& e2) {
          LOG_ERROR("finishCommitBody Exception Caught in error handling cleanup done: exception={}",
            e2.what());
          // we can't do much here, just log it.
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
      try {
        this->mergeSegmentsBody(*msg);
      } catch (std::exception& e) {
        LOG_ERROR("mergeSegmentsNode Exception Caught: exception={}", e.what());
        try {
          msg->result.setException(e);
          msg->done(*this);  // TODO: could exception have happened after commit was requested (and hence I shouldn't call done here?)
        } catch (std::exception& e2) {
          LOG_ERROR("mergeSegmentsNode Exception Caught in error handling cleanup done: exception={}", e2.what());
          // we can't do much here, just log it.
        }
      }
    });
}

IndexWriter::~IndexWriter() {
  // without this, in gcc release mode we can get a crash when the IndexWriter is destroyed, even when
  // the graph wasn't used. Presumably because the test was so fast and there was some async initialization
  // of the graph still going on?
  updateGraph.wait_for_all();
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
  inverter->updateVersions(updateVersion);
  return *inverter;
}


void IndexWriter::releaseInverter(Inverter& inverter, bool flush) {
  const std::lock_guard<std::mutex> lock(indexMutex);
  auto it = busyInverters.find(&inverter);
  if (it == busyInverters.end()) {
    LOG_ERROR("Inverter not found in busy list.");
    assert(false); // should never happen
  }

  // TODO: update and check global statistics
  // TODO: if the inverter is over a certain size, flush it

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
      callback();
      delete this;
    }
  };

  UpdateMessageWithCallback* updateMessage = new UpdateMessageWithCallback();
  updateMessage->commit = commitType;
  updateMessage->callback = std::move(callback);
  auto success = submitUpdate(updateMessage);
  assert(success);
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
  bool success = submitUpdate(&updateMessage);
  assert(success);

  updateMessage.blocker.wait();
  INDEX_DEBUG("SYNC_COMMIT_END: msg={}", (void*)&updateMessage);
}


void IndexWriter::initiateCommit(UpdateMessage& msg) {
  INDEX_DEBUG("initiateCommit: msg={} STARTING", (void*)&msg);

  {
    const std::lock_guard<std::mutex> lock(indexMutex);

    // Grab the global commit info and move it to the UpdateMessage.
    msg.commitInfo = std::move(nextCommitInfo);
    nextCommitInfo = std::make_unique<CommitInfo>();
    auto& commitInfo = *msg.commitInfo;
    commitInfo.updateMessage = &msg; // set the update message that triggered this commit

    // Register with the merge gate.  Invariant: every member of
    // waitingForMerges has +1 on leftToFlush while a merge is running.
    // If we're joining mid-merge, self-bump so the merge tail's decrement
    // walk picks us up.
    if (msg.waitForMerges) {
      waitingForMerges.push_back(&msg);
      if (mergePolicy->mergeRunning) {
        commitInfo.leftToFlush++;
      }
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
          LOG_ERROR("Idle inverter is part of a commit.");
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

    // Normally a commit would be kicked off by the last segment flushing.  But if there are no segments to flush,
    // we need to kick it off here.
    if (commitInfo.leftToFlush == 0) {
      _releaseToCommitSequencer(&msg);
    }
  } // end mutex protected section
}

// Caller must hold indexMutex.
void IndexWriter::_releaseToCommitSequencer(UpdateMessage* msg) {
  if (msg->waitForMerges) {
    auto it = std::find(waitingForMerges.begin(), waitingForMerges.end(), msg);
    if (it != waitingForMerges.end()) {
      waitingForMerges.erase(it);
    }
  }
  commitSequencerNode->try_put(msg);
}

// Inverter for the segment should already be in the flushingInverters list.
// This is called in parallel.  The inverter will be deleted.
void IndexWriter::segmentFlushBody(Inverter& inverter) {
  INDEX_DEBUG("segmentFlushBody: inverter={} commitInfo={} msg.leftToFlush={}", inverter,
              (void*)inverter.commitInfo,
              inverter.commitInfo == nullptr ? -1 : inverter.commitInfo->leftToFlush);

  std::vector<std::string> flushedFiles;
  bool success;
  try {
    // uncomment to serialize inverter flushing (for testing purposes)
    // const std::lock_guard<std::mutex> lock(indexMutex);
    success = inverter.flush(&flushedFiles);
  }
  catch (std::exception& e) {
    LOG_ERROR("Exception caught while flushing inverter: {}", e.what());
    // Now what?  This is pretty catastrophic.
  }

  auto segInfo = std::make_unique<SegInfo>(inverter.getPostingsWriter().segId,
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
    // liveDocs == 0 (empty segment) should be dropped later during commit.
    // We still need to carry over deletes-by-string-id
  } else {
    segInfo->liveDocs = segInfo->maxDoc;
  }

  std::unique_ptr<Inverter> inverterPtr;

  {
    const std::lock_guard<std::mutex> lock(indexMutex);
    INDEX_DEBUG("segmentFlushBody: inverter {} flushed. Adding {}", (void*)&inverter, *segInfo);

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
    if (inverter.hasDeletions()) {
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
      if (--inverter.commitInfo->leftToFlush == 0) {
        _releaseToCommitSequencer(inverter.commitInfo->updateMessage);
      }
    }
  }

  // the inverter (inverterPtr) should go out of scope and be deleted at this point
}

// This applies deletes and writes out the new segments file.
// called from the commitFinishNode which has concurrency==1 (single-threaded)
void IndexWriter::finishCommitBody(UpdateMessage& msg) {
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
    applyDeletes(segsToApplyDeletes, commitInfo.multiDeletesData, filesToSync);
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
  // Sort segsToKeep by segId now (rather than later in writeIndexInfoFile) so
  // every commit-stage step - buildAuxIndexes, IndexInfo serialization, and
  // future query-time derivation of FAISS-id -> segment mapping - sees the
  // same canonical segment order.
  std::sort(segsToKeep.begin(), segsToKeep.end(),
            [](const SegInfo* a, const SegInfo* b) { return a->segId < b->segId; });

  // Collect unsynced segment data files from segments being committed for the first time.
  for (auto seg : segsToKeep) {
    if (seg->firstCommitTime == 0 && !seg->unsyncedFiles.empty()) {
      filesToSync.insert(filesToSync.end(),
                         std::make_move_iterator(seg->unsyncedFiles.begin()),
                         std::make_move_iterator(seg->unsyncedFiles.end()));
      seg->unsyncedFiles.clear();
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

  msg.done(*this); // don't access msg after this point, it is now invalid.

  // handling deletions should probably be done asynchronously elsewhere,
  // but we'll just do it here for now.

  // Delete segment files only after the IndexInfo file is written.
  tryDeleteSegments();
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
std::vector<proto::AuxIndexInfo> IndexWriter::buildAuxIndexes(
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
  std::vector<proto::AuxIndexInfo> carried;
  carried.reserve(currentAuxIndexes_.size());
  for (const auto& prev : currentAuxIndexes_) {
    if (prev.kind() == VectorIndexBuilder::KIND) {
      continue;
    }
    if (prev.built_core_gen() == 0 || prev.built_core_gen() == newCoreGen) {
      carried.push_back(prev);
    }
  }

  return carried;
}

void IndexWriter::buildSegmentOverlays(const UpdateMessage& msg,
                                       std::span<SegInfo*> segsToKeep,
                                       std::vector<std::string>& outFilesToSync) {
  if (segsToKeep.empty()) {
    return;
  }
  assert(msg.commitInfo && msg.commitInfo->indexGen > 0);

  auto selectorMatches = [](const std::vector<std::string>& selectors, std::string_view name) {
    for (const auto& s : selectors) {
      if (s == name) return true;
    }
    return false;
  };

  if (selectorMatches(msg.buildAuxIndexes, TestOverlayAuxReader::NAME)) {
    for (size_t i = 0; i < segsToKeep.size(); i++) {
      auto* seg = segsToKeep[i];
      bool exists = false;
      for (const auto& overlay : seg->auxOverlays) {
        if (overlay.kind() == TestOverlayAuxReader::KIND
            && overlay.name() == TestOverlayAuxReader::NAME) {
          exists = true;
          break;
        }
      }
      if (exists) continue;

      std::string fileName = Postings::getAuxIndexFileName(
        TestOverlayAuxReader::NAME, msg.commitInfo->indexGen, (uint32_t)i);
      {
        auto file = dir.createFile(fileName);
        OutputStream os;
        os.setFile(&*file);
        static constexpr std::string_view payload = "solux test overlay\n";
        os.write(payload.data(), payload.size());
        os.close();
        dir.finishFile(*file);
      }
      outFilesToSync.push_back(fileName);

      proto::AuxIndexInfo info;
      info.set_kind(std::string(TestOverlayAuxReader::KIND));
      info.set_name(std::string(TestOverlayAuxReader::NAME));
      info.set_gen(msg.commitInfo->indexGen);
      info.add_files(fileName);
      info.set_opaque_meta("test");
      seg->auxOverlays.push_back(std::move(info));
    }
  }

  std::vector<std::string> vectorSelectors;
  boost::unordered_flat_set<std::string> seenVectorSelectors;
  for (const auto& selector : msg.buildAuxIndexes) {
    if (selector == "*" || selector.starts_with(VectorIndexBuilder::NAME_PREFIX)) {
      if (seenVectorSelectors.emplace(selector).second) {
        vectorSelectors.push_back(selector);
      }
    }
  }

  if (vectorSelectors.empty()) {
    for (const auto& overlay : currentSegmentOverlays_) {
      if (overlay.kind() != VectorIndexBuilder::KIND) continue;
      if (seenVectorSelectors.emplace(overlay.name()).second) {
        vectorSelectors.emplace_back(overlay.name());
      }
    }
  }

  if (vectorSelectors.empty() || !schemaProvider_) {
    return;
  }
  auto schema = schemaProvider_();
  if (!schema) {
    return;
  }

  std::vector<std::shared_ptr<PostingsReader>> prHolders;
  prHolders.reserve(segsToKeep.size());
  for (size_t i = 0; i < segsToKeep.size(); i++) {
    auto* seg = segsToKeep[i];
    boost::unordered_flat_set<std::string> skipNames;
    for (const auto& overlay : seg->auxOverlays) {
      if (overlay.kind() == VectorIndexBuilder::KIND) {
        skipNames.emplace(overlay.name());
      }
    }

    auto pr = seg->sharedPostingsReader.load();
    if (!pr) {
      pr = std::make_shared<PostingsReader>(dir, seg->segId);
      seg->sharedPostingsReader.store(pr);
    }
    VectorIndexBuilder::SegInput input{seg->segId, pr.get()};
    prHolders.push_back(pr);

    VectorIndexBuilder vb(dir, std::span<const VectorIndexBuilder::SegInput>(&input, 1),
                          *schema, msg.commitInfo->indexGen, msg.commitInfo->coreGen,
                          (uint32_t)i);
    auto newlyBuilt = vb.build(vectorSelectors, skipNames, outFilesToSync);
    for (auto& info : newlyBuilt) {
      seg->auxOverlays.push_back(std::move(info));
    }
  }
}

std::vector<proto::AuxIndexInfo> IndexWriter::flattenSegmentOverlays(std::span<SegInfo*> segs) const {
  std::vector<proto::AuxIndexInfo> out;
  size_t total = 0;
  for (auto* seg : segs) total += seg->auxOverlays.size();
  out.reserve(total);
  for (auto* seg : segs) {
    for (const auto& overlay : seg->auxOverlays) {
      out.push_back(overlay);
    }
  }
  return out;
}

// After a successful IndexInfo write, delete files referenced by the previous
// aux index list that aren't referenced by the new one.  Carried-forward
// entries appear in both lists, so their files survive.  Files written by a
// rebuild for a given name supersede files from the previous build of the same
// name and the old ones get cleaned up here.
void IndexWriter::deleteOrphanedAuxFiles(const std::vector<proto::AuxIndexInfo>& oldList,
                                         const std::vector<proto::AuxIndexInfo>& newList) {
  boost::unordered_flat_set<std::string> keep;
  for (const auto& info : newList) {
    for (const auto& f : info.files()) keep.emplace(f);
  }
  for (const auto& info : oldList) {
    for (const auto& f : info.files()) {
      std::string fname(f);
      if (!keep.contains(fname)) {
        INDEX_DEBUG("deleteOrphanedAuxFiles: deleting {}", fname);
        dir.deleteFile(fname);
      }
    }
  }
}


// This is only called from the finishCommit node, which has concurrency==1 (single-threaded)
// hence we only need to protect against changes in the segInfos map, not multiple invocations of this method.
// The passed span of segments may be reordered after this is finished.
void IndexWriter::writeIndexInfoFile(std::span<SegInfo*> segs, CommitInfo* commitInfo,
                                     std::span<const proto::AuxIndexInfo> auxIndexes) {
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
    updateVersion = commitInfo->updateMessage->updateVersion;
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
    updateVersion = updateBase + 1;
  }

  // Build the protobuf message
  google::protobuf::Arena arena;
  proto::IndexInfo& indexInfo = *google::protobuf::Arena::Create<proto::IndexInfo>(&arena);
  indexInfo.set_commit_time(now_us);
  indexInfo.set_version(1);
  indexInfo.set_index_gen(thisIndexGen);
  indexInfo.set_update_version(updateVersion);
  indexInfo.set_core_gen(coreGen);
  indexInfo.set_schema_gen(currentSchemaGen());
  indexInfo.mutable_segments()->Reserve(segs.size());

  for (auto seg : segs) {
    // Update the commit time for the seg. Important to know if this seg is part of the last commit.
    // This does mean that this may be visible before the commit is done and before lastCommitTime is updated.
    // Any comparison with lastCommitTime should be done with this in mind.
    seg->lastCommitTime = now_us;
    if (seg->firstCommitTime == 0) {
      seg->firstCommitTime = now_us;
    }

    auto* segmentInfo = indexInfo.add_segments();
    segmentInfo->set_seg_id(seg->segId);
    segmentInfo->set_max_doc(seg->maxDoc);
    segmentInfo->set_live_gen(seg->liveGen);
    segmentInfo->set_min_version(seg->minVersion);
    segmentInfo->set_max_version(seg->maxVersion);
    segmentInfo->set_commit_time(seg->firstCommitTime);
    segmentInfo->set_live_docs(seg->liveDocs);
    segmentInfo->set_schema_gen(seg->schemaGen);
    for (const auto& overlay : seg->auxOverlays) {
      auto* dst = segmentInfo->add_overlays();
      *dst = overlay;
      if (dst->commit_time() == 0) {
        dst->set_commit_time(now_us);
      }
    }

    numDocs += seg->maxDoc;
    INDEX_DEBUG("\t{}", *seg);
  }

  // Carry over aux indexes built during this commit.
  for (const auto& aux : auxIndexes) {
    auto* dst = indexInfo.add_aux_indexes();
    *dst = aux;
    if (dst->commit_time() == 0) {
      dst->set_commit_time(now_us);
    }
  }

  // Serialize the protobuf message - TODO: hook into other serialization methods to avoid string
  std::string serialized;
  serialized.reserve(200 + segs.size() * 24);
  if (!indexInfo.SerializeToString(&serialized)) {
    throw std::runtime_error("Failed to serialize IndexInfo protobuf");
  }

  indexOut.write(serialized.data(), serialized.size());
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


// called from the mergeSegmentsNode which has concurrency==1 (single-threaded)
// Only one merge will be running at a time.
// We do run concurrently with everything else, including commits and segment deletions.
// So we don't delete segments that the commit code is about to use, the commit code marks
// those segments.
// We also mark segments that are going to be merged, so the commit code knows about them.
void IndexWriter::mergeSegmentsBody(MergeMessage& msg) {
  std::vector<SegInfo*> segs;
  segs.reserve(mergePolicy->mergeFactor * 2);

  // We grab the list of segments to merge with the lock held, but use them outside of the lock.
  // This is safe since the only place where segments are deleted is in a merge, and this has concurrency==1
  {
    const std::lock_guard<std::mutex> lock(indexMutex);
    for (auto& [segId, seg] : segInfos) {
      if (seg->mergeLevel == msg.mergeLevel || msg.maxSegments == 1) {
        if (seg->liveDocs == 0) {
          // it's possible to see this right after commit handler applied deletes, but before it has a chance to delete the segment.
          continue;
        }
        segs.push_back(seg.get());
        INDEX_DEBUG("mergeSegmentsBody: will merge {}", *segs.back());

        // mark as being merged so they won't be deleted - see finishCommitBody
        // they could still be removed from segMap and moved to the deleteList however.
        segs.back()->merging = true;

        // Record what liveGen we are going to use for this merge.  It's important to set up-front
        // with the index lock held to avoid races with the commit code.
        segs.back()->mergedLiveGen = segs.back()->liveGen;
      }
    }

    // Sanity check this merge. a bug in testDeleteAllData led to merge accounting getting out-of-sync
    // with actual segments and resulted in a merge loop.
    mergePolicy->_sanityCheck();
  }
  if (segs.empty()) {
    // This is possible if a merge was correctly triggered, but all of the segments were deleted.
    INDEX_DEBUG("mergeSegmentsBody: no segments to merge for msg={}", (void*)&msg);
    // Nothing merged, so no synthetic commit needed - but we still need to
    // balance the gate: clear mergeRunning, chain a follow-up merge if some
    // other level is now full, and decrement leftToFlush on every member of
    // waitingForMerges to undo our start-bump.  Same ordering as the normal
    // merge tail: chain check before decrement, so a chain re-bump can keep
    // a commit waiting.
    {
      const std::lock_guard<std::mutex> lock(indexMutex);
      mergePolicy->mergeRunning = false;
      mergePolicy->_maybeMergeSegments(nullptr);
      std::vector<UpdateMessage*> toRelease;
      for (auto* waitingMsg : waitingForMerges) {
        assert(waitingMsg->commitInfo);
        if (--waitingMsg->commitInfo->leftToFlush == 0) {
          toRelease.push_back(waitingMsg);
        }
      }
      for (auto* releasedMsg : toRelease) {
        _releaseToCommitSequencer(releasedMsg);
      }
    }
    msg.done(*this);
    return; // nothing to merge
  }

  solux::Signal::emit("mergeStart", (void*)(int64_t)msg.mergeLevel, (void*)segs.size());

  // Sort the list of segments by the segId.
  // Some tests rely on not reordering segments.
  std::sort(segs.begin(), segs.end(), [](const SegInfo* a, const SegInfo* b) {
    return a->segId < b->segId;
  });


  {
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
          // TODO: need to retry... liveGen may have changed
        }
        liveDocsVec.push_back(liveDocs);
        liveDocsPtrs.push_back(liveDocs.get());
      } else {
        // No deletes for this segment
        liveDocsVec.push_back(nullptr);
        liveDocsPtrs.push_back(nullptr);
      }
    }
    
    // Copy the preaders to a vector of pointers for our underlying merge code.
    std::vector<PostingsReader*> preaderPtrs;
    preaderPtrs.reserve(preaders.size());
    for (auto& preader : preaders) {
      preaderPtrs.push_back(preader.get());
    }

    PostingsWriter pwriter(dir, ++lastSegId);

    // Do the actual merge.
    // TODO: catch any errors and restore state / clean up.
    SegmentMerger merger(preaderPtrs, liveDocsPtrs, pwriter);
    merger.merge();

    // Create the new SegInfo for the output segment.
    auto newSegInfo = std::make_unique<SegInfo>(pwriter.getSegId(), pwriter.getMaxDoc());
    newSegInfo->schemaGen = currentSchemaGen();
    pwriter.finish(&newSegInfo->unsyncedFiles);

    // Move old segments to the delete list and add the new segment info.
    std::vector<std::unique_ptr<SegInfo>> toDeleteSegs;
    {
      const std::lock_guard<std::mutex> lock(indexMutex);
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

      // add new segInfo to the index if it has any docs.
      if (newSegInfo->liveDocs > 0) {
        mergePolicy->_update(newSegInfo.get());
        segInfos.emplace(pwriter.getSegId(), std::move(newSegInfo));
      }
    } // end index lock

    // This races with the commit code (see comments in finishCommitBody()).
    // tryDeleteSegments(); // try to delete segments that are now empty
  } // end of scope for preaders, pool, and pwriter


  // TODO: if merge was kicked off and no more commits are in the pipeline, should we submit
  // one ourselves so the new segment is visible?
  // We could look to see if there are any busy inverters - if so, indexing is still happening.
  // Also maybe only do if there are no more merges.



  // Coordinate merge teardown with the commit-with-aux gate.  Order under
  // indexMutex matters:
  //   1. mergeRunning = false.
  //   2. Possibly chain a follow-up merge.  This re-sets mergeRunning and
  //      bumps leftToFlush on every member of waitingForMerges.
  //   3. Decrement leftToFlush on every member of waitingForMerges to undo
  //      our own bump, collecting any that hit zero.
  // Doing (2) before (3) is the "wait for the merge wave" semantic: if a
  // chain re-bumps before we decrement, net change is zero and the commit
  // keeps waiting; if no chain, the decrement may release.
  bool triggerCommit = false;
  {
    const std::lock_guard<std::mutex> lock(indexMutex);
    mergePolicy->mergeRunning = false;
    auto anotherMerge = mergePolicy->_maybeMergeSegments(nullptr);

    std::vector<UpdateMessage*> toRelease;
    for (auto* waitingMsg : waitingForMerges) {
      assert(waitingMsg->commitInfo);
      if (--waitingMsg->commitInfo->leftToFlush == 0) {
        toRelease.push_back(waitingMsg);
      }
    }

    // Synthetic commit only fires when nothing else will publish the
    // merged segment.  Skip it if any commit-with-aux is in waitingForMerges
    // (those commits will publish the merged segment via their own
    // IndexInfo write and would otherwise race with a synthetic on a
    // coreGen bump).  toRelease members are still in waitingForMerges at
    // this point so the empty check covers them.
    if (!anotherMerge && waitingForMerges.empty()) {
      if (busyInverters.empty() && flushingInverters.empty() && idleInverters.empty()) {
        // no indexing activity, so let's trigger a commit.
        triggerCommit = true;
      }
    }

    for (auto* releasedMsg : toRelease) {
      _releaseToCommitSequencer(releasedMsg);
    }
  }

  if (triggerCommit) {
    // send a commit message to force a commit.
    class CommitMessage : public UpdateMessage {
    public:
      MergeMessage* origMessage;
      void handle(IndexWriter& iw) override {
        unused(iw);
      }

      void done(IndexWriter& iw) override {
        unused(iw);
        origMessage->done(iw);
        delete this;
      }
    };

    CommitMessage* commitMessage = new CommitMessage();
    commitMessage->commit = UpdateMessage::COMMIT;
    commitMessage->origMessage = &msg;
    INDEX_DEBUG("mergeSegmentsBody: requesting commit. msg={}", (void*)commitMessage);
    this->submitUpdate(commitMessage);
  }
  else {
    INDEX_DEBUG(
      "mergeSegmentsBody: merge done, but not triggering commit since there are busy, flushing, or idle inverters.");
  }

  if (!triggerCommit) {
    msg.done(*this);
  }
}

// This is currently for tests only and blocks until all indexing activity has ceased!
void IndexWriter::mergeSegments() {
  class BlockingMergeMessage : public MergeMessage {
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
  BlockingMergeMessage mergeMessage;
  mergeMessage.maxSegments = 1;

  mergeSegmentsNode->try_put(&mergeMessage);

  // If merge code decides to commit, this call back won't be done until the commit is finished.
  mergeMessage.blocker.wait();
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
void IndexWriter::testDeleteAllData() {
  INDEX_DEBUG("testDeleteAllData: deleting all data.");
  // wait for things in the execution graph to finish.
  updateGraph.wait_for_all();

  {
    const std::lock_guard<std::mutex> lock(indexMutex);
    // check if there are any unflushed segments
    // TODO: just dropping the segments may not be safe in the future, it may leave stuff around in the directory
    // (or even open files in the future).  We should probably do a commit first before we drop?
    if (!busyInverters.empty() || !flushingInverters.empty()) {
      LOG_ERROR("Error trying to clear index. There are busy or flushing inverters!");
      return;
    }
    if (mergePolicy && mergePolicy->mergeRunning) {
      LOG_ERROR("Error trying to clear index. There is a merge running!");
      return;
    }

    // dump the current IndexReader
    {
      const std::lock_guard<std::mutex> lock(indexReaderMutex);
      indexReader.reset();
    }

    // drop all idle inverters (unflushed segments)
    idleInverters.clear();

    // drop all segments
    segInfos.clear();

    // drop segments to delete
    segmentsToDelete.clear();

    // drop all index files
    dir.clear();

    // Hmmm, what about coreGen, indexGen, and lastSegId?
    // If we use coreGen or indexGen as cache keys, we shouldn't start over.  We could also mix in the
    // commitTime of the first or last segment to make sure it's the same index.
    lastSegId = 0;
    indexGen = 0;
    coreGen = 0;
    lastCommittedSegIds.clear();
    currentAuxIndexes_.clear();
    currentSegmentOverlays_.clear();
    nextCommitInfo = std::make_unique<CommitInfo>();

    lastCommitTime = lastAdvertisedCommitTime = 0;
  }
  mergePolicy->refresh(); // we can't call this with lock held since it tries to acquire.




  // don't touch commitNumber or updateNumber... the TBB graph relies on the exact sequence of numbers.
}

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
  std::erase_if(overlays, [&](const proto::AuxIndexInfo& info) {
    return info.name() == name;
  });
  if (overlays.size() == oldSize) return false;
  currentSegmentOverlays_ = flattenSegmentOverlays(std::span<SegInfo*>(ordered.data(), ordered.size()));
  return true;
}

// TEST CODE
void IndexWriter::debugInfo() {
  {
    std::lock_guard<std::mutex> lock(indexMutex);
    LOG_INFO("IndexWriter: segInfos.size={} idleInverters.size={} busyInverters.size={} flushingInverters.size={}",
             segInfos.size(), idleInverters.size(), busyInverters.size(), flushingInverters.size());
    LOG_INFO("\tupdateNumber={} commitNumber={} lastCommitTime={} lastAdvertisedCommitTime={}",
             updateNumber, commitNumber, lastCommitTime.load(), lastAdvertisedCommitTime.load());
    LOG_INFO("\tmergePolicy->mergeRunning={}", mergePolicy->mergeRunning);
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

void IndexWriter::applyDeletes(std::span<SegInfo*> segs, MultiDeletesData& multiDeletesData, std::vector<std::string>& filesToSync) {
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

  // Apply deletes to segments in parallel. Each task accumulates into its own
  // local vector so the inner path doesn't need synchronization, then we splice
  // results under a mutex once per segment.
  std::mutex filesToSyncMutex;
  oneapi::tbb::task_group tg;
  for (SegInfo* seg : segs) {
    tg.run([this, seg, commitDeletes, &filesToSync, &filesToSyncMutex]() {
      std::vector<std::string> localFiles;
      applyDeletes(*seg, commitDeletes, localFiles);
      if (!localFiles.empty()) {
        const std::lock_guard<std::mutex> lock(filesToSyncMutex);
        filesToSync.insert(filesToSync.end(),
                           std::make_move_iterator(localFiles.begin()),
                           std::make_move_iterator(localFiles.end()));
      }
    });
  }
  tg.wait();
}

void IndexWriter::applyDeletes(SegInfo& seg, SortedDeletes::EntrySpan commitDeletes, std::vector<std::string>& filesToSync) {
  if (commitDeletes.empty() && seg.personalDeletes.empty()) {
    return;
  }

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
  FieldReader fieldReader(pool, reader);
  if (!fieldReader.seek("id")) {
    LOG_WARN("applyDeletes: segment {} has no 'id' field", seg.segId);
    return;
  }

  SegFieldInfo idFieldInfo;
  fieldReader.readFieldInfo(idFieldInfo);
  TermsEnum termsEnum(pool, reader, idFieldInfo);

  // Also get the "_version_" field for version comparison
  FieldReader versionFieldReader(pool, reader);
  SegFieldInfo versionFieldInfo;
  bool hasVersionField = false;
  std::optional<IntColReader> versionColReader;
  std::optional<IntColReader::DenseValues> versionValues;
  if (versionFieldReader.seek("_version_")) {
    versionFieldReader.readFieldInfo(versionFieldInfo);
    hasVersionField = true;
    versionColReader.emplace(reader, versionFieldInfo);
    assert(!versionColReader->multiValued());
    versionValues.emplace(*versionColReader);
  }

  // Apply the merged delete span to this segment.
  // The span is sorted by id, so we use seekForward() to scan the segment's terms
  // in order, avoiding redundant binary searches across blocks.
  // version==0 entries (non-overwrite adds) are filtered during merge, but can still
  // appear in the single-span fast path which skips the merge.
  for (auto& entry : deleteSpan) {
    uint64_t deleteVersion = entry.val().version;
    if (deleteVersion == 0) continue;
    std::string_view deleteId = (std::string_view)entry;

    INDEX_TRACE("applyDeletes: looking up term '{}' with version {} in segment {}",
             deleteId, deleteVersion, seg.segId);

    if (termsEnum.seekForward(deleteId)) {
      // Found the ID term, now get documents containing this ID
      DocsEnum docsEnum(pool, reader, termsEnum);

      for (int32_t docId = docsEnum.next(); docId != DocsEnum::END; docId = docsEnum.next()) {
        if (currLiveBits && !currLiveBits->get(docId)) {
          continue;
        }

        bool shouldDelete = true;

        if (hasVersionField) {
          uint64_t docVersion = (uint64_t)versionValues->valueAt(docId);

          INDEX_TRACE("applyDeletes: found version {} for docId {} in segment {}",
                   docId, docVersion, seg.segId);

          shouldDelete = (docVersion < deleteVersion);
        }

        if (shouldDelete) {
          if (!liveBits) {
            liveBits = std::make_unique<screaming::RAMFixedBitSet>(maxDocId, true);

            if (existingLiveDocs) {
              const auto& existingBitset = existingLiveDocs->bitset();
              size_t wordsSize = screaming::FixedBitSet::sizeInWords(maxDocId) * sizeof(uint64_t);
              std::memcpy(liveBits->words, existingBitset.words, wordsSize);
            }

            currLiveBits = liveBits.get();
          }

          INDEX_TRACE("applyDeletes: marking docId {} as deleted in segment {}",
                   docId, seg.segId);
          liveBits->clear(docId);
          newDeletesCount++;
        }
      }
    } // end if termsEnum.seek()
  }

  // If we found any new documents to delete, write a new delete generation
  if (newDeletesCount > 0) {
    auto newLiveGen = seg.liveGen + 1;
    numLiveDocs -= newDeletesCount;

    bool success = LiveDocsWriter::writeLiveDocs(dir, seg.segId, newLiveGen, *liveBits, maxDocId, numLiveDocs, filesToSync);
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
