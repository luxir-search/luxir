#pragma once

#include <string>
#include <charconv>
#include <thread>
#include <mutex>
#include <span>
#include "boost/unordered/unordered_flat_map.hpp"
#include "oneapi/tbb/flow_graph.h"
#include "solux/store/Directory.h"
#include "solux/store/OutputStream.h"
#include "solux/store/InputStream.h"
#include "solux/search/IndexReader.h"
#include "Inverter.h"
#include "PostingsWriter.h"

// redefine DEBUG to TRACE level which shouldn't currently be logged!
#define INDEX_DEBUG LOG_TRACE
// #define INDEX_DEBUG LOG_DEBUG

namespace solux {


class IndexWriter;

// An update message to be processed by the TBB update flow graph.
// See ProtoUpdateMessage.h/cpp for protobuf update handling code
class UpdateMessage {
public:
  virtual ~UpdateMessage() {}

  // For now, we will allow the handler to obtain/release an inverter.  We could also optionally pass it
  // as a param in the future if obtain/release becomes more complex.
  virtual void handle(IndexWriter& iw) = 0;

  // Called after all operations are complete.  Would typically delete this instance if it was heap allocated.
  // Consumers of UpdateMessage will not touch it after this call.
  virtual void done(IndexWriter& iw) = 0;

  // See docs in solux.proto:UpdateRequest
  // NOTE: These values should be kept in sync with the protobuf definition.
  enum CommitType {
    NO_COMMIT = 0,         // the default
    COMMIT = 1,            // ensure new data is searchable
    SILENT_COMMIT = 2,     // the commit will be "silent" (won't necessarily cause new searchers to be opened)
    // CONSISTENT_COMMIT = 3; // FUTURE - ensure distributed searchers will see new data
  };
  CommitType commit;
  int32_t commit_within;  // TODO: implement this


  // Filled in by the IndexWriter when the message is received.
  uint64_t seqNum;                    // The sequence number of this update, used to ensure updates are processed in order when needed
  uint64_t commitNum;                 // The commit number of this update, used to ensure commits are finished in order

  // Number of segments left to flush, protected by same mutex that protects the inverter lists.
  // making this an atomic is not enough to avoid race conditions since we also depend on coordination with
  // inverter->updateMessage, among other things.
  uint32_t leftToFlush = 0;  // internal use only
};


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
    // write segment info (size,docs) segments file as well so we don't have to open the segment to determine it?
    int64_t sizeInBytes = 0;
    int32_t nDocs = 0;
    int32_t mergeLevel = 0;
    bool merging = false;  // set to true when a merge is in progress with this segment as input.

    // atomic shared pointer since it could be set / mutated by either the IW (setting or clearing),
    // or by IndexReader opening code.
    std::atomic<std::shared_ptr<PostingsReader>> sharedPostingsReader;

    SegInfo(uint64_t segId, int nDocs) : segId(segId), nDocs(nDocs) {}
  };

  // TODO: perhaps make this a subclass of UpdateMessage?  This may make it easier to expose merge requests through
  // the gRPC API.
  class MergeMessage {
  public:
    std::vector<SegInfo*> segs; // the segments to merge
    std::function<void(MergeMessage*)> callback;    // Called after the merge has completed.
  };

  Directory& dir;
  std::atomic_uint64_t gen;  // the last segId generated

  uint64_t lastCommitedGen = 0;  // the last index gen that was committed
  std::shared_ptr<IndexReader> indexReader;


  // Tracks current segments in the index.  Keyed by uint64_t segId.
  // There needs to be higher level protection for transactions like replacing N segments with a new merged segment.
  // protected by indexMutex
  boost::unordered::unordered_flat_map<uint64_t, std::unique_ptr<SegInfo>> segInfos;
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

  // sequence numbers for updates and commits.  protected by indexMutex
  std::atomic_uint64_t updateNumber = 0;
  std::atomic_uint64_t commitNumber = 0;



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
  //  - if something fails, we need to still flow it through the graph so the sequencers are updated.

  using UpdateMessageFunc = tbb::flow::function_node<UpdateMessage*, UpdateMessage*>;
  using UpdateMessageMultiFunc = tbb::flow::multifunction_node<UpdateMessage*, std::tuple<UpdateMessage*>>;
  // use a multfunction node when there is no downstream consumer (i.e. the output is dropped or can be dropped)
  // using a normal function node would cause buffering.

  tbb::flow::graph updateGraph;
  std::unique_ptr<UpdateMessageFunc> startUpdateNode;
  std::unique_ptr<UpdateMessageFunc> processUpdateNode;
  std::unique_ptr<tbb::flow::sequencer_node<UpdateMessage*> > updateSequencerNode;
  std::unique_ptr<UpdateMessageMultiFunc> updateFinishNode;
  // TODO: what if no one reads from this node??? Will it buffer output and eventually fail or block? Or because
  // there is no edge does it just drop.  Make a test for this!

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
    std::shared_ptr<InputFile> segFile = dir.openFile(Postings::INDEX_INFO_FILE);
    if (segFile.get() == nullptr) {
      gen = 0;
      // TODO: verify directory has no other index files? (i.e. this would tend to indicate corruption)
    } else {
      InputStream segmentsIs = segFile->getInputStream();
      gen = segmentsIs.readVlong();
      auto nSegs = segmentsIs.readVint();
      segInfos.reserve(nSegs);
      for (auto i = 0u; i < nSegs; i++) {
        auto segId = segmentsIs.readVlong();
        int32_t nDocs = segmentsIs.readVint();
        // having ndocs in the list of segments is redundant with info in the segment itself and may be removed later.
        // for now it makes it easy to populate nDocs for merge decisions.
        segInfos.emplace(segId, std::make_unique<SegInfo>(segId, nDocs));
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
      [](UpdateMessage* msg) -> size_t {
        INDEX_DEBUG("updateSequencerNode: msg={} seqNum={}", (void*)msg, msg->seqNum);
        return msg->seqNum;
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
        this->mergeSegmentsBody(*msg);
    });
  }

  void startUpdateBody(UpdateMessage& msg) {
    // if the start node can reject updates, then assigning sequence numbers should be done after that.
    // Sequences must start at 0 for the sequencer nodes.
    msg.seqNum = updateNumber++;
    if (msg.commit != UpdateMessage::NO_COMMIT) {
      msg.commitNum = commitNumber++;
    }
    INDEX_DEBUG("startUpdateBody: msg={} seqNum={} commitNum={}", (void*)&msg, msg.seqNum, msg.commitNum);
  }

  void processUpdateBody(UpdateMessage& msg) {
    INDEX_DEBUG("processUpdateBody: msg={}", (void*)&msg);
    msg.handle(*this);
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

      // first look at any flushing inverters that are not marked for a commit yet
      // and mark them if necessary.
      for (auto it = flushingInverters.begin(); it != flushingInverters.end(); it++) {
        if (it->second->updateMessage == nullptr && it->second->lowestUpdateNum) {
          INDEX_DEBUG("\tinitiateCommit: msg={} marking flushing inverter={} for commit", (void*)&msg, (void*)it->second.get());
          it->second->updateMessage = &msg;
          msg.leftToFlush++;
        }
      }

      // now look at all idle inverters and initiate a flush if necessary.
      for (auto it = idleInverters.begin(); it != idleInverters.end(); it++) {
        if (it->second->lowestUpdateNum <= msg.seqNum) {
          if (it->second->updateMessage == nullptr) {
            INDEX_DEBUG("\tinitiateCommit: msg={} marking idle inverter={} for commit", (void*)&msg, (void*)it->second.get());
            it->second->updateMessage = &msg;
            msg.leftToFlush++;
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
        if (it->second->updateMessage == nullptr && it->second->lowestUpdateNum <= msg.seqNum) {
          INDEX_DEBUG("\tinitiateCommit: msg={} marking busy inverter={} for commit", (void*)&msg, (void*)it->second.get());
          it->second->updateMessage = &msg;
          msg.leftToFlush++;
        }
      }

      INDEX_DEBUG("initiateCommit: msg={} leftToFlush={}", (void*)&msg, msg.leftToFlush);

      // Normally a commit would be kicked off by the last segment flushing.  But if there are no segments to flush,
      // we need to kick it off here.
      if (msg.leftToFlush == 0) {
        commitSequencerNode->try_put(&msg);
      }
    } // end mutex protected section
  }

  // Inverter for the segment should already be in the flushingInverters list.
  // This is called in parallel.
  void segmentFlushBody(Inverter& inverter) {
    INDEX_DEBUG("segmentFlushBody: inverter={} msg={} msg.leftToFlush={}", (void*)&inverter, (void*)inverter.updateMessage,
                inverter.updateMessage == nullptr ? -1 : inverter.updateMessage->leftToFlush);

    {
      // uncomment to serialize inverter flushing
      // const std::lock_guard<std::mutex> lock(indexMutex);
      inverter.flush();
    }

    auto segInfo = std::make_unique<SegInfo>(inverter.getPostingsWriter().segId, inverter.getPostingsWriter().getMaxDoc());

    std::unique_ptr<Inverter> inverterPtr;

    bool triggerCommit = false;
    {
      const std::lock_guard<std::mutex> lock(indexMutex);

      // segments are flushed in parallel, so the segids are not in order... (or in the completed order.) should be fine.
      segInfos.emplace(segInfo->segId, std::move(segInfo));

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
      if (inverter.updateMessage != nullptr) {
        if (--inverter.updateMessage->leftToFlush == 0) {
          triggerCommit = true;
        }
      }
    }

    // It shouldn't be a big deal to do a try_put inside the sync block, but it's safe to do outside.
    if (triggerCommit) {
      commitSequencerNode->try_put(inverter.updateMessage);
    }

    // the inverter (inverterPtr) should go out of scope and be deleted at this point
  }

  // called from the commitFinishNode which has concurrency==1 (single-threaded)
  void finishCommitBody(UpdateMessage& msg) {
    INDEX_DEBUG("finishCommitBody: msg={}", (void*)&msg);
    // TODO: if nothing actually changed, we could skip writing a new commit at this point.

    writeIndexInfoFile();
    msg.done(*this); // don't access msg after this point, it could be deleted.
  }




  // return a copy of the shared_ptr so that the instance it points to will never change while in use.
  std::shared_ptr<IndexReader> getIndexReader() {
    const std::lock_guard<std::mutex> lock(indexReaderMutex);
    if (indexReader.get() != nullptr) {
      return indexReader;
    }

    std::shared_ptr<IndexReader> newReader = std::make_shared<IndexReader>(dir);
    indexReader = newReader;
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
    if (inverter.updateMessage != nullptr) {
      INDEX_DEBUG("releaseInverter: inverter={} message={} triggering flush.", (void*)&inverter, (void*)inverter.updateMessage);
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

  Inverter& obtainInverter() {
    const std::lock_guard<std::mutex> lock(indexMutex);
    if (idleInverters.empty()) {
      auto newInverter = std::make_unique<Inverter>(dir, ++gen);
      Inverter* newInverterPtr = newInverter.get();
      busyInverters.emplace(newInverterPtr, std::move(newInverter));
      return *newInverterPtr;
    } else {
      auto it = idleInverters.begin();
      Inverter& inverter = *it->second;
      busyInverters.emplace(&inverter, std::move(it->second));
      idleInverters.erase(it);
      return inverter;
    }
  }

  // TODO: this is test commit code.  Needs to migrate to use the TBB flow graph.
  void commit() {
    {
      const std::lock_guard<std::mutex> lock(indexMutex);
      // TODO: can copy the map and then iterate over the copy to avoid holding the lock for too long
      while (!idleInverters.empty()) {
        auto it = idleInverters.begin();
        auto& inverter = *it->second;
        inverter.flush();
        segInfos.emplace(inverter.getPostingsWriter().segId,
                         std::make_unique<SegInfo>(inverter.getPostingsWriter().segId,
                                                   inverter.getPostingsWriter().getMaxDoc()));
        idleInverters.erase(it);
      }
    }

    writeIndexInfoFile();
  }


  // This is only called from the finishCommit node, which has concurrency==1 (single-threaded)
  // hence we only need to protect against changes in the segInfos map, not multiple invocations of this method.
  void writeIndexInfoFile() {
    // write new segments file
    // TODO: TBD if we write new segments files or just use the same name
    // Since this could involve network or IO, we should do it outside the lock.
    auto indexFile = dir.createFile(Postings::INDEX_INFO_FILE);
    OutputStream indexOut;
    indexOut.setFile(&*indexFile);


    // TODO: should we write out segments in order of size or creation?

    // need to lock the indexMutex to get a consistent view of the segments.
    // writing the info should be fast (no IO since it should all be buffered in mem)
    {
      const std::lock_guard<std::mutex> lock(indexMutex);

      // Sort the list of segments by the segId.
      // Some tests rely on not reordering segments.
      std::vector<SegInfo*> sortedSegs;
      sortedSegs.reserve(segInfos.size());
      for (auto& seg: segInfos) {
        sortedSegs.push_back(seg.second.get());
      }

      std::sort(sortedSegs.begin(), sortedSegs.end(), [](const SegInfo* a, const SegInfo* b) {
        /*
        // first sort on number of documents (largest first), then on segment id (smallest first)
        if (a->nDocs != b->nDocs) {
          return a->nDocs > b->nDocs;
        }
         */
        return a->segId < b->segId;
      });

      indexOut.writeVlong(gen);
      indexOut.writeVint(sortedSegs.size());
      for (auto seg: sortedSegs) {
        indexOut.writeVlong(seg->segId);
        indexOut.writeVint(seg->nDocs); // TODO: remove this redundancy in the future?
      }

      // TODO: do we need to track exactly what was committed? Or at least the committed "gen"?
    }

    // now actually do the IO outside of the lock
    indexOut.close();
    dir.finishFile(*indexFile);
  }



  // These should live in a merge policy presumably
  int32_t MERGE_FACTOR = 10;
  int32_t MERGE_DOCS_FLOOR = 1000;  // all segments below this will be counted as level 0
  bool mergeRunning = false;


  // TODO: we could alternately pass in the last segment flushed to speed up the merge decision.
  // Merges should be asynchronous... i.e. they should not block the calling thread, a commit, or any updates.
  void maybeMergeSegments() {
    std::vector<SegInfo*> segsCopy;
    {
      const std::lock_guard<std::mutex> lock(indexMutex);
      if (mergeRunning) {
        return;
      }
      if ((int)segInfos.size() < MERGE_FACTOR) {
        return;
      }
      // Make a copy of the segment list and work on it with the lock released?
      // No... not with pointers since a merge could both kick off and delete segments before we continue.
      // We could set mergeRunning to true and then release the lock and work on the copy.
      mergeRunning = true;

      segsCopy.reserve(segInfos.size());
      for (auto& seg : segInfos) {
        segsCopy.push_back(seg.second.get());
      }
    }

    int32_t mergeLevel = -1;
    int32_t level0 = 0;

    // first try common case of a 0 level merge
    for (auto seg : segsCopy) {
      if (seg->nDocs < MERGE_DOCS_FLOOR) {
        level0++;
      }
    }

    if (level0 >= MERGE_FACTOR) {
      mergeLevel = 0;
    } else if (level0 < MERGE_FACTOR && (int)segsCopy.size() >= level0 + MERGE_FACTOR) {
      // look for merges at all levels
      std::vector<int32_t> levelCounts;
      // logM(x) = log2(x)/log2(M) where M is merge factor.  Calculate 1/log2(M) once and cache.
      float inverseLogM = 1.0f / log2(MERGE_FACTOR);
      for (auto& seg : segsCopy) {
        // calculate the merge level
        if (seg->nDocs < MERGE_DOCS_FLOOR) {
          seg->mergeLevel = 0;
        } else {
          int32_t adjustedDocs = seg->nDocs - MERGE_DOCS_FLOOR + 1;
          seg->mergeLevel = (int32_t) (log2(adjustedDocs) * inverseLogM);
        }
        if (seg->mergeLevel >= (int)levelCounts.size()) {
          levelCounts.resize(seg->mergeLevel + 1);
        }
        levelCounts[seg->mergeLevel]++;
      }

      for (auto i = 0u; i < levelCounts.size(); i++) {
        if (levelCounts[i] >= MERGE_FACTOR) {
          mergeLevel = i;
          break;
        }
      }
    }

    if (mergeLevel < 0) {
      // no merge needed
      const std::lock_guard<std::mutex> lock(indexMutex);
      mergeRunning = false;
      return;
    }
  }

  // called from the mergeSegmentsNode which has concurrency==1 (single-threaded)
  // This is passed SegInfo pointers, which should be safe since the only place where segments
  // are currently deleted is via a merge.
  // FUTURE: what if we want to be able to drop all segments or drop certain segments.  Frame that as a merge to
  // maintain that invariant?
  void mergeSegmentsBody(MergeMessage& msg) {
    {
      // grab or open all the postings readers
      std::vector<std::shared_ptr<PostingsReader>> preaders;
      preaders.reserve(msg.segs.size());
      for (auto segInfo: msg.segs) {
        preaders.emplace_back(segInfo->sharedPostingsReader.load());
        if (!preaders.back()) {
          preaders.back() = std::make_shared<PostingsReader>(dir, segInfo->segId);
          segInfo->sharedPostingsReader.store(preaders.back());
        }
      }

      MemPool pool;

      // Copy the preaders to a vector of pointers for our underlying merge code.
      // this is temprary... mergers will need more info at some point to handle deletes.
      std::vector<PostingsReader*> preaderPtrs;
      preaderPtrs.reserve(preaders.size());
      for (auto& preader : preaders) {
        preaderPtrs.push_back(preader.get());
      }

      PostingsWriter pwriter(dir, ++gen);
      mergeSegments(pool, preaderPtrs, pwriter);

      // now remove the merged segments from the segInfos and add the new segment.
      // Any removed segments that were not part of a previous commit can be deleted immediately.
      // If segments were part of a previous commit, then we need to write out a new info file *before* removing the index files.
      // Example scenario: merge starts, commit happens, merge finishes, crash.
      std::vector<std::unique_ptr<SegInfo>> removedSegs;
      {
        const std::lock_guard<std::mutex> lock(indexMutex);
        for (auto segInfo : msg.segs) {
          auto it = segInfos.find(segInfo->segId);
          assert(it != segInfos.end());
          if (it == segInfos.end()) {
            LOG_ERROR("Segment not found in segInfos.");
            assert(false);
          }
          removedSegs.emplace_back(std::move(it->second));
          segInfos.erase(it);
        }

        // Create & add the new SegmentInfo
        segInfos.emplace(pwriter.getSegId(), std::make_unique<SegInfo>(pwriter.getSegId(), pwriter.getMaxDoc()));
      }

      // TODO: we need to go through finishCommit node so that it remains single threaded!
      // Slipping into the normal commit order should be fine since finishing a commit always grabs
      // the latest list of segments (i.e. there is no going back in time)

      writeIndexInfoFile();
    } // end of scope for preaders, pool, and pwriter

    // TODO: FIXME: finishCommitNode->try_put(&msg);
  }


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
    uint64_t segId = gen++;
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
};

// Should there be a single-threaded IndexWriter and a different multi-threaded version?

}
