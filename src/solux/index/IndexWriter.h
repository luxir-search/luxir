#pragma once

#include <string>
#include <mutex>
#include <span>
#include <boost/unordered/unordered_flat_map.hpp>
#include <oneapi/tbb/flow_graph.h>
#include "solux/store/Directory.h"
#include "solux/store/OutputStream.h"
#include "solux/store/InputStream.h"
#include "solux/search/IndexReader.h"
#include "solux/util/thread.h"
#include "solux/util/Signal.h"
#include "solux/server/SoluxError.h"
#include "Inverter.h"
#include "PostingsWriter.h"
#include "UpdateMessage.h"
#include "protos/solux_types.pb.h"
#include <google/protobuf/io/coded_stream.h>
#include <google/protobuf/io/zero_copy_stream_impl_lite.h>

namespace solux {

// redefine DEBUG to TRACE level which shouldn't currently be logged!
#define INDEX_DEBUG LOG_TRACE
// #define INDEX_DEBUG LOG_DEBUG

/// The IndexWriter is a level above Inverter & PostingsWriter that coordinates
/// indexing activity for a single index / directory.
class IndexWriter {
  // increment a base 36 string that is prefixed with the number of digits.
  void incrementGen(std::string &gen) {
    int index = gen.size() - 1;
    for (;;) {
      if (index < 0) {
        // we need another digit on the front
        gen.insert(gen.begin(), '1');
      } else {
        gen[index]++;
        if (gen[index] == ('9' + 1)) {
          gen[index] = 'A';
        } else if (gen[index] == 'Z' + 1) {
          gen[index] = '0';
          index--;  // carry to next position
          continue;
        }
      }
      break;
    }
  }

  std::mutex indexMutex;
  std::mutex indexReaderMutex;

public:
  class SegInfo {
  public:
    uint64_t segId;
    int32_t maxDoc;            // one-past-highest-doc (doesn't count deletes)
    int32_t liveDocs = 0;      // number of live documents in latest liveGen.
    int32_t mergeLevel = -1;  // maintained by the MergePolicy.
    uint64_t liveGen = 0;  // the latest version of the deletes that this segment contains, or 0 if no deletes.
    uint64_t mergedLiveGen = 0;  // if this segment was merged into another, what deletesVersion was used.
    uint64_t mergedIntoSegId = 0;  // segId of the segment this segment was merged into.
    uint64_t commitTime = 0;  // time when this segment was first committed as part of the index.
    // write segment info (size,docs) segments file as well so we don't have to open the segment to determine it?
    int64_t sizeInBytes = 0;

    bool merging = false;  // set to true when a merge is in progress with this segment as input.


    // atomic shared pointer since it could be set / mutated by either the IW (setting or clearing),
    // or by IndexReader opening code.
    std::atomic<std::shared_ptr<PostingsReader>> sharedPostingsReader = nullptr;

    // info about the min and max versions of documents in this segment, derived from the update message sequence number.
    // this can help us determine if we can skip applying deletes to this segment from another segment.
    uint64_t minVersion = 0;
    uint64_t maxVersion = 0;

    // "personal" deletes to be applied to this segment.  Only used to catch up when merging was happening concurrently
    // with applying deletions and hence they could not be applied to this segment yet.  See finishCommitBody()
    // where deletes are applied.  This is a shared_ptr because multiple merges may have been done that need to
    // apply deletes.  We don't really need the thread safety of shared_ptr, could switch to boost::intrusive_ptr
    // for straight ref counting.
    std::vector<std::shared_ptr<MultiDeletesData>> personalDeletes;

    SegInfo(uint64_t segId, int nDocs) : segId(segId), maxDoc(nDocs), liveDocs(nDocs) {}


  };


  class MergePolicy {
    std::vector<SegInfo*> segsCopy;
    std::vector<int32_t> levelCounts;
    int32_t segCount = 0;  // a sanity check that we are in-sync with segments in the IndexWriter.
  public:
    IndexWriter& iw;
    bool mergeRunning = false;
    int32_t MERGE_FACTOR = 10;
    // TODO: Hmmm, a high merge floor can lead to some O^N2 behavior: see https://issues.apache.org/jira/browse/LUCENE-10574
    // Perhaps an alternative would be to remove the floor and then kick off merges like normal, *but*
    // when a merge happens at tier 3, sweep up all the smaller segments as well.  I'm not sure this
    // really makes sense though since it would only occasionally fix the "many small segments" problem, and
    // it is the indexing pattern that is causing the issue.  Perhaps this should be fixed by the user
    // through an API that requests a more aggressive merge to sweep up small segments.
    float inverseLogM = 1.0f / log2(MERGE_FACTOR);

    // Methods with _ prefix should be called with the indexMutex already locked.
    MergePolicy(IndexWriter& iw) : iw(iw) {}

    void setMergeFactor(int32_t mergeFactor) {
      MERGE_FACTOR = mergeFactor;
      inverseLogM = 1.0f / log2(MERGE_FACTOR);
    }

    // re-calculate the merges from scratch (i.e. not incrementally)
    // does not kick off any merges.
    void refresh() {
      std::lock_guard<std::mutex> lock(iw.indexMutex);
      _refresh();
    }

    void _refresh() {
      segCount = 0;
      levelCounts.clear();
      for (auto& [segId, seg] : iw.segInfos) {
        _update(seg.get());
      }
    }

    void _sanityCheck() {
      if ((size_t)segCount != iw.segInfos.size()) {
        LOG_ERROR(
                "Internal Error, please report. MergePolicy segCount {} does not match live segment count of {}.",
                segCount, iw.segInfos.size());
        _refresh();  // Fix the bug.  This currently no longer triggers, but is left here for defensive reasons.
      }
    }

    // Update the merge level of a segment and return the segment level to merge, or -1 if no merge needed.
    // If seg is nullptr, then we check all segment levels for a merge.
    // Call with indexMutex locked.
    int32_t _update(SegInfo* seg) {
      // assert that the index mutex is locked
      int32_t mergeLevel = -1;

      if (seg != nullptr) {
        segCount++;

        // For MERGE_FACTOR 10, docs 0-9 = level 0, 10-99 = level 1, etc.
        if (seg->maxDoc < MERGE_FACTOR) {
          seg->mergeLevel = 0;
        } else {
          seg->mergeLevel = (int32_t) (log2(seg->maxDoc) * inverseLogM);
        }
        if (seg->mergeLevel >= (int) levelCounts.size()) {
          levelCounts.resize(seg->mergeLevel + 1);
        }
        if (++levelCounts[seg->mergeLevel] >= MERGE_FACTOR) {
          mergeLevel = seg->mergeLevel;
        }
      } else {
        // check all levels
        for (auto i = 0u; i < levelCounts.size(); i++) {
          if (levelCounts[i] >= MERGE_FACTOR) {
            mergeLevel = i;
            break;
          }
        }
      }

      INDEX_DEBUG("update: seg={} segLevel={} segLevelCount={} mergeLevel={}", (void*)seg, !seg?-1:seg->mergeLevel, !seg?-1:levelCounts[seg->mergeLevel], mergeLevel);

      return mergeLevel;
    }

    // Call with indexMutex locked.
    void _remove(SegInfo* seg) {
      segCount--;
      if (seg->mergeLevel >= 0) {
        levelCounts[seg->mergeLevel]--;
      }
    }

    // Call with indexMutex locked
    void _maybeMergeSegments(SegInfo* seg) {
      int mergeLevel = -1;
      {
        // const std::lock_guard<std::mutex> lock(iw.indexMutex);
        mergeLevel = _update(seg);
        if (mergeLevel < 0 || mergeRunning) {
          return;
        }

        mergeRunning = true;
      }

      class MyMergeMessage : public MergeMessage {
      public:
        void handle(IndexWriter& iw) override { unused(iw); }
        void done(IndexWriter& iw) override {
          unused(iw);
          delete this;
        }
      };

      MyMergeMessage* msg = new MyMergeMessage();
      msg->mergeLevel = mergeLevel;
      iw.mergeSegmentsNode->try_put(msg);
    }
  };  // end MergePolicy

  Directory& dir;

  // the last segId generated. Atomic since we don't grab any lock in the merge code to generate a new segment id.
  std::atomic_uint64_t lastSegId;

  std::shared_ptr<IndexReader> indexReader;

  std::unique_ptr<MergePolicy> mergePolicy;

  // Tracks current segments in the index.  Keyed by uint64_t segId.
  // There needs to be higher level protection for transactions like replacing N segments with a new merged segment.
  // protected by indexMutex
  using SegMap = boost::unordered_flat_map<uint64_t, std::unique_ptr<SegInfo>>;
  SegMap segInfos;
  // boost::unordered::unordered_flat_set<std::unique_ptr<SegInfo>, SegIdHash, SegIdEqual> segInfos;
  // boost::unordered::unordered_flat_set can't currently be used because it lacks an extract() method, which is
  // the only way of removing a move-only object from the set.


  // In the future, we way want to get an inverter by segment id (delete handling?).
  // We could convert to unordered_flat_set keyed by uint64_t segId, just like segInfos.
  // protected by indexMutex
  // If we don't need to look up by segment id, we could just use a vector for idleInverters.
  boost::unordered_flat_map<Inverter*, std::unique_ptr<Inverter>> idleInverters;
  boost::unordered_flat_map<Inverter*, std::unique_ptr<Inverter>> busyInverters;
  boost::unordered_flat_map<Inverter*, std::unique_ptr<Inverter>> flushingInverters;

  // The last updateNumber generated (the first update number generated will be 1)
  uint64_t updateNumber = 0;
  // Read from the index when this IW instance was created.  Does not change.
  // sequence numbers for TBB serializers are calculated via updateVersion - updateBase - 1.
  uint64_t updateBase = 0;

  // Used for sequencing update messages containing commits.
  // Not used for index_gen since not every commit will end up changing the index.
  uint64_t commitNumber = 0;

  // last index generation number... incremented before each commit.
  uint64_t indexGen = 0;

  // commit info for the index, used to track deletes.
  // This is moved to the UpdateMessage when a commit is processed and a new one is created for the next commit.
  std::unique_ptr<CommitInfo> nextCommitInfo;

  // In an update response, we could return an update number, or a commit number, or even a monotonic time.
  // This would allow a searching client to specify a time to search up to.
  // Time of the last commit (since 1970 epoch) in microseconds. Guaranteed to be strictly increasing.
  std::atomic_uint64_t lastCommitTime;
  std::atomic_uint64_t lastAdvertisedCommitTime;

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


  using UpdateMessageFunc = tbb::flow::function_node<UpdateMessage*, UpdateMessage*>;
  using UpdateMessageMultiFunc = tbb::flow::multifunction_node<UpdateMessage*, std::tuple<UpdateMessage*>>;
  // use a multfunction node when there is no downstream consumer (i.e. the output is dropped or can be dropped)
  // using a normal function node would cause buffering.

  tbb::flow::graph updateGraph;
  std::unique_ptr<UpdateMessageFunc> startUpdateNode;
  std::unique_ptr<UpdateMessageFunc> processUpdateNode;
  std::unique_ptr<tbb::flow::sequencer_node<UpdateMessage*> > updateSequencerNode;
  std::unique_ptr<UpdateMessageMultiFunc> updateFinishNode;

  using InverterMultiFunc = tbb::flow::multifunction_node<Inverter*, std::tuple<UpdateMessage*>>;
  std::unique_ptr<InverterMultiFunc> segmentFlushNode;

  std::unique_ptr<tbb::flow::sequencer_node<UpdateMessage*> > commitSequencerNode;
  std::unique_ptr<UpdateMessageMultiFunc> commitFinishNode;

  using MergeMessageMultiFunc = tbb::flow::multifunction_node<MergeMessage*, std::tuple<void*>>;
  std::unique_ptr<MergeMessageMultiFunc> mergeSegmentsNode;

  ~IndexWriter() {
    // without this, in gcc release mode we can get a crash when the IndexWriter is destroyed, even when
    // the graph wasn't used. Presumably because the test was so fast and there was some async initialization
    // of the graph still going on?
    updateGraph.wait_for_all();
  }

  explicit IndexWriter(Directory &dir) : dir(dir) {
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
        lastSegId = std::max(lastSegId.load(), segId);
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

  // Submit an update to the task graph.
  bool submitUpdate(UpdateMessage* msg) {
    // this is currently a simple submit to the startUpdateNode, but could be more complex in the future.
    // We could also eliminate the startUpdateNode completely and just submit to the processUpdateNode
    // after setting the sequence numbers.
    return startUpdateNode->try_put(msg);
  }

private:
  void startUpdateBody(UpdateMessage& msg) {
    // if the start node can reject updates, then assigning sequence numbers should be done after that.
    // Sequences must start at 0 for the sequencer nodes.
    msg.updateVersion = ++updateNumber;
    if (msg.commit != UpdateMessage::NO_COMMIT) {
      msg.commitNum = commitNumber++;
    } else {
      msg.commitNum = 0;
    }
    INDEX_DEBUG("startUpdateBody: msg={} updateVersion={} commitNum={}", (void*)&msg, msg.updateVersion, msg.commitNum);
  }

  void processUpdateBody(UpdateMessage& msg) {
    INDEX_DEBUG("processUpdateBody: msg={}", (void*)&msg);
    try {
      msg.handle(*this);
    } catch (std::exception& e) {
      INDEX_DEBUG("processUpdateBody Exception Caught: exception={}", (void*)&msg, e.what());
      msg.result.setException(e);
    }
  }

  void finishUpdateBody(UpdateMessage& msg) {
    INDEX_DEBUG("finishUpdateBody: msg={}", (void*)&msg);

    // If this update has a commit, we could either kick it off here, or send a message to a commit node.
    // Gathering the required inverters as quickly as possible might be good (i.e. do it here)
    // Aside: if each inverter keeps track of the highest (and lowest?) update message it has seen, could that be used somehow?
    //  - could avoid dragging in an unneeded inverter.

    if (msg.commit != UpdateMessage::NO_COMMIT) {
      initiateCommit(msg);
    } else {
      msg.done(*this);
    }
  }

  void initiateCommit(UpdateMessage& msg) {
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
  void segmentFlushBody(Inverter& inverter) {
    INDEX_DEBUG("segmentFlushBody: inverter={} commitInfo={} msg.leftToFlush={}", (void*)&inverter, (void*)inverter.commitInfo,
                inverter.commitInfo == nullptr ? -1 : inverter.commitInfo->leftToFlush);

    bool success;
    try {
      // uncomment to serialize inverter flushing (for testing purposes)
      // const std::lock_guard<std::mutex> lock(indexMutex);
      success = inverter.flush();
      INDEX_DEBUG("segmentFlushBody: inverter={} flushed successfully for segment {}", (void*)&inverter, inverter.getPostingsWriter().segId);
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
  void finishCommitBody(UpdateMessage& msg) {
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


    // TODO: segmentMerger sends a message with no commit info currently....
    auto maxDeleteVersion = msg.commitInfo == nullptr ? 0 : msg.commitInfo->multiDeletesData.getLargestVersion();

    std::vector<SegInfo*> segs;
    std::vector<SegInfo*> segsWithPersonalDeletes;
    std::vector<SegInfo*> segsToApplyDeletes;

    {
      const std::lock_guard<std::mutex> lock(indexMutex);
      segs.reserve(segInfos.size());
      // grab all segments and mark them as being part of a commit.
      for (auto& [segId, seg] : segInfos) {
        segs.push_back(seg.get());
        if (seg->commitTime == 0) {
          seg->commitTime = 1;  // for marking it in use only... it will be updated later when writing segments file.
        }
        if (!seg->personalDeletes.empty()) {
          // this segment has personal deletes that need to be applied.
          segsWithPersonalDeletes.push_back(seg.get());
        }
        if (seg->minVersion < maxDeleteVersion) {
          // this segment has deletes that need to be applied.
          segsToApplyDeletes.push_back(seg.get());
        }
      }
    }

    // TODO: segmentMerger sends a message with no commit info currently....
    if (msg.commitInfo) {
      CommitInfo& commitInfo = *msg.commitInfo;
      if (commitInfo.multiDeletesData.empty()) {
        INDEX_DEBUG("finishCommitBody: msg={} no deletes to apply.", (void*)&msg);
      } else {
        INDEX_DEBUG("finishCommitBody: msg={} applying deletes to {} segments.", (void*)&msg, segs.size());

        // TODO: parallelize this.
        for (auto seg : segsToApplyDeletes) {
          applyDeletes(*seg, commitInfo.multiDeletesData);
        }
      }
    }

    // now check if any of the segments were merged without necessary deletes applied.
    {
      const std::lock_guard<std::mutex> lock(indexMutex);
      bool mergedOldVersion = false;


      // check if any segments were merged that need deletes applied.
      for (auto& seg : segs) {
        if (seg->mergedLiveGen != 0 && seg->mergedLiveGen != seg->liveGen) {
          // we changed the deletesVersion of the segment, but an older one was merged.
          // that new segment could also have been merged, so we need to go through all of the
          // segments and see if the deletes on this commit might apply to it.
          // this segment was merged, so we need to apply deletes to it.
          INDEX_DEBUG("finishCommitBody: msg={} segment {} was merged with old deletes.", (void*)&msg, seg->segId);
          mergedOldVersion = true;
          break;
        }
      }

      if (mergedOldVersion) {
        // add personal deletes to all new segments that could be applicable.
        // first make a shared_ptr and then move the deletes from the commit info to it.
        CommitInfo& commitInfo = *msg.commitInfo;

        std::shared_ptr<MultiDeletesData> multiDeletesDataPtr = std::make_shared<MultiDeletesData>();
        *multiDeletesDataPtr = std::move(commitInfo.multiDeletesData);

        // now add these deletes to any new applicable segments
        auto numSegmentsMissingDeletes = 0;  // sanity check - we should find some.

        // Check the up-to-date segments.
        for (auto& [segId, seg] : segInfos) {
          if (seg->minVersion < maxDeleteVersion) {
            // this segment has deletes that need to be applied, so append to it's personal deletes.
            INDEX_DEBUG("finishCommitBody: msg={} segment {} adding personal deletes", (void*)&msg, segId);
            numSegmentsMissingDeletes++;
            seg->personalDeletes.push_back(multiDeletesDataPtr);  // copy the shared_ptr
          }
        }

        if (numSegmentsMissingDeletes == 0) {
          LOG_ERROR("Internal Error, a segment was merged with old deletes, but no merged segments were found that needed deletes applied.");
        }
      }
    }  // end index lock

    // write the segments file with the snapshot of segments we took at the beginning.
    writeIndexInfoFile(segs, msg.commitInfo.get());
    msg.done(*this); // don't access msg after this point, it could be deleted.
  }

  void applyDeletes(std::span<SegInfo*> segs, MultiDeletesData& multiDeletesData);

  // apply the given deletes to a segment, in addition to any personal deletes that may be present.
  void applyDeletes(SegInfo& seg, MultiDeletesData& multiDeletesData);


public:
  // return a copy of the shared_ptr so that the instance it points to will never change while in use.
  std::shared_ptr<IndexReader> getIndexReader(uint64_t freshness_us = 0) {
    const std::lock_guard<std::mutex> lock(indexReaderMutex);
    bool needNewReader = false;
    if (!indexReader) {
      needNewReader = true;
    } else {
      // is there a new commit?
      if (lastAdvertisedCommitTime > indexReader->commitTime()) {
        // check if we want this new commit based on freshness requirement
        if (freshness_us == 0 || std::chrono::duration_cast<std::chrono::microseconds>(
          std::chrono::system_clock::now().time_since_epoch()).count() - indexReader->commitTime() > freshness_us) {
          needNewReader = true;
        }
      }
    }

    // TODO: FIXME: this blocks other threads from getting the current reader.
    // C++20 has waiting on atomic variables, that might be an easy way to prevent this.
    // Tricky part will be handing different requests with different freshness requirements.  Multiple readers
    // with different timestamps could be opening at once.  It's also the wrong tool if opening an IndexReader
    // can take too long since blocking a thread won't allow other threads to perform other work.
    // We should see if there is a TBB friendly way to do this.
    if (needNewReader) {
      indexReader = std::make_shared<IndexReader>(dir);
    }

    return indexReader;
  }

  void releaseInverter(Inverter& inverter) {
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

  // Obtains an inverter for writing documents and sets it's updateVersion.
  Inverter& obtainInverter(uint64_t updateVersion = 0) {
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


  // Asynchronous commit that calls the callback when the commit is finished.  This should be preferred over blocking.
  void commit(std::function <void()>&& callback, UpdateMessage::CommitType commitType=UpdateMessage::COMMIT) {
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

  // Synchronous commit.  This will effectively block but enter work-stealing mode if there is other work to do.
  // If one is not careful, this work-stealing can result in deadlocks.  Consider using the async version.
  void commit(UpdateMessage::CommitType commitType=UpdateMessage::COMMIT) {
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

  // This is only called from the finishCommit node, which has concurrency==1 (single-threaded)
  // hence we only need to protect against changes in the segInfos map, not multiple invocations of this method.
  // The passed span of segments may be reordered after this is finished.
  void writeIndexInfoFile(std::span<SegInfo*> segs, CommitInfo* commitInfo = nullptr) {
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
    uint64_t numSegs = 0;

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
      auto* segmentInfo = indexInfo.add_segments();
      segmentInfo->set_seg_id(seg->segId);
      segmentInfo->set_max_doc(seg->maxDoc);
      segmentInfo->set_live_gen(seg->liveGen);
      segmentInfo->set_min_version(seg->minVersion);
      segmentInfo->set_max_version(seg->maxVersion);
      segmentInfo->set_live_docs(seg->liveDocs);
      
      if (seg->commitTime <= 1) {  // keep track of the first commit this segment appeared in.
        seg->commitTime = now_us;
      }
      numDocs += seg->maxDoc;
      INDEX_DEBUG("\tsegId={} maxDoc={} commitTime={}", seg->segId, seg->maxDoc, seg->commitTime);
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

    INDEX_DEBUG("\twriteIndexInfoFile DONE: commitTime={} numDocs={} numSegs={}", now_us, numDocs, numSegs);
    unused(numSegs, numDocs);

    // advertise this commit only after the file is closed.
    lastCommitTime = now_us;
    lastAdvertisedCommitTime = now_us;
  }


  // Onlt called from test code.
  void writeIndexInfoFile() {
    std::vector<SegInfo*> segs;
    
    // need to lock the indexMutex to get a consistent view of the segments.
    {
      const std::lock_guard<std::mutex> lock(indexMutex);
      segs.reserve(segInfos.size());
      for (auto& [segId, seg] : segInfos) {
        segs.push_back(seg.get());
      }
    }
    
    // Call the parameterized version
    writeIndexInfoFile(segs);
  }


private:
  // called from the mergeSegmentsNode which has concurrency==1 (single-threaded)
  void mergeSegmentsBody(MergeMessage& msg) {
    std::vector<SegInfo*> segs;
    segs.reserve(mergePolicy->MERGE_FACTOR*2);

    // We grab the list of segments to merge with the lock held, but use them outside of the lock.
    // This is safe since the only place where segments are deleted is in a merge, and this has concurrency==1
    {
      const std::lock_guard<std::mutex> lock(indexMutex);
      for (auto& seg: segInfos) {
        if (seg.second->mergeLevel == msg.mergeLevel || msg.maxSegments == 1) {
          segs.push_back(seg.second.get());
          segs.back()->merging = true;  // mark as being merged - see finishCommitBody
        }
      }

      // Sanity check this merge. a bug in testDeleteAllData led to merge accounting getting out-of-sync
      // with actual segments and resulted in a merge loop.
      mergePolicy->_sanityCheck();
    }

    solux::Signal::emit("mergeStart", (void*)(int64_t)msg.mergeLevel, (void*)segs.size());

    // We need a way to do deletes after the commit has finished.  Create a new Msg that wraps the old one for this.
    // We could alternately have a std::vector of SegInfo to delete, and have it happen in post-commit
    // for any segments no longer part of the committed index.
    class CommitDeleteMsg : public UpdateMessage {
    public:
      UpdateMessage* prevMsg;
      std::vector<std::unique_ptr<SegInfo>> removedSegs;
      void handle(IndexWriter& iw) override {
        prevMsg->handle(iw);  // should be unused in this context
      }
      void done(IndexWriter& iw) override {
        for (auto& segInfo : removedSegs) {
          // delete the segment files
          iw.dir.deletePrefix(Postings::getIndexFileNamePrefix(segInfo->segId));
        }
        prevMsg->done(iw);
        delete this;
      }
    };

    std::unique_ptr<CommitDeleteMsg> commitDeleteMsg;  // created on demand if we need a commit

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
      mergeSegments(pool, preaderPtrs, pwriter);

      // Create the new SegInfo for the output segment.
      auto newSegInfo = std::make_unique<SegInfo>(pwriter.getSegId(), pwriter.getMaxDoc());

      // now remove the merged segments from the segInfos and add the new segment.
      // Any removed segments that were not part of a previous commit can be deleted immediately.
      // If segments were part of a previous commit, then we need to write out a new info file *before* removing the index files.
      // Example scenario: merge starts, commit happens, merge finishes, crash.
      std::vector<std::unique_ptr<SegInfo>> toDeleteSegs;
      {
        const std::lock_guard<std::mutex> lock(indexMutex);
        for (auto segInfo : segs) {
          segInfo->mergedIntoSegId = newSegInfo->segId;  // mark the segment as merged into the new segment
          mergePolicy->_remove(segInfo);
          auto it = segInfos.find(segInfo->segId);
          assert(it != segInfos.end());
          if (it == segInfos.end()) {
            LOG_ERROR("Segment not found in segInfos.");
            assert(false);
          }

          if (segInfo->commitTime != 0) {
            if (!commitDeleteMsg) {
              commitDeleteMsg = std::make_unique<CommitDeleteMsg>();
            }
            // This makes it so the SegInfo data structure is also not deleted.
            commitDeleteMsg->removedSegs.emplace_back(std::move(it->second));
          } else {
            toDeleteSegs.emplace_back(std::move(it->second));
          }

          // if the segment had personal deletes, we need to transfer them to the new segment.
          if (!segInfo->personalDeletes.empty()) {
            INDEX_DEBUG("Merging personal deletes from segId={} to newSegId={}", segInfo->segId, newSegInfo->segId);
            newSegInfo->personalDeletes.insert(newSegInfo->personalDeletes.end(),
                                               std::make_move_iterator(segInfo->personalDeletes.begin()),
                                               std::make_move_iterator(segInfo->personalDeletes.end()));
          }

          // finally remove the old entry from segInfos
          segInfos.erase(it);
        }

        // add new segInfo to the index
        mergePolicy->_update(newSegInfo.get());
        segInfos.emplace(pwriter.getSegId(), std::move(newSegInfo));
      } // end index lock

      // delete unreferenced segments immediately
      for (auto& segInfo : toDeleteSegs) {
        dir.deletePrefix(Postings::getIndexFileNamePrefix(segInfo->segId));
      }

      // even though we're not quite done yet, it's OK if another merge is checked/submitted since
      // we've updated segInfos and the mergePolicy.
      mergePolicy->mergeRunning = false;
    } // end of scope for preaders, pool, and pwriter


    // if we need a commit, send to the finishCommit node
    // TODO: FIXME: with deletes, this is now a problem sending directly to commitFinishNode w/o CommitInfo -
    // that could expose a flushed segment that does not have deletes applied yet (those deletes are on a CommitInfo!)
    if (commitDeleteMsg) {
      commitDeleteMsg->commit = UpdateMessage::COMMIT;
      commitDeleteMsg->prevMsg = &msg;

      // Bypass the sequencer node and go directly to the commit node so we don't have to flow through the complete graph.
      // This is fine since we don't care how the commit interleaves with other updates or commits.
      commitFinishNode->try_put(commitDeleteMsg.release());
    } else {
      msg.done(*this);
    }

    // check if we need another merge
    {
      const std::lock_guard<std::mutex> lock(indexMutex);
      mergePolicy->_maybeMergeSegments(nullptr);
    }
  }

public:
  /// mostly for testing merge code currently... there is no concurrency control, etc.
  void mergeSegments() {
    // make sure we are getting the latest index reader (wasteful!)
    indexReader.reset();
    auto reader = getIndexReader();
    std::vector<PostingsReader*> preaders;  // TODO: make sure we're not trying to merge a segment that is being built!
    preaders.reserve(reader->segments().size());
    for (auto& seg : reader->segments()) {
      preaders.push_back(&seg.postingsReader());
    }
    // we could calc maxdoc at this point...
    uint64_t segId = lastSegId++;
    PostingsWriter pwriter(dir, segId);

    MemPool pool;
    mergeSegments(pool, preaders, pwriter);

    // update the list of segments... not safe currently
    // TODO: add unused segments to the "to be deleted" list
    segInfos.clear();
    segInfos.emplace(segId, std::make_unique<SegInfo>(segId, pwriter.getMaxDoc()));

    writeIndexInfoFile();  // TODO: currently for testing... we wouldn't normally do this here.
  }


  // TODO: can merging be decoupled and done by something else?  What about even on a different node?
  // overwrites would be the only tricky part...


  void mergeSegments(MemPool &pool, std::span<PostingsReader *> preaders, PostingsWriter &postingsWriter);


  // Called from tests only to remove all data.
  // This is difficult to get right though... we should really add the ability to empty the index through
  // the API and then use that (prob through the merge code since it's the only place segments are removed)
  void testDeleteAllData() {
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

      // drop all index files
      dir.clear();

      lastCommitTime = lastAdvertisedCommitTime = 0;
    }
    mergePolicy->refresh();  // we can't call this with lock held since it tries to acquire.

    // don't touch commitNumber or updateNumber... the TBB graph relies on the exact sequence of numbers.
  }



  void debugInfo() {
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


};


}
