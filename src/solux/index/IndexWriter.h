#pragma once

#include <string>
#include <charconv>
#include <thread>
#include <mutex>
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
  std::unique_ptr<Inverter> inverter;
  std::vector<std::string> segs;  // all of the referenced segments (TODO: replace with something containing more info when needed)

  std::shared_ptr<IndexReader> indexReader;

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



  // What about multiple inverters on the same thread (because of sharding)?  Another alternative (if micro-sharding will be common)
  // is to enable it from a single Inverter (i.e. inverter can split and write to multiple postings writers)
  // TODO: should we be able to provide an inverter instead of get?
  // TODO: currently not thread safe
  // Only valid until a flush!
  // Make this a thread-local?
  Inverter &getInverter() {
    if (inverter == nullptr) {
      gen++;
      inverter = std::make_unique<Inverter>(dir, Postings::getSortableString(gen));
    }
    return *inverter;
  }

  // Use proto3 for segments file?  Easier back compat / modification?

  // TODO: currently not thread safe
  // just pass in Inverter here?
  void flush() {
    const std::lock_guard<std::mutex> lock(indexMutex);

    if (inverter == nullptr) return;
    // TODO: check if inverter actually inverted any docs?

    gen++;
    std::string genStr = Postings::getSortableString(gen);
    inverter->flush();
    std::string segid = inverter->getPostingsWriter().getSegId();
    inverter.reset();

    segs.emplace_back(segid);
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
  void update(solux::proto::UpdateRequest& request) {
    const std::lock_guard<std::mutex> lock(indexMutex);

    Inverter& inverter = getInverter();

    std::vector<Inverter::IndexHandler*> handlers;
    // int ndocs = request->docs_size();

    if (request.docs_size() > 0) {
      for (auto &doc : request.docs()) {
        size_t nFields = doc.fields_size();
        if (handlers.size() < nFields) {
          handlers.resize(nFields);
        }

        inverter.startDoc();

        // TODO: wrap in try/catch here?

        int idx = 0;
        for (auto&[fname, fval] : doc.fields()) {
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

  }

  // TODO: can merging be decoupled and done by something else?  What about even on a different node?
  // overwrites would be the only tricky part...
  void mergeSegments() {

  }

};

// Should there be a single-threaded IndexWriter and a different multi-threaded version?

}
