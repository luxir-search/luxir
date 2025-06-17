#include "IndexWriter.h"

#include "solux/store/OutputStream.h"
#include "solux/store/InputStream.h"

#include "protos/solux_types.pb.h"
#include <google/protobuf/io/coded_stream.h>
#include <google/protobuf/io/zero_copy_stream_impl_lite.h>
#include "solux/util/heap.h"
#include "solux/util/Signal.h"
#include "solux/util/thread.h"
#include "SegmentMerger.h"


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

IndexWriter::IndexWriter(Directory &dir) : dir(dir) {
  mergePolicy = std::make_unique<MergePolicy>(*this);  // defer creation until needed?
  nextCommitInfo = std::make_unique<CommitInfo>();
  std::shared_ptr<InputFile> segFile = dir.openFile(Postings::INDEX_INFO_FILE);
  if (segFile.get() == nullptr) {
    lastSegId = 0;
    // TODO: verify directory has no other index files? (i.e. this would tend to indicate corruption)
  } else {
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
    updateBase = indexInfo.update_version() + 1;
    segInfos.reserve(indexInfo.segments_size());

    // TODO: maybe maintain segment order by recording ord in segments file.
    for (int i = 0; i < indexInfo.segments_size(); i++) {
      const auto& segment = indexInfo.segments(i);
      auto segId = segment.seg_id();
      lastSegId = std::max(lastSegId.load(std::memory_order::relaxed), segId);
      int32_t nDocs = segment.max_doc();
      // having ndocs in the list of segments is redundant with info in the segment itself and may be removed later.
      // for now it makes it easy to populate nDocs for merge decisions.
      auto [iter, success] = segInfos.emplace(segId, std::make_unique<SegInfo>(segId, nDocs));
      assert(success);  // should be no repeated segments
      auto& seg = *iter->second;
      seg.liveGen = segment.live_gen();
      seg.minVersion = segment.min_version();
      seg.maxVersion = segment.max_version();
      seg.liveDocs = segment.live_docs();
      seg.commitTime = indexInfo.commit_time();
      mergePolicy->_update(&seg);
    }
  }

  // Create the updateGraph.  Start with a serial node that assigns an order to each update command
  // This could be done in a quick synchronized block instead... and could then be safely inspected by the client
  // if necessary before submitting to the actual processing graph.
  startUpdateNode = std::make_unique<UpdateMessageFunc>(updateGraph, 1,
    [this](UpdateMessage* msg) -> UpdateMessage* {
       this->startUpdateBody(*msg);
       return msg;
  });

  // next, the node to actually process the update, indexing documents, etc.
  processUpdateNode = std::make_unique<UpdateMessageFunc>(updateGraph, tbb::flow::unlimited,
    [this](UpdateMessage* msg) -> UpdateMessage* {
       this->processUpdateBody(*msg);
       return msg;
  });

  // then make sure that the updates are finished in order so all updates are done before a commit is processed.
  updateSequencerNode = std::make_unique<tbb::flow::sequencer_node<UpdateMessage*> >(updateGraph,
    [this](UpdateMessage* msg) -> size_t {
      INDEX_DEBUG("updateSequencerNode: msg={} updateVersion={}", (void*)msg, msg->updateVersion);
      return msg->updateVersion - this->updateBase - 1;   // get a 0 based sequence number for the sequencer node;
    });

  // updates flow into the updateFinishNode in order which is single threaded and ensures that updates are finished in order.
  updateFinishNode = std::make_unique<UpdateMessageMultiFunc>(updateGraph, 1,
    [this](UpdateMessage* msg, UpdateMessageMultiFunc::output_ports_type& op) {
       unused(op);
       this->finishUpdateBody(*msg);
       // std::get<0>(op).try_put(msg);
  });

  // connect the update nodes
  tbb::flow::make_edge(*startUpdateNode, *processUpdateNode);
  tbb::flow::make_edge(*processUpdateNode, *updateSequencerNode);
  tbb::flow::make_edge(*updateSequencerNode, *updateFinishNode);

  // now the commit nodes
  segmentFlushNode = std::make_unique<InverterMultiFunc>(updateGraph, tbb::flow::unlimited,
    [this](Inverter* inverter, InverterMultiFunc::output_ports_type& op) {
      unused(op);
      this->segmentFlushBody(*inverter);
  });

  commitSequencerNode = std::make_unique<tbb::flow::sequencer_node<UpdateMessage*> >(updateGraph,
    [](UpdateMessage* msg) -> size_t {
      INDEX_DEBUG("commitSequencerNode: msg={} commitNum={}", (void*)&msg, msg->commitNum);
      return msg->commitNum;
    });

  // must be single-threaded
  commitFinishNode = std::make_unique<UpdateMessageMultiFunc>(updateGraph, 1,
    [this](UpdateMessage* msg, UpdateMessageMultiFunc::output_ports_type& op) {
      unused(op);
      this->finishCommitBody(*msg);
       // std::get<0>(op).try_put(msg);
  });

  tbb::flow::make_edge(*commitSequencerNode, *commitFinishNode);

  // must be single-threaded
  mergeSegmentsNode = std::make_unique<MergeMessageMultiFunc>(updateGraph, 1,
    [this](MergeMessage* msg, MergeMessageMultiFunc::output_ports_type& op) {
      unused(op);
      INDEX_DEBUG("mergeSegmentsNode: msg={}", (void*)&msg, msg->commitNum);
      this->mergeSegmentsBody(*msg);
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
    auto newInverter = std::make_unique<Inverter>(dir, ++lastSegId);
    inverter = newInverter.get();
    busyInverters.emplace(inverter, std::move(newInverter));
  } else {
    auto it = idleInverters.begin();
    inverter = it->first;
    busyInverters.emplace(inverter, std::move(it->second));
    idleInverters.erase(it);
  }
  inverter->updateVersions(updateVersion);
  return *inverter;
}



  void IndexWriter::releaseInverter(Inverter& inverter) {
    const std::lock_guard<std::mutex> lock(indexMutex);
    auto it = busyInverters.find(&inverter);
    if (it == busyInverters.end()) {
      LOG_ERROR("Inverter not found in busy list.");
      assert(false);  // should never happen
    }

    // TODO: update and check global statistics
    // TODO: if the inverter is over a certain size, flush it

    // if this inverter is part of a commit, initiate a flush.
    if (inverter.commitInfo != nullptr) {
      INDEX_DEBUG("releaseInverter: inverter={} message={} triggering flush.", (void*)&inverter, (void*)inverter.commitInfo->updateMessage);
      flushingInverters.emplace(&inverter, std::move(it->second));
      it = busyInverters.erase(it);
      segmentFlushNode->try_put(&inverter);
    } else {
      INDEX_DEBUG("releaseInverter: inverter={} adding back to idleInverters.", (void*)&inverter);
      // return inverter to idle pool
      idleInverters.emplace(&inverter, std::move(it->second));
      busyInverters.erase(it);
    }
  }


  void IndexWriter::commit(std::function <void()>&& callback, UpdateMessage::CommitType commitType) {
    class UpdateMessageWithCallback : public UpdateMessage {
    public:
      std::function <void()> callback;
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

    bool success = submitUpdate(&updateMessage);
    assert(success);

    updateMessage.blocker.wait();
  }





  void IndexWriter::initiateCommit(UpdateMessage& msg) {
    INDEX_DEBUG("initiateCommit: msg={} STARTING", (void*)&msg);

    {
      const std::lock_guard<std::mutex> lock(indexMutex);

      // Grab the global commit info and move it to the UpdateMessage.
      msg.commitInfo = std::move(nextCommitInfo);
      nextCommitInfo = std::make_unique<CommitInfo>();
      auto& commitInfo = *msg.commitInfo;
      commitInfo.updateMessage = &msg;  // set the update message that triggered this commit

      // first look at any flushing inverters that are not marked for a commit yet
      // and mark them if necessary.
      for (auto it = flushingInverters.begin(); it != flushingInverters.end(); it++) {
        if (it->second->commitInfo == nullptr && it->second->minVersion) {
          INDEX_DEBUG("\tinitiateCommit: msg={} marking flushing inverter={} for commit", (void*)&msg, (void*)it->second.get());
          it->second->commitInfo = &commitInfo;
          commitInfo.leftToFlush++;
        }
      }

      // now look at all idle inverters and initiate a flush if necessary.
      for (auto it = idleInverters.begin(); it != idleInverters.end(); it++) {
        if (it->second->minVersion <= msg.updateVersion) {
          if (it->second->commitInfo == nullptr) {
            INDEX_DEBUG("\tinitiateCommit: msg={} marking idle inverter={} for commit", (void*)&msg, (void*)it->second.get());
            it->second->commitInfo = &commitInfo;
            commitInfo.leftToFlush++;
          } else {
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
          INDEX_DEBUG("\tinitiateCommit: msg={} marking busy inverter={} for commit", (void*)&msg, (void*)it->second.get());
          it->second->commitInfo = &commitInfo;
          commitInfo.leftToFlush++;
        }
      }

      INDEX_DEBUG("initiateCommit: msg={} leftToFlush={}", (void*)&msg, msg.commitInfo->leftToFlush);

      // Normally a commit would be kicked off by the last segment flushing.  But if there are no segments to flush,
      // we need to kick it off here.
      if (commitInfo.leftToFlush == 0) {
        commitSequencerNode->try_put(&msg);
      }
    } // end mutex protected section
  }

  // Inverter for the segment should already be in the flushingInverters list.
  // This is called in parallel.  The inverter will be deleted.
  void IndexWriter::segmentFlushBody(Inverter& inverter) {
    INDEX_DEBUG("segmentFlushBody: inverter={} commitInfo={} msg.leftToFlush={}", (void*)&inverter, (void*)inverter.commitInfo,
                inverter.commitInfo == nullptr ? -1 : inverter.commitInfo->leftToFlush);

    bool success;
    try {
      // uncomment to serialize inverter flushing (for testing purposes)
      // const std::lock_guard<std::mutex> lock(indexMutex);
      success = inverter.flush();
    } catch (std::exception& e) {
      LOG_ERROR("Exception caught while flushing inverter: {}", e.what());
      // Now what?  This is pretty catastrophic.
    }

    auto segInfo = std::make_unique<SegInfo>(inverter.getPostingsWriter().segId, inverter.getPostingsWriter().getMaxDoc());
    segInfo->minVersion = inverter.minVersion;
    segInfo->maxVersion = inverter.maxVersion;
    // TODO FUTURE: set liveDocs + liveGen for deleted docs from errors or overwrites in the same inverter.

    std::unique_ptr<Inverter> inverterPtr;

    bool triggerCommit = false;
    {
      const std::lock_guard<std::mutex> lock(indexMutex);
      INDEX_DEBUG("segmentFlushBody: inverter {} flushed. Adding {}", (void*)&inverter, *segInfo);

      // segments are flushed in parallel, so the segids are not in order... (or in the completed order.) should be fine.
      std::pair<SegMap::iterator, bool> iter;
      if (success) {
        iter = segInfos.emplace(segInfo->segId, std::move(segInfo));
        assert(iter.second);  // should never already exist
      }

      // remove the inverter from the flushingInverters set, but remember it until the end of this function.
      auto it = flushingInverters.find(&inverter);
      if (it == flushingInverters.end()) {
        assert(false);
      }
      inverterPtr = std::move(it->second);
      flushingInverters.erase(it);

      // The mutex protects against races in the setting of inverter.updateMessage as well as leftToFlush.
      // Higher level logical races protected against would be kicking off a segment flush and then that completing and
      // kicking off a commit before the next segment flush is kicked off.
      if (inverter.commitInfo != nullptr) {
        if (--inverter.commitInfo->leftToFlush == 0) {
          triggerCommit = true;
        }
      }

      // move any deletes from the inverter to the relevant commit info.
      if (inverter.hasDeletions()) {
        CommitInfo& commitInfo = inverter.commitInfo ? *inverter.commitInfo : *nextCommitInfo;
        INDEX_DEBUG("segmentFlushBody: inverter={} moving deletes to commitInfo={}", (void*)&inverter, (void*)&commitInfo);
        commitInfo.multiDeletesData.deletesData.emplace_back(std::move(inverter.deletesData));
      }

      // Check if we should merge anything.
      // We do this with the lock held since segInfo could go away otherwise.
      if (success) {
        mergePolicy->_maybeMergeSegments(iter.first->second.get());
      }
    }

    // It shouldn't be a big deal to do a try_put inside the sync block, but it's safe to do outside anyway.
    if (triggerCommit) {
      commitSequencerNode->try_put(inverter.commitInfo->updateMessage);
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

    {
      const std::lock_guard<std::mutex> lock(indexMutex);
      auto lastCommit = lastCommitTime.load(std::memory_order::relaxed);
      segs.reserve(segInfos.size());
      // grab all segments and mark them as being part of a commit.
      for (auto& [segId, seg] : segInfos) {
        segs.push_back(seg.get());
        seg->commitTime = lastCommit;  // IMPORTANT - for marking it in use for the last commit to prevent its deletion.

        if (!seg->personalDeletes.empty()) {
          // this segment has personal deletes that need to be applied.
          segsToApplyDeletes.push_back(seg.get());
        }
        if (seg->minVersion < maxDeleteVersion) {
          // this segment has deletes that need to be applied.
          segsToApplyDeletes.push_back(seg.get());
        }
      }
    }

    // If a segmentMerge kicks off here, it could be merging segments without deletes applied yet.

    CommitInfo& commitInfo = *msg.commitInfo;
    if (segsToApplyDeletes.empty()) {
      INDEX_DEBUG("finishCommitBody: msg={} no deletes to apply.", (void*)&msg);
    }
    else {
      // TODO: doesn't take into account personal deletes.
      INDEX_DEBUG("finishCommitBody: msg={} applying deletes to {} segments.", (void*)&msg, segs.size());
      // This is where seg.liveGen can change!
      // TODO: parallelize this.
      for (auto seg : segsToApplyDeletes) {
        applyDeletes(*seg, commitInfo.multiDeletesData);
      }
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
      std::vector<std::shared_ptr<MultiDeletesData>> personalDeletes;  // personal deletes from segments that finished merging too early.

      // Check if any segments were merged before deletes were applied.
      for (auto& seg : segs) {
        if (seg->mergedLiveGen != 0) {
          // segment was merged.
          if (seg->mergedLiveGen == seg->liveGen) {
            // this segment was merged (or is currently merging) with the latest liveGen, so no issues. We've applied all deletes,
            // so we can drop any personal deletes to save space. Merger does not currently try to apply personal deletes.
            seg->personalDeletes.clear();
            INDEX_DEBUG("finishCommitBody: msg={} segment {} was merged with latest liveGen {}", (void*)&msg, seg->name(), seg->liveGen);
            continue;
          }

          if (seg->merging == true) {
            startedMergingOldVersions.push_back(seg);
          } else {
            finishedMergingOldVersions.push_back(seg);
            // since this segment was already merged, we can just move it's personal deletes off
            personalDeletes.insert(personalDeletes.end(),
              std::make_move_iterator(seg->personalDeletes.begin()),
              std::make_move_iterator(seg->personalDeletes.end()));
            seg->personalDeletes.clear();
          }
        } else {
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
        std::shared_ptr<MultiDeletesData> multiDeletesDataPtr = std::make_shared<MultiDeletesData>(std::move(commitInfo.multiDeletesData));

        // Since only one merge can happen at once, we only need to add personal deletes to
        // one of the segments that are being merged to get them transferred to the new segment when it is done.
        if (!startedMergingOldVersions.empty()) {
          auto& seg = *startedMergingOldVersions.front();
          // no segments were merged, so we can just apply deletes to the new segments.
          LOG_DEBUG("finishCommitBody: Added personal deletes currently merging {} deletes={}", seg, (void*)multiDeletesDataPtr.get());
          seg.personalDeletes.push_back(multiDeletesDataPtr);  // copy the shared_ptr
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
        auto numSegmentsMissingDeletes = 0;  // sanity check - we should find some.
        for (auto& [segId, seg] : segInfos) {
          // TODO: is it possible for any personal deletes to be higher than the maxDeletedVersion here?
          // Uhhh, yes! There could be *no* deletes in a commit other than the personal deletes.
          if (seg->minVersion < maxAllDeleteVersion && seg->commitTime == 0) {
            // this segment has deletes that need to be applied, so append to its personal deletes.
            numSegmentsMissingDeletes++;
            // *copy* all of the collected delete sets
            seg->personalDeletes.append_range(personalDeletes);
            INDEX_DEBUG("finishCommitBody: Added personal deletes for {}", *seg);
          }
        }

        if (numSegmentsMissingDeletes == 0) {
          // Is this really guaranteed to be an error? We handle some potential races by carefully ordering
          // and realizing that delete application is idempotent.  Although we do stick to merging to what
          // liveGen we said we did, so I guess this should never happen.
          LOG_ERROR("Internal Error, a segment was merged with old deletes, but no merged segments were found that needed deletes applied.");
        }
      }
    }  // end index lock

    // Check if any of the segments are now empty and remove them from the list.
    std::vector<SegInfo*> segsToKeep;
    std::vector<SegInfo*> toDelete;
    segs.reserve(segs.size());
    {
      const std::lock_guard<std::mutex> lock(indexMutex);
      for (auto& seg : segs) {
        if (seg->liveDocs == 0) {
          toDelete.push_back(seg);
        } else {
          segsToKeep.push_back(seg);
        }
      }
    }

    // write the segments file with only the segments that have live documents
    writeIndexInfoFile(segsToKeep, msg.commitInfo.get());

    // Only move the segment to the delete list after the new IndexInfo file is written.
    // This way it should be safe for other threads to also try deletions.
    if (!toDelete.empty()) {
      const std::lock_guard<std::mutex> lock(indexMutex);
      for (auto& seg : toDelete) {
        seg->commitTime = 0; // mark as not being part of the last commit so it may be deleted immediately.
        moveSegmentToDelete(seg->segId);
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
    // Delete segment files only after the IndexInfo file is written.
    // Do not delete any segments that are being merged.
    std::vector<std::unique_ptr<SegInfo>> localDeleteList;
    localDeleteList.reserve(segmentsToDelete.size());
    {
      const std::lock_guard<std::mutex> lock(indexMutex);
      auto lastCommit = lastCommitTime.load(std::memory_order::relaxed);
      // grab all segments and put back ones we shouldn't delete yet.
      std::swap(localDeleteList, segmentsToDelete);
      for (auto& seg : localDeleteList) {
        // NOTE! we can observe seg->commitTime > lastCommit because the segments file
        // may be in the process of being written out, and lastCommitTime is only
        // updated *after* the IndexInfo file is written.
        if (seg->merging || seg->commitTime >= lastCommit) {
          segmentsToDelete.push_back(std::move(seg));  // keep it in the list, we can't delete it yet.
        }
      }
    }

    // Now delete the segments outside of the mutex.
    // In the future, if we wanted to support windows, we need a background deleter to
    // retry deletes after some time in case they are still open.
    for (auto& seg : localDeleteList) {
      if (!seg) continue;  // skip null segments
      INDEX_DEBUG("Deleting files {}", *seg);
      dir.deletePrefix(Postings::getIndexFileNamePrefix(seg->segId));
      seg.reset();  // deletes the in-memory segment info
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


  // This is only called from the finishCommit node, which has concurrency==1 (single-threaded)
  // hence we only need to protect against changes in the segInfos map, not multiple invocations of this method.
  // The passed span of segments may be reordered after this is finished.
  void IndexWriter::writeIndexInfoFile(std::span<SegInfo*> segs, CommitInfo* commitInfo) {
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

    INDEX_DEBUG("writeIndexInfoFile: now_us={} lastCommitTime={} diff={}", now_us, lastCommitTime.load(), now_us - lastCommitTime.load());
    uint64_t numDocs = 0;

    // Sort the list of segments by the segId.
    // Some tests rely on not reordering segments.
    std::sort(segs.begin(), segs.end(), [](const SegInfo* a, const SegInfo* b) {
         /*
         // first sort on number of documents (largest first), then on segment id (smallest first)
         if (a->maxDoc != b->maxDoc) {
           return a->maxDoc > b->maxDoc;
         }
          */
         return a->segId < b->segId;
       });

    uint64_t updateVersion = 0;
    uint64_t thisIndexGen = ++indexGen;  // increment the index generation for this commit.
    if (commitInfo) {
      // if we have a commit info, use the update version from it.
      updateVersion = commitInfo->updateMessage->updateVersion;
      commitInfo->indexGen = thisIndexGen;
    } else {
      // Otherwise, use the current update base.  This is only for older tests.
      updateVersion = updateBase + 1;
    }

    // Build the protobuf message
    google::protobuf::Arena arena;
    proto::IndexInfo& indexInfo = *google::protobuf::Arena::Create<proto::IndexInfo>(&arena);
    indexInfo.set_commit_time(now_us);
    indexInfo.set_version(1);
    indexInfo.set_index_gen(thisIndexGen);
    indexInfo.set_update_version(updateVersion);
    indexInfo.mutable_segments()->Reserve(segs.size());

    for (auto seg: segs) {
      // Update the commit time for the seg. Important to know if this seg is part of the last commit.
      // This does mean that this may be visible before the commit is done and before lastCommitTime is updated.
      // Any comparison with lastCommitTime should be done with this in mind.
      seg->commitTime = now_us;

      auto* segmentInfo = indexInfo.add_segments();
      segmentInfo->set_seg_id(seg->segId);
      segmentInfo->set_max_doc(seg->maxDoc);
      segmentInfo->set_live_gen(seg->liveGen);
      segmentInfo->set_min_version(seg->minVersion);
      segmentInfo->set_max_version(seg->maxVersion);
      segmentInfo->set_live_docs(seg->liveDocs);

      numDocs += seg->maxDoc;
      INDEX_DEBUG("\t{}", *seg);
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

    INDEX_DEBUG("\twriteIndexInfoFile DONE: commitTime={} nSegs={} gen={} maxDoc={}", now_us, segs.size(), thisIndexGen, numDocs);

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
    segs.reserve(mergePolicy->MERGE_FACTOR*2);

    // We grab the list of segments to merge with the lock held, but use them outside of the lock.
    // This is safe since the only place where segments are deleted is in a merge, and this has concurrency==1
    {
      const std::lock_guard<std::mutex> lock(indexMutex);
      for (auto& seg: segInfos) {
        if (seg.second->mergeLevel == msg.mergeLevel || msg.maxSegments == 1) {
          segs.push_back(seg.second.get());
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

    solux::Signal::emit("mergeStart", (void*)(int64_t)msg.mergeLevel, (void*)segs.size());

    {
      // grab or open all the postings readers
      std::vector<std::shared_ptr<PostingsReader>> preaders;
      preaders.reserve(segs.size());
      for (auto segInfo: segs) {
        preaders.emplace_back(segInfo->sharedPostingsReader.load());
        if (!preaders.back()) {
          preaders.back() = std::make_shared<PostingsReader>(dir, segInfo->segId);
          segInfo->sharedPostingsReader.store(preaders.back());
        }
      }

      MemPool pool;

      // Copy the preaders to a vector of pointers for our underlying merge code.
      // this is temporary... mergers will need more info at some point to handle deletes.
      std::vector<PostingsReader*> preaderPtrs;
      preaderPtrs.reserve(preaders.size());
      for (auto& preader : preaders) {
        preaderPtrs.push_back(preader.get());
      }

      PostingsWriter pwriter(dir, ++lastSegId);

      // Do the actual merge.
      // TODO: catch any errors and restore state / clean up.
      SegmentMerger merger(preaderPtrs, pwriter);
      merger.merge();

      // Create the new SegInfo for the output segment.
      auto newSegInfo = std::make_unique<SegInfo>(pwriter.getSegId(), pwriter.getMaxDoc());

      // Move old segments to the delete list and add the new segment info.
      std::vector<std::unique_ptr<SegInfo>> toDeleteSegs;
      {
        const std::lock_guard<std::mutex> lock(indexMutex);
        for (auto segInfo : segs) {
          segInfo->merging = false;  // mark as no longer merging so it can be removed.
          segInfo->mergedIntoSegId = newSegInfo->segId;  // mark the segment as merged into the new segment

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

        // add new segInfo to the index
        mergePolicy->_update(newSegInfo.get());
        segInfos.emplace(pwriter.getSegId(), std::move(newSegInfo));
      } // end index lock

      tryDeleteSegments();  // try to delete segments that are now empty

      // even though we're not quite done yet, it's OK if another merge is checked/submitted since
      // we've updated segInfos and the mergePolicy.
      mergePolicy->mergeRunning = false;
    } // end of scope for preaders, pool, and pwriter


    // TODO: if merge was kicked off and no more commits are in the pipeline, should we submit
    // one ourselves so the new segment is visible?
    // We could look to see if there are any busy inverters - if so, indexing is still happening.
    // Also maybe only do if there are no more merges.

    // TODO: if we have a merge message, with a commit on it, we should submit a commit message
    // that wraps this message and doesn't call done() until the commit is finished.
    msg.done(*this);


    // check if we should send a commit so the new segment gets referenced.
    bool triggerCommit = false;
    {
      const std::lock_guard<std::mutex> lock(indexMutex);
      auto anotherMerge = mergePolicy->_maybeMergeSegments(nullptr);
      if (!anotherMerge) {
        if (busyInverters.empty() && flushingInverters.empty() && idleInverters.empty()) {
          // no indexing activity, so let's trigger a commit.
          triggerCommit = true;
        }
      }
    }

    if (triggerCommit) {
      // send a commit message to force a commit.
      class CommitMessage : public UpdateMessage {
      public:
        void handle(IndexWriter& iw) override {
          unused(iw);
        }
        void done(IndexWriter& iw) override {
          unused(iw);
          delete this;  // delete the message after done
        }
      };

      CommitMessage* commitMessage = new CommitMessage();
      commitMessage->commit = UpdateMessage::COMMIT;
      INDEX_DEBUG("mergeSegmentsBody: requesting commit. msg={}", (void*)commitMessage);
      this->submitUpdate(commitMessage);
    } else {
      INDEX_DEBUG("mergeSegmentsBody: merge done, but not triggering commit since there are busy, flushing, or idle inverters.");
    }
  }






/// TEST CODE
/// Only for test code... there is no concurrency control, etc.
void IndexWriter::mergeSegments() {
  // make sure we are getting the latest index reader (wasteful!)
  indexReader.reset();
  auto reader = getIndexReader();
  std::vector<PostingsReader*> preaders;  // TODO: make sure we're not trying to merge a segment that is being built!
  preaders.reserve(reader->segments().size());
  for (auto& seg : reader->segments()) {
    preaders.push_back(&seg.postingsReader());
  }
  // we could calc maxdoc at this point...
  uint64_t segId = ++lastSegId;
  PostingsWriter pwriter(dir, segId);

  SegmentMerger merger(preaders, pwriter);
  merger.merge();

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
    lastSegId = 0;
    indexGen = 0;
    nextCommitInfo = std::make_unique<CommitInfo>();

    lastCommitTime = lastAdvertisedCommitTime = 0;
  }
  mergePolicy->refresh(); // we can't call this with lock held since it tries to acquire.

  // don't touch commitNumber or updateNumber... the TBB graph relies on the exact sequence of numbers.
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
      LOG_INFO("\t\tsegId={} nDocs={} mergeLevel={} commitTime={}", segId, seg->maxDoc, seg->mergeLevel, seg->commitTime);
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

void IndexWriter::applyDeletes(std::span<SegInfo*> segs, MultiDeletesData& multiDeletesData) {
  for (SegInfo* seg : segs) {
    applyDeletes(*seg, multiDeletesData);
  }
}

void IndexWriter::applyDeletes(SegInfo& seg, MultiDeletesData& multiDeletesData) {
  if (multiDeletesData.empty() && seg.personalDeletes.empty()) {
    return;
  }

  auto guard = MemPool::threadLocalPoolGuard();
  MemPool& pool = guard.pool();
  
  // Read the segment to find documents that should be deleted
  PostingsReader reader(dir, seg.segId);
  int32_t maxDocId = reader.numDocs();
  
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
  if (versionFieldReader.seek("_version_")) {
    versionFieldReader.readFieldInfo(versionFieldInfo);
    hasVersionField = true;
  }
  
  // Helper function to process deletes from a DeletesData
  auto processDeletes = [&](const DeletesData& deletesData) {
    for (size_t i = 0; i < deletesData.deletedIds.size(); i++) {
      const std::string& deleteId = deletesData.deletedIds[i];
      uint64_t deleteVersion = deletesData.deletedVersions[i];
      
      // Seek to the specific ID term
      if (termsEnum.seek(deleteId)) {
        // Found the ID term, now get documents containing this ID
        DocsEnum docsEnum(pool, reader, termsEnum);

        for (int32_t docId = docsEnum.next(); docId != DocsEnum::END; docId = docsEnum.next()) {
          // first check if the document has already been deleted.  If so, we don't need
          // to check the version or do anything else.
          if (currLiveBits && !currLiveBits->get(docId)) {
            // Document is already deleted, skip it
            continue;
          }

          // doc is live, so check the _version_ field if it exists.
          bool shouldDelete = true;

          // If version field exists, check if document version is less than delete version
          if (hasVersionField) {
            std::vector<int32_t> singleDoc = {docId};
            uint64_t docVersion = 0;
            
            IntColReader::getSingleValues(pool, reader, versionFieldInfo, singleDoc,
              [&](size_t, int32_t, int64_t version) {
                docVersion = (uint64_t)version;
              });
            
            // Only delete if document version is less than delete version
            shouldDelete = (docVersion < deleteVersion);
          }
          
          if (shouldDelete) {
            // Allocate the bitset on first new delete
            if (!liveBits) {
              liveBits = std::make_unique<screaming::RAMFixedBitSet>(maxDocId, true);

              // If we have existing deletes, copy them using memcpy
              if (existingLiveDocs) {
                const auto& existingBitset = existingLiveDocs->bitset();
                size_t wordsSize = screaming::FixedBitSet::sizeInWords(maxDocId) * sizeof(uint64_t);
                std::memcpy(liveBits->words, existingBitset.words, wordsSize);
              }
            }

            // Mark the document as deleted
            liveBits->clear(docId);
            numLiveDocs--;
            newDeletesCount++;
          }
        }
      }
    }
  };
  
  // Process personal deletes for this segment
  for (const auto& personalDelete : seg.personalDeletes) {
    for (const auto& deletesData : personalDelete->deletesData) {
      processDeletes(*deletesData);
    }
  }
  
  // Process deletes from multiDeletesData
  for (const auto& deletesData : multiDeletesData.deletesData) {
    processDeletes(*deletesData);
  }
  

  
  // If we found any new documents to delete, write a new delete generation
  if (newDeletesCount > 0) {
    auto newLiveGen = seg.liveGen + 1;

    // Write the delete bitmap file
    std::string deleteFileName = Postings::getDeleteFileName(
      Postings::getSortableString(seg.segId), newLiveGen);
    
    auto deleteFile = dir.createFile(deleteFileName);

    OutputStream out(deleteFile.get());

    // Write new format header
    out.writeBytes(Postings::SOLUX_HEADER);  // "SOLUX001"
    out.writeLong(1);  // the format info
    out.writeInt(maxDocId);
    out.writeInt(numLiveDocs);  // number of bits set

    // Write the live docs bitset data (already 64-bit aligned after 24-byte header)
    size_t bitsDataSize = screaming::FixedBitSet::sizeInWords(maxDocId) * sizeof(uint64_t);
    out.write(liveBits->words, bitsDataSize);
    out.close();

    dir.finishFile(*deleteFile);

    INDEX_DEBUG("Applied {} new deletes to segment {} (new delete generation: {}, total live docs: {})",
                newDeletesCount, seg.segId, seg.liveGen, seg.liveDocs);

    // Update segment metadata only after successfully writing the delete file to avoid races.
    {
      const std::lock_guard<std::mutex> lock(indexMutex);
      seg.liveDocs = numLiveDocs;  // Set to live document count
      if (newDeletesCount > 0) {
        seg.liveGen++;
      }
    }

  }
}




} // end namespace solux