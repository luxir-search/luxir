#pragma once

#include <deque>
#include "DocStream.h"
#include "solux/util/MemPool.h"

namespace solux {

// uninverts a field, collecting docids for a given ordinal and then allowing
// ordinals to be looked up by docid.  Ordinals should be 1-based and be collected in increasing order
// for any given docid.
class OrdCollector {
  MemPool& pool;
  // TODO: OPT: This isn't good for sparse fields... we should probably have a version that uses a hash table as well.
  // Or, we could have a SparseOrdCollector that inherits from OrdCollector (or just uses the same interface).
  // Merge logic would be able to tell which implementation should be used based on stats of the fields to be merged.
  std::vector<int32_t> ords;
  bool multiValued_ = false; // multiple values encountered for a docid?
  int32_t docsWithValue_ = 0;
public:
  /// Ord collector adds ords to the pool over time, so be sure you don't rewind the pool
  /// before you are done with the OrdCollector.  Or more specifically, if X is the pool size
  /// after the last call to add(), then don't rewind the pool to a size less than X.
  OrdCollector(MemPool& pool, int32_t numDocs) : pool(pool), ords(numDocs)
  {}

  // add a 1-based ord for the given docid
  void add(int32_t docid, int32_t ord) {
    assert(ord > 0 && docid >= 0);
    // For something more memory efficient, look at Solr's UnInvertedField.
    auto v = ords[docid];
    if (v == 0) {
      ords[docid] = ord;
      docsWithValue_++;
      return;
    }
    IntDeltaStream* stream;
    if (v & 0x80000000) {
      // list of ords in a stream
      int streamAddr = v & 0x7fffffff;
      stream = (IntDeltaStream*)pool.ptr(streamAddr);
    } else {
      multiValued_ = true;
      // need to convert from a single value to a stream
      auto [ptr, streamAddr] = pool.allocateAddrs(sizeof(IntDeltaStream));  // IntDeltaStream is current 22 bytes, relatively heavyweight.
      stream = new (ptr) IntDeltaStream(pool);
      stream->addVal(pool, v);
      ords[docid] = streamAddr | 0x80000000;
    }
    stream->addVal(pool, ord);
  }

  bool hasValues(int32_t docid) const {
    return ords[docid] != 0;
  }

  bool multiValued() const {
    return multiValued_;
  }

  int32_t docsWithValue() const {
    return docsWithValue_;
  }

  // calls acceptor(int32_t ord) for each ord for the given docid
  template <class Acceptor>
  void pushValues(int32_t docid, Acceptor&& acceptor) const {
    auto v = ords[docid];
    if (v == 0) {
      return;
    }
    if (v & 0x80000000) {
      // list of ords in a stream
      int streamAddr = v & 0x7fffffff;
      IntDeltaStream* stream = (IntDeltaStream*)pool.ptr(streamAddr);
      stream->pushValues(pool, acceptor);
    } else {
      acceptor(v);
    }
  }

};


}