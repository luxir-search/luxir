#pragma once

#include <string>
#include <charconv>
#include "solux/store/Directory.h"
#include "solux/store/OutputStream.h"
#include "solux/store/InputStream.h"
#include "Inverter.h"
#include "PostingsWriter.h"

namespace solux {

/// The IndexWriter is a level above Inverter & PostingsWriter that coordinates
/// indexing activity for a single index / directory.
class IndexWriter {
  // Create a sortable string from a number.  It's currently
  // a base36 representation prefixed with the number of digits to make it sort correctly.
  std::string getSortableString(uint64_t val) {
    std::array<char, 14> arr; // Need 13 digits (log(2**64)/log(36)==12.3) plus one for the length prefix.
    auto[end, ec] = std::to_chars(arr.begin() + 1, arr.end(), val, 36);
    int digits = end - (arr.begin() + 1);
    arr[0] = digits <= 9 ? ('0' + digits) : ('a' + (digits - 10));  // base36 prefix
    return std::string(arr.begin(), end);
  }


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


public:

  Directory &dir;
  uint64_t gen;
  std::unique_ptr<Inverter> inverter;
  std::vector<std::string> segs;  // all of the referenced segments (TODO: replace with something containing more info when needed)

  explicit IndexWriter(Directory &dir) : dir(dir) {
    std::shared_ptr<InputFile> segFile = dir.openFile(Postings::SEGFILE);
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

  // What about multiple inverters on the same thread (because of sharding)?  Another alternative (if micro-sharding will be common)
  // is to enable it from a single Inverter (i.e. inverter can split and write to multiple postings writers)
  // TODO: should we be able to provide an inverter instead of get?
  // TODO: currently not thread safe
  // Only valid until a flush
  Inverter &getInverter() {
    if (inverter.get() == nullptr) {
      inverter = std::make_unique<Inverter>();
    }
    return *inverter;
  }

  // Use proto3 for segments file?  Easier back compat / modification?

  // TODO: currently not thread safe
  // just pass in Inverter here?
  void flush() {
    if (inverter.get() == nullptr) return;
    // TODO: check if inverter actually inverted any docs?

    gen++;
    std::string genStr = getSortableString(gen);
    PostingsWriter postingsWriter(dir, genStr);
    inverter->writePostings(postingsWriter);
    postingsWriter.finish();

    segs.emplace_back(genStr);
    // write new segments file
    // TODO: TBD if we write new segments files or just use the same name
    auto segFile = dir.createFile(Postings::SEGFILE);
    OutputStream segOut;
    segOut.setFile(&*segFile);

    segOut.writeVlong(gen);
    segOut.writeVint(segs.size());
    for (auto &seg : segs) {
      segOut.writeStr(seg);
    }
    segOut.flush(true);
    dir.finishFile(*segFile);

    inverter.reset();
  }

  // TODO: can merging be decoupled and done by something else?  What about even on a different node?

};

// Should there be a single-threaded IndexWriter and a different multi-threaded version?

}
