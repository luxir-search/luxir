#pragma once

#include <string>
#include <mutex>
#include <span>
#include <boost/unordered/unordered_flat_map.hpp>
#include <oneapi/tbb/flow_graph.h>
#include "solux/store/Directory.h"
#include "solux/search/IndexReader.h"
#include "solux/server/SoluxError.h"
#include "Inverter.h"
#include "UpdateMessage.h"


namespace solux {

#define INDEX_TRACE LOG_TRACE
// redefine DEBUG to TRACE level which shouldn't currently be logged!
#define INDEX_DEBUG LOG_TRACE
// #define INDEX_DEBUG LOG_DEBUG

// this is currently outside of the IW class just so we can format it in loggin.
  class SegInfo {
  public:
    uint64_t segId;
    int32_t maxDoc;            // one-past-highest-doc (doesn't count deletes)
    int32_t liveDocs = 0;      // number of live documents in latest liveGen.
    int32_t mergeLevel = -1;  // maintained by the MergePolicy.
    uint64_t liveGen = 0;  // the latest version of the deletes that this segment contains, or 0 if no deletes.
    int64_t mergedLiveGen = -1;  // if this segment was merged into another, what liveGen was used.
    uint64_t mergedIntoSegId = 0;  // segId of the segment this segment was merged into.
    uint64_t commitTime = 0;  // last time this segment was committed as part of the index.
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

    // firendly name for logs that match the segment filenames for easier debugging.
    static std::string name(uint64_t segId) {
      return Postings::getIndexFileNamePrefix(segId);
    }

    std::string name() const {
      return name(segId);
    }
  };


inline std::string format_as(const SegInfo& seg) {
  return fmt::format("(seg={} max={} live={} lgen={} mlevel={} merging={} mlgen={} mto={} ctime={} minV={} maxV={} pdel={})", seg.name(), seg.maxDoc, seg.liveDocs, seg.liveGen, seg.mergeLevel, seg.merging,
                     seg.mergedLiveGen, seg.mergedIntoSegId, seg.commitTime, seg.minVersion, seg.maxVersion, seg.personalDeletes.size());
}



/// The IndexWriter is a level above Inverter & PostingsWriter that coordinates
/// indexing activity for a single index / directory.
class IndexWriter {
  std::mutex indexMutex;
  std::mutex indexReaderMutex;

public:

  // TODO: if we don't need to expose MergePolicy, this could also be moved to the cpp file
  // if we add a method to IndexWriter to get/set the merge factor or other configurable things.
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
        INDEX_DEBUG("merge level update: seg={} segLevel={} segLevelCount={} mergeLevel={}", *seg, !seg?-1:seg->mergeLevel, !seg?-1:levelCounts[seg->mergeLevel], mergeLevel);
      } else {
        // check all levels
        for (auto i = 0u; i < levelCounts.size(); i++) {
          if (levelCounts[i] >= MERGE_FACTOR) {
            mergeLevel = i;
            break;
          }
        }
        INDEX_DEBUG("merge level update: seg=ALL mergeLevel={}", mergeLevel);
      }


      return mergeLevel;
    }

    // Call with indexMutex locked.
    void _remove(SegInfo* seg) {
      segCount--;
      if (seg->mergeLevel >= 0) {
        levelCounts[seg->mergeLevel]--;
      }
    }

    // Call with indexMutex locked, returns true if new merge message was sent.
    bool _maybeMergeSegments(SegInfo* seg) {
      int mergeLevel = -1;
      {
        // const std::lock_guard<std::mutex> lock(iw.indexMutex);
        mergeLevel = _update(seg);
        if (mergeLevel < 0 || mergeRunning) {
          return false;
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
      return true;
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

  // Segments marked for deletion (all docs deleted) - will be removed after next commit
  // protected by indexMutex
  std::vector<std::unique_ptr<SegInfo>> segmentsToDelete;


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

  // TBB flow graph nodes for processing updates.
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


  explicit IndexWriter(Directory &dir);
  ~IndexWriter();

  // Submit an update to the IndexWriter.
  // This is the primary entry point for indexing documents.
  bool submitUpdate(UpdateMessage* msg) {
    // this is currently a simple submit to the startUpdateNode, but could be more complex in the future.
    // We could also eliminate the startUpdateNode completely and just submit to the processUpdateNode
    // after setting the sequence numbers.
    return startUpdateNode->try_put(msg);
  }

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


  // Obtains an inverter for writing documents and sets it's updateVersion.
  Inverter& obtainInverter(uint64_t updateVersion = 0);

  // Releases an inverter back to the pool.
  void releaseInverter(Inverter& inverter);

  // Asynchronous commit that calls the callback when the commit is finished.  This should be preferred over blocking.
  void commit(std::function <void()>&& callback, UpdateMessage::CommitType commitType=UpdateMessage::COMMIT);

  // Synchronous commit.  This will effectively block but enter work-stealing mode if there is other work to do.
  // If one is not careful, this work-stealing can result in deadlocks.  Consider using the async version.
  void commit(UpdateMessage::CommitType commitType=UpdateMessage::COMMIT);

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
    if (msg.commit != UpdateMessage::NO_COMMIT) {
      initiateCommit(msg);
    } else {
      msg.done(*this);
    }
  }

  void initiateCommit(UpdateMessage& msg);
  void segmentFlushBody(Inverter& inverter);
  void finishCommitBody(UpdateMessage& msg);
  void writeIndexInfoFile(std::span<SegInfo*> segs, CommitInfo* commitInfo = nullptr);
  void tryDeleteSegments();
  void moveSegmentToDelete(uint64_t segId);
  void applyDeletes(std::span<SegInfo*> segs, MultiDeletesData& multiDeletesData);
  void applyDeletes(SegInfo& seg, MultiDeletesData& multiDeletesData);
  void mergeSegmentsBody(MergeMessage& msg);

public:
  /// THIS SECTION ONLY FOR TEST CODE!
  /// Only for test code... there is no concurrency control, etc.
  void mergeSegments();

  // Called from tests only to remove all data.
  // This is difficult to get right though... we should really add the ability to empty the index through
  // the API and then use that (prob through the merge code since it's the only place segments are removed)
  void testDeleteAllData();

  // dump some useful info for tests
  void debugInfo();
};


}
