#pragma once
#include "solux/search/DocSet.h"
#include "solux/reader/IntColReader.h"
#include "solux/reader/OrdColReader.h"
#include "solux/util/solux_util.h"

namespace solux {

// The single place that knows how to walk a facet/stats domain over an int
// column: domain type (array / bitset / null=all-docs) x single-vs-multi-valued
// x dense-vs-sparse column iterator.
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

template <bool TrackStats, class F>
void forEachOrdValueImpl(DocSet* domain, OrdColReader& ordColReader,
                         int32_t maxDoc, int64_t& missing_num, F&& callback,
                         OrdColReader::ForEachOrdStats* stats) {
  if (domain && domain->type == DocSet::Type::ARRAY) {
    ordColReader.forEachOrd(
        ((ArrDocSet*)domain)->docs(), missing_num, callback, stats);
    return;
  }

  const FixedBitSet* bits = domain ? &((BitDocSet*)domain)->bits() : nullptr;
  if (bits != nullptr && ordColReader.docIdIndexed()) {
    ordColReader.forEachDocIdBitOrd(
        *bits, maxDoc, OrdColReader::configuredBulkMinHits(),
        missing_num, callback, stats);
    return;
  }

  if (ordColReader.docIdIndexed()) {
    assert(!ordColReader.multiValued());
    OrdColReader::BulkOrds values(ordColReader);
    int32_t docid = -1;
    while (docid + 1 < maxDoc) {
      docid = bits ? bits->nextSetBit(docid + 1) : docid + 1;
      if (docid >= maxDoc) break;
      int32_t ord = values.valueAt(docid);
      if constexpr (TrackStats) stats->bulkLoads++;
      if (ord != 0) callback(docid, ord);
      else missing_num++;
    }
    return;
  }

  // Preserve the pre-adaptive fixed-bulk route for RANK columns. Domain
  // density is in doc-id space and cannot select a value-rank decode policy.
  OrdColReader::Iterator iter(ordColReader);
  int32_t docid = -1;
  while (docid + 1 < maxDoc) {
    docid = bits ? bits->nextSetBit(docid + 1) : docid + 1;
    if (docid >= maxDoc) break;
    if (iter.docId() < docid) iter.advance(docid);
    if (iter.docId() != docid) {
      missing_num++;
      continue;
    }
    if (!ordColReader.multiValued()) {
      callback(docid, iter.value());
      if constexpr (TrackStats) stats->bulkLoads++;
    } else {
      auto [start, end] = ordColReader.getStartEndValueRank(iter.rank());
      for (int64_t rank = start; rank < end; rank++) {
        callback(docid, iter.values().valueAt(rank));
        if constexpr (TrackStats) stats->bulkLoads++;
      }
    }
  }
}

template <class F>
void forEachOrdValue(DocSet* domain, OrdColReader& ordColReader,
                     int32_t maxDoc, int64_t& missing_num, F&& callback) {
  forEachOrdValueImpl<false>(
      domain, ordColReader, maxDoc, missing_num,
      std::forward<F>(callback), nullptr);
}

template <class F>
void forEachOrdValue(DocSet* domain, OrdColReader& ordColReader,
                     int32_t maxDoc, int64_t& missing_num, F&& callback,
                     OrdColReader::ForEachOrdStats* stats) {
  forEachOrdValueImpl<true>(
      domain, ordColReader, maxDoc, missing_num,
      std::forward<F>(callback), stats);
}

}
