#pragma once
#include "solux/search/DocSet.h"
#include "solux/reader/IntColReader.h"
#include "solux/util/solux_util.h"

namespace solux {

// The single place that knows how to walk a facet/stats domain over an int
// column: domain type (array / bitset / null=all-docs) x single-vs-multi-valued
// x dense-vs-sparse column iterator.  Centralizing it kills the hand-rolled
// (BitDocSet*)/(ArrDocSet*) downcasts that previously lived at every consumer
// (the P0-2 mis-cast bug came from exactly such a bypass), and gives AvgOp a
// domain-driven scan instead of a full 0..maxDoc loop with a virtual
// DocSet::get() per doc (a binary search for array domains).
//
// callback(int32_t docid, int64_t value) is invoked once per value of each
// in-domain doc that has the field.  In-domain docs with no value bump
// missing_num.  The field-absent case is the caller's responsibility (it must
// be handled before constructing intColReader).
//
// IMPORTANT: mark the callback SOLUX_INLINE at the call site (e.g.
// `[&](int32_t docid, int64_t val) SOLUX_INLINE { ... }`).  The per-value work
// is the hot loop body; always_inline folds it into the loop here instead of
// leaving it to the inliner's heuristics through this template layer.
template <class F>
void forEachIntColValue(DocSet* domain, IntColReader& intColReader,
                        int32_t maxDoc, int64_t& missing_num, F&& callback) {
  if (domain && domain->type == DocSet::Type::ARRAY) {
    // Sparse domain: iterate the (sorted) doc list and big-skip the column with
    // the value-decoding SparseIterator.
    ArrDocSet* arrDocs = (ArrDocSet*) domain;
    IntColReader::SparseIterator iter(intColReader);
    if (!intColReader.multiValued()) {
      for (auto docid : arrDocs->docs()) {
        if (iter.docId() < docid) {
          iter.advance(docid);
        }
        if (iter.docId() == docid) {
          callback(docid, iter.value());
        } else {
          missing_num++;
        }
      }
    } else {
      for (auto docid : arrDocs->docs()) {
        if (iter.docId() < docid) {
          iter.advance(docid);
        }
        if (iter.docId() == docid) {
          auto [start, end] = intColReader.getStartEndValueRank(iter.rank());
          for (int64_t vrank = start; vrank < end; vrank++) {
            callback(docid, iter.values().valueAt(vrank));
          }
        } else {
          missing_num++;
        }
      }
    }
    return;
  }

  // Bitset domain (skip to set bits) or null domain (every doc): dense scan with
  // the block-decoding BulkIterator.
  const FixedBitSet* bits = nullptr;
  if (domain) {
    bits = &((BitDocSet*) domain)->bits();
  }
  IntColReader::Iterator intColIter(intColReader);
  int32_t docid = -1;
  while (docid + 1 < maxDoc) {
    if (bits) {
      docid = bits->nextSetBit(docid + 1);
      if (docid >= maxDoc) {
        break;
      }
    } else {
      docid++;
    }
    if (intColIter.docId() < docid) {
      intColIter.advance(docid);
    }
    if (intColIter.docId() == docid) {
      if (!intColReader.multiValued()) {
        callback(docid, intColIter.value());
      } else {
        auto [start, end] = intColReader.getStartEndValueRank(intColIter.rank());
        auto n = end - start;
        for (int64_t vrank = 0; vrank < n; vrank++) {
          callback(docid, intColIter.values().valueAt(start + vrank));
        }
      }
    } else {
      missing_num++;
    }
  }
}

}
