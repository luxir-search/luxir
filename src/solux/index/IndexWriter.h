#pragma once

#include <string>
#include <charconv>
#include <thread>
#include <mutex>
#include <span>
#include "boost/unordered/unordered_flat_map.hpp"
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
      std::cout << "\tindexer got columns: " << request.columns().columns_size() << std::endl;
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
