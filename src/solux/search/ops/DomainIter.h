#pragma once
#include "solux/search/DocSet.h"
#include "solux/reader/DocsEnum.h"
#include "solux/reader/DocsReader.h"
#include "solux/reader/IntColReader.h"
#include "solux/reader/OrdColReader.h"
#include "solux/util/solux_util.h"

namespace solux {

// Resolved once per (segment, facet), rather than rediscovering the domain
// representation for every term.
struct DomainView {
  DocSet* domain;
  const FixedBitSet* bits;
  ArrDocSet* arr;
  int32_t maxDoc;
  int32_t card;
  int32_t compCard;

  DomainView(DocSet* domain, int32_t maxDoc)
      : domain(domain),
        bits(domain != nullptr && domain->type == DocSet::Type::BITSET
                 ? &((BitDocSet*)domain)->bits()
                 : nullptr),
        arr(domain != nullptr && domain->type == DocSet::Type::ARRAY
                ? (ArrDocSet*)domain
                : nullptr),
        maxDoc(maxDoc),
        card(domain != nullptr ? domain->card() : maxDoc),
        compCard(maxDoc - card) {
    assert(card >= 0 && card <= maxDoc);
  }
};

// Count one term's postings in the domain, driving from the smallest available
// side. An array domain does not offer complement drive: its complement is
// dense and cannot be enumerated without reconstructing a bitset.
inline int32_t countTermInDomain(const DomainView& view, DocsOnlyEnum& postings,
                                 int32_t docFreq) {
  // RootOp only uses a null domain when the segment has no deletions. Thus an
  // empty complement means the domain is all [0,maxDoc), and deletion-unaware
  // docFreq is already the exact answer without advancing postings.
  if (view.compCard == 0) {
    return docFreq;
  }
  assert(view.domain != nullptr);  // a null domain always has an empty complement

  bool driveComplement =
      view.bits != nullptr && view.compCard < view.card
      && view.compCard < docFreq;
  bool driveDomain = !driveComplement && view.card < docFreq;

  if (!driveComplement && !driveDomain) {
    // Postings drive: the membership test is the loop body, so resolve the
    // domain representation out here.  A virtual DocSet::get() per posting is
    // never devirtualized through this header and costs more than the test.
    int32_t hits = 0;
    auto walk = [&](auto&& member) SOLUX_INLINE {
      for (int32_t doc = postings.nextDoc(); doc != DocsEnumMeta::END;
           doc = postings.nextDoc()) {
        if (member(doc)) {
          hits++;
        }
      }
    };
    if (view.bits != nullptr) {
      walk([&](int32_t doc) SOLUX_INLINE { return view.bits->get(doc); });
    } else {
      walk([&](int32_t doc) SOLUX_INLINE { return view.arr->get(doc); });
    }
    return hits;
  }

  auto match = [&](int32_t target) SOLUX_INLINE {
    if (postings.docId() < target) {
      postings.advance(target);
    }
    return postings.docId() == target;
  };

  int32_t hits = 0;
  if (driveDomain) {
    if (view.arr != nullptr) {
      for (int32_t doc : view.arr->docs()) {
        if (match(doc)) {
          hits++;
        }
        if (postings.docId() == DocsEnumMeta::END) {
          break;
        }
      }
    } else {
      int32_t doc = -1;
      while (doc + 1 < view.maxDoc) {
        doc = view.bits->nextSetBit(doc + 1);
        if (doc >= view.maxDoc) {
          break;
        }
        if (match(doc)) {
          hits++;
        }
        if (postings.docId() == DocsEnumMeta::END) {
          break;
        }
      }
    }
    return hits;
  }

  // The complement includes deleted docs while the domain does not. This is
  // intentional: docFreq also includes deleted postings, so df-complement is
  // the live/filter-domain count with no separate liveDocs correction.
  int32_t doc = -1;
  while (doc + 1 < view.maxDoc) {
    doc = view.bits->nextClearBit(doc + 1);
    if (doc >= view.maxDoc) {
      break;
    }
    if (match(doc)) {
      hits++;
    }
    if (postings.docId() == DocsEnumMeta::END) {
      break;
    }
  }
  assert(hits <= docFreq);
  return docFreq - hits;
}

// Number of in-domain docs without the field. DocsReader's bitset is the
// segment-wide docs-with-field set, so this is shared by text facets and the
// term-driven string strategy.
inline int32_t countMissingInDomain(const DomainView& view,
                                    const DocsReader& docsReader) {
  if (!docsReader.hasBitset()) {
    return 0;
  }
  if (view.domain == nullptr) {
    return view.card - docsReader.numDocs();
  }

  int32_t haveField = 0;
  screaming::BitSet::Iterator it(docsReader.bitset());
  for (int32_t doc = it.next(); doc != screaming::BitSet::END; doc = it.next()) {
    if (view.domain->get(doc)) {
      haveField++;
    }
  }
  assert(haveField <= view.card);
  return view.card - haveField;
}

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
