// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "luxir/search/Collector.h"
#include "luxir/search/IndexReader.h"
#include "luxir/reader/PostingsReader.h"
#include "luxir/reader/IntColReader.h"
#include "luxir/reader/OrdColReader.h"
#include "luxir/reader/StrColReader.h"
#include "luxir/reader/TermsEnum.h"
#include "luxir/search/OrdMap.h"
#include "luxir/util/MemPool.h"
#include "luxir/util/luxir_util.h"
#include <bit>
#include <algorithm>
#include <cstring>
#include <memory>
#include <vector>
#include <limits>
#include <cassert>
#include <optional>
#include <span>

namespace luxir {

class FieldComparator {
public:
  struct RankedLeaf {
    int64_t leaf;
    int64_t bound;
  };

  // One segment's primary sort keys as a planner sees them, in a
  // comparator's transformed key space (smaller is better; descending keys
  // are ~value): the leaves in bound order, and how the keys of the
  // column's values inside a value envelope are distributed (the column's
  // value sketch, NumColumnFormat::ValueSketch). A view over the mapped
  // column, so a planner can hold one per segment.
  class SegmentKeys {
    NumColumnFormat::LeafMeta leaves;
    bool descending = false;
    // The envelope, in values, and the sketch's count of values below it
    // and inside it.
    int64_t lo = std::numeric_limits<int64_t>::min();
    int64_t hi = std::numeric_limits<int64_t>::max();
    double below = 0;
    double inside = 0;

    const NumColumnFormat::ValueSketch& sketch() const {
      return leaves.valueSketch();
    }

    double atOrBelow(int64_t v) const {
      return v == std::numeric_limits<int64_t>::max()
          ? (double)sketch().values() : sketch().countBelow(v + 1);
    }

  public:
    SegmentKeys(const NumColumnFormat::LeafMeta& leaves, bool descending,
                std::optional<Query::ValueEnvelope> envelope)
        : leaves(leaves), descending(descending) {
      if (envelope.has_value()) {
        lo = envelope->lo;
        hi = envelope->hi;
      }
      if (sketch().empty() || lo > hi) return;
      below = sketch().countBelow(lo);
      inside = std::max(0.0, atOrBelow(hi) - below);
    }

    int64_t leafCount() const { return leaves.numLeafZones(); }

    int64_t leafBestKey(int64_t leaf) const {
      NumBlockZone zone = leaves.leafZone(leaf);
      return descending ? ~zone.max : zone.min;
    }

    bool leafIntersectsValues(int64_t leaf) const {
      NumBlockZone zone = leaves.leafZone(leaf);
      return lo <= hi && zone.max >= lo && zone.min <= hi;
    }

    // The leaf at `rank` of bound order (boundOrderLeaf), without its bound.
    int64_t leafAt(int64_t rank) const {
      return descending ? leaves.leafByMaxDescending(rank)
                        : leaves.leafByMinAscending(rank);
    }

    // The leaf at `rank` ascending by (leafBestKey, leaf), with that bound:
    // the column persists both directions' orders, and descending zone.max
    // with ties by leaf is ascending (~max, leaf).
    RankedLeaf boundOrderLeaf(int64_t rank) const {
      int64_t leaf = leafAt(rank);
      return {leaf, leafBestKey(leaf)};
    }

    // How much later in doc order the best keys sit than the worst: the
    // mean leaf of the kTrendLeaves best-bounded leaves less that of the
    // kTrendLeaves worst, over the most it could be. Near 1 for keys that
    // improve with doc order sorted newest first (time-ordered keys), near
    // -1 sorted oldest first, and 0 +- 0.08 for keys in random order. Both
    // ends of the persisted order are contiguous: a few cache lines.
    static constexpr int64_t kTrendLeaves = 32;
    double docOrderTrend() const {
      int64_t n = leafCount();
      int64_t m = std::min(kTrendLeaves, n / 2);
      if (m == 0) return 0;
      int64_t sum = 0;
      for (int64_t r = 0; r < m; r++) sum += leafAt(r) - leafAt(n - 1 - r);
      return (double)sum / (double)(m * (n - m));
    }

    // Leaves bounded strictly before `key`: a prefix of bound order,
    // galloped from its head (most prefixes a top-k asks about are short).
    int64_t leavesBoundedBefore(int64_t key) const {
      int64_t n = leafCount();
      int64_t lo = 0;
      int64_t probe = 0;
      while (probe < n && boundOrderLeaf(probe).bound < key) {
        lo = probe + 1;
        probe = 2 * probe + 1;
      }
      int64_t hi = std::min(probe, n);
      while (lo < hi) {
        int64_t mid = lo + (hi - lo) / 2;
        if (boundOrderLeaf(mid).bound < key) lo = mid + 1;
        else hi = mid;
      }
      return lo;
    }

    // Leaves bounded at or before `key`.
    int64_t leavesBoundedAtOrBefore(int64_t key) const {
      return key == std::numeric_limits<int64_t>::max()
          ? leafCount() : leavesBoundedBefore(key + 1);
    }

    // The envelope's best edge as a key: every value inside the envelope
    // keys at or past it, so no leaf bounded before it can be proven out.
    int64_t keyFloor() const { return descending ? ~hi : lo; }

    // Leaves holding no value inside the envelope, on its best side (their
    // every key is before keyFloor, so bound order visits them first) and on
    // its worst side: zones wholly below lo or wholly above hi. With no
    // envelope, none. No domain doc lies in either.
    struct Outside {
      int64_t best = 0;
      int64_t worst = 0;
    };
    Outside leavesOutside() const {
      Outside out;
      int64_t n = leafCount();
      if (lo > hi) return {n, 0};
      // Leaves whose max reaches lo are a prefix of the max-descending
      // order, and leaves whose min reaches no further than hi a prefix of
      // the min-ascending one.
      auto prefix = [&](auto&& keep) {
        int64_t a = 0;
        int64_t b = n;
        while (a < b) {
          int64_t mid = a + (b - a) / 2;
          if (keep(mid)) a = mid + 1;
          else b = mid;
        }
        return a;
      };
      int64_t below = lo == std::numeric_limits<int64_t>::min() ? 0
          : n - prefix([&](int64_t r) {
              return leaves.leafZone(leaves.leafByMaxDescending(r)).max >= lo;
            });
      int64_t above = hi == std::numeric_limits<int64_t>::max() ? 0
          : n - prefix([&](int64_t r) {
              return leaves.leafZone(leaves.leafByMinAscending(r)).min <= hi;
            });
      out.best = descending ? above : below;
      out.worst = descending ? below : above;
      return out;
    }

    // Expected number of the segment's values inside the envelope.
    double values() const { return inside; }

    // Expected fraction of the in-envelope values keyed strictly before
    // `key`.
    double fractionBefore(int64_t key) const {
      if (inside <= 0) return 0;
      double before;
      if (!descending) {
        if (key <= lo) return 0;
        before = key > hi ? inside : sketch().countBelow(key) - below;
      } else {
        // key < x for value v means v > ~x.
        int64_t value = ~key;
        if (value >= hi) return 0;
        before = value < lo ? inside : below + inside - atOrBelow(value);
      }
      return std::clamp(before / inside, 0.0, 1.0);
    }

    // The key of the in-envelope value ranked `fraction` of the way from
    // the best: the last of the best fraction * values() (the best one when
    // that is under one value). Requires values() > 0.
    int64_t keyAt(double fraction) const {
      assert(inside > 0);
      double offset = std::max(0.0, std::clamp(fraction, 0.0, 1.0) * inside - 1);
      if (!descending) {
        return std::clamp(sketch().valueAt(below + offset), lo, hi);
      }
      return ~std::clamp(sketch().valueAt(below + inside - 1 - offset), lo, hi);
    }
  };

  // Optional batch sort-key capability for comparators whose complete sort
  // order is an ascending transformed int64 key. Valid for the current segment
  // only; setSegment resets its cursors. Implementations must not depend on
  // setBottom notifications while this capability is in use.
  // NOTE: no virtual destructor; never owned or deleted through this type.
  class KeyBatch {
  public:
    // Live comparator slot storage, read and written directly by the collector.
    std::span<int64_t> slotKeys;

    // Gather transformed keys for ascending in-segment docs.
    virtual void gatherKeys(std::span<const int32_t> docs,
                            std::span<int64_t> keys) = 0;

    // Optional two-level pruning-bounds directory over the current segment.
    // A nonzero coarse size means doc d reads its key from block
    // d / keyBlockSize() and blockBestKey(b) lower-bounds the transformed
    // key of every doc in block b. Zero means no bounds (collection stays
    // exhaustive). A nonzero leaf size (only ever offered together with the
    // coarse level; leaves nest exactly in coarse blocks) refines the same
    // contract at leaf granularity: leafBestKey(l) lower-bounds every doc in
    // leaf l. Coarse bounds are the min over their leaves, so a coarse skip
    // proves every child skips. With leaves comes their bound order:
    // boundOrderLeaf(r) is the leaf at rank r ascending by (leafBestKey,
    // leaf), with that bound - for every r < leafBlockCount().
    using RankedLeaf = FieldComparator::RankedLeaf;
    virtual int32_t keyBlockSize() const { return 0; }
    virtual int64_t keyBlockCount() const { return 0; }
    virtual int64_t blockBestKey(int64_t block) const {
      unused(block);
      assert(false);
      return std::numeric_limits<int64_t>::min();
    }
    virtual int32_t leafBlockSize() const { return 0; }
    virtual int64_t leafBlockCount() const { return 0; }
    virtual int64_t leafBestKey(int64_t leaf) const {
      unused(leaf);
      assert(false);
      return std::numeric_limits<int64_t>::min();
    }
    virtual RankedLeaf boundOrderLeaf(int64_t rank) const {
      unused(rank);
      assert(false);
      return {0, std::numeric_limits<int64_t>::min()};
    }

    // The segment's keys (SegmentKeys) for docs whose values lie inside
    // `values` (Query::requiredValueEnvelope; none: every value), offered
    // with the leaf bounds. nullopt when there are none, or when a doc's key
    // need not come from a value inside the envelope (a multi-valued column
    // keys on its first stored value while a predicate matches any value).
    virtual std::optional<SegmentKeys> segmentKeys(
        std::optional<Query::ValueEnvelope> values) const {
      unused(values);
      return std::nullopt;
    }

    // Order-independent fused leaf gather: extract one leaf's in-domain docs
    // and transformed keys straight from the domain bitset, without a
    // doc-array round trip. `words` is the whole-segment bitset (word w
    // covers docs [64w, 64w+64)); an EMPTY span means no mask - every doc in
    // the leaf (match-all with no deletes). Leaves may be requested in ANY
    // order - only comparators with no forward-only cursor (dense
    // single-valued) offer this. outDocs/outKeys must hold leafBlockSize()
    // entries. Returns the number gathered.
    virtual bool supportsMaskedLeafGather() const { return false; }
    virtual int32_t gatherLeafMasked(int64_t leaf,
                                     std::span<const uint64_t> words,
                                     std::span<int32_t> outDocs,
                                     std::span<int64_t> outKeys) {
      unused(leaf, words, outDocs, outKeys);
      assert(false);
      return 0;
    }
  };

protected:
  IndexReader::FieldSegments fields;

public:
  explicit FieldComparator(IndexReader::FieldSegments fields) : fields(fields) {}

  virtual ~FieldComparator() = default;

  // Set the current segment for this comparator
  virtual void setSegment(const IndexReader::Segment& segment) = 0;

  virtual void setBottom(int32_t slot) { unused(slot); }

  // compare new document to bottom slot
  virtual int compareBottom(int32_t bottomSlot, segdoc bottomDoc, segdoc newDoc) = 0;

  // Compare any retained slot against the current document. Bottom-only
  // implementations can cache setBottom(), so this is a distinct capability.
  virtual int compareCurrent(int32_t slot, segdoc oldDoc, segdoc newDoc) {
    return compareBottom(slot, oldDoc, newDoc);
  }

  // compare two slots
  virtual int compare(int32_t slotA, segdoc docA, int32_t slotB, segdoc docB) = 0;

  // compare a slot in this comparator with a slot in another comparator
  virtual int compare(int32_t slotA, segdoc docA, FieldComparator& other, int32_t slotB, segdoc docB) = 0;

  // Copy the value from a document to a slot.
  virtual void copy(int32_t slot, segdoc doc) = 0;

  // Copy the value from a different comparator to this comparator
  virtual void copy(int32_t slot, FieldComparator& other, int32_t otherSlot, segdoc otherDoc) = 0;

  // Grow per-slot storage to hold at least `capacity` slots.  The collector
  // mints slots densely and calls this before the first write to a new slot,
  // so storage need not be allocated for the worst-case bound up front.
  // Implementations that publish KeyBatch::slotKeys must re-point it here.
  virtual void growSlots(int32_t capacity) = 0;

  virtual KeyBatch* keyBatch() { return nullptr; }

  // Optional competitive segment-ord interval for string-sort pruning,
  // derived from the heap bottom (so unlike KeyBatch it REQUIRES the
  // setBottom lifecycle). READY yields the inclusive 1-based segment-ord
  // interval [lo, hi] of terms whose docs could still enter the heap
  // (lo > hi: none can - the segment is done). PENDING means no usable
  // bound yet (e.g. the bottom is a missing value); UNSUPPORTED means this
  // comparator or segment can never provide intervals.
  //
  // `from` is the collection cursor: every doc still to be collected has
  // segdoc(segment, doc) >= segdoc(segment, from). The boundary (equal to
  // bottom) term is excluded only for a sole-clause sort with an exact
  // bottom whose segdoc tie-break every remaining doc loses.
  //
  // The interval is anchored at the sort's best end: lo == 1 ascending,
  // hi == nTerms descending. Within a segment only the other end moves, and
  // only toward the anchor: the bottom only improves, and the boundary
  // exclusion, once earned, holds for every later bottom (one with the same
  // term has an earlier segdoc).
  enum class OrdIntervalStatus { UNSUPPORTED, PENDING, READY };
  struct CompetitiveOrdState {
    int64_t lo = 0;
    int64_t hi = -1;
    int64_t nTerms = 0;
    // The anchored end: hi when descending, lo otherwise.
    bool descending = false;
    PostingsReader* postingsReader = nullptr;
    const SegFieldInfo* fieldInfo = nullptr;
  };
  virtual OrdIntervalStatus competitiveOrdInterval(
      CompetitiveOrdState& out, int32_t bottomSlot, segdoc bottomDoc,
      int32_t segment, int32_t from, bool soleSort) {
    unused(out, bottomSlot, bottomDoc, segment, from, soleSort);
    return OrdIntervalStatus::UNSUPPORTED;
  }

  enum MissingValue {
    MISSING_FIRST,
    MISSING_LAST
  };
};



// A comparator that can be used in a simplified collector that has values
// that can be compared across different segments.
class SimpleNumericFieldComparator : public FieldComparator,
                                     public FieldComparator::KeyBatch {
  // IntColBM break-even: decodeFrame wins at eight gathered values per frame.
  static constexpr size_t FRAME_DECODE_MIN = 8;

  std::optional<IntColReader> reader;
  std::optional<IntColReader::Iterator> iter;
  std::optional<IntColReader::SparseValues> pointValues;
  std::optional<screaming::BitSet::Iterator> docsIter;
  std::vector<int64_t> values; // Storage for bottom values
  int64_t sortMultiplier; // 1 for ascending, -1 for descending
  int64_t missingValueSubstitute;
  int64_t lastValue = 0;  // TODO: cache the last value lookup in compareBottom so we don't have to re-fetch if competitive
  segdoc lastDoc = {-1, -1};
  int64_t decoded[NumColumn::BULK_SIZE];
  int64_t decodedStart = -1;
  int64_t decodedEnd = -1;
  int32_t landedDoc = -1;
  int32_t landedRank = -1;
  bool multiValued = false;
  // Bound order and leaf bounds (no envelope), when keyBlockSize() offers
  // leaves.
  std::optional<SegmentKeys> leaves;

  const SegmentKeys& leafKeys() const {
    assert(leaves.has_value());
    return *leaves;
  }

  int64_t transformPresent(int64_t value) const {
    return sortMultiplier < 0 ? ~value : value;
  }

  void gatherDenseSingle(std::span<const int32_t> docs,
                         std::span<int64_t> keys) {
    size_t i = 0;
    while (i < docs.size()) {
      int64_t frameStart =
          (int64_t)docs[i] / NumColumn::BULK_SIZE * NumColumn::BULK_SIZE;
      int64_t frameEnd = frameStart + NumColumn::BULK_SIZE;
      size_t groupEnd = i + 1;
      while (groupEnd < docs.size() && docs[groupEnd] < frameEnd) {
        groupEnd++;
      }

      if (decodedStart == frameStart) {
        for (; i < groupEnd; i++) {
          keys[i] = transformPresent(decoded[docs[i] - decodedStart]);
        }
      } else if (groupEnd - i >= FRAME_DECODE_MIN) {
        uint32_t count;
        decodedStart = reader->decodeValueSubBlock(docs[i], decoded, count);
        decodedEnd = decodedStart + count;
        for (; i < groupEnd; i++) {
          assert(docs[i] >= decodedStart && docs[i] < decodedEnd);
          keys[i] = transformPresent(decoded[docs[i] - decodedStart]);
        }
      } else {
        for (; i < groupEnd; i++) {
          keys[i] = transformPresent(pointValues->valueAt(docs[i]));
        }
      }
    }
  }

  bool land(int32_t doc) {
    if (doc > landedDoc) {
      landedDoc = docsIter->advance(doc);
      landedRank = docsIter->rank();
    }
    return doc == landedDoc;
  }

  void gatherSparseSingle(std::span<const int32_t> docs,
                          std::span<int64_t> keys) {
    for (size_t i = 0; i < docs.size(); i++) {
      if (!land(docs[i])) {
        keys[i] = missingValueSubstitute;
      } else {
        keys[i] = transformPresent(pointValues->valueAt(landedRank));
      }
    }
  }

  void gatherMultiValued(std::span<const int32_t> docs,
                         std::span<int64_t> keys) {
    bool dense = !docsIter.has_value();
    for (size_t i = 0; i < docs.size(); i++) {
      int32_t rank;
      if (dense) {
        rank = docs[i];
      } else if (land(docs[i])) {
        rank = landedRank;
      } else {
        keys[i] = missingValueSubstitute;
        continue;
      }
      keys[i] = transformPresent(reader->selectedValue(rank, sortMultiplier < 0, *pointValues));
    }
  }

public:
  SimpleNumericFieldComparator(bool reversed, MissingValue missingValue, IndexReader::FieldSegments fields)
    : FieldComparator(fields),
      sortMultiplier(reversed ? -1 : 1) {

    // Missing placement is a fixed edge regardless of direction (the string
    // comparators and expression sort keys follow the same convention), so
    // the substitute is chosen in the TRANSFORMED key space: getDocValue
    // ~-maps present values for descending sorts, and an untransformed
    // extreme lands at the same output edge either way.
    missingValueSubstitute = (missingValue == MISSING_FIRST)
                               ? std::numeric_limits<int64_t>::min()
                               : std::numeric_limits<int64_t>::max();
  }
  
  void setSegment(const IndexReader::Segment& segment) override {
    auto* postingsReader = &segment.postingsReader();
    // Reset reader and iterator
    iter.reset();
    pointValues.reset();
    docsIter.reset();
    reader.reset();
    leaves.reset();
    multiValued = false;
    decodedStart = -1;
    decodedEnd = -1;
    landedDoc = -1;
    landedRank = -1;

    if (auto* info = fields.at(segment)) {
      const auto& fieldInfo = *info;
      if (fieldInfo.columnLoc.offset() > 0) {
        reader.emplace(*postingsReader, fieldInfo);
        iter.emplace(*reader);
        pointValues.emplace(*reader);
        multiValued = reader->multiValued();
        if (!reader->denseDocsWithValue()) {
          docsIter.emplace(reader->docsWithValueBitSet());
        }
        if (keyBlockSize() != 0) {
          leaves.emplace(reader->leafMeta(), sortMultiplier < 0, std::nullopt);
        }
      }
    }
  }

  FieldComparator::KeyBatch* keyBatch() override {
    return this;
  }

  void growSlots(int32_t capacity) override {
    if ((size_t)capacity <= values.size()) return;
    resizeExact(values, (size_t)capacity);
    slotKeys = values;
  }

  // Dense single-valued columns map doc == value rank, so the per-4096-value
  // zone maps are exact per-doc-block key bounds. Sparse and multi-valued
  // columns mix ranks and docs, so no bounds are offered there.
  int32_t keyBlockSize() const override {
    if (!reader.has_value() || !reader->leafBounded()) return 0;
    return (int32_t)NumColumnFormat::BLOCK_SIZE;
  }

  int64_t keyBlockCount() const override {
    return reader->numBlocks();
  }

  int64_t blockBestKey(int64_t block) const override {
    NumBlockZone zone = reader->blockZone(block);
    return sortMultiplier < 0 ? ~zone.max : zone.min;
  }

  void gatherKeys(std::span<const int32_t> docs,
                  std::span<int64_t> keys) override {
    assert(keys.size() == docs.size());
    if (!reader.has_value()) {
      std::fill(keys.begin(), keys.end(), missingValueSubstitute);
    } else if (multiValued) {
      gatherMultiValued(docs, keys);
    } else if (docsIter.has_value()) {
      gatherSparseSingle(docs, keys);
    } else {
      gatherDenseSingle(docs, keys);
    }
  }

  int32_t leafBlockSize() const override {
    return keyBlockSize() != 0 ? (int32_t)NumColumnFormat::LEAF_ZONE_SIZE : 0;
  }

  int64_t leafBlockCount() const override {
    return reader->numLeafZones();
  }

  int64_t leafBestKey(int64_t leaf) const override {
    return leafKeys().leafBestKey(leaf);
  }

  FieldComparator::RankedLeaf boundOrderLeaf(int64_t rank) const override {
    return leafKeys().boundOrderLeaf(rank);
  }

  // Envelope bounds are inclusive encoded values, the same encoding the
  // column stores (FLOAT/DOUBLE sortable bits included). A missing value
  // never satisfies an envelope, so the missing substitute plays no part.
  std::optional<SegmentKeys> segmentKeys(
      std::optional<Query::ValueEnvelope> values) const override {
    if (keyBlockSize() == 0) return std::nullopt;
    return SegmentKeys(reader->leafMeta(), sortMultiplier < 0, values);
  }

  // Dense single-valued only: doc == value rank and no forward-only landing
  // cursor, so leaves can be gathered in any order.
  bool supportsMaskedLeafGather() const override {
    return keyBlockSize() != 0;
  }

  int32_t gatherLeafMasked(int64_t leaf, std::span<const uint64_t> words,
                           std::span<int32_t> outDocs,
                           std::span<int64_t> outKeys) override {
    assert(keyBlockSize() != 0);
    int64_t leafStart = leaf * (int64_t)NumColumnFormat::LEAF_ZONE_SIZE;
    if (words.empty()) {
      // No mask: every doc in the leaf (dense single-valued, doc == rank).
      int64_t leafLimit = std::min<int64_t>(
          leafStart + NumColumnFormat::LEAF_ZONE_SIZE, reader->numValues());
      int32_t out = 0;
      for (int64_t frameStart = leafStart; frameStart < leafLimit;
           frameStart += NumColumn::BULK_SIZE) {
        if (decodedStart != frameStart) {
          uint32_t count;
          decodedStart =
              reader->decodeValueSubBlock((int32_t)frameStart, decoded, count);
          decodedEnd = decodedStart + count;
        }
        int64_t frameEnd = std::min(frameStart + NumColumn::BULK_SIZE,
                                    leafLimit);
        for (int64_t doc = frameStart; doc < frameEnd; doc++) {
          outDocs[(size_t)out] = (int32_t)doc;
          outKeys[(size_t)out] = transformPresent(decoded[doc - decodedStart]);
          out++;
        }
      }
      return out;
    }
    int64_t leafLimit = std::min<int64_t>(
        leafStart + NumColumnFormat::LEAF_ZONE_SIZE,
        (int64_t)words.size() * 64);
    int32_t out = 0;
    for (int64_t frameStart = leafStart; frameStart < leafLimit;
         frameStart += NumColumn::BULK_SIZE) {
      size_t w0 = (size_t)(frameStart >> 6);
      size_t wEnd = std::min(w0 + NumColumn::BULK_SIZE / 64, words.size());
      int32_t frameCard = 0;
      for (size_t w = w0; w < wEnd; w++) {
        frameCard += (int32_t)std::popcount(words[w]);
      }
      if (frameCard == 0) continue;
      bool useDecoded = frameCard >= (int32_t)FRAME_DECODE_MIN;
      if (useDecoded && decodedStart != frameStart) {
        uint32_t count;
        decodedStart =
            reader->decodeValueSubBlock((int32_t)frameStart, decoded, count);
        decodedEnd = decodedStart + count;
      }
      for (size_t w = w0; w < wEnd; w++) {
        uint64_t bits = words[w];
        int32_t base = (int32_t)(w << 6);
        while (bits != 0) {
          int32_t doc = base + std::countr_zero(bits);
          bits &= bits - 1;
          outDocs[(size_t)out] = doc;
          outKeys[(size_t)out] = transformPresent(
              useDecoded ? decoded[doc - decodedStart]
                         : pointValues->valueAt(doc));
          out++;
        }
      }
    }
    return out;
  }

  LUXIR_INLINE int64_t getDocValue(int32_t docid) {
    if (!iter.has_value()) {
      return missingValueSubstitute;
    }

    // Dense presence maps doc ids directly to value ordinals. Avoid moving
    // a presence cursor for each lookup; the value decoder caches by ordinal.
    int32_t rank = docid;
    if (docsIter.has_value()) {
      if (iter->docId() < docid) iter->advance(docid);
      if (iter->docId() > docid) return missingValueSubstitute;
      rank = iter->rank();
    }
    // Min ascending, max descending: sets read an edge, arrays scan.
    int64_t value = multiValued
        ? reader->selectedValue(rank, sortMultiplier < 0, iter->values())
        : iter->values().valueAt(rank);
    // For descending sort, negate the value.
    // if we wanted to make this branchless, we could have a an adder and multiplier
    // but this will be a predictable branch anyway.
    if (sortMultiplier < 0) {
      // Descending: ~value == -(value+1) but with no signed-overflow UB.
      // -(value+1) overflows when value==INT64_MAX (and negating INT64_MIN is
      // UB too); ~value is the same order-reversing map over the full range.
      value = ~value;
    }
    return value;
  }
  
  int compare(int32_t slotA, segdoc docA, int32_t slotB, segdoc docB) override {
    // SimpleNumericFieldComparator can ignore segments when comparing values
    // since numeric values are comparable across segments
    assert(slotA >= 0 && slotA < (int32_t)values.size());
    assert(slotB >= 0 && slotB < (int32_t)values.size());
    int64_t valA = values[slotA];
    int64_t valB = values[slotB];
    return (valA > valB) - (valA < valB);
  }
  
  // NOINLINE: keeps the per-doc rejection path a single out-of-line unit with the
  // column iterator inlined into it, rather than speculative-devirt inlining a
  // half of it into the collect loop and spilling value()/advance() to calls.
  LUXIR_NOINLINE int compareBottom(int32_t bottomSlot, segdoc bottomDoc, segdoc newDoc) override {
    // Get the bottom value from the slot
    assert(bottomSlot >= 0 && bottomSlot < (int32_t)values.size());
    int64_t bottomVal = values[bottomSlot];

    // Get the new document's value
    int64_t newVal = getDocValue(newDoc.docId());

    return (bottomVal > newVal) - (bottomVal < newVal);
  }
  
  void copy(int32_t slot, segdoc doc) override {
    assert(slot >= 0 && slot < (int32_t)values.size());
    int64_t value = getDocValue(doc.docId());
    values[slot] = value;
  }
  
  int compare(int32_t slotA, segdoc docA, FieldComparator& other, int32_t slotB, segdoc docB) override {
    // For numeric comparators, we can compare the stored values directly
    assert(dynamic_cast<SimpleNumericFieldComparator*>(&other) != nullptr);
    auto* otherNumeric = static_cast<SimpleNumericFieldComparator*>(&other);
    
    assert(slotA >= 0 && slotA < (int32_t)values.size());
    assert(slotB >= 0 && slotB < (int32_t)otherNumeric->values.size());
    int64_t valA = values[slotA];
    int64_t valB = otherNumeric->values[slotB];
    return (valA > valB) - (valA < valB);
  }
  
  void copy(int32_t slot, FieldComparator& other, int32_t otherSlot, segdoc otherDoc) override {
    // Copy value from another comparator
    assert(dynamic_cast<SimpleNumericFieldComparator*>(&other) != nullptr);
    auto* otherNumeric = static_cast<SimpleNumericFieldComparator*>(&other);
    
    assert(slot >= 0 && slot < (int32_t)values.size());
    assert(otherSlot >= 0 && otherSlot < (int32_t)otherNumeric->values.size());
    values[slot] = otherNumeric->values[otherSlot];
  }
  
  ~SimpleNumericFieldComparator() override = default;
};

// A comparator for string fields that uses global ordinals for efficient cross-segment comparison
// Selected segment ord for sorting: single-valued docs read their one ord;
// multi-valued docs sort by their smallest ord ascending and largest ord
// descending (min/max term semantics). Returns 0 for missing. Forced inline:
// the comparators' hot paths depend on it folding into their callers.
LUXIR_INLINE inline int32_t selectedSortOrd(const OrdColReader& reader, int32_t docid,
                                            bool descending) {
  if (!reader.multiValued()) return reader.ordAt(docid);
  if (reader.multiArray()) return reader.selectedArrayOrdAt(docid, descending);
  if (!descending) return reader.firstOrdAt(docid);
  const DocsReader& docs = reader.docsReader();
  int32_t docRank;
  if (!docs.hasBitset()) {
    docRank = docid;
  } else {
    screaming::BitSet::Iterator iter(docs.bitset());
    if (iter.advance(docid) != docid) return 0;
    docRank = iter.rank();
  }
  auto [start, end] = reader.getStartEndValueRank(docRank);
  assert(start < end);
  OrdColReader::PointOrds ords(reader);
  return ords.valueAt(end - 1);
}

class GlobalOrdComparator : public FieldComparator {
  std::shared_ptr<OrdMap> ordMap;
  std::optional<OrdColReader> reader;
  std::vector<int64_t> globalOrds; // Storage for global ordinals
  OrdMap::SegToGlobal segmentMapping;
  PostingsReader* postingsReader = nullptr;
  const SegFieldInfo* fieldInfo = nullptr;
  int64_t sortMultiplier; // 1 for ascending, -1 for descending
  int64_t missingOrd;

public:
  GlobalOrdComparator(std::shared_ptr<OrdMap> ordMap,
                      bool reversed, MissingValue missingValue, IndexReader::FieldSegments fields)
    : FieldComparator(fields),
      ordMap(ordMap),
      sortMultiplier(reversed ? -1 : 1) {
    
    // For string fields, ordinal 0 means missing value
    // For MISSING_FIRST: missing values should sort before all other values
    // For MISSING_LAST: missing values should sort after all other values
    // Real ords get the direction multiplier; missing is already placed at
    // the requested edge and must not be reversed a second time.
    if (missingValue == MISSING_FIRST) {
      missingOrd = std::numeric_limits<int64_t>::min();
    } else {
      missingOrd = std::numeric_limits<int64_t>::max();
    }
  }
  
  void setSegment(const IndexReader::Segment& segment) override {
    reader.reset();
    segmentMapping = {};
    postingsReader = &segment.postingsReader();
    fieldInfo = nullptr;

    // Get segment-to-global ordinal mapping from OrdMap
    if (ordMap) {
      segmentMapping = ordMap->getSegToGlobal(segment.ord);
    }

    // Load the ordinal column reader for this segment
    fieldInfo = fields.at(segment);
    if (fieldInfo) {
      if (!ordMap) {
        segmentMapping.numOrds = fieldInfo->nTerms;
      }
      if (fieldInfo->columnLoc.offset() > 0) {
        reader.emplace(*postingsReader, *fieldInfo);
      }
    }
  }

  void growSlots(int32_t capacity) override {
    if ((size_t)capacity > globalOrds.size()) {
      resizeExact(globalOrds, (size_t)capacity);
    }
  }

  // Identity mapping only (ordMap == nullptr, the single-segment selection):
  // slot values are then raw segment ords times the direction, so the bottom
  // slot converts straight back to a segment-ord bound. The multi-segment
  // global mode has no global-to-segment inverse and stays unsupported.
  OrdIntervalStatus competitiveOrdInterval(
      CompetitiveOrdState& out, int32_t bottomSlot, segdoc bottomDoc,
      int32_t segment, int32_t from, bool soleSort) override {
    if (!reader.has_value() || ordMap != nullptr || postingsReader == nullptr
        || fieldInfo->nTerms == 0) {
      return OrdIntervalStatus::UNSUPPORTED;
    }
    // Missing-first: docs without the field beat any real-valued bottom but
    // have no postings, so a term interval can never represent them.
    if (missingOrd == std::numeric_limits<int64_t>::min()) {
      return OrdIntervalStatus::PENDING;
    }
    assert(bottomSlot >= 0 && bottomSlot < (int32_t)globalOrds.size());
    int64_t key = globalOrds[(size_t)bottomSlot];
    if (key == missingOrd) return OrdIntervalStatus::PENDING;
    int64_t ord = sortMultiplier < 0 ? -key : key;
    assert(ord >= 1 && ord <= segmentMapping.numOrds);
    // The bottom ord is always exact here (same-segment slot values).
    bool closable = soleSort && segdoc(segment, from) >= bottomDoc;
    if (sortMultiplier > 0) {
      out.lo = 1;
      out.hi = closable ? ord - 1 : ord;
    } else {
      out.lo = closable ? ord + 1 : ord;
      out.hi = segmentMapping.numOrds;
    }
    out.nTerms = segmentMapping.numOrds;
    out.descending = sortMultiplier < 0;
    out.postingsReader = postingsReader;
    out.fieldInfo = fieldInfo;
    return OrdIntervalStatus::READY;
  }

  LUXIR_INLINE int64_t getGlobalOrd(int32_t docid) {
    if (!reader.has_value()) {
      return missingOrd;
    }

    int64_t segmentOrd = selectedSortOrd(*reader, docid, sortMultiplier < 0);
    if (segmentOrd == 0) {
      // Ordinal 0 means missing value
      return missingOrd;
    }
    
    // Convert segment ordinal to global ordinal
    // Segment ords are 1-based while the delta array is 0-based.
    int64_t globalOrd = segmentOrd + segmentMapping.deltaAt(segmentOrd - 1);
    
    // Apply sort multiplier for ascending/descending sort
    return sortMultiplier * globalOrd;
  }
  
  int compare(int32_t slotA, segdoc docA, int32_t slotB, segdoc docB) override {
    assert(slotA >= 0 && slotA < (int32_t)globalOrds.size());
    assert(slotB >= 0 && slotB < (int32_t)globalOrds.size());
    int64_t ordA = globalOrds[slotA];
    int64_t ordB = globalOrds[slotB];
    return (ordA > ordB) - (ordA < ordB);
  }
  
  // NOINLINE: see SimpleNumericFieldComparator::compareBottom.
  LUXIR_NOINLINE int compareBottom(int32_t bottomSlot, segdoc bottomDoc, segdoc newDoc) override {
    assert(bottomSlot >= 0 && bottomSlot < (int32_t)globalOrds.size());
    int64_t bottomOrd = globalOrds[bottomSlot];
    int64_t newOrd = getGlobalOrd(newDoc.docId());
    return (bottomOrd > newOrd) - (bottomOrd < newOrd);
  }
  
  void copy(int32_t slot, segdoc doc) override {
    assert(slot >= 0 && slot < (int32_t)globalOrds.size());
    globalOrds[slot] = getGlobalOrd(doc.docId());
  }
  
  int compare(int32_t slotA, segdoc docA, FieldComparator& other, int32_t slotB, segdoc docB) override {
    assert(dynamic_cast<GlobalOrdComparator*>(&other) != nullptr);
    auto* otherOrd = static_cast<GlobalOrdComparator*>(&other);
    
    assert(slotA >= 0 && slotA < (int32_t)globalOrds.size());
    assert(slotB >= 0 && slotB < (int32_t)otherOrd->globalOrds.size());
    int64_t ordA = globalOrds[slotA];
    int64_t ordB = otherOrd->globalOrds[slotB];
    return (ordA > ordB) - (ordA < ordB);
  }
  
  void copy(int32_t slot, FieldComparator& other, int32_t otherSlot, segdoc otherDoc) override {
    assert(dynamic_cast<GlobalOrdComparator*>(&other) != nullptr);
    auto* otherOrd = static_cast<GlobalOrdComparator*>(&other);
    
    assert(slot >= 0 && slot < (int32_t)globalOrds.size());
    assert(otherSlot >= 0 && otherSlot < (int32_t)otherOrd->globalOrds.size());
    globalOrds[slot] = otherOrd->globalOrds[otherSlot];
  }
  
  ~GlobalOrdComparator() override = default;
};

class SegmentOrdComparator : public FieldComparator {
  struct Slot {
    int32_t ord = 0;
    int32_t ordGen = -1;
    char* term = nullptr;
    char* buffer = nullptr;
    uint32_t length = 0;
    uint32_t capacity = 0;
  };

  MemPool termPool;
  MemPool enumPool;
  MemPool::save_point enumPoolStart;
  const SegFieldInfo* fieldInfo = nullptr;
  std::optional<OrdColReader> reader;
  std::optional<TermsEnum> termsEnum;
  PostingsReader* postingsReader = nullptr;
  std::vector<Slot> slots;
  int32_t currentSegment = -1;
  int32_t bottomOrd = 0;
  int32_t sortMultiplier;
  int32_t missingSortCmp;
  bool missingFirst;
  bool bottomMissing = true;
  bool bottomExact = false;

  static int compareTerms(std::string_view a, std::string_view b) {
    size_t len = std::min(a.size(), b.size());
    int cmp = memcmp(a.data(), b.data(), len);
    if (cmp != 0) return cmp;
    return (a.size() > b.size()) - (a.size() < b.size());
  }

  std::string_view term(const Slot& slot) const {
    assert(slot.ord != 0);
    assert(slot.term != nullptr);
    return {slot.term, slot.length};
  }

  int compareMissing(bool aMissing, bool bMissing) const {
    if (aMissing == bMissing) return 0;
    return aMissing ? missingSortCmp : -missingSortCmp;
  }

  int compareSlots(const Slot& a, const Slot& b) const {
    bool aMissing = a.ord == 0;
    bool bMissing = b.ord == 0;
    if (aMissing || bMissing) return compareMissing(aMissing, bMissing);
    if (a.ordGen == b.ordGen) {
      return (a.ord > b.ord) - (a.ord < b.ord);
    }
    return compareTerms(term(a), term(b));
  }

  void setMissing(Slot& slot) {
    slot.ord = 0;
    slot.ordGen = -1;
    slot.term = nullptr;
    slot.length = 0;
  }

  void copyTerm(Slot& slot, std::string_view value) {
    if (slot.capacity < value.size() || slot.buffer == nullptr) {
      size_t capacity = std::bit_ceil(std::max<size_t>(8, value.size()));
      slot.buffer = termPool.alloc(capacity);
      slot.capacity = (uint32_t)capacity;
    }
    if (!value.empty()) memmove(slot.buffer, value.data(), value.size());
    slot.term = slot.buffer;
    slot.length = (uint32_t)value.size();
  }

  LUXIR_INLINE int32_t selectedOrd(int32_t docid) const {
    if (!reader.has_value()) return 0;
    return selectedSortOrd(*reader, docid, sortMultiplier < 0);
  }

public:
  SegmentOrdComparator(bool reversed, MissingValue missingValue, IndexReader::FieldSegments fields)
    : FieldComparator(fields),
      enumPoolStart(enumPool.getSavePoint()),
      sortMultiplier(reversed ? -1 : 1),
      // GlobalOrdComparator keeps its missing sentinel at the requested edge.
      missingSortCmp((missingValue == MISSING_FIRST ? -1 : 1) * sortMultiplier),
      missingFirst(missingValue == MISSING_FIRST) {}

  void setSegment(const IndexReader::Segment& segment) override {
    termsEnum.reset();
    reader.reset();
    enumPool.rewind(enumPoolStart);
    fieldInfo = nullptr;
    currentSegment = segment.ord;
    postingsReader = &segment.postingsReader();


    fieldInfo = fields.at(segment);
    if (!fieldInfo) return;
    if (fieldInfo->nTerms == 0 || fieldInfo->columnLoc.offset() == 0 ||
        fieldInfo->ordFormat == SegFieldInfo::ORD_NONE) {
      return;
    }
    reader.emplace(*postingsReader, *fieldInfo);
    termsEnum.emplace(enumPool, *postingsReader, *fieldInfo);
  }

  void growSlots(int32_t capacity) override {
    if ((size_t)capacity > slots.size()) resizeExact(slots, (size_t)capacity);
  }

  // Bottom state semantics (see setBottom): exact => bottomOrd is the
  // bottom's own 1-based ord, so equal-ord docs tie with the bottom;
  // inexact => the bottom term is absent here and sorts strictly between
  // bottomOrd and bottomOrd + 1, so equal-ord docs strictly beat it and
  // there is no boundary tie to reason about.
  OrdIntervalStatus competitiveOrdInterval(
      CompetitiveOrdState& out, int32_t bottomSlot, segdoc bottomDoc,
      int32_t segment, int32_t from, bool soleSort) override {
    unused(bottomSlot);
    if (!termsEnum.has_value() || !reader.has_value()) {
      return OrdIntervalStatus::UNSUPPORTED;
    }
    // Missing-first: docs without the field beat any real-valued bottom but
    // have no postings, so a term interval can never represent them.
    if (missingFirst || bottomMissing) return OrdIntervalStatus::PENDING;
    int64_t nTerms = (int64_t)fieldInfo->nTerms;
    bool closable = soleSort && bottomExact
        && segdoc(segment, from) >= bottomDoc;
    if (sortMultiplier > 0) {
      out.lo = 1;
      out.hi = closable ? (int64_t)bottomOrd - 1 : (int64_t)bottomOrd;
    } else {
      out.lo = bottomExact ? (closable ? (int64_t)bottomOrd + 1
                                       : (int64_t)bottomOrd)
                           : (int64_t)bottomOrd + 1;
      out.hi = nTerms;
    }
    out.nTerms = nTerms;
    out.descending = sortMultiplier < 0;
    out.postingsReader = postingsReader;
    out.fieldInfo = fieldInfo;
    return OrdIntervalStatus::READY;
  }

  void setBottom(int32_t slot) override {
    assert(slot >= 0 && slot < (int32_t)slots.size());
    Slot& bottom = slots[slot];
    bottomMissing = bottom.ord == 0;
    bottomExact = false;
    bottomOrd = 0;
    if (bottomMissing || !termsEnum.has_value()) return;

    if (bottom.ordGen == currentSegment) {
      bottomOrd = bottom.ord;
      bottomExact = true;
      return;
    }

    std::string_view value = term(bottom);
    if (!termsEnum->seekCeil(value)) {
      assert(fieldInfo->nTerms <= INT32_MAX);
      bottomOrd = (int32_t)fieldInfo->nTerms;
      return;
    }
    if (termsEnum->term() == value) {
      bottomOrd = (int32_t)termsEnum->ord() + 1;
      bottomExact = true;
      bottom.ord = bottomOrd;
      bottom.ordGen = currentSegment;
    } else {
      bottomOrd = (int32_t)termsEnum->ord();
    }
  }

  int compare(int32_t slotA, segdoc docA, int32_t slotB, segdoc docB) override {
    unused(docA, docB);
    assert(slotA >= 0 && slotA < (int32_t)slots.size());
    assert(slotB >= 0 && slotB < (int32_t)slots.size());
    return sortMultiplier * compareSlots(slots[slotA], slots[slotB]);
  }

  LUXIR_NOINLINE int compareBottom(int32_t bottomSlot, segdoc bottomDoc,
                                   segdoc newDoc) override {
    unused(bottomDoc);
    assert(bottomSlot >= 0 && bottomSlot < (int32_t)slots.size());
    assert(bottomMissing == (slots[bottomSlot].ord == 0));
    int32_t docOrd = selectedOrd(newDoc.docId());
    bool docMissing = docOrd == 0;
    int cmp;
    if (bottomMissing || docMissing) {
      cmp = compareMissing(bottomMissing, docMissing);
    } else if (bottomExact) {
      cmp = (bottomOrd > docOrd) - (bottomOrd < docOrd);
    } else {
      cmp = bottomOrd >= docOrd ? 1 : -1;
    }
    return sortMultiplier * cmp;
  }

  int compareCurrent(int32_t slot, segdoc oldDoc, segdoc newDoc) override {
    unused(oldDoc);
    const Slot& head = slots[(size_t)slot];
    int32_t ord = selectedOrd(newDoc.docId());
    int cmp;
    if (head.ord == 0 || ord == 0) {
      cmp = compareMissing(head.ord == 0, ord == 0);
    } else if (head.ordGen == currentSegment) {
      cmp = (head.ord > ord) - (head.ord < ord);
    } else {
      termsEnum->seekOrd((int64_t)ord - 1);
      cmp = compareTerms(term(head), (std::string_view)termsEnum->term());
    }
    return sortMultiplier * cmp;
  }

  void copy(int32_t slot, segdoc doc) override {
    assert(slot >= 0 && slot < (int32_t)slots.size());
    Slot& target = slots[slot];
    int32_t ord = selectedOrd(doc.docId());
    if (ord == 0) {
      setMissing(target);
      return;
    }
    assert(termsEnum.has_value());
    termsEnum->seekOrd(ord - 1);
    copyTerm(target, (std::string_view)termsEnum->term());
    target.ord = ord;
    target.ordGen = currentSegment;
  }

  int compare(int32_t slotA, segdoc docA, FieldComparator& other,
              int32_t slotB, segdoc docB) override {
    unused(docA, docB);
    assert(dynamic_cast<SegmentOrdComparator*>(&other) != nullptr);
    auto* otherOrd = static_cast<SegmentOrdComparator*>(&other);
    assert(fields == otherOrd->fields);
    assert(slotA >= 0 && slotA < (int32_t)slots.size());
    assert(slotB >= 0 && slotB < (int32_t)otherOrd->slots.size());
    return sortMultiplier * compareSlots(slots[slotA], otherOrd->slots[slotB]);
  }

  void copy(int32_t slot, FieldComparator& other, int32_t otherSlot,
            segdoc otherDoc) override {
    unused(otherDoc);
    assert(dynamic_cast<SegmentOrdComparator*>(&other) != nullptr);
    auto* otherOrd = static_cast<SegmentOrdComparator*>(&other);
    assert(fields == otherOrd->fields);
    assert(slot >= 0 && slot < (int32_t)slots.size());
    assert(otherSlot >= 0 && otherSlot < (int32_t)otherOrd->slots.size());
    Slot& target = slots[slot];
    const Slot& source = otherOrd->slots[otherSlot];
    if (source.ord == 0) {
      setMissing(target);
      return;
    }
    copyTerm(target, otherOrd->term(source));
    target.ord = source.ord;
    target.ordGen = source.ordGen;
  }
};

// A comparator for non-indexed string columns that compares by value
class StrColComparator : public FieldComparator {
  std::optional<StrColReader> reader;
  std::optional<StrColReader::Iterator> iter;
  std::optional<StrColReader::DocValues> docValues;
  bool multiValued = false;
  std::vector<std::string> values; // Storage for string values
  // Missing is tracked apart from the value: any string, "" included, is a
  // real value that must not tie with a missing one.
  std::vector<uint8_t> missing;
  int sortMultiplier; // 1 for ascending, -1 for descending
  int missingCmp; // missing vs present, in final order (direction applied)

public:
  StrColComparator(bool reversed, MissingValue missingValue, IndexReader::FieldSegments fields)
    : FieldComparator(fields),
      sortMultiplier(reversed ? -1 : 1),
      missingCmp(missingValue == MISSING_FIRST ? -1 : 1) {}

  void setSegment(const IndexReader::Segment& segment) override {
    auto* postingsReader = &segment.postingsReader();
    // Reset reader and iterator
    docValues.reset();
    iter.reset();
    reader.reset();
    multiValued = false;

    if (auto* info = fields.at(segment)) {
      const auto& fieldInfo = *info;
      if (fieldInfo.columnLoc.offset() > 0) {
        reader.emplace(*postingsReader, fieldInfo);
        iter.emplace(*reader);
        multiValued = reader->isMultiValued();
        if (multiValued) docValues.emplace(*reader);
      }
    }
  }

  void growSlots(int32_t capacity) override {
    if ((size_t)capacity > values.size()) {
      resizeExact(values, (size_t)capacity);
      resizeExact(missing, (size_t)capacity);
    }
  }

  std::optional<std::string_view> getDocValue(int32_t docid) {
    if (!iter.has_value()) {
      return std::nullopt;
    }

    // Advance iterator to docid if needed
    if (iter->docId() < docid) {
      int32_t foundDoc = iter->advance(docid);
      if (foundDoc == StrColReader::Iterator::ENDDOC || foundDoc > docid) {
        return std::nullopt;
      }
    } else if (iter->docId() > docid) {
      return std::nullopt;
    }

    return multiValued
        ? reader->selectedValue(iter->rank(), sortMultiplier < 0, *docValues)
        : iter->value();
  }

  int compareValues(bool missingA, std::string_view valA,
                    bool missingB, std::string_view valB) const {
    if (missingA || missingB) {
      return missingA == missingB ? 0 : missingA ? missingCmp : -missingCmp;
    }
    return sortMultiplier * valA.compare(valB);
  }

  int compare(int32_t slotA, segdoc docA, int32_t slotB, segdoc docB) override {
    assert(slotA >= 0 && slotA < (int32_t)values.size());
    assert(slotB >= 0 && slotB < (int32_t)values.size());
    return compareValues(missing[slotA], values[slotA], missing[slotB], values[slotB]);
  }

  // NOINLINE: see SimpleNumericFieldComparator::compareBottom.
  LUXIR_NOINLINE int compareBottom(int32_t bottomSlot, segdoc bottomDoc, segdoc newDoc) override {
    assert(bottomSlot >= 0 && bottomSlot < (int32_t)values.size());
    auto newVal = getDocValue(newDoc.docId());
    return compareValues(missing[bottomSlot], values[bottomSlot],
                         !newVal.has_value(), newVal.value_or(std::string_view()));
  }

  void copy(int32_t slot, segdoc doc) override {
    assert(slot >= 0 && slot < (int32_t)values.size());
    auto value = getDocValue(doc.docId());
    values[slot] = std::string(value.value_or(std::string_view()));
    missing[slot] = !value.has_value();
  }

  int compare(int32_t slotA, segdoc docA, FieldComparator& other, int32_t slotB, segdoc docB) override {
    assert(dynamic_cast<StrColComparator*>(&other) != nullptr);
    auto* otherStr = static_cast<StrColComparator*>(&other);

    assert(slotA >= 0 && slotA < (int32_t)values.size());
    assert(slotB >= 0 && slotB < (int32_t)otherStr->values.size());
    return compareValues(missing[slotA], values[slotA],
                         otherStr->missing[slotB], otherStr->values[slotB]);
  }

  void copy(int32_t slot, FieldComparator& other, int32_t otherSlot, segdoc otherDoc) override {
    assert(dynamic_cast<StrColComparator*>(&other) != nullptr);
    auto* otherStr = static_cast<StrColComparator*>(&other);

    assert(slot >= 0 && slot < (int32_t)values.size());
    assert(otherSlot >= 0 && otherSlot < (int32_t)otherStr->values.size());
    values[slot] = otherStr->values[otherSlot];
    missing[slot] = otherStr->missing[otherSlot];
  }

  // Retained variable-width payload; fixed slot arrays are accounted by the
  // owning collection route. Ordinary top-k never calls this accessor.
  size_t retainedSlotBytes(int32_t slot) const;

  ~StrColComparator() override = default;
};

} // namespace luxir
