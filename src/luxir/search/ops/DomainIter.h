#pragma once
#include "luxir/search/PostingsIntersection.h"
#include "luxir/reader/DocsReader.h"
#include "luxir/reader/IntColReader.h"
#include "luxir/reader/OrdColReader.h"
#include "luxir/util/MemPool.h"
#include "luxir/util/luxir_util.h"

namespace luxir {

// Resolved once per (segment, facet), rather than rediscovering the domain
// representation for every term.
struct DomainView {
  DocSet* domain;
  const FixedBitSet* bits;
  ArrDocSet* arr;
  int32_t maxDoc;
  int32_t card;
  int32_t compCard;
  std::optional<FixedBitSet> ownedBits;

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

  // Expand an array domain into a pool-backed bitset so per-term counting can
  // probe O(1) and AND whole word blocks. One expansion per (segment, facet)
  // amortized over every counted term; `arr` stays set for the paths where the
  // sorted array is the better representation.
  void materializeBits(MemPool& pool) {
    if (bits != nullptr || arr == nullptr) {
      return;
    }
    size_t numWords = FixedBitSet::sizeInWords(maxDoc);
    auto* words = (uint64_t*) pool.alloc(numWords * sizeof(uint64_t),
                                         alignof(uint64_t));
    memset(words, 0, numWords * sizeof(uint64_t));
    for (int32_t doc : arr->docs()) {
      words[(uint32_t) doc >> 6] |= 1ULL << (doc & 63);
    }
    ownedBits.emplace(words, maxDoc);
    bits = &*ownedBits;
  }
};

// Walk every document in a null (all docs), array, or bitset domain. The
// bitset arm mirrors the established column-domain loop: never ask
// nextSetBit() to start at maxDoc when the last document is set.
template <class F>
void forEachDomainDoc(DocSet* domain, int32_t maxDoc, F&& callback) {
  if (domain == nullptr) {
    for (int32_t doc = 0; doc < maxDoc; doc++) callback(doc);
    return;
  }
  if (domain->type == DocSet::Type::ARRAY) {
    for (int32_t doc : ((ArrDocSet*)domain)->docs()) callback(doc);
    return;
  }

  const FixedBitSet& bits = ((BitDocSet*)domain)->bits();
  int32_t doc = -1;
  while (doc + 1 < maxDoc) {
    doc = bits.nextSetBit(doc + 1);
    if (doc >= maxDoc) break;
    callback(doc);
  }
}

// Count one term's postings in the domain. Counting callers materialize an
// array domain's bitset once per (segment, facet), so the common route for
// every domain shape is one block-wise walk of the postings: word and
// contiguous blocks count by AND+popcount straight from the stream, packed
// blocks decode and probe O(1) bits.
//
// The exception is a very sparse domain against a much longer postings list,
// where driving the domain and advancing the postings through the skip
// structure leaps whole undecoded blocks. That needs the domain's doc spacing
// to exceed a block's doc span, i.e. docFreq > card * DOCS_BLOCK_SIZE - only
// reachable by array domains (a bitset domain's card is at least
// maxDoc / 32, putting the crossover past maxDoc).
inline int32_t countTermInDomain(const DomainView& view, DocsOnlyEnum& postings,
                                 int32_t docFreq) {
  // RootOp only uses a null domain when the segment has no deletions. Thus an
  // empty complement means the domain is all [0,maxDoc), and deletion-unaware
  // docFreq is already the exact answer without advancing postings.
  if (view.compCard == 0) {
    return docFreq;
  }
  assert(view.domain != nullptr);  // a null domain always has an empty complement
  if (view.card == 0) {
    return 0;
  }

  if (view.arr != nullptr
      && shouldDrivePostingsFromArray(docFreq, view.card)) {
    // Domain drive: advance postings to each array doc, galloping the array
    // cursor to wherever the postings actually land.
    int32_t hits = 0;
    std::span<int32_t> docs = view.arr->docs();
    const int32_t* p = docs.data();
    const int32_t* end = p + docs.size();
    while (p != end) {
      int32_t target = *p;
      if (postings.docId() < target
          && postings.advance(target) == DocsEnumMeta::END) {
        break;
      }
      int32_t landing = postings.docId();
      if (landing == target) {
        hits++;
        p++;
      } else {
        assert(landing > target);
        p = screaming::gallopLowerBound(p + 1, end, landing);
      }
    }
    return hits;
  }

  assert(view.bits != nullptr);  // counting callers materialize array domains
  return postings.countInBitSet(
      {view.bits->words, FixedBitSet::sizeInWords(view.bits->size())});
}

// Number of in-domain docs without the field. DocsReader's bitset is the
// segment-wide docs-with-field set, so this is shared by text facets and the
// term-driven string strategy. Drives from the smaller side: either the domain
// walks and the field set advances underneath it, or the field set walks and
// the resolved domain representation answers O(1) probes.
inline int32_t countMissingInDomain(const DomainView& view,
                                    const DocsReader& docsReader) {
  if (!docsReader.hasBitset()) {
    return 0;
  }
  if (view.domain == nullptr) {
    return view.card - docsReader.numDocs();
  }

  int32_t haveField = 0;
  if (view.card <= docsReader.numDocs()) {
    screaming::BitSet::Iterator it(docsReader.bitset());
    if (view.arr != nullptr) {
      for (int32_t doc : view.arr->docs()) {
        if (it.val() < doc && it.advance(doc) == screaming::BitSet::END) {
          break;
        }
        haveField += (int32_t) (it.val() == doc);
      }
    } else {
      int32_t doc = 0;
      while (doc < view.maxDoc) {
        doc = view.bits->nextSetBit(doc);
        if (doc >= view.maxDoc) {
          break;
        }
        if (it.val() < doc && it.advance(doc) == screaming::BitSet::END) {
          break;
        }
        haveField += (int32_t) (it.val() == doc);
        doc = std::max(doc + 1, it.val());
      }
    }
  } else {
    screaming::BitSet::Iterator it(docsReader.bitset());
    if (view.bits != nullptr) {
      for (int32_t doc = it.next(); doc != screaming::BitSet::END;
           doc = it.next()) {
        haveField += (int32_t) view.bits->get(doc);
      }
    } else {
      std::span<int32_t> docs = view.arr->docs();
      const int32_t* lo = docs.data();
      const int32_t* hi = lo + docs.size();
      for (int32_t doc = it.next(); doc != screaming::BitSet::END;
           doc = it.next()) {
        lo = screaming::gallopLowerBound(lo, hi, doc);
        if (lo == hi) {
          break;
        }
        haveField += (int32_t) (*lo == doc);
      }
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
// IMPORTANT: mark the callback LUXIR_INLINE at the call site (e.g.
// `[&](int32_t docid, int64_t val) LUXIR_INLINE { ... }`).  The per-value work
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

template <bool TrackStats, bool Complement, class F>
void forEachOrdValueImpl(DocSet* domain, OrdColReader& ordColReader,
                         int32_t maxDoc, int64_t& missing_num, F&& callback,
                         OrdColReader::ForEachOrdStats* stats) {
  if constexpr (!Complement) {
    if (domain != nullptr && domain->type == DocSet::Type::ARRAY) {
      auto* arrayDomain = dynamic_cast<ArrDocSet*>(domain);
      if (arrayDomain == nullptr) {
        throw std::logic_error("ARRAY DocSet has a non-array dynamic type");
      }
      ordColReader.forEachOrd(
          arrayDomain->docs(), missing_num, callback, stats);
      return;
    }
  }

  // Complement mode is only entered with a bitset domain: a null domain's
  // complement is empty and array domains expand dense complement words first
  // (forEachComplementOrdValue).
  const FixedBitSet* bits = domain ? &((BitDocSet*)domain)->bits() : nullptr;
  assert(!Complement || bits != nullptr);
  if (bits != nullptr && ordColReader.docIdIndexed()) {
    ordColReader.forEachDocIdBitOrd(
        *bits, maxDoc, OrdColReader::configuredBulkMinHits(),
        missing_num, callback, stats, Complement ? ~0ULL : 0);
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
    if constexpr (Complement) {
      docid = (int32_t)bits->nextClearBit(docid + 1);
    } else {
      docid = bits ? bits->nextSetBit(docid + 1) : docid + 1;
    }
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
  forEachOrdValueImpl<false, false>(
      domain, ordColReader, maxDoc, missing_num,
      std::forward<F>(callback), nullptr);
}

template <class F>
void forEachOrdValue(DocSet* domain, OrdColReader& ordColReader,
                     int32_t maxDoc, int64_t& missing_num, F&& callback,
                     OrdColReader::ForEachOrdStats* stats) {
  forEachOrdValueImpl<true, false>(
      domain, ordColReader, maxDoc, missing_num,
      std::forward<F>(callback), stats);
}

// Walk the complement of a resolved facet domain over the ord column. Bitset
// domains drive the shared adaptive machinery through inverted word reads, so
// no second bitset is materialized. Array domains (reachable only when a
// complement strategy is forced; the cost model never picks complement for a
// sparse domain) expand dense complement words into the pool first.
template <class F>
void forEachComplementOrdValue(DomainView& view, MemPool& pool,
                               OrdColReader& ordColReader,
                               int64_t& missing_num, F&& callback) {
  assert(view.domain != nullptr && view.compCard > 0);
  if (view.bits == nullptr) {
    size_t numWords = FixedBitSet::sizeInWords(view.maxDoc);
    auto* words = (uint64_t*)pool.alloc(numWords * sizeof(uint64_t),
                                        alignof(uint64_t));
    std::memset(words, 0xff, numWords * sizeof(uint64_t));
    for (int32_t doc : view.arr->docs()) {
      words[(uint32_t)doc >> 6] &= ~(1ULL << ((uint32_t)doc & 63));
    }
    int32_t trailing = view.maxDoc & 63;
    if (trailing != 0) {
      words[numWords - 1] &= (1ULL << trailing) - 1;
    }
    FixedBitSet complementBits(words, view.maxDoc);
    BitDocSet complement(complementBits, view.compCard);
    forEachOrdValue(&complement, ordColReader, view.maxDoc, missing_num,
                    std::forward<F>(callback));
    return;
  }
  // Word-wise inversion reads the domain's whole word array, so it must cover
  // the same doc space (liveDocs and every DocSetBuilder output are sized at
  // maxDoc).
  assert(view.bits->size() == view.maxDoc);
  forEachOrdValueImpl<false, true>(
      view.domain, ordColReader, view.maxDoc, missing_num,
      std::forward<F>(callback), nullptr);
}

}
