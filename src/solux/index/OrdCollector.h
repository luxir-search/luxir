#pragma once

#include <deque>
#include "DocStream.h"
#include "solux/util/MemPool.h"

namespace solux {

// Uninverts a field: collects ords for a given index during a term-major walk and
// lets the ords for that index be read back in index order.  The index is a dense
// 0-based key -- either a docid (dense/full fields) or a field-rank (the rank among
// docs-with-value, for sparse fields; the caller resolves rank->docid via the
// docs-with-field set, see StrHandler).  Passing a rank keeps storage at one slot
// per doc-with-value instead of one per doc, so sparse fields cost O(docsWithValue)
// rather than O(maxDoc).  Ords are 1-based and, for any given index, must be added
// in increasing order.
class OrdCollector {
  MemPool& pool;
  std::vector<int32_t> ords;
  bool multiValued_ = false; // multiple values encountered for an index?
  int32_t docsWithValue_ = 0;
public:
  /// Ord collector adds ords to the pool over time, so be sure you don't rewind the pool
  /// before you are done with the OrdCollector.  Or more specifically, if X is the pool size
  /// after the last call to add(), then don't rewind the pool to a size less than X.
  OrdCollector(MemPool& pool, int32_t numSlots) : pool(pool), ords(numSlots)
  {}

  // add a 1-based ord for the given index (docid or field-rank)
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
      // The tag bit halves the pool's 4 GiB address space: this collector's
      // pool must stay under 2 GiB (its per-flush scratch pools are far
      // smaller in practice).
      assert((streamAddr & 0x80000000u) == 0);
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

  // number of index slots (maxDoc for docid-indexed, docsWithValue for rank-indexed)
  int32_t size() const {
    return (int32_t)ords.size();
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