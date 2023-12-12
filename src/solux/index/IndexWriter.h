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

// we may want to decouple from protobuf in the future, but for now it makes it easy to develop
#include "protos/solux_types.pb.h"

// redefine DEBUG to TRACE level which shouldn't currently be logged!
#define INDEX_DEBUG LOG_TRACE
// #define INDEX_DEBUG LOG_DEBUG

namespace solux {

// An update message to be processed by the TBB update flow graph.
// This was an inner class to IndexWriter, but it can't be forward declared in Inverter that way.
class UpdateMessage {
public:
  // update protobuf message
  solux::proto::UpdateRequest* req;  // The request object may become unavailable after the callback is called
  std::function<void(UpdateMessage*)> callback;    // Called after the update has completed
  bool commit = false;                // commit after this update is done?
  uint64_t seqNum;                    // The sequence number of this update, used to ensure updates are processed in order when needed
  uint64_t commitNum;                 // The commit number of this update, used to ensure commits are finished in order

  // Number of segments left to flush, protected by same mutex that protects the inverter lists.
  // making this an atomic is not enough to avoid race conditions since we also depend on coordination with
  // inverter->updateMessage, among other things.
  uint32_t leftToFlush = 0;
};


/// The IndexWriter is a level above Inverter & PostingsWriter that coordinates
/// indexing activity for a single index / directory.
class IndexWriter {
  // increment a base 36 string
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

  Directory &dir;
  uint64_t gen;

  // segs is currently only used when writing the index info file.
  std::vector<std::string> segs;  // all of the referenced segments (TODO: replace with something containing more info when needed)

  std::shared_ptr<IndexReader> indexReader;

  // protected by indexMutex
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
      int nSegs = segmentsIs.readVint();
      segs.reserve(nSegs);
      for (int i = 0; i < nSegs; i++) {
        auto s = segmentsIs.readStr();
        segs.emplace_back(s);
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
         this->segmentFlushBody(*inverter);
    });

    commitSequencerNode = std::make_unique<tbb::flow::sequencer_node<UpdateMessage*> >(updateGraph,
      [](UpdateMessage* msg) -> size_t {
        INDEX_DEBUG("commitSequencerNode: msg={} commitNum={}", (void*)&msg, msg->commitNum);
        return msg->commitNum;
      });

    commitFinishNode = std::make_unique<UpdateMessageMultiFunc>(updateGraph, 1,
      [this](UpdateMessage* msg, UpdateMessageMultiFunc::output_ports_type& op) {
         this->finishCommitBody(*msg);
         // std::get<0>(op).try_put(msg);
    });

    tbb::flow::make_edge(*commitSequencerNode, *commitFinishNode);
  }

  void startUpdateBody(UpdateMessage& msg) {
    // if the start node can reject updates, then assigning sequence numbers should be done after that.
    // Sequences must start at 0 for the sequencer nodes.
    msg.seqNum = updateNumber++;
    if (msg.commit) {
      msg.commitNum = commitNumber++;
    }
    INDEX_DEBUG("startUpdateBody: msg={} seqNum={} commitNum={}", (void*)&msg, msg.seqNum, msg.commitNum);
  }

  void processUpdateBody(UpdateMessage& msg) {
    INDEX_DEBUG("processUpdateBody: msg={}", (void*)&msg);
    update(*msg.req);
  }

  void finishUpdateBody(UpdateMessage& msg) {
    INDEX_DEBUG("finishUpdateBody: msg={}", (void*)&msg);

    // If this update has a commit, we could either kick it off here, or send a message to a commit node.
    // Gathering the required inverters as quickly as possible might be good (i.e. do it here)
    // Aside: if each inverter keeps track of the highest (and lowest?) update message it has seen, could that be used somehow?
    //  - could avoid dragging in an unneeded inverter.

    if (msg.commit) {
      initiateCommit(msg);
    }

    if (!msg.commit && msg.callback) {
      msg.callback(&msg);
      // don't access msg after this point, it could be deleted.
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
    std::string segid = inverter.getPostingsWriter().getSegId();

    std::unique_ptr<Inverter> inverterPtr;

    bool triggerCommit = false;
    {
      const std::lock_guard<std::mutex> lock(indexMutex);

      // segments are flushed in parallel, so the segids are not in order... (or in the completed order.) should be fine.
      segs.emplace_back(segid);

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
    {
      const std::lock_guard<std::mutex> lock(indexMutex);
      writeIndexInfoFile();
    }

    if (msg.callback) {
      msg.callback(&msg);
      // don't access msg after this point, it could be deleted.
    }
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
      gen++;
      std::string genStr = Postings::getSortableString(gen);
      auto newInverter = std::make_unique<Inverter>(dir, genStr);
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

  // Currently only commits idle inverters!
  // future strategy: record a list of busy inverters and flush them when they are released.
  // Multiple commits in flight:
  //   - index info files must be written in order
  //      - can we dynamically make one commit task depend on the other?
  //   - on a second commit, make sure we only flush inverters *not* marked to be flushed by other commits.
  //   - on a commit, mark all inverters as to be flushed by this commit.
  //   - segment merges may complete before or after, so the commit writes the latest list of segments.
  //     - flushing small segments first could give an opportunity to merge them before the commit.
  // we can have multiple commits in flight, but we need to make sure that
  // the index info file is written in the correct order.

  void commit() {
    const std::lock_guard<std::mutex> lock(indexMutex);
    // TODO: can copy the map and then iterate over the copy to avoid holding the lock for too long
    while (!idleInverters.empty()) {
      auto it = idleInverters.begin();
      auto& inverter = *it->second;
      inverter.flush();
      std::string segid = inverter.getPostingsWriter().getSegId();
      segs.emplace_back(segid);
      idleInverters.erase(it);
    }
    writeIndexInfoFile();
  }


  void writeIndexInfoFile() {
    // write new segments file
    // TODO: TBD if we write new segments files or just use the same name
    auto indexFile = dir.createFile(Postings::INDEX_INFO_FILE);
    OutputStream indexOut;
    indexOut.setFile(&*indexFile);

    indexOut.writeVlong(gen);
    indexOut.writeVint(segs.size());
    for (auto &seg : segs) {
      indexOut.writeStr(seg);
    }
    indexOut.close();
    dir.finishFile(*indexFile);
  }


  // TODO: this should perhaps be moved to the grpc specific code?
  void update(solux::proto::UpdateRequest& request) {
    // if there are no docs, then we can just return before grabbing an inverter.
    if (request.docs_size() == 0) {
      if (request.has_columns()) {
        std::cout << "\tindexer got columns (not yet implemented!): " << request.columns().columns_size() << std::endl;
      }
      return;
    }

    Inverter& inverter = obtainInverter();  // TODO: make sure to release it on exceptions (use a unique_ptr with a custom deleter?)

    std::vector<Inverter::IndexHandler*> handlers;
    // int ndocs = request->docs_size();

    if (request.docs_size() > 0) {
      for (const auto &doc : request.docs()) {
        size_t nFields = doc.fields_size();
        if (handlers.size() < nFields) {
          handlers.resize(nFields);
        }

        inverter.startDoc();

        // TODO: wrap in try/catch here?

        int idx = 0;
        for (const auto&[fname, fval] : doc.fields()) {
          auto handler = handlers[idx];
          if (handler == nullptr || *handler != fname) {
            handlers[idx] = handler = &inverter.getIndexHandler(fname);
          }
          handler->index(inverter, fval);
          idx++;
        }

        inverter.finishDoc();
      }
    }

    releaseInverter(inverter);
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
    gen++;  // TODO: not thread safe or logic safe with rest of IW
    auto genStr = Postings::getSortableString(gen);
    PostingsWriter pwriter(dir, genStr);

    MemPool pool;
    mergeSegments(pool, preaders, pwriter);

    // update the list of segments... not safe currently
    // TODO: add unused segments to the "to be deleted" list
    segs.clear();
    segs.push_back(genStr);

    writeIndexInfoFile();  // TODO: currently for testing... we wouldn't normally do this here.
  }


  // TODO: can merging be decoupled and done by something else?  What about even on a different node?
  // overwrites would be the only tricky part...


  void mergeSegments(MemPool &pool, std::span<PostingsReader *> preaders, PostingsWriter &postingsWriter);
};

// Should there be a single-threaded IndexWriter and a different multi-threaded version?

}
