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


namespace solux {

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
  std::vector<std::string> segs;  // all of the referenced segments (TODO: replace with something containing more info when needed)

  std::shared_ptr<IndexReader> indexReader;

  // TODO: change mutex used to protect the inverter lists.
  boost::unordered_flat_map<Inverter*, std::unique_ptr<Inverter>> idleInverters;
  boost::unordered_flat_map<Inverter*, std::unique_ptr<Inverter>> busyInverters;
  boost::unordered_flat_map<Inverter*, std::unique_ptr<Inverter>> flushingInverters;


  // An update message to be processed by the TBB update flow graph
  class UpdateMessage {
  public:
    // update protobuf message
    solux::proto::UpdateRequest* req;  // The request object may become unavailable after the callback is called
    std::function<void(UpdateMessage*)> callback;    // Called after the update has completed
    uint64_t seqNum;                    // The sequence number of this update, used to ensure updates are processed in order when needed
    uint64_t commitNum;                 // The commit number of this update, used to ensure commits are finished in order
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
  //  - if something fails, we need to still flow it through the graph so the sequencers are happy (i.e. so they
  //    won't block updates / commits that come after.

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

  using InverterMultiFunc = tbb::flow::multifunction_node<UpdateMessage*, std::tuple<UpdateMessage*>>;
  std::unique_ptr<InverterMultiFunc> segmentFlushNode;

  std::unique_ptr<tbb::flow::sequencer_node<UpdateMessage*> > commitSequencerNode;
  std::unique_ptr<UpdateMessageMultiFunc> commitFinishNode;

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
      [this](UpdateMessage* msg, InverterMultiFunc::output_ports_type& op) {
         this->flushSegBody(*msg);
         int left = 1; // --msg->leftToFlush; // TODO
         if (left <= 0) {
           // no more flushes left to wait for, so put message to output port.
           // is there an advantage to doing it this way (using an edge) vs just doing a try_put on the node directly?
           std::get<0>(op).try_put(msg);
         }
    });

    commitSequencerNode = std::make_unique<tbb::flow::sequencer_node<UpdateMessage*> >(updateGraph,
      [](UpdateMessage* msg) -> size_t {
        return msg->commitNum;
      });

    commitFinishNode = std::make_unique<UpdateMessageMultiFunc>(updateGraph, 1,
      [this](UpdateMessage* msg, UpdateMessageMultiFunc::output_ports_type& op) {
         this->finishCommitBody(*msg);
         // std::get<0>(op).try_put(msg);
    });

    // connect the segment flush + commit nodes
    tbb::flow::make_edge(*segmentFlushNode, *commitSequencerNode);
    tbb::flow::make_edge(*commitSequencerNode, *commitFinishNode);
  }

  void startUpdateBody(UpdateMessage& msg) {
  }
  void processUpdateBody(UpdateMessage& msg) {
  }
  void finishUpdateBody(UpdateMessage& msg) {
    // If this update has a commit, we could either kick it off here, or send a message to a commit node.
    // Gathering the required inverters as quickly as possible might be good (i.e. do it here)
    // Aside: if each inverter keeps track of the highest (and lowest?) update message it has seen, could that be used somehow?
    //  - could avoid dragging in an unneeded inverter.
  }

  void flushSegBody(UpdateMessage& msg) {
  }
  void finishCommitBody(UpdateMessage& msg) {
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
      throw std::runtime_error("Inverter not found in busyInverters");
    }

    // TODO: update and check global statistics
    // TODO: if the inverter is over a certain size, flush it
    // TODO: if inverter is one we were waiting for before a commit, update that info (on a commit request, take a snapshot of the busy inverters?)

    idleInverters.emplace(&inverter, std::move(it->second));
    busyInverters.erase(it);
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


  // TODO: Currently single threaded and protected by the IndexWriter mutex... we need something different
  // in the future that can utilize multi-threading.
  // TODO: this should perhaps be moved to the grpc specific code.
  void update(solux::proto::UpdateRequest& request) {
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

    if (request.has_columns()) {
      std::cout << "\tindexer got columns (not yet implemented!): " << request.columns().columns_size() << std::endl;
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
