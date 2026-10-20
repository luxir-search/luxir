// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once
#include "luxir/search/PostingsIntersection.h"
#include "luxir/reader/DocsReader.h"
#include "luxir/reader/IntColReader.h"
#include "luxir/reader/OrdColReader.h"
#include "luxir/search/IndexReader.h"
#include "luxir/search/OrdMap.h"
#include "luxir/util/MemPool.h"
#include "luxir/util/luxir_util.h"
#include <optional>

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

// A sparse (array) domain decodes values by point, big-skipping the column
// with SparseIterator; bitset and all-docs domains use the block-decoding
// BulkIterator. Every walk over an int column follows this one rule.
inline bool sparseIntColWalk(const DocSet* domain) {
  return domain != nullptr && domain->type == DocSet::Type::ARRAY;
}

// Call visit.template operator()<Iterator>() with the walk's iterator type.
template <typename Visit>
void withIntColIteratorType(const DocSet* domain, Visit&& visit) {
  if (sparseIntColWalk(domain)) visit.template operator()<IntColReader::SparseIterator>();
  else visit.template operator()<IntColReader::Iterator>();
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
  if (sparseIntColWalk(domain)) {
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

  // Bitset domain or null domain (every doc): dense scan with the
  // block-decoding BulkIterator. One loop per domain and value shape, so each
  // loop carries only its own shape's per-document work. A bitset domain is
  // drained a word at a time; nextSetBit only finds the next nonempty word.
  const FixedBitSet* bits = nullptr;
  if (domain) {
    bits = &((BitDocSet*) domain)->bits();
  }
  auto scan = [&]<bool Bits, bool Multi>() LUXIR_INLINE {
    IntColReader::Iterator intColIter(intColReader);
    auto visit = [&](int32_t docid) LUXIR_INLINE {
      if (intColIter.docId() < docid) {
        intColIter.advance(docid);
      }
      if (intColIter.docId() != docid) {
        missing_num++;
      } else if constexpr (!Multi) {
        callback(docid, intColIter.value());
      } else {
        auto [start, end] = intColReader.getStartEndValueRank(intColIter.rank());
        auto n = end - start;
        for (int64_t vrank = 0; vrank < n; vrank++) {
          callback(docid, intColIter.values().valueAt(start + vrank));
        }
      }
    };
    if constexpr (Bits) {
      int32_t docid = bits->nextSetBit(0, maxDoc);
      while (docid < maxDoc) {
        int32_t base = docid & ~63;
        uint64_t word = bits->words[(uint32_t) docid >> 6] & (~0ULL << (docid & 63));
        if (maxDoc - base < 64) {
          word &= (1ULL << (maxDoc - base)) - 1;
        }
        do {
          visit(base + std::countr_zero(word));
          word &= word - 1;
        } while (word != 0);
        docid = bits->nextSetBit(base + 64, maxDoc);
      }
    } else {
      for (int32_t docid = 0; docid < maxDoc; docid++) {
        visit(docid);
      }
    }
  };
  bool multi = intColReader.multiValued();
  if (bits) {
    multi ? scan.template operator()<true, true>() : scan.template operator()<true, false>();
  } else {
    multi ? scan.template operator()<false, true>() : scan.template operator()<false, false>();
  }
}

// What a zone-pruned int column walk does with one zone (a block, or one of
// its leaves).
enum class IntZoneVerdict : uint8_t {
  SKIP,    // no value of the zone contributes
  WHOLE,   // every value contributes alike: one visitor.run(), no decode
  VALUES   // decode: a block retries leaf by leaf, a leaf calls visitor.value()
};

// The in-domain values of one WHOLE zone. [begin, end) are value ranks; a walk
// with a domain, or for a visitor that needsDocs(), is over a leafBounded()
// column, where they are also the docs.
struct IntZoneRun {
  enum class Kind : uint8_t { ALL, BITS, ARRAY };
  int64_t begin;
  int64_t end;
  int64_t count;  // in-domain values, > 0
  Kind kind;
  const uint64_t* words;          // BITS: the domain words from doc begin
  std::span<const int32_t> docs;  // ARRAY: the domain docs in [begin, end)
};

// Walk an int column zone by zone: visitor.zone(min, max) classifies each
// block zone, and each leaf zone of a block it answers VALUES for. A WHOLE
// zone reaches visitor.run(IntZoneRun) with its in-domain count and no decode;
// a VALUES leaf decodes and calls visitor.value(rank, value) per in-domain
// value. In-domain docs without a value bump missing_num.
//
// A domain needs value ranks to be docs, and so does a visitor that collects
// docs (V::needsDocs()), so either needs a leafBounded() column; otherwise this
// returns false having visited nothing and the caller walks docs
// (forEachIntColValue). Bitset domains decode a 128-value frame only when the
// frame has domain docs, array domains decode by point.
template <class V>
bool forEachIntColZone(DocSet* domain, const IntColReader& col, int32_t maxDoc,
                       int64_t& missing_num, V& visitor) {
  constexpr int64_t BLOCK = NumColumnFormat::BLOCK_SIZE;
  constexpr int64_t LEAF = NumColumnFormat::LEAF_ZONE_SIZE;
  constexpr int64_t FRAME = NumColumnFormat::BULK_SIZE;
  if ((domain != nullptr || V::needsDocs()) && !col.leafBounded()) return false;
  // A dense column has a value for every doc, so only a null domain (any
  // shape) can see docs without one.
  if (domain == nullptr) missing_num += maxDoc - col.docsWithValue();

  using Kind = IntZoneRun::Kind;
  Kind kind = domain == nullptr ? Kind::ALL
      : domain->type == DocSet::Type::ARRAY ? Kind::ARRAY : Kind::BITS;
  const uint64_t* words = kind == Kind::BITS
      ? ((BitDocSet*)domain)->bits().words : nullptr;
  const int32_t* arrNext = nullptr;
  const int32_t* arrEnd = nullptr;
  if (kind == Kind::ARRAY) {
    std::span<int32_t> docs = ((ArrDocSet*)domain)->docs();
    arrNext = docs.data();
    arrEnd = arrNext + docs.size();
  }
  // Domain word `word` of a zone ending at `end`, masked there.
  auto wordAt = [&](int64_t word, int64_t end) LUXIR_INLINE {
    uint64_t w = words[word];
    int64_t validBits = end - word * 64;
    if (validBits < 64) w &= (1ULL << validBits) - 1;
    return w;
  };
  int64_t frame[FRAME];
  IntColReader::SparseValues points(col);

  auto visitZone = [&](int64_t begin, int64_t end, IntZoneVerdict verdict) {
    const int32_t* arrBegin = arrNext;
    if (kind == Kind::ARRAY) {
      arrNext = screaming::gallopLowerBound(arrNext, arrEnd, (int32_t)end);
    }
    if (verdict == IntZoneVerdict::SKIP) return;
    if (verdict == IntZoneVerdict::WHOLE) {
      IntZoneRun run{begin, end, 0, kind, nullptr, {}};
      if (kind == Kind::ALL) {
        run.count = end - begin;
      } else if (kind == Kind::BITS) {
        run.words = words + begin / 64;
        for (int64_t w = begin / 64; w * 64 < end; w++) {
          run.count += std::popcount(wordAt(w, end));
        }
      } else {
        run.docs = {arrBegin, arrNext};
        run.count = (int64_t)run.docs.size();
      }
      if (run.count > 0) visitor.run(run);
      return;
    }
    if (kind == Kind::ARRAY) {
      for (const int32_t* p = arrBegin; p != arrNext; p++) {
        visitor.value((int64_t)*p, points.valueAt(*p));
      }
      return;
    }
    for (int64_t start = begin; start < end; start += FRAME) {
      uint64_t lowWord = 0;
      uint64_t highWord = 0;
      if (kind == Kind::BITS) {
        lowWord = wordAt(start / 64, end);
        if (start + 64 < end) highWord = wordAt(start / 64 + 1, end);
        if ((lowWord | highWord) == 0) continue;
      }
      uint32_t count;
      [[maybe_unused]] int64_t decoded =
          col.decodeValueSubBlock(start, frame, count);
      assert(decoded == start);
      if (kind == Kind::ALL) {
        for (uint32_t i = 0; i < count; i++) visitor.value(start + i, frame[i]);
        continue;
      }
      for (; lowWord != 0; lowWord &= lowWord - 1) {
        int32_t i = std::countr_zero(lowWord);
        visitor.value(start + i, frame[i]);
      }
      for (; highWord != 0; highWord &= highWord - 1) {
        int32_t i = 64 + std::countr_zero(highWord);
        visitor.value(start + i, frame[i]);
      }
    }
  };

  int64_t numValues = col.numValues();
  const auto& leaves = col.leafMeta();
  for (int64_t block = 0; block < col.numBlocks(); block++) {
    int64_t begin = block * BLOCK;
    int64_t end = std::min(begin + BLOCK, numValues);
    // An array domain with no doc in the block skips it unread.
    if (kind == Kind::ARRAY && (arrNext == arrEnd || *arrNext >= end)) continue;
    NumBlockZone zone = col.blockZone(block);
    IntZoneVerdict verdict = visitor.zone(zone.min, zone.max);
    if (verdict != IntZoneVerdict::VALUES) {
      visitZone(begin, end, verdict);
      continue;
    }
    for (int64_t leafBegin = begin; leafBegin < end; leafBegin += LEAF) {
      int64_t leafEnd = std::min(leafBegin + LEAF, end);
      NumBlockZone leaf = leaves.leafZone(leafBegin / LEAF);
      visitZone(leafBegin, leafEnd, visitor.zone(leaf.min, leaf.max));
    }
  }
  return true;
}

// A domain's walk over an ord column, decoded BATCH_SIZE values at a time.
// The walk over the domain's shape and the column's format and indexing lives
// here, out of line and independent of what consumes the values: every
// caller shares this one copy of it and keeps only its own loop over a
// decoded batch. Values arrive in doc order with a document's values
// consecutive (a batch may end inside a multi-valued document); in-domain
// documents without a value are counted, not produced. Each batch's ords are
// mapped once, into the 0-based ord space the walk was asked for: a segment's
// SegToGlobal for global ords, or SegToGlobal::identity() for the segment's
// own term ords.
//
// Docid-indexed columns decode an array domain by point, a bitset domain
// (or its complement) per 128-doc block by point or one bulk unpack as
// OrdColReader::bulkBlock prices the block's hits, and a null domain in
// bulk. Rank-indexed columns decode an array domain by point and every
// other domain through bulk frames: domain density is in doc-id space and
// cannot price a value-rank decode.
class OrdValueBatches {
public:
  static constexpr int32_t BATCH_SIZE = 256;
  struct Batch {
    int32_t size = 0;
    int32_t docs[BATCH_SIZE];
    // As the column stores them: 1-based segment ords.
    int32_t stored[BATCH_SIZE];
    // In the walk's ord space.
    int64_t ords[BATCH_SIZE];
  };

  // complement walks the docs not in `domain`, which must then be a bitset.
  OrdValueBatches(DocSet* domain, const OrdColReader& column,
                  const OrdMap::SegToGlobal& ordSpace, int32_t maxDoc,
                  bool complement, OrdColReader::ForEachOrdStats* stats);

  // Refills `batch` with the walk's next values; false once none are left.
  LUXIR_NOINLINE bool next(Batch& batch);

  // In-domain documents without a value, so far.
  int64_t missing() const { return missingCount; }

private:
  enum class Walk : uint8_t { DOCID_ARRAY, DOCID_BITS, DOCID_ALL, RANK };
  // Where a rank walk takes its domain docs from.
  enum class Source : uint8_t { ARRAY, BITS, COMPLEMENT, ALL };

  const OrdColReader& column;
  OrdMap::SegToGlobal ordSpace;
  OrdColReader::ForEachOrdStats* stats;
  const FixedBitSet* bits = nullptr;
  const int32_t* arrayNext = nullptr;
  const int32_t* arrayEnd = nullptr;
  int32_t maxDoc;
  // The next doc (or block start) a bits or all-docs walk visits.
  int32_t cursor = 0;
  int32_t bulkMinHits = 0;
  uint64_t invertMask = 0;
  Walk walk;
  Source source = Source::ALL;
  int64_t missingCount = 0;
  // A rank walk's position in the docs with the field, and what remains of
  // a multi-valued document a full batch cut short.
  std::optional<OrdColReader::Iterator> rankIter;
  int32_t pendingDoc = -1;
  int64_t pendingRank = 0;
  int64_t pendingEnd = 0;

  void fillDocIdArray(Batch& batch);
  void fillDocIdBits(Batch& batch);
  void fillDocIdAll(Batch& batch);
  template <Source S> void fillRank(Batch& batch);
  template <Source S> int32_t nextRankDoc();
};

// The consuming half of a walk: the caller's callback, once per decoded
// value, in a loop over each batch.
template <class F>
void consumeOrdValues(OrdValueBatches& values, int64_t& missing_num,
                      F& callback) {
  OrdValueBatches::Batch batch;
  while (values.next(batch)) {
    for (int32_t i = 0; i < batch.size; i++) {
      callback(batch.docs[i], batch.ords[i]);
    }
  }
  missing_num += values.missing();
}

// callback(int32_t docid, int64_t ord) is invoked once per value of each
// in-domain doc that has the field, in doc order; ord is the value's 0-based
// ord in `ordSpace` (OrdValueBatches). In-domain docs with no value bump
// missing_num. Mark the callback LUXIR_INLINE at the call site, as for
// forEachIntColValue: it is the body of the loop over each decoded batch.
template <class F>
void forEachOrdValue(DocSet* domain, OrdColReader& ordColReader,
                     const OrdMap::SegToGlobal& ordSpace, int32_t maxDoc,
                     int64_t& missing_num, F&& callback,
                     OrdColReader::ForEachOrdStats* stats = nullptr) {
  OrdValueBatches values(domain, ordColReader, ordSpace, maxDoc, false, stats);
  consumeOrdValues(values, missing_num, callback);
}

// Walk the complement of a resolved facet domain over the ord column. Bitset
// domains drive the shared adaptive machinery through inverted word reads, so
// no second bitset is materialized. Array domains (reachable only when a
// complement strategy is forced; the cost model never picks complement for a
// sparse domain) expand dense complement words into the pool first. ords are
// the segment's own 0-based term ords: complement counts stage against the
// segment's docFreqs.
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
    forEachOrdValue(&complement, ordColReader, OrdMap::SegToGlobal::identity(),
                    view.maxDoc, missing_num, std::forward<F>(callback));
    return;
  }
  // Word-wise inversion reads the domain's whole word array, so it must cover
  // the same doc space (liveDocs and every DocSetBuilder output are sized at
  // maxDoc).
  assert(view.bits->size() == view.maxDoc);
  OrdValueBatches values(view.domain, ordColReader,
                         OrdMap::SegToGlobal::identity(), view.maxDoc, true,
                         nullptr);
  consumeOrdValues(values, missing_num, callback);
}

}
