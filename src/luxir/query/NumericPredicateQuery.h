// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <boost/container/small_vector.hpp>

#include <algorithm>
#include <bit>
#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <span>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <vector>

#if defined(__SSE2__)
#include <immintrin.h>
#endif

#include "luxir/query/Query.h"
#include "luxir/query/PointsMaterialize.h"
#include "luxir/reader/IntColReader.h"
#include "luxir/reader/PointsReader.h"
#include "SummaryPredicates.h"

namespace luxir {

// Numeric predicate over the doc-order numeric column: either one inclusive
// range or an exact sorted value set. Every numeric field type stores an
// order-preserving encoded int64 (INT raw, FLOAT/DOUBLE Lucene sortable bits,
// DATE epoch units), so pruning and comparisons happen in encoded space. A
// document matches when any value satisfies the range or set.
class NumericPredicateQuery final : public Query {
  std::string_view field;
  PointsReader::ValueRange envelope;
  // hi - lo, so matchesValue's range test is one subtract and one compare.
  // An empty range (hi < lo) wraps; every scorer entry point rejects it first.
  uint64_t envelopeWidth;
  std::span<const int64_t> exactValues;
  std::span<const PointsReader::ValueRange> exactIntervals;
  // Normally SEGMENT_STABLE; REQUEST_STABLE when a bound came from the request
  // clock (unrounded NOW). Such keys never repeat across requests, so skip the
  // shared cache and admission history while still deduplicating in the request.
  // equals()/hash() ignore this: BooleanQuery dedups by equals(), and cache
  // lifetime must not change membership.
  FilterKeyScope filterKeyScope = FilterKeyScope::SEGMENT_STABLE;

public:
  bool recoverableAsPredicate() const noexcept override { return true; }

  static inline bool disableShapesForTests = false;
#ifndef NDEBUG
  // Observe actual verification, not column decoding or metadata navigation.
  // Tests install this only after the product has been built.
  static inline thread_local void (*verificationObserver)(const NumericPredicateQuery&, int32_t) = nullptr;
  // Rank-to-document selects on sparse docsWithValue in zone fills.
  static inline thread_local int64_t rankSelectsForTests = 0;
#endif

  NumericPredicateQuery(std::string_view field, int64_t lo, int64_t hi,
                        FilterKeyScope filterKeyScope = FilterKeyScope::SEGMENT_STABLE)
    : Query(QueryKind::NUMERIC_PREDICATE), field(field), envelope{lo, hi},
      envelopeWidth((uint64_t)hi - (uint64_t)lo), filterKeyScope(filterKeyScope) {}

  NumericPredicateQuery(std::string_view field,
                        std::span<const int64_t> exactValues,
                        std::span<const PointsReader::ValueRange> intervals)
    : Query(QueryKind::NUMERIC_PREDICATE), field(field),
      envelope{exactValues.front(), exactValues.back()},
      envelopeWidth((uint64_t)envelope.hi - (uint64_t)envelope.lo),
      exactValues(exactValues), exactIntervals(intervals) {
    assert(exactValues.size() > 1);
    assert(!intervals.empty());
    assert(std::is_sorted(exactValues.begin(), exactValues.end()));
    assert(std::adjacent_find(exactValues.begin(), exactValues.end())
           == exactValues.end());
  }

  bool equalsSameKind(const Query& other) const override {
    const auto& rhs = static_cast<const NumericPredicateQuery&>(other);
    if (field != rhs.field
        || envelope.lo != rhs.envelope.lo || envelope.hi != rhs.envelope.hi
        || exactValues.size() != rhs.exactValues.size()
        || exactIntervals.size() != rhs.exactIntervals.size()
        || !std::equal(exactValues.begin(), exactValues.end(),
                       rhs.exactValues.begin())) {
      return false;
    }
    return std::equal(
        exactIntervals.begin(), exactIntervals.end(),
        rhs.exactIntervals.begin(),
        [](const PointsReader::ValueRange& a,
           const PointsReader::ValueRange& b) {
          return a.lo == b.lo && a.hi == b.hi;
        });
  }

  uint64_t hashImpl() const override {
    uint64_t value = mixHash(Query::hashImpl(), field);
    value = mixHash(value, envelope.lo);
    value = mixHash(value, envelope.hi);
    value = mixSampledSequence(
        value, exactValues,
        [](uint64_t seed, int64_t exact) {
          return mixHash(seed, exact);
        });
    return mixSampledSequence(
        value, exactIntervals,
        [](uint64_t seed, const PointsReader::ValueRange& interval) {
          seed = mixHash(seed, interval.lo);
          return mixHash(seed, interval.hi);
        });
  }

  std::string_view getField() const { return field; }
  int64_t getLo() const { return envelope.lo; }
  int64_t getHi() const { return envelope.hi; }
  bool isExactSet() const { return !exactValues.empty(); }
  std::span<const int64_t> getExactValues() const { return exactValues; }
  std::span<const PointsReader::ValueRange> intervals() const {
    return exactIntervals.empty()
        ? std::span<const PointsReader::ValueRange>(&envelope, 1)
        : exactIntervals;
  }

  // The range test is inlined into every per-value loop; the exact-set
  // search stays out of line. One unsigned compare: a short-circuit lower
  // bound mispredicts on about half the values of an interior range.
  LUXIR_INLINE bool matchesValue(int64_t value) const {
    if (exactValues.empty()) {
      return (uint64_t)value - (uint64_t)envelope.lo <= envelopeWidth;
    }
    return matchesExact(value);
  }

  bool matchesExact(int64_t value) const {
    auto found = std::lower_bound(
        exactIntervals.begin(), exactIntervals.end(), value,
        [](const PointsReader::ValueRange& interval, int64_t target) {
          return interval.hi < target;
        });
    return found != exactIntervals.end() && found->lo <= value;
  }

  bool intersects(int64_t min, int64_t max) const {
    if (exactValues.empty()) {
      return !(max < envelope.lo || envelope.hi < min);
    }
    auto found = std::lower_bound(
        exactIntervals.begin(), exactIntervals.end(), min,
        [](const PointsReader::ValueRange& interval, int64_t target) {
          return interval.hi < target;
        });
    return found != exactIntervals.end() && found->lo <= max;
  }

  bool covers(int64_t min, int64_t max) const {
    if (exactValues.empty()) return envelope.lo <= min && max <= envelope.hi;
    auto found = std::lower_bound(
        exactIntervals.begin(), exactIntervals.end(), min,
        [](const PointsReader::ValueRange& interval, int64_t target) {
          return interval.hi < target;
        });
    return found != exactIntervals.end()
        && found->lo <= min && max <= found->hi;
  }

  // A matching doc has a value inside a range or equal to a set member, so
  // either way inside [lo, hi] (the set's first and last values).
  std::optional<ValueEnvelope> requiredValueEnvelope(
      std::string_view target) const override {
    if (target != field) return std::nullopt;
    return ValueEnvelope{envelope.lo, envelope.hi};
  }

  MatchState mayMatch(const SummaryView& view) const override {
    if (envelope.lo > envelope.hi) {
      view.prove("empty numeric range");
      return MatchState::EMPTY;
    }
    return summary::numericMatch(view, field, [this](int64_t min, int64_t max) { return intersects(min, max); });
  }

  float valueMatchCost() const {
    if (!isExactSet()) return 1.0f;
    return (float)(std::bit_width(exactIntervals.size()) + 1);
  }
  FilterKeyScope appendFilterKey(FilterKeyBuilder& out,
                                 const FilterKeyContext& ctx) const override {
    out.appendKind(kind);
    unused(ctx);
    out.appendByte(exactValues.empty() ? 0 : 1);
    out.appendString(field);
    if (exactValues.empty()) {
      out.appendInt64(envelope.lo);
      out.appendInt64(envelope.hi);
    } else {
      out.appendSize(exactValues.size());
      for (int64_t value : exactValues) out.appendInt64(value);
    }
    return filterKeyScope;
  }

  Query::Weight* createWeight(Context& context, int32_t flags,
                              float multiplier = 1.0f) override;

  // PredicateScorer is the sparse two-phase verifier and, when instantiated
  // with IntColReader::Iterator, the benchmark's full column-scan baseline.
  template <class ColIter>
  class PredicateScorer final : public Query::ConstantScorer {
    IntColReader& reader;
    ColIter iter;
    const NumericPredicateQuery& query;
    bool allMatch;
    bool multi;
    int32_t docid = -1;

    LUXIR_INLINE void observe(int32_t doc) {
#ifndef NDEBUG
      if (verificationObserver != nullptr) verificationObserver(query, doc);
#else
      unused(doc);
#endif
    }

    bool matchesAt(int32_t doc, int32_t rank) {
      observe(doc);
      if (!multi) {
        if (allMatch) return true;
        return query.matchesValue(iter.values().valueAt(rank));
      }
      auto [start, end] = reader.getStartEndValueRank(rank);
      if (allMatch) return start < end;
      for (int64_t r = start; r < end; r++) {
        if (query.matchesValue(iter.values().valueAt(r))) return true;
      }
      return false;
    }

    bool valueMatches() {
      return docid != PostingsReader::END && matchesAt(docid, iter.rank());
    }

    // Calls scan(test) with this column's per-document test. Scans step with
    // iter.nextWhere, which keeps the cursor in registers, and store docid
    // once per call; a single-valued predicate gets its own small loop so the
    // value test inlines into it.
    template <typename Scan>
    LUXIR_INLINE int32_t withTest(Scan&& scan) {
      if (!multi && !allMatch) {
        return scan([this](int32_t doc, int32_t rank) LUXIR_INLINE {
          observe(doc);
          return query.matchesValue(iter.values().valueAt(rank));
        });
      }
      return scan([this](int32_t doc, int32_t rank) { return matchesAt(doc, rank); });
    }

  public:
    PredicateScorer(IntColReader& reader, const NumericPredicateQuery& query,
                bool allMatch,
                float constantScore)
      : Query::ConstantScorer(constantScore), reader(reader), iter(reader),
        query(query), allMatch(allMatch), multi(reader.multiValued()) {}

    [[gnu::cold]] void reset() override {
      iter.reset();
      docid = -1;
    }

    int32_t approximationNext() override {
      docid = iter.next();
      return docid;
    }
    int32_t approximationAdvance(int32_t target) override {
      docid = iter.advance(target);
      return docid;
    }
    int32_t approximationDocId() override { return docid; }
    bool matches() override { return valueMatches(); }
    float matchCost() override {
      if (allMatch) return 0.0f;
      if (!multi) return query.valueMatchCost();
      int64_t docs = reader.docsWithValue();
      float valuesPerDoc = docs > 0
          ? (float)reader.numValues() / (float)docs : 1.0f;
      return valuesPerDoc * query.valueMatchCost();
    }

    int32_t next() override {
      docid = withTest([this](auto test) { return iter.nextWhere(test); });
      return docid;
    }
    int32_t advance(int32_t target) override {
      assert(docid < target);
      docid = iter.advance(target);
      if (docid == PostingsReader::END) return docid;
      docid = withTest([this](auto test) {
        return test(docid, iter.rank()) ? docid : iter.nextWhere(test);
      });
      return docid;
    }
    int32_t docId() override { return docid; }

    int32_t advanceApproximationWindow(int32_t min, int32_t max) override {
      if (min >= max) return max;
      if (docid < min) docid = iter.advance(min);
      return docid;
    }

    int32_t advanceWindow(int32_t min, int32_t max) override {
      if (min >= max) return max;
      if (docid < min) docid = iter.advance(min);
      if (docid >= max) return docid;
      docid = withTest([this, max](auto test) {
        if (test(docid, iter.rank())) return docid;
        return iter.nextWhere([&test, max](int32_t doc, int32_t rank) LUXIR_INLINE {
          return doc >= max || test(doc, rank);
        });
      });
      return docid;
    }

    void intersectCandidateBits(std::span<uint64_t> words, int32_t min) override {
      // Keep each value loop's registers independent of the filter dispatch.
      docid = withTest([this, words, min](auto test) LUXIR_NOINLINE {
        int32_t current = docid;
        for (size_t w = 0; w < words.size(); w++) {
          uint64_t candidates = words[w];
          while (candidates != 0) {
            int32_t bit = (int32_t)std::countr_zero(candidates);
            int32_t doc = min + (int32_t)(w * 64) + bit;
            if (current < doc) current = iter.advance(doc);
            if (current != doc || !test(doc, iter.rank())) words[w] &= ~(1ULL << bit);
            candidates &= candidates - 1;
          }
        }
        return current;
      });
    }

    void fillWindowBits(std::span<uint64_t> words, int32_t min,
                        int32_t max) override {
      skipCount(SkipStats::countBulkFillCalls);
      if (min >= max) return;
      if (docid < min) docid = iter.advance(min);
      if (docid >= max) return;
      docid = withTest([this, words, min, max](auto test) {
        auto mark = [&test, words, min, max](int32_t doc, int32_t rank) LUXIR_INLINE {
          if (doc >= max) return true;
          if (test(doc, rank)) {
            int32_t index = doc - min;
            words[(size_t)(index >> 6)] |= 1ULL << (index & 63);
          }
          return false;
        };
        mark(docid, iter.rank());
        return iter.nextWhere(mark);
      });
    }
  };

  enum class BlockRelation : uint8_t {
    OUTSIDE,
    INSIDE,
    CROSSES
  };

  // A block's relation to the predicate. A CROSSES block is refined below
  // block granularity: per leaf zone (any encoding), and for a linear-fit
  // block by the rank intervals the codec bounds. runAt walks the refinement
  // as runs of in-block ranks sharing one relation, so consumers emit INSIDE
  // runs, skip OUTSIDE runs and check values (CrossingValues) only in CROSSES
  // runs.
  struct BlockPlan {
    static constexpr uint32_t LEAF_SIZE = NumColumnFormat::LEAF_ZONE_SIZE;
    static_assert(NumColumnFormat::LEAVES_PER_BLOCK <= 8);
    static_assert(IntColReader::BLOCK_SIZE <= UINT16_MAX);

    BlockRelation relation = BlockRelation::OUTSIDE;
    // Bit i describes the block's leaf i: inside, crossing, or neither
    // (outside).
    uint8_t leafInside = 0;
    uint8_t leafCrossing = 0;
    // Classification already establishes the domain of the residual bounds.
    // Do not re-read codec metadata and query kind for every crossing value.
    bool residualComparable = false;
    // In-block ranks outside [checkBegin, checkEnd) cannot match; ranks in
    // [insideBegin, insideEnd) all match.
    uint16_t checkBegin = 0;
    uint16_t checkEnd = 0;
    uint16_t insideBegin = 0;
    uint16_t insideEnd = 0;
    // Read only for CROSSES runs of residual-comparable blocks: matching
    // residuals are [residualLower, residualLower + residualWidth].
    uint32_t residualLower = 0;
    uint32_t residualWidth = 0;

    struct Run {
      BlockRelation relation;
      uint32_t end;
    };

    // The relation shared by in-block ranks [rank, end). end is capped at
    // BLOCK_SIZE, not at the block's value count; callers clamp it.
    Run runAt(uint32_t rank) const {
      if (relation != BlockRelation::CROSSES) {
        return {relation, IntColReader::BLOCK_SIZE};
      }
      if (rank < checkBegin) return {BlockRelation::OUTSIDE, checkBegin};
      if (rank >= checkEnd) {
        return {BlockRelation::OUTSIDE, IntColReader::BLOCK_SIZE};
      }
      if (rank >= insideBegin && rank < insideEnd) {
        return {BlockRelation::INSIDE, insideEnd};
      }
      uint32_t leaf = rank / LEAF_SIZE;
      uint32_t end = std::min<uint32_t>(checkEnd, (leaf + 1) * LEAF_SIZE);
      if (rank < insideBegin) end = std::min<uint32_t>(end, insideBegin);
      uint32_t bit = 1u << leaf;
      BlockRelation leafRelation = (leafInside & bit) != 0
          ? BlockRelation::INSIDE
          : (leafCrossing & bit) != 0 ? BlockRelation::CROSSES
                                      : BlockRelation::OUTSIDE;
      return {leafRelation, end};
    }

    // Calls fn(relation, begin, end) for each run of the block's `count`
    // values.
    template <typename Fn>
    void forEachRun(uint32_t count, Fn&& fn) const {
      for (uint32_t rank = 0; rank < count;) {
        Run run = runAt(rank);
        uint32_t end = std::min(run.end, count);
        fn(run.relation, rank, end);
        rank = end;
      }
    }
  };

  // Query-local execution metadata. Neither construction nor planning touches
  // the blocks. Producers and the compact rejection index share the same
  // refinement on first executing use.
  class BlockPlans {
    MemPool& pool;
    IntColReader& reader;
    const NumericPredicateQuery& query;
    std::span<BlockPlan> plans;
    std::span<uint64_t> initialized;
    bool complete = false;

    void allocate() {
      if (!plans.empty()) return;
      size_t count = (size_t)reader.numBlocks();
      auto* storage = (BlockPlan*)pool.alloc(count * sizeof(BlockPlan), alignof(BlockPlan));
      plans = {storage, count};
    }

    void classify(size_t block) {
      // Construct in place: narrow stores followed by a whole-plan assignment
      // cause store/reload stalls on broad execution.
      new (plans.data() + block) BlockPlan(query.classifyBlock(reader, (int64_t)block));
      skipCount(SkipStats::numericPredicateClassifiedBlocks);
    }

  public:
    BlockPlans(MemPool& pool, IntColReader& reader,
               const NumericPredicateQuery& query)
        : pool(pool), reader(reader), query(query) {}

    const BlockPlan& operator[](size_t block) {
      if (!complete) {
        allocate();
        if (initialized.empty()) {
          initialized = pool.make_span<uint64_t>((plans.size() + 63) / 64);
          std::fill(initialized.begin(), initialized.end(), 0);
        }
        uint64_t bit = 1ULL << (block & 63);
        if ((initialized[block >> 6] & bit) == 0) {
          classify(block);
          initialized[block >> 6] |= bit;
        }
      }
      return plans[block];
    }

    std::span<const BlockPlan> all() {
      if (!complete) {
        allocate();
        for (size_t block = 0; block < plans.size(); block++) {
          if (initialized.empty() || (initialized[block >> 6] & (1ULL << (block & 63))) == 0) {
            classify(block);
          }
        }
        complete = true;
      }
      return plans;
    }

    // Prepare just the value blocks a window can read. The returned span is
    // indexed by absolute block number; only this range is initialized. Keep
    // the readiness checks out of the run/value loops used by both producers.
    std::span<const BlockPlan> forValues(int64_t begin, int64_t end) {
      assert(begin < end);
      size_t first = (size_t)(begin / IntColReader::BLOCK_SIZE);
      size_t last = (size_t)((end - 1) / IntColReader::BLOCK_SIZE);
      for (size_t block = first; block <= last; block++) (*this)[block];
      return plans;
    }
  };

  class ColumnProof final : public Query::MembershipProof {
    MemPool& pool;
    IntColReader& reader;
    const NumericPredicateQuery& query;
    BlockPlans& plans;
    bool fullIndex;
    bool indexed = false;
    std::span<Range> outside;
    size_t cursor = 0;
    screaming::BitSet::Selector* selector = nullptr;
    screaming::BitSet::Iterator* docs = nullptr;

    void ensureSelector() {
      if (selector == nullptr && !reader.denseDocsWithValue()) {
        const auto& bits = reader.docsWithValueBitSet();
        selector = pool.make<screaming::BitSet::Selector>(
            bits, pool.make_span<int32_t>((size_t)bits.nBuckets + 1));
      }
    }

    int32_t docForRank(int32_t rank) {
      if (rank >= reader.docsWithValue()) return PostingsReader::END;
      return selector == nullptr ? rank : selector->select(rank);
    }

    Range localRange(int32_t target) {
      ensureSelector();
      int32_t rank = target;
      if (selector != nullptr) {
        if (docs == nullptr) {
          docs = pool.make<screaming::BitSet::Iterator>(reader.docsWithValueBitSet());
        } else if (target < docs->val()) {
          docs->reset();
        }
        target = target > docs->val() ? docs->advance(target) : docs->val();
        if (target == screaming::BitSet::END) return {PostingsReader::END, PostingsReader::END};
        rank = docs->rank();
      }
      while (rank < reader.docsWithValue()) {
        int64_t block = rank / IntColReader::BLOCK_SIZE;
        NumBlockZone zone = reader.blockZone(block);
        int64_t endRank = (block + 1) * IntColReader::BLOCK_SIZE;
        bool possible = query.intersects(zone.min, zone.max);
        if (possible && !query.covers(zone.min, zone.max)) {
          // Sparse use needs only a local rejection bound. Crossing leaves
          // remain possible; the candidate verifier handles their values.
          int64_t leaf = rank / NumColumnFormat::LEAF_ZONE_SIZE;
          zone = reader.leafMeta().leafZone(leaf);
          possible = query.intersects(zone.min, zone.max);
          endRank = (leaf + 1) * NumColumnFormat::LEAF_ZONE_SIZE;
        }
        int32_t end = (int32_t)std::min<int64_t>(reader.numValues(), endRank);
        if (possible) return {target, docForRank(end)};
        rank = end;
        target = docForRank(rank);
      }
      return {PostingsReader::END, PostingsReader::END};
    }

    void buildIndex() {
      ensureSelector();
      boost::container::small_vector<Range, 8> ranges;
      auto refined = plans.all();
      for (int64_t block = 0; block < reader.numBlocks(); block++) {
        const auto& plan = refined[(size_t)block];
        uint32_t count = (uint32_t)reader.valuesInBlock(block);
        plan.forEachRun(count, [&](BlockRelation relation, uint32_t begin, uint32_t end) {
          if (relation != BlockRelation::OUTSIDE) return;
          int32_t base = (int32_t)(block * IntColReader::BLOCK_SIZE);
          Range range{base + (int32_t)begin, base + (int32_t)end};
          if (!ranges.empty() && ranges.back().end == range.begin) ranges.back().end = range.end;
          else ranges.push_back(range);
        });
      }
      if (selector != nullptr) {
        int32_t first = selector->select(0);
        for (auto& range : ranges) {
          if (range.begin != 0) range.begin = docForRank(range.begin);
          range.end = docForRank(range.end);
        }
        if (first > 0 && (ranges.empty() || ranges.front().begin > 0)) {
          ranges.insert(ranges.begin(), Range{0, first});
        }
      } else if (!ranges.empty() && ranges.back().end == reader.numValues()) {
        ranges.back().end = PostingsReader::END;
      }
      int32_t afterLast = selector == nullptr ? (int32_t)reader.numValues()
          : selector->select(reader.docsWithValue() - 1) + 1;
      if ((ranges.empty() || ranges.back().end != PostingsReader::END)
          && afterLast < PostingsReader::END) {
        ranges.push_back({afterLast, PostingsReader::END});
      }
      outside = pool.make_span<Range>(ranges.size());
      std::copy(ranges.begin(), ranges.end(), outside.begin());
      indexed = true;
      skipCount(SkipStats::numericPredicateProofIndexes);
    }

    Range nextRange(int32_t target) override {
      if (!fullIndex) return localRange(target);
      if (!indexed) buildIndex();
      if (cursor > 0 && target < outside[cursor - 1].end) cursor = 0;
      cursor = (size_t)screaming::gallopLowerBound(
          (int64_t)cursor, (int64_t)outside.size(), target + 1,
          [&](int64_t i) { return outside[(size_t)i].end; });
      if (cursor < outside.size() && target >= outside[cursor].begin) {
        target = outside[cursor++].end;
      }
      return {target, cursor < outside.size() ? outside[cursor].begin : PostingsReader::END};
    }

  public:
    ColumnProof(MemPool& pool, IntColReader& reader,
                const NumericPredicateQuery& query, BlockPlans& plans, bool fullIndex)
        : pool(pool), reader(reader), query(query), plans(plans), fullIndex(fullIndex) {
      assert(!reader.multiValued());
    }

    void reset() override {
      MembershipProof::reset();
      cursor = 0;
      if (docs != nullptr) docs->reset();
    }
  };

  BlockPlan classifyBlock(const IntColReader& reader, int64_t blockNum) const {
    BlockPlan plan;
    NumBlockZone zone = reader.blockZone(blockNum);
    if (!intersects(zone.min, zone.max)) {
      return plan;
    }
    if (covers(zone.min, zone.max)) {
      plan.relation = BlockRelation::INSIDE;
      return plan;
    }

    plan.relation = BlockRelation::CROSSES;
    uint32_t count = (uint32_t)reader.valuesInBlock(blockNum);
    plan.checkEnd = (uint16_t)count;
    int64_t firstLeaf = blockNum * NumColumnFormat::LEAVES_PER_BLOCK;
    uint32_t leaves = (count + BlockPlan::LEAF_SIZE - 1) / BlockPlan::LEAF_SIZE;
    if (firstLeaf + leaves <= reader.numLeafZones()) {
      const auto& leafMeta = reader.leafMeta();
      for (uint32_t leaf = 0; leaf < leaves; leaf++) {
        NumBlockZone leafZone = leafMeta.leafZone(firstLeaf + leaf);
        if (!intersects(leafZone.min, leafZone.max)) continue;
        if (covers(leafZone.min, leafZone.max)) {
          plan.leafInside |= (uint8_t)(1u << leaf);
        } else {
          plan.leafCrossing |= (uint8_t)(1u << leaf);
        }
      }
    } else {
      plan.leafCrossing = (uint8_t)((1u << leaves) - 1);
    }

    if (plan.leafCrossing != 0) {
      if (auto bounds = reader.rankBounds(blockNum, envelope.lo, envelope.hi)) {
        plan.checkBegin = (uint16_t)bounds->checkBegin;
        plan.checkEnd = (uint16_t)bounds->checkEnd;
        // An exact set's envelope proves exclusion, not inclusion.
        if (!isExactSet()) {
          plan.insideBegin = (uint16_t)bounds->insideBegin;
          plan.insideEnd = (uint16_t)bounds->insideEnd;
        }
      }
    }

    // Leaves can settle a block its hull zone could not: an exact set whose
    // intervals each cover different leaves, or a range in a gap between
    // unsorted leaves.
    bool anyInside = false;
    bool anyOutside = false;
    bool anyCrossing = false;
    plan.forEachRun(count, [&](BlockRelation relation, uint32_t, uint32_t) {
      anyInside |= relation == BlockRelation::INSIDE;
      anyOutside |= relation == BlockRelation::OUTSIDE;
      anyCrossing |= relation == BlockRelation::CROSSES;
    });
    if (!anyCrossing && !anyInside) return BlockPlan();
    if (!anyCrossing && !anyOutside) {
      BlockPlan inside;
      inside.relation = BlockRelation::INSIDE;
      return inside;
    }

    if (!anyCrossing || isExactSet()) return plan;
    auto block = reader.blockInfo(blockNum);
    if (block.residualComparable()) {
      auto bounds = NumColumnFormat::residualBounds(
          block, envelope.lo, envelope.hi);
      // No lattice point falls in the range, so nothing in the block matches.
      if (bounds.lower > bounds.upper) {
        assert(!anyInside);
        return BlockPlan();
      }
      plan.residualLower = bounds.lower;
      plan.residualWidth = bounds.upper - bounds.lower;
      plan.residualComparable = true;
    }
    return plan;
  }

  // The values of CROSSES runs, decoded one 128-value codec frame at a time
  // and kept until a request leaves the frame. A 64-value group never spans
  // frames, so consumers ask by group: mask() answers every requested value
  // at once, any() stops at the first match.
  class CrossingValues {
    // The decoders fill at most one frame, and frames start at multiples of
    // its size, so no 64-value group spans two.
    static constexpr uint32_t FRAME = NumColumnFormat::BULK_SIZE;
    static_assert(FRAME % 64 == 0);

    IntColReader& reader;
    int64_t residualStart = -1;
    int64_t rawStart = -1;
    uint32_t residualCount = 0;
    uint32_t rawCount = 0;
    uint32_t residuals[FRAME];
    int64_t raw[FRAME];

    // Once per frame, out of line: the group loops carry no codec dispatch.
    LUXIR_NOINLINE void decodeResiduals(int64_t rank) {
      residualStart = reader.decodeResidualSubBlock(rank, residuals,
                                                    residualCount);
    }

    LUXIR_NOINLINE void decodeRaw(int64_t rank) {
      rawStart = reader.decodeValueSubBlock(rank, raw, rawCount);
    }

    LUXIR_INLINE const uint32_t* residualsAt(int64_t rank) {
      if ((uint64_t)(rank - residualStart) >= residualCount) decodeResiduals(rank);
      return residuals + (rank - residualStart);
    }

    LUXIR_INLINE const int64_t* rawAt(int64_t rank) {
      if ((uint64_t)(rank - rawStart) >= rawCount) decodeRaw(rank);
      return raw + (rank - rawStart);
    }

    // Bit i: values[i] - lower <= width in unsigned 32-bit arithmetic, for
    // 64 decoded residuals.
    static LUXIR_INLINE uint64_t residualMask64(const uint32_t* values,
                                                uint32_t lower, uint32_t width) {
#if defined(__AVX512F__) && defined(__AVX512BW__)
      const __m512i base = _mm512_set1_epi32((int)lower);
      const __m512i span = _mm512_set1_epi32((int)width);
      auto group = [&](int g) LUXIR_INLINE {
        __m512i v = _mm512_loadu_si512((const void*)(values + 16 * g));
        return _mm512_cmple_epu32_mask(_mm512_sub_epi32(v, base), span);
      };
      __mmask32 low = _mm512_kunpackw(group(1), group(0));
      __mmask32 high = _mm512_kunpackw(group(3), group(2));
      return (uint64_t)_mm512_kunpackd(high, low);
#elif defined(__AVX2__)
      // Biasing both sides by 2^31 turns the unsigned test into a signed one:
      // v - (lower ^ 2^31) equals (v - lower) ^ 2^31.
      const __m256i base = _mm256_set1_epi32((int)(lower ^ 0x80000000u));
      const __m256i span = _mm256_set1_epi32((int)(width ^ 0x80000000u));
      uint64_t outside = 0;
      for (int g = 0; g < 8; g++) {
        __m256i v = _mm256_sub_epi32(
            _mm256_loadu_si256((const __m256i*)(values + 8 * g)), base);
        outside |= (uint64_t)(uint32_t)_mm256_movemask_ps(
            _mm256_castsi256_ps(_mm256_cmpgt_epi32(v, span))) << (8 * g);
      }
      return ~outside;
#elif defined(__SSE2__)
      // The AVX2 bias, four lanes at a time.
      const __m128i base = _mm_set1_epi32((int)(lower ^ 0x80000000u));
      const __m128i span = _mm_set1_epi32((int)(width ^ 0x80000000u));
      uint64_t outside = 0;
      for (int g = 0; g < 16; g++) {
        __m128i v = _mm_sub_epi32(
            _mm_loadu_si128((const __m128i*)(values + 4 * g)), base);
        outside |= (uint64_t)(uint32_t)_mm_movemask_ps(
            _mm_castsi128_ps(_mm_cmpgt_epi32(v, span))) << (4 * g);
      }
      return ~outside;
#else
      uint64_t bits = 0;
      for (uint32_t i = 0; i < 64; i++) {
        bits |= (uint64_t)((uint32_t)(values[i] - lower) <= width) << i;
      }
      return bits;
#endif
    }

  public:
    explicit CrossingValues(IntColReader& reader) : reader(reader) {}

    // The length of the chunk at rank: to end, but within rank's 64-value
    // group.
    static uint32_t groupChunk(int64_t rank, int64_t end) {
      return (uint32_t)std::min<int64_t>(end - rank, 64 - (rank & 63));
    }

    // Bit i reports whether the value at rank + i matches, for the
    // 1 <= n <= 64 values of one CROSSES run in one 64-value group. A whole
    // group compares its 64 lanes at once; a shorter chunk compares only its
    // own values, so no lane the decoder did not write is read. Short chunks
    // occur only at run and window edges and raw values take matchesValue one
    // at a time, so both loops stay scalar: vectorized, they outweighed the
    // 64-lane kernel in the fill loops' code.
    LUXIR_INLINE uint64_t mask(int64_t rank, uint32_t n, const BlockPlan& plan,
                               const NumericPredicateQuery& query) {
      assert(n >= 1 && (uint32_t)(rank & 63) + n <= 64);
      uint64_t bits = 0;
      if (plan.residualComparable) {
        const uint32_t* values = residualsAt(rank);
        assert(rank + n <= residualStart + (int64_t)residualCount);
        if (n == 64) [[likely]] {
          return residualMask64(values, plan.residualLower, plan.residualWidth);
        }
#pragma GCC novector
        for (uint32_t i = 0; i < n; i++) {
          bits |= (uint64_t)((uint32_t)(values[i] - plan.residualLower)
                             <= plan.residualWidth) << i;
        }
        return bits;
      }
      const int64_t* values = rawAt(rank);
      assert(rank + n <= rawStart + (int64_t)rawCount);
#pragma GCC novector
      for (uint32_t i = 0; i < n; i++) {
        bits |= (uint64_t)query.matchesValue(values[i]) << i;
      }
      return bits;
    }

    // Whether any value at ranks [rank, end) of one CROSSES run matches,
    // stopping at the first that does. Scalar like mask()'s short chunks:
    // documents hold few values and often match early, and vectorized, the
    // scans' peeling and cleanup doubled the multivalued fills.
    bool any(int64_t rank, int64_t end, const BlockPlan& plan,
             const NumericPredicateQuery& query) {
      for (int64_t frameEnd; rank < end; rank = frameEnd) {
        if (plan.residualComparable) {
          const uint32_t* values = residualsAt(rank);
          frameEnd = std::min<int64_t>(end, residualStart + (int64_t)residualCount);
#pragma GCC novector
          for (int64_t r = rank; r < frameEnd; r++, values++) {
            if ((uint32_t)(*values - plan.residualLower) <= plan.residualWidth) {
              return true;
            }
          }
        } else {
          const int64_t* values = rawAt(rank);
          frameEnd = std::min<int64_t>(end, rawStart + (int64_t)rawCount);
#pragma GCC novector
          for (int64_t r = rank; r < frameEnd; r++, values++) {
            if (query.matchesValue(*values)) return true;
          }
        }
      }
      return false;
    }
  };

  // Whether any value at ranks [start, end) matches.
  static bool anyValueMatches(std::span<const BlockPlan> plans,
                              CrossingValues& crossing,
                              const NumericPredicateQuery& query,
                              int64_t start, int64_t end) {
    int64_t rank = start;
    while (rank < end) {
      int64_t blockStart = rank / IntColReader::BLOCK_SIZE
          * (int64_t)IntColReader::BLOCK_SIZE;
      const BlockPlan& plan =
          plans[(size_t)(blockStart / IntColReader::BLOCK_SIZE)];
      BlockPlan::Run run = plan.runAt((uint32_t)(rank - blockStart));
      int64_t runEnd = std::min<int64_t>(end, blockStart + run.end);
      if (run.relation == BlockRelation::INSIDE) return true;
      if (run.relation == BlockRelation::CROSSES
          && crossing.any(rank, runEnd, plan, query)) {
        return true;
      }
      rank = runEnd;
    }
    return false;
  }

  class ZoneMapScorer final : public Query::ConstantScorer {
  public:
    // Pull iteration refines one window of documents at a time.
    static constexpr int32_t ITER_WINDOW_SIZE = 4096;

  private:
    static constexpr int32_t ITER_WINDOW_WORDS = ITER_WINDOW_SIZE / 64;

    IntColReader& reader;
    BlockPlans& blockPlans;
    std::span<const BlockPlan> plans;
    // Sparse docsWithValue only. docs sits on the first document with a
    // value at or after docsTarget; fills step walk from rank to rank and
    // select only to jump past an OUTSIDE run.
    screaming::BitSet::Selector* selector = nullptr;
    screaming::BitSet::Iterator* docs = nullptr;
    screaming::BitSet::Iterator* walk = nullptr;
    int32_t docsTarget = -1;
    CrossingValues crossing;
    std::span<uint64_t> iterBits;
    const NumericPredicateQuery& query;
    int32_t maxDoc;
    int32_t docid = -1;
    int32_t iterWindowStart = 0;
    int32_t iterWindowEnd = 0;
    const int32_t initialMaxDoc;
    bool eager;

    static void setBit(std::span<uint64_t> words, int32_t windowStart,
                       int32_t doc) {
      int32_t index = doc - windowStart;
      words[(size_t)(index >> 6)] |= 1ULL << (index & 63);
    }

    // ORs bits [0, n) into words from window offset index on. Only a chunk
    // that crosses into the next word stores there, which also keeps both
    // shifts below 64.
    static void orBits(std::span<uint64_t> words, int32_t index,
                       uint64_t bits, uint32_t n) {
      uint32_t shift = (uint32_t)index & 63;
      size_t word = (size_t)(index >> 6);
      words[word] |= bits << shift;
      if (shift + n > 64) words[word + 1] |= bits >> (64 - shift);
    }

    static void setRun(std::span<uint64_t> words, int32_t windowStart,
                       int32_t start, int32_t end) {
      if (start >= end) return;
      int32_t first = start - windowStart;
      int32_t last = end - windowStart;
      int32_t firstWord = first >> 6;
      int32_t lastWord = (last - 1) >> 6;
      uint64_t firstMask = ~0ULL << (first & 63);
      uint64_t lastMask = ((last & 63) == 0)
          ? ~0ULL : ((1ULL << (last & 63)) - 1ULL);
      if (firstWord == lastWord) {
        words[(size_t)firstWord] |= firstMask & lastMask;
        return;
      }
      words[(size_t)firstWord] |= firstMask;
      for (int32_t w = firstWord + 1; w < lastWord; w++) {
        words[(size_t)w] = ~0ULL;
      }
      words[(size_t)lastWord] |= lastMask;
    }

    // The first document with a value at or after target, and its rank; past
    // the last one, {END, docsWithValue}. Sparse: from the docs cursor, which
    // a target below docsTarget rewinds.
    template <bool SPARSE>
    LUXIR_INLINE std::pair<int32_t, int32_t> firstDocRank(int32_t target) {
      if constexpr (SPARSE) {
        if (target < docsTarget) docs->reset();
        docsTarget = target;
        int32_t doc = target > docs->val() ? docs->advance(target) : docs->val();
        if (doc != screaming::BitSet::END) return {doc, docs->rank()};
      } else if (target < reader.docsWithValue()) {
        return {target, target};
      }
      return {PostingsReader::END, reader.docsWithValue()};
    }

    // Position walk on doc, a document with a value.
    void walkTo(int32_t doc) {
      if (doc < walk->val()) walk->reset();
      if (doc > walk->val()) walk->advance(doc);
    }

    // Out of line: GCC otherwise inlines next()'s bucket scans into the
    // sparse crossing loop, quadrupling fillSingle<true>.
    LUXIR_NOINLINE int32_t stepWalk() { return walk->next(); }

    // The document of rank, the rank after the one a fill just visited.
    template <bool SPARSE>
    LUXIR_INLINE int32_t nextDoc(int32_t rank) {
      if constexpr (SPARSE) {
        return stepWalk();
      } else {
        return rank;
      }
    }

    // The document of rank, past an OUTSIDE run. Sparse: a select; past max,
    // walk stays behind for the next window.
    template <bool SPARSE>
    LUXIR_INLINE int32_t jumpDoc(int32_t rank, int32_t max) {
      if constexpr (SPARSE) {
#ifndef NDEBUG
        rankSelectsForTests++;
#endif
        int32_t doc = selector->select(rank);
        if (doc < max) walk->advance(doc);
        return doc;
      } else {
        return rank;
      }
    }

    // Dense CROSSES ranks [rank, end), where doc == rank: one mask per
    // 64-value group, ORed in at the group's window offset.
    LUXIR_INLINE void fillCrossing(std::span<uint64_t> words, int32_t min,
                                   int32_t rank, int32_t end, const BlockPlan& plan) {
      while (rank < end) {
        uint32_t n = CrossingValues::groupChunk(rank, end);
#ifndef NDEBUG
        if (verificationObserver != nullptr) {
          for (int32_t doc = rank; doc < rank + (int32_t)n; doc++) {
            verificationObserver(query, doc);
          }
        }
#endif
        orBits(words, rank - min, crossing.mask(rank, n, plan, query), n);
        rank += (int32_t)n;
      }
    }

    // One source per loop, instantiated per docsWithValue density: dense
    // fills bound each run by the window once, then set INSIDE runs whole and
    // OR CROSSES runs in a mask per 64-value group; sparse fills step walk,
    // and in CROSSES runs read each value's bit from its group's mask.
    // Separate functions keep each loop's work inlined and its registers
    // independent of the other loop. Pin the start alignment: zone fills
    // dominate column-only range filters, and a 16-byte shift from unrelated
    // edits cost frange2 r01_old10 TOP_10 11-18% more cycles at identical
    // instruction counts (hugin, 2026-10-29).
    template <bool SPARSE>
    [[gnu::aligned(64)]] LUXIR_NOINLINE void fillSingle(std::span<uint64_t> words, int32_t min,
                                   int32_t max, int32_t doc, int32_t rank) {
      int64_t nvals = reader.numValues();
      while (doc < max && rank < nvals) {
        int64_t blockNum = rank / IntColReader::BLOCK_SIZE;
        int64_t blockStart = blockNum * (int64_t)IntColReader::BLOCK_SIZE;
        const BlockPlan& plan = plans[(size_t)blockNum];
        BlockPlan::Run run = plan.runAt((uint32_t)(rank - blockStart));
        int64_t runEnd = std::min<int64_t>(nvals, blockStart + run.end);
        if (run.relation == BlockRelation::OUTSIDE) {
          rank = (int32_t)runEnd;
          if (rank >= nvals) break;
          doc = jumpDoc<SPARSE>(rank, max);
          continue;
        }
        if constexpr (!SPARSE) {
          int32_t end = (int32_t)std::min<int64_t>(runEnd, max);
          if (run.relation == BlockRelation::INSIDE) setRun(words, min, rank, end);
          else fillCrossing(words, min, rank, end, plan);
          rank = end;
          doc = end;
          continue;
        }
        if (run.relation == BlockRelation::INSIDE) {
          while (rank < runEnd && doc < max) {
            setBit(words, min, doc);
            rank++;
            doc = nextDoc<SPARSE>(rank);
          }
          continue;
        }
        // max bounds documents, not ranks: a group's mask may cover values
        // past it, but walk consumes ranks only while doc < max.
        while (rank < runEnd && doc < max) {
          int32_t groupEnd = rank + (int32_t)CrossingValues::groupChunk(rank, runEnd);
          uint64_t bits = crossing.mask(rank, (uint32_t)(groupEnd - rank), plan, query);
          do {
#ifndef NDEBUG
            if (verificationObserver != nullptr) verificationObserver(query, doc);
#endif
            if ((bits & 1) != 0) setBit(words, min, doc);
            bits >>= 1;
            rank++;
            doc = nextDoc<SPARSE>(rank);
          } while (rank < groupEnd && doc < max);
        }
      }
    }

    template <bool SPARSE>
    LUXIR_NOINLINE void fillMulti(std::span<uint64_t> words, int32_t min,
                                  int32_t max, int32_t doc, int32_t docRank) {
      while (doc < max && docRank < reader.docsWithValue()) {
        auto [start, end] = reader.getStartEndValueRank(docRank);
#ifndef NDEBUG
        if (verificationObserver != nullptr) verificationObserver(query, doc);
#endif
        if (start < end
            && anyValueMatches(plans, crossing, query, start, end)) {
          setBit(words, min, doc);
        }
        docRank++;
        if (docRank >= reader.docsWithValue()) break;
        doc = nextDoc<SPARSE>(docRank);
      }
    }

    // Once per eager scorer. Out of line so both fill instantiations share one
    // call to all(): a call in each made GCC outline all() at every site,
    // which moved Supplier::exactCount's count loop across a line boundary
    // (range_col COUNT +6% at equal instructions, hugin, 2026-11-01).
    LUXIR_NOINLINE void classifyAllPlans() { plans = blockPlans.all(); }

    template <bool SPARSE>
    LUXIR_INLINE void fillWindow(std::span<uint64_t> words, int32_t min, int32_t max) {
      auto [doc, rank] = firstDocRank<SPARSE>(min);
      if (doc >= max) return;
      if (eager) {
        if (plans.empty()) classifyAllPlans();
      } else {
        int32_t lastRank = firstDocRank<SPARSE>(max).second;
        int64_t begin = rank;
        int64_t end = lastRank;
        if (reader.multiValued()) {
          begin = reader.getStartValueRank(rank);
          end = reader.getStartValueRank(lastRank);
        }
        if (begin == end) return;
        plans = blockPlans.forValues(begin, end);
      }
      if constexpr (SPARSE) walkTo(doc);
      if (reader.multiValued()) fillMulti<SPARSE>(words, min, max, doc, rank);
      else fillSingle<SPARSE>(words, min, max, doc, rank);
    }

    // Out of line, so fillWindowBits' dense entry carries no sparse code: an
    // entry shared at run time cost short dense fills 20% more cycles at
    // equal instructions, in branch mispredictions (frange_ts_old50 COUNT,
    // hugin, 2026-10-31).
    LUXIR_NOINLINE void fillSparseWindow(std::span<uint64_t> words, int32_t min,
                                         int32_t max) {
      fillWindow<true>(words, min, max);
    }

    int32_t findInIterationWindow(int32_t target) const {
      if (target < iterWindowStart || target >= iterWindowEnd) {
        return PostingsReader::END;
      }
      int32_t index = target - iterWindowStart;
      int32_t word = index >> 6;
      uint64_t bits = iterBits[(size_t)word] & (~0ULL << (index & 63));
      while (true) {
        if (bits != 0) {
          int32_t found = iterWindowStart + (word << 6)
              + (int32_t)std::countr_zero(bits);
          return found < iterWindowEnd ? found : PostingsReader::END;
        }
        word++;
        if (word >= ITER_WINDOW_WORDS
            || iterWindowStart + (word << 6) >= iterWindowEnd) {
          return PostingsReader::END;
        }
        bits = iterBits[(size_t)word];
      }
    }

    int32_t seek(int32_t target) {
      int32_t found = findInIterationWindow(target);
      if (found != PostingsReader::END) return docid = found;
      while (target < maxDoc) {
        iterWindowStart = target;
        iterWindowEnd = (int32_t)std::min<int64_t>(maxDoc,
            (int64_t)target + ITER_WINDOW_SIZE);
        std::fill(iterBits.begin(), iterBits.end(), 0);
        fillWindowBits(iterBits, iterWindowStart, iterWindowEnd);
        found = findInIterationWindow(target);
        if (found != PostingsReader::END) return docid = found;
        target = iterWindowEnd;
      }
      return docid = PostingsReader::END;
    }

  public:
    [[gnu::cold]] void reset() override {
      docid = -1;
      maxDoc = initialMaxDoc;
      iterWindowStart = iterWindowEnd = 0;
      if (docs != nullptr) {
        docs->reset();
        walk->reset();
      }
      docsTarget = -1;
    }

    ZoneMapScorer(MemPool& pool, IntColReader& reader,
                  BlockPlans& blockPlans,
                  const NumericPredicateQuery& query,
                  int32_t maxDoc, float constantScore, bool eager)
      : Query::ConstantScorer(constantScore), reader(reader), blockPlans(blockPlans),
          crossing(reader),
          iterBits(pool.make_arr<uint64_t>(ITER_WINDOW_WORDS), ITER_WINDOW_WORDS),
          query(query), maxDoc(maxDoc), initialMaxDoc(maxDoc), eager(eager) {
      if (!reader.denseDocsWithValue()) {
        const auto& bits = reader.docsWithValueBitSet();
        selector = pool.make<screaming::BitSet::Selector>(
            bits, pool.make_span<int32_t>((size_t)bits.nBuckets + 1));
        docs = pool.make<screaming::BitSet::Iterator>(bits);
        walk = pool.make<screaming::BitSet::Iterator>(bits);
      }
      std::fill(iterBits.begin(), iterBits.end(), 0);
    }

    void fillScoredWindowBits(std::span<uint64_t> words,
                              int32_t start, int32_t end) override {
      fillWindowBits(words, start, end);
    }

    void fillWindowBits(std::span<uint64_t> words, int32_t min,
                        int32_t max) override {
      assert(min >= 0 && min <= max && max <= maxDoc);
      if (min == max) return;
      if (selector == nullptr) fillWindow<false>(words, min, max);
      else fillSparseWindow(words, min, max);
    }

    int32_t next() override {
      assert(docid != PostingsReader::END);
      return seek(docid + 1);
    }
    int32_t advance(int32_t target) override {
      assert(docid < target);
      return seek(target);
    }
    int32_t docId() override { return docid; }

    int32_t advanceWindow(int32_t min, int32_t max) override {
      if (min >= max) return max;
      if (min >= maxDoc) return docid = PostingsReader::END;
      int32_t limit = std::min(max, maxDoc);
      int32_t found = findInIterationWindow(min);
      if (found < limit) return docid = found;
      // The cached window is complete from its start; continue after it
      // only when it covers min. Fill no further than the caller's bound,
      // clearing just the words that bound uses: point probes stay O(1).
      int32_t target = min >= iterWindowStart && min < iterWindowEnd
          ? iterWindowEnd : min;
      while (target < limit) {
        iterWindowStart = target;
        iterWindowEnd = (int32_t)std::min<int64_t>(limit,
            (int64_t)target + ITER_WINDOW_SIZE);
        std::fill_n(iterBits.begin(),
                    (iterWindowEnd - iterWindowStart + 63) >> 6, 0);
        fillWindowBits(iterBits, iterWindowStart, iterWindowEnd);
        found = findInIterationWindow(target);
        if (found < limit) return docid = found;
        target = iterWindowEnd;
      }
      return docid = limit == maxDoc ? PostingsReader::END : max;
    }

  protected:
    // The current iteration window is behind docid, so emptying the doc
    // range is enough; no per-call test on the seek path.
    void exhaust() override {
      maxDoc = 0;
      iterWindowStart = 0;
      iterWindowEnd = 0;
    }
  };

  class PredicateBulkScorer final : public BulkScorer {
    static constexpr int32_t WINDOW_SIZE = 4096;
    static constexpr int32_t WINDOW_WORDS = WINDOW_SIZE / 64;

    ZoneMapScorer* scorer;
    std::span<uint64_t> windowBits;
    std::span<int32_t> outDocs;
    std::span<float> outScores;
    int32_t maxDoc;
    int32_t windowStart = 0;
    int32_t windowEnd = 0;
    float constantScore;

    static uint64_t validMask(int32_t remaining) {
      if (remaining >= 64) return ~0ULL;
      if (remaining <= 0) return 0;
      return (1ULL << remaining) - 1ULL;
    }

    void setWindowBounds(int32_t min, int32_t max) {
      windowStart = min;
      windowEnd = (int32_t)std::min<int64_t>(
          std::min(maxDoc, max), (int64_t)min + WINDOW_SIZE);
    }

    void applyBitFilter(const FixedBitSet& filter) {
      int32_t sourceWords = (int32_t)FixedBitSet::sizeInWords(filter.size());
      for (int32_t w = 0; w < WINDOW_WORDS; w++) {
        int32_t firstDoc = windowStart + (w << 6);
        int32_t remaining = windowEnd - firstDoc;
        if (remaining <= 0) {
          windowBits[(size_t)w] = 0;
          continue;
        }
        int32_t sourceWord = firstDoc >> 6;
        int32_t shift = firstDoc & 63;
        uint64_t source = 0;
        if (sourceWord < sourceWords) {
          source = filter.words[sourceWord] >> shift;
          if (shift != 0 && sourceWord + 1 < sourceWords) {
            source |= filter.words[sourceWord + 1] << (64 - shift);
          }
        }
        windowBits[(size_t)w] &= source & validMask(remaining);
      }
    }

    void applyFilter(DocSet* filter) {
      if (filter == nullptr) return;
      if (filter->type == DocSet::BITSET) {
        applyBitFilter(((BitDocSet*)filter)->bits());
        return;
      }
      int32_t nbits = windowEnd - windowStart;
      for (int32_t w = 0; w < WINDOW_WORDS; w++) {
        uint64_t bits = windowBits[(size_t)w];
        while (bits != 0) {
          int32_t bit = (int32_t)std::countr_zero(bits);
          int32_t index = (w << 6) + bit;
          if (index >= nbits) break;
          if (!filter->get(windowStart + index)) {
            windowBits[(size_t)w] &= ~(1ULL << bit);
          }
          bits &= bits - 1;
        }
      }
    }

    int32_t windowCardinality() const {
      int32_t count = 0;
      for (uint64_t bits : windowBits) count += (int32_t)std::popcount(bits);
      return count;
    }

    void fillWindow(DocSet* filter, int32_t min, int32_t max) {
      setWindowBounds(min, max);
      std::fill(windowBits.begin(), windowBits.end(), 0);
      scorer->fillWindowBits(windowBits, windowStart, windowEnd);
      applyFilter(filter);
    }

  public:
    [[gnu::cold]] void reposition(int32_t target) override {
      unused(target);
      assert(target >= 0 && target <= maxDoc);
      scorer->reset();
      windowStart = windowEnd = 0;
    }

    PredicateBulkScorer(MemPool& pool, ZoneMapScorer* scorer, int32_t maxDoc,
                    float constantScore)
        : scorer(scorer),
          windowBits(pool.make_arr<uint64_t>(WINDOW_WORDS), WINDOW_WORDS),
          outDocs(pool.make_arr<int32_t>(WINDOW_SIZE), WINDOW_SIZE),
          outScores(pool.make_arr<float>(WINDOW_SIZE), WINDOW_SIZE),
          maxDoc(maxDoc), constantScore(constantScore) {}

    // Bounded windows of exact matches: the default matchNextWindow serves
    // them, as the bulk plan declares.
    bool supportsMatchWindows() const override { return true; }

    int32_t countNextWindow(int64_t& count, DocSetBuilder* domainOut,
                            DocSet* filter, int32_t min, int32_t max) override {
      max = std::min(max, maxDoc);
      if (min >= max || (filter != nullptr && filter->card() == 0)) {
        return PostingsReader::END;
      }
      fillWindow(filter, min, max);
      int32_t wordCard = windowCardinality();
      if (domainOut != nullptr && wordCard != 0) {
        skipCount(SkipStats::bulkDomainWindowsFed);
        domainOut->addWindowWords(
            windowBits.data(), windowStart, windowEnd, wordCard);
      }
      count += wordCard;
      return windowEnd >= max ? PostingsReader::END : windowEnd;
    }

    int32_t scoreNextWindow(ScoreWindow& out, DocSet* filter, int32_t min,
                            int32_t max, float minCompetitiveScore) override {
      max = std::min(max, maxDoc);
      out.min = min;
      out.max = min;
      out.size = 0;
      out.docs = outDocs;
      out.scores = outScores;
      if (min >= max || (filter != nullptr && filter->card() == 0)) {
        return PostingsReader::END;
      }
      fillWindow(filter, min, max);
      out.min = windowStart;
      out.max = windowEnd;
      if (minCompetitiveScore <= constantScore) {
        int32_t nbits = windowEnd - windowStart;
        for (int32_t w = 0; w < WINDOW_WORDS; w++) {
          uint64_t bits = windowBits[(size_t)w];
          while (bits != 0) {
            int32_t bit = (int32_t)std::countr_zero(bits);
            int32_t index = (w << 6) + bit;
            if (index >= nbits) break;
            outDocs[(size_t)out.size] = windowStart + index;
            outScores[(size_t)out.size] = constantScore;
            out.size++;
            bits &= bits - 1;
          }
        }
      }
      return windowEnd >= max ? PostingsReader::END : windowEnd;
    }
  };

  using PointsArrayScorer = PointsMaterialize::PointsArrayScorer;
  using PointsBitScorer = PointsMaterialize::PointsBitScorer;
  using PointsArrayBulkScorer = PointsMaterialize::PointsArrayBulkScorer;
  using PointsBitBulkScorer = PointsMaterialize::PointsBitBulkScorer;
  class Weight final : public Query::Weight {
    NumericPredicateQuery& query;
    IndexReader::FieldSegments segInfos;
    float constantScore;

    static size_t fillPointRanges(
        PointsReader& points, const NumericPredicateQuery& query,
        std::span<PointsReader::OrdinalRange> output) {
      MemPool scratchPool;
      auto residuals = scratchPool.make_span<uint32_t>(
          points.maxPointsPerLeaf());
      auto values = scratchPool.make_span<int64_t>(
          points.maxPointsPerLeaf());
      return points.ordinalRanges(
          query.intervals(), output, residuals, values);
    }

    static uint64_t pointCount(
        std::span<const PointsReader::OrdinalRange> ranges) {
      uint64_t count = 0;
      for (const auto& range : ranges) count += range.end - range.begin;
      return count;
    }

    const PostingsReader::Field* segmentInfo(IndexReader::Segment& segment) const {
      auto* info = segInfos.at(segment.ord);
      return info && info->columnLoc.offset() > 0 ? info : nullptr;
    }

  public:
    Weight(Context& context, NumericPredicateQuery& query, int32_t flags,
           float constantScore)
        : Query::Weight(context, query, flags), query(query),
          constantScore(constantScore) {
      traits |= IS_CONSTANT_SCORING;
      segInfos = context.topReader.field(query.getField());
    }

    // Bounds answer covering and disjoint ranges without opening points.
    // Partial single-valued points counts resolve at supplier construction;
    // multi-valued and points-less counts walk the column only at execution.
    CountShortcut countShortcut(IndexReader::Segment& segment, DocSet* domain) override {
      if (query.getHi() < query.getLo()) return {0, CountHow::METADATA};
      const auto* info = segmentInfo(segment);
      if (!info) return {0, CountHow::METADATA};
      if (!info->columnBounds) {
        assert(false && "numeric column has no bounds");
        return {countBound(segment, domain, info->docsWithField).maxMatches, CountHow::ITERATE};
      }
      if (info->numValues == 0
          || !query.intersects(info->columnBounds->min, info->columnBounds->max)) {
        return {0, CountHow::METADATA};
      }
      CountHow how = query.covers(info->columnBounds->min, info->columnBounds->max)
          ? CountHow::METADATA
          : info->pointsMetaOff != 0 && info->monoLoc.offset() == 0
              ? CountHow::SUPPLIER : CountHow::ITERATE;
      auto shortcut = countBound(segment, domain, info->docsWithField, how);
      if (shortcut.how == CountHow::SUPPLIER) shortcut.resolver = this;
      return shortcut;
    }

    class Supplier final : public Query::ScorerSupplier {
      // Crossing values cost about 2x a dense scan. Require at least
      // half the values to be structurally prunable before using zone maps.
      // Measured with gcc-release on 2026-07-10 on a hybrid-core laptop, the
      // least representative hardware described by the tuning caveat.
      static constexpr int64_t ZONE_MAP_MIN_PRUNABLE_FRACTION_DENOMINATOR = 2;

      using Materialized = PointsMaterialize::Materialized;

      enum class ScorerArm : uint8_t {
        SPARSE_VERIFY,
        POINTS,
        ZONE_MAP,
        SCAN,
      };

      NumericPredicateQuery::Weight& weight;
      MemPool& planPool;
      IndexReader::Segment& segment;
      IntColReader& reader;
      PointsReader* points;
      BlockPlans blockPlans;
      std::span<const PointsReader::OrdinalRange> pointRanges;
      uint64_t pointValueCount;
      int64_t estimatedCost;
      Query::MatchState matchState;
      bool allMatch;
      struct ColumnCosts {
        bool useZones;
        bool hasOutside;
        int32_t possibleDocs;
        int32_t insideDocs;
        // Zone production over the whole column: `checks` counts the values
        // tested in unprunable blocks and `docVisits` the documents whose
        // value ranges a multivalued column combines; `inside` is already in
        // points, the emission of the inside runs (whole mask words for dense
        // single-valued columns, values otherwise).
        double checks;
        double inside;
        double docVisits;
      };
      mutable std::optional<ColumnCosts> columnCosts;

      ColumnCosts priceColumn(NumColumnFormat::LeafMeta::RangeSummary summary) const {
        int64_t prunable = summary.outside + summary.insideLower;
        int64_t denominator = ZONE_MAP_MIN_PRUNABLE_FRACTION_DENOMINATOR;
        bool single = !reader.multiValued();
        // Only dense single-valued INSIDE runs can emit whole mask words.
        // Sparse columns map each rank to a doc; multivalued producers also
        // visit documents whose values are entirely outside the predicate.
        double insideWords = single && reader.denseDocsWithValue()
            ? (double)((summary.insideLower + 63) / 64) : -1;
        return {
          prunable >= reader.numValues() / denominator
              + (reader.numValues() % denominator != 0),
          summary.outside != 0,
          reader.docsWithValue() - (single ? (int32_t)summary.outside : 0),
          single ? (int32_t)summary.insideLower : 0,
          (double)(reader.numValues() - prunable),
          insideWords >= 0 ? INSIDE_WORD_POINTS * insideWords
              : INSIDE_VALUE_POINTS * (double)summary.insideLower,
          single ? 0.0 : (double)reader.docsWithValue(),
        };
      }

      const ColumnCosts& column() const {
        // Points already supply exact cost/nonemptiness. Scalar COUNT needs
        // neither a column summary nor executing block plans.
        if (!columnCosts) {
          auto summary = reader.leafMeta().summarizeRange(
              weight.query.getLo(), weight.query.getHi());
          if (weight.query.isExactSet() && !allMatch) {
            summary.insideLower = summary.insideUpper = 0;
          }
          columnCosts = priceColumn(summary);
        }
        return *columnCosts;
      }

      // Prices in point emissions (see Query::Demand); points production and
      // verification are PointsMaterialize's. Block classification was
      // measured by forcing each arm under a 35-candidate pull lead (5M docs,
      // one segment, 2026-10-28). The rest are fitted on that board's
      // forced-arm oracle: a scan tests a value in about 5 ns; zone checks and
      // inside words are priced per unit of the column summary, which a dense
      // lead's refills can reach about twice.
      static constexpr double CLASSIFY_POINTS = 25;       // classify one column block
      static constexpr double CHECK_POINTS = 4;           // test one value of an unprunable block
      static constexpr double INSIDE_WORD_POINTS = 3;     // emit one dense inside mask word
      static constexpr double INSIDE_VALUE_POINTS = 1;    // emit one inside value's document
      static constexpr double DOC_VISIT_POINTS = CHECK_POINTS;  // combine one document's values
      static constexpr double SCAN_POINTS = 6;            // test one value of a scanned column

      struct Production {
        ScorerArm arm;
        double points;
      };

      double pointsPrice() const {
        bool complement = useComplement(pointValueCount);
        return PointsMaterialize::price(
            complement ? points->pointCount() - pointValueCount : pointValueCount,
            segment.maxDoc(), complement);
      }

      // Fractions of the column a consumer makes a producer refine (zones),
      // classify (zone blocks) and test (scan).
      struct Reach {
        double zones;
        double scan;
        double classified;
      };

      // A single-valued column's OUTSIDE leaves are docid runs no match can
      // occupy; the hull is the rest of the docid space. Its share of the
      // documents with a value estimates its share of every docid, because a
      // sibling's documents fall in those runs whether or not they have a
      // value. EITHER's proof factor (possibleDocs over maxDoc) is a different
      // fraction: the candidates inside the hull that also have a value.
      double hull() const {
        const auto& costs = column();
        return !reader.multiValued() && costs.hasOutside
            ? (double)costs.possibleDocs / std::max(1, reader.docsWithValue()) : 1.0;
      }

      // A window consumer fills `span` documents.
      Reach windowReach(int64_t span) const {
        double fraction = std::clamp(
            (double)span / std::max(1, segment.maxDoc()), 0.0, 1.0);
        return {fraction, fraction, fraction};
      }

      // A pull consumer's candidates each land in one zone iteration window,
      // and a scan advanced to a candidate tests values up to the next match.
      // A candidate ahead of the next zone match walks the windows up to it,
      // classifying the blocks of the OUTSIDE runs between.
      Reach pullReach(int64_t candidates) const {
        double count = (double)std::max<int64_t>(candidates, 0);
        double maxDoc = (double)std::max(1, segment.maxDoc());
        double zones = std::min(1.0, count * ZoneMapScorer::ITER_WINDOW_SIZE / maxDoc);
        return {zones, std::min(1.0, count / std::max(1.0, estimatedMatches())),
                std::max(zones, 1 - hull())};
      }

      // The cheapest producer under `reach`. Points materialize every match;
      // zones classify the column's blocks (all of them when built for the
      // whole segment) and refine what is reached; a scan tests the values
      // it passes.
      Production cheapestProducer(Reach reach, int64_t producerSpan) const {
        const auto& costs = column();
        std::optional<Production> best;
        if (points != nullptr) best = Production{ScorerArm::POINTS, pointsPrice()};
        if (costs.useZones) {
          double classified = producerSpan >= segment.maxDoc() ? 1.0 : reach.classified;
          double zones = CLASSIFY_POINTS * (double)reader.numBlocks() * classified
              + reach.zones * (CHECK_POINTS * costs.checks + costs.inside
                  + DOC_VISIT_POINTS * costs.docVisits);
          if (!best || zones < best->points) best = Production{ScorerArm::ZONE_MAP, zones};
        }
        if (!best) {
          best = Production{ScorerArm::SCAN,
              SCAN_POINTS * reach.scan * (double)reader.numValues()};
        }
        return *best;
      }

      double estimatedMatches() const {
        double matches = (double)estimatedCost;
        const auto& sketch = reader.leafMeta().valueSketch();
        if (points == nullptr && !sketch.empty()) {
          matches = 0;
          for (auto range : weight.query.intervals()) {
            double end = range.hi == INT64_MAX ? (double)reader.numValues()
                : sketch.countBelow(range.hi + 1);
            matches += end - sketch.countBelow(range.lo);
          }
          matches = std::min(matches, (double)reader.docsWithValue());
        }
        return matches;
      }

      struct Selection {
        ScorerArm arm;
        Query::MembershipConsumption consumption;
      };

      Materialized materializePoints(MemPool& pool) {
        assert(points != nullptr);
        uint64_t expected = pointValueCount;
        auto docScratch = pool.make_span<uint32_t>(points->maxPointsPerLeaf());
        if (PointsMaterialize::useBitset(expected, segment.maxDoc())) {
          auto words = pool.make_span<uint64_t>(
              FixedBitSet::sizeInWords(segment.maxDoc()));
          std::fill(words.begin(), words.end(), 0);
          FixedBitSet bits(words.data(), segment.maxDoc());
          for (const auto& range : pointRanges) {
            points->emitOrdinalRange(range.begin, range.end, docScratch,
                [&bits](int32_t doc) { bits.set(doc); },
                [&bits](int32_t runBegin, int32_t runEnd) {
                  PointsMaterialize::setRun(bits, runBegin, runEnd);
                },
                [&bits](int32_t wordIndex, uint64_t word) {
                  bits.words[wordIndex] |= word;
                });
          }
          return {{}, words.data()};
        }

        auto docs = pool.make_span<int32_t>((size_t)expected);
        size_t size = 0;
        for (const auto& range : pointRanges) {
          points->emitOrdinalRange(range.begin, range.end, docScratch,
              [&docs, &size](int32_t doc) { docs[size++] = doc; },
              [&docs, &size](int32_t runBegin, int32_t runEnd) {
                for (int32_t doc = runBegin; doc < runEnd; doc++) {
                  docs[size++] = doc;
                }
              },
              [&docs, &size](int32_t wordIndex, uint64_t word) {
                while (word != 0) {
                  int32_t bit = (int32_t)std::countr_zero(word);
                  docs[size++] = wordIndex * 64 + bit;
                  word &= word - 1;
                }
              });
        }
        assert(size == expected);
        boost::sort::spreadsort::integer_sort(docs.begin(), docs.end());
        if (reader.multiValued()) {
          size = (size_t)(std::unique(docs.begin(), docs.end()) - docs.begin());
        }
        return {docs.first(size), nullptr};
      }

      Materialized materializeComplement(MemPool& pool) {
        assert(points != nullptr && !reader.multiValued());
        auto words = pool.make_span<uint64_t>(
            FixedBitSet::sizeInWords(segment.maxDoc()));
        std::fill(words.begin(), words.end(), 0);
        FixedBitSet bits(words.data(), segment.maxDoc());
        if (reader.denseDocsWithValue()) {
          PointsMaterialize::setRun(bits, 0, segment.maxDoc());
        } else {
          screaming::BitSet::Iterator iter(reader.docsWithValueBitSet());
          for (int32_t doc = iter.next(); doc != screaming::BitSet::END;
               doc = iter.next()) {
            bits.set(doc);
          }
        }

        auto docScratch = pool.make_span<uint32_t>(points->maxPointsPerLeaf());
        auto clearDoc = [&bits](int32_t doc) { bits.clear(doc); };
        auto clearDocRun = [&bits](int32_t runBegin, int32_t runEnd) {
          PointsMaterialize::clearRun(bits, runBegin, runEnd);
        };
        auto clearWord = [&bits](int32_t wordIndex, uint64_t word) {
          bits.words[wordIndex] &= ~word;
        };
        uint64_t cursor = 0;
        for (const auto& range : pointRanges) {
          points->emitOrdinalRange(
              cursor, range.begin, docScratch,
              clearDoc, clearDocRun, clearWord);
          cursor = range.end;
        }
        points->emitOrdinalRange(
            cursor, points->pointCount(), docScratch,
            clearDoc, clearDocRun, clearWord);
        return {{}, words.data()};
      }

      Query::Scorer* scorerFor(MemPool& pool, const Materialized& result) {
        return PointsMaterialize::scorerFor(
            pool, result, segment.maxDoc(), weight.constantScore);
      }

      BulkScorer* bulkFor(MemPool& pool, const Materialized& result) {
        return PointsMaterialize::bulkFor(
            pool, result, segment.maxDoc(), weight.constantScore);
      }

      BulkScorer* zoneBulkScorer(MemPool& pool, bool eager) {
        assert(column().useZones);
        skipCount(SkipStats::numericPredicateZoneArms);
        auto* scorer = pool.make<ZoneMapScorer>(
            pool, reader, blockPlans, weight.query,
            segment.maxDoc(), weight.constantScore, eager);
        return pool.make<PredicateBulkScorer>(
            pool, scorer, segment.maxDoc(), weight.constantScore);
      }

      // Complement is a wash against direct materialization when the range's
      // docids are clustered (both emit word-masked runs) and only clearly
      // wins on scattered docids above ~75% selectivity; below that the
      // outside-tail decode dominates. Sparse fields pay an unpriced
      // base-bitset build, so the threshold rounds up. Measured with
      // gcc-release on 2026-07-10 on a hybrid-core laptop per the tuning
      // caveat.
      bool useComplement(uint64_t exactCount) const {
        return points != nullptr && !reader.multiValued()
            && exactCount > (uint64_t)reader.docsWithValue() * 3 / 4;
      }

      // Candidates a single-valued verifier can face: no more than the docs
      // that can match.
      double verifiableCandidates(int64_t candidates) const {
        return reader.multiValued() ? (double)candidates
            : (double)std::min<int64_t>(candidates, column().possibleDocs);
      }

      // EITHER verifies the survivors of the entries ahead, whose proof skips
      // the docs that cannot match: only survivors inside the hull with a value
      // face a check. An unknown yield verifies every candidate. The
      // consumer's spared work is what producing instead would save it.
      double eitherVerifyPoints(const Query::Demand& demand) const {
        const auto& costs = column();
        double verified = verifiableCandidates(demand.candidates);
        if (demand.expectedCandidates >= 0) {
          verified = std::min(demand.expectedCandidates, (double)demand.candidates);
          if (!reader.multiValued() && costs.hasOutside) {
            verified *= costs.possibleDocs / (double)std::max(1, segment.maxDoc());
          }
        }
        return PointsMaterialize::verifyPrice(verified, demand);
      }

      // Verify each candidate or produce the range: one comparison in point
      // emissions at every consumption site.
      Selection selectScorer(const Query::PlanContext& context) const {
        using Consumption = Query::MembershipConsumption;
        const auto& demand = context.demand;
        if (context.membershipConsumption == Consumption::CANDIDATES) {
          // Zones' per-candidate protocol is not priced; only points produce.
          int64_t candidates = demand.expectedCandidates >= 0
              ? std::min(demand.candidates, (int64_t)std::ceil(demand.expectedCandidates))
              : demand.candidates;
          bool produce = points != nullptr && pointsPrice()
              < PointsMaterialize::verifyPrice(verifiableCandidates(candidates), demand);
          return {produce ? ScorerArm::POINTS : ScorerArm::SPARSE_VERIFY,
                  Consumption::CANDIDATES};
        }
        if (context.membershipConsumption == Consumption::WINDOWS) {
          // An explicit exposure fills windows; any other window consumer
          // pulls the candidates of its lead, or those the consumer expects
          // to reach this clause.
          bool windows = fillsExposedWindows(context);
          Production producer = cheapestProducer(
              windows ? windowReach(context.expectedSpan) : pullReach(demand.span),
              context.expectedSpan);
          // A pull verifier sees only the candidates its approximation, the
          // documents with a value, lands on.
          double candidates = windows ? (double)demand.candidates
              : demand.expectedCandidates >= 0
                  ? std::min(demand.expectedCandidates, (double)reader.docsWithValue())
                  : (double)demand.span;
          double verify = PointsMaterialize::verifyPrice(candidates, demand);
          return {verify <= producer.points ? ScorerArm::SPARSE_VERIFY : producer.arm,
                  Consumption::WINDOWS};
        }
        Production producer = cheapestProducer(windowReach(demand.span), demand.span);
        if (eitherVerifyPoints(demand) <= producer.points) {
          return {ScorerArm::SPARSE_VERIFY, Consumption::CANDIDATES};
        }
        return {producer.arm, Consumption::WINDOWS};
      }

      static Query::ScorerShape shapeFor(
          Selection selection, Query::MatchState matchState, bool pricedCheck) {
        ScorerArm arm = selection.arm;
        bool twoPhase = arm == ScorerArm::SPARSE_VERIFY;
        return {
          .matchState = matchState,
          .directKind = Query::DirectScorerKind::OTHER,
          .reportedTwoPhase = twoPhase
              ? Query::ReportedTwoPhase::YES
              : Query::ReportedTwoPhase::NO,
          .windowFillClause = Query::ClauseShape::DIRECT,
          .termDisjunctionClause = Query::ClauseShape::NONE,
          .termConjunctionClause = Query::ClauseShape::NONE,
          .independentTerm = Query::IndependentTermAccess::UNSUPPORTED,
          .docsOnly = Query::DocsOnlyAccess::UNSUPPORTED,
          .directDocSet = Query::DirectDocSetAccess::UNSUPPORTED,
          .membershipConsumption = selection.consumption,
          // The sparse verifier's check of one value against one interval, the
          // case VERIFY_POINTS was fitted on; other predicates do not quote.
          .verificationPoints = twoPhase && pricedCheck
              ? std::optional<double>(PointsMaterialize::VERIFY_POINTS) : std::nullopt,
        };
      }

      Query::Scorer* buildScorer(
          MemPool& targetPool, ScorerArm arm, bool complement, int64_t expectedSpan) {
        switch (arm) {
          case ScorerArm::SPARSE_VERIFY:
            skipCount(SkipStats::numericPredicateSparseVerifyArms);
            return targetPool.make<
                PredicateScorer<IntColReader::SparseIterator>>(
                    reader, weight.query,
                    allMatch, weight.constantScore);
          case ScorerArm::POINTS:
            if (complement) {
              skipCount(SkipStats::numericPredicateComplementArms);
              return scorerFor(targetPool,
                  materializeComplement(targetPool));
            }
            skipCount(SkipStats::numericPredicatePointsArms);
            return scorerFor(targetPool,
                materializePoints(targetPool));
          case ScorerArm::ZONE_MAP:
            skipCount(SkipStats::numericPredicateZoneArms);
            return targetPool.make<ZoneMapScorer>(
                targetPool, reader, blockPlans, weight.query,
                segment.maxDoc(), weight.constantScore, expectedSpan >= segment.maxDoc());
          case ScorerArm::SCAN:
            skipCount(SkipStats::numericPredicateScanArms);
            return targetPool.make<PredicateScorer<IntColReader::Iterator>>(
                reader, weight.query, allMatch,
                weight.constantScore);
        }
        std::unreachable();
      }

      class Plan final : public Query::ScorerPlan {
        Supplier& supplier;
        ScorerArm arm;
        bool complement;

      protected:
        Query::Scorer* buildScorer(MemPool& targetPool) override {
          // A lead's demand describes candidate economics, not a stopping
          // consumer. Only explicit exposure justifies incremental setup.
          return supplier.buildScorer(targetPool, arm, complement, context().expectedSpan);
        }

      public:
        Query::MembershipProof* buildMembershipProof(MemPool& targetPool) override {
          // Materialized points already provide exact membership; do not
          // attach a second, column-wide classification/index to that arm.
          if (arm == ScorerArm::POINTS || supplier.reader.multiValued()
              || !supplier.column().hasOutside) return nullptr;
          // A full span alone can describe a handful of scattered candidates.
          // Reserve whole-column refinement for expected broad block coverage.
          bool fullIndex = context().expectedSpan >= supplier.segment.maxDoc()
              && context().demand.span >= supplier.segment.maxDoc()
              && context().demand.candidates >= supplier.reader.numLeafZones();
          return targetPool.make<ColumnProof>(
              targetPool, supplier.reader, supplier.weight.query,
              supplier.blockPlans, fullIndex);
        }

        [[gnu::cold]] bool supportsReset() const noexcept override {
          return unscoredResetUse() && !supplier.weight.needsScores();
        }

        Plan(Supplier& supplier, const Query::PlanContext& planContext,
             const Query::ScorerShape& shape, int64_t cost, ScorerArm arm,
             bool complement)
          : Query::ScorerPlan(planContext, shape, cost),
            supplier(supplier), arm(arm), complement(complement) {}
      };

      struct BulkState final : Query::ScorerSupplier::BulkBuildState {
        enum class Arm : uint8_t {
          POINTS,
          ZONE_MAP,
        };

        Arm arm;
        bool complement = false;
        bool eager = true;
        BulkUse use;

        BulkState(const Supplier* owner, Arm arm, BulkUse use)
          : arm(arm), use(use) {
          this->owner = owner;
        }
      };

      int64_t exactCount() {
        if (points != nullptr && !reader.multiValued()) {
          return (int64_t)pointValueCount;
        }
        auto plans = blockPlans.all();
        CrossingValues crossing(reader);
        if (!reader.multiValued()) {
          int64_t count = 0;
          for (int64_t blockNum = 0;
               blockNum < reader.numBlocks(); blockNum++) {
            const BlockPlan& plan = plans[(size_t)blockNum];
            int64_t start =
                blockNum * (int64_t)IntColReader::BLOCK_SIZE;
            plan.forEachRun(
                (uint32_t)reader.valuesInBlock(blockNum),
                [&](BlockRelation relation, uint32_t begin, uint32_t end) {
                  if (relation == BlockRelation::INSIDE) {
                    count += end - begin;
                  } else if (relation == BlockRelation::CROSSES) {
                    // One stored value per document, sparse or dense.
                    for (int64_t rank = start + begin; rank < start + end;) {
                      uint32_t n = CrossingValues::groupChunk(rank, start + end);
                      count += std::popcount(
                          crossing.mask(rank, n, plan, weight.query));
                      rank += n;
                    }
                  }
                });
          }
          return count;
        }

        int64_t count = 0;
        for (int32_t docRank = 0;
             docRank < reader.docsWithValue(); docRank++) {
          auto [start, end] = reader.getStartEndValueRank(docRank);
          count += anyValueMatches(plans, crossing, weight.query, start, end);
        }
        return count;
      }

    public:
      Supplier(NumericPredicateQuery::Weight& weight, MemPool& planPool,
               IndexReader::Segment& segment, IntColReader& reader,
               std::optional<NumColumnFormat::LeafMeta::RangeSummary> summary,
               int64_t estimatedCost, Query::MatchState matchState,
               bool allMatch, PointsReader* points,
               std::span<const PointsReader::OrdinalRange> pointRanges,
               uint64_t pointValueCount)
          : weight(weight), planPool(planPool), segment(segment), reader(reader),
            points(points), blockPlans(planPool, reader, weight.query),
            pointRanges(pointRanges), pointValueCount(pointValueCount),
            estimatedCost(estimatedCost), matchState(matchState), allMatch(allMatch) {
        if (summary) columnCosts = priceColumn(*summary);
      }

      int64_t cost() override { return estimatedCost; }

      // Whether a sparse verifier's check is the calibrated one: a single
      // value against one interval.
      bool pricedCheck() const {
        return !reader.multiValued() && !weight.query.isExactSet()
            && weight.query.intervals().size() == 1;
      }

      MembershipWork membershipWork(int32_t maxDoc) override {
        unused(maxDoc);
        // A sparse verifier can touch every value in its requested span.
        // Price that work independently of the values that pass. The sketch
        // refines yield without changing cost()'s half-crossing heuristic.
        // Points may still resolve to sparse verification under a parent's
        // bounded demand. Only a resolved materialized supplier can claim the
        // cheaper retained-set price; this prior must cover either arm.
        double production = 64.0 * reader.numValues();
        // A dense single-valued column's leaf summary places the matches'
        // docids; sparse columns' value-less documents need not spread evenly.
        return {estimatedMatches(), production, 0, 64, 192, 0, true, hull(),
                !reader.multiValued() && reader.docsWithValue() == segment.maxDoc()};
      }

      std::optional<MembershipQuote> quoteMembership(
          const Query::PlanContext& context) const override {
        using Consumption = Query::MembershipConsumption;
        if (!isMembershipWindowUse(context.demand.horizon)
            || context.numericPredicateDisableShapesForTests
            || points != nullptr || reader.multiValued()
            || !reader.denseDocsWithValue() || reader.docsWithValue() != segment.maxDoc()
            || weight.query.isExactSet() || weight.query.intervals().size() != 1
            || reader.leafMeta().valueSketch().empty()) return std::nullopt;
        const auto& costs = column();
        int32_t possibleDocs = costs.possibleDocs;
        int32_t insideDocs = costs.insideDocs;
        if (context.membershipConsumption == Consumption::WINDOWS) {
          if (costs.useZones || selectScorer(context).arm != ScorerArm::SCAN) return std::nullopt;
          double matches = std::clamp(estimatedMatches(), (double)insideDocs, (double)possibleDocs);
          return MembershipQuote{matches, 24.0 * reader.numValues() + 9.0 * matches};
        }
        if (context.membershipConsumption != Consumption::CANDIDATES) return std::nullopt;
        double candidates = (double)std::clamp<int64_t>(context.demand.candidates, 0, possibleDocs);
        if (candidates == 0) return MembershipQuote{0, 0};
        double matches = std::clamp(estimatedMatches(), (double)insideDocs, (double)possibleDocs);
        double words = std::ceil((double)std::clamp<int64_t>(context.demand.span, 0, segment.maxDoc()) / 64);
        // Measured branchless single-value kernels. Bound occupied words by
        // the candidate count rather than assuming uniformly spaced probes.
        double occupied = std::min(candidates, words);
        return MembershipQuote{candidates * matches / possibleDocs,
            56.0 * candidates + 8.0 * (words - occupied) + 21.0 * occupied};
      }

      Query::ScorerShape describeScorer(
          const Query::PlanContext& buildContext) const override {
        if (buildContext.numericPredicateDisableShapesForTests) return {};
        return shapeFor(selectScorer(buildContext), matchState, pricedCheck());
      }

      Query::UnresolvedSupplierCause unresolvedScorerCause(
          const Query::PlanContext& buildContext) const override {
        return buildContext.numericPredicateDisableShapesForTests
            ? Query::UnresolvedSupplierCause::NUMERIC_GEO
            : Query::UnresolvedSupplierCause::NONE;
      }

      Query::Scorer* createPointsScorerForTests(MemPool& targetPool) {
        if (points == nullptr) return nullptr;
        return scorerFor(targetPool, materializePoints(targetPool));
      }

      Query::Scorer* createComplementScorerForTests(MemPool& targetPool) {
        if (points == nullptr || reader.multiValued()) return nullptr;
        return scorerFor(targetPool, materializeComplement(targetPool));
      }

      Query::ScorerPlan* resolve(
          MemPool& planPool,
          const Query::PlanContext& planContext) override {
        Selection selection = selectScorer(planContext);
        ScorerArm arm = selection.arm;
        bool complement = arm == ScorerArm::POINTS
            && useComplement(pointValueCount);
        // A verifier iterates every document with a value.
        int64_t iterated = arm == ScorerArm::SPARSE_VERIFY
            ? std::max(cost(), (int64_t)reader.docsWithValue()) : cost();
        return planPool.make<Plan>(
            *this, planContext, shapeFor(selection, matchState, pricedCheck()), iterated, arm,
            complement);
      }

      Query::PlanContext makePlanContext(
          const Query::Demand& demand) const override {
        Query::PlanContext context;
        context.demand = demand;
        context.numericPredicateDisableShapesForTests =
            NumericPredicateQuery::disableShapesForTests;
        return context;
      }

      BulkPlan planBulk(
          BulkUse use, const BulkScorerContext& bulkContext) override {
        unused(use);
        if (bulkContext.requireConstantCount) {
          auto shortcut = weight.countShortcut(segment, nullptr);
          if (shortcut.how == CountHow::METADATA) {
            return {BulkAnswer::YES, BulkAnswer::NO, BulkAnswer::NO,
                    BulkAnswer::NO, nullptr, shortcut.maxMatches};
          }
          if (shortcut.how == CountHow::SUPPLIER) {
            assert(points != nullptr && !reader.multiValued());
            return {BulkAnswer::YES, BulkAnswer::NO, BulkAnswer::NO,
                    BulkAnswer::NO, nullptr, (int64_t)pointValueCount};
          }
          if (segment.hasDeletes()) {
            return {
              BulkAnswer::NO, BulkAnswer::NO, BulkAnswer::NO,
              BulkAnswer::NO,
            };
          }
          return {
            BulkAnswer::YES, BulkAnswer::NO, BulkAnswer::NO,
            BulkAnswer::NO, nullptr, exactCount(),
          };
        }
        if (bulkContext.requireFilterConsumption) {
          return {
            BulkAnswer::NO, BulkAnswer::NO, BulkAnswer::NO,
            BulkAnswer::NO,
          };
        }
        int64_t span = bulkContext.expectedConsumption.span;
        Production producer = cheapestProducer(windowReach(span), span);
        if (producer.arm == ScorerArm::POINTS || producer.arm == ScorerArm::ZONE_MAP) {
          auto* state = planPool.make<BulkState>(this,
              producer.arm == ScorerArm::POINTS ? BulkState::Arm::POINTS : BulkState::Arm::ZONE_MAP,
              use);
          state->eager = bulkContext.expectedConsumption.span >= segment.maxDoc();
          state->complement = producer.arm == ScorerArm::POINTS && useComplement(pointValueCount);
          return {
            BulkAnswer::YES, BulkAnswer::YES, BulkAnswer::NO,
            BulkAnswer::NO, state, -1, WindowFilterConsumption::NO,
          };
        }
        return {
          BulkAnswer::NO, BulkAnswer::NO, BulkAnswer::NO,
          BulkAnswer::NO,
        };
      }

      [[gnu::cold]] bool bulkStateSupportsReposition(const BulkBuildState& state) const noexcept override {
        assert(state.owner == this);
        const auto& plan = static_cast<const BulkState&>(state);
        return !weight.needsScores() && isMembershipWindowUse(plan.use);
      }

      BulkScorer* buildBulk(
          MemPool& targetPool, const BulkPlan& plan) override {
        assert(plan.available == BulkAnswer::YES);
        assert(!plan.hasConstantCount());
        assert(plan.buildState != nullptr);
        assert(plan.buildState->owner == this);
        const auto& state =
            *static_cast<const BulkState*>(plan.buildState);
        if (state.arm == BulkState::Arm::ZONE_MAP) {
          return zoneBulkScorer(targetPool, state.eager);
        }
        assert(points != nullptr);
        if (state.complement) {
          skipCount(SkipStats::numericPredicateComplementArms);
          return bulkFor(targetPool, materializeComplement(targetPool));
        }
        skipCount(SkipStats::numericPredicatePointsArms);
        return bulkFor(targetPool, materializePoints(targetPool));
      }

    };

    Query::ScorerSupplier* scorerSupplierImpl(
        MemPool& targetPool, IndexReader::Segment& segment,
        Query::SupplierExecutionMode executionMode) override {
      unused(executionMode);
      if (query.getHi() < query.getLo()) return nullptr;
      const auto* segInfo = segmentInfo(segment);
      if (!segInfo) return nullptr;
      auto* reader = targetPool.make<IntColReader>(segment.postingsReader(), *segInfo);
      if (reader->numValues() == 0) return nullptr;
      int64_t colMin = reader->getMin();
      int64_t colMax = reader->getMax();
      if (colMax < query.getLo() || query.getHi() < colMin) return nullptr;

      std::optional<NumColumnFormat::LeafMeta::RangeSummary> summary;
      bool allMatch = query.covers(colMin, colMax);
      int64_t cost = 0;
      PointsReader* points = nullptr;
      std::span<const PointsReader::OrdinalRange> pointRanges;
      uint64_t pointValueCount = 0;
      Query::MatchState matchState = Query::MatchState::UNKNOWN;
      if (segInfo->pointsMetaOff != 0) {
        points = targetPool.make<PointsReader>(segment.postingsReader(), *segInfo);
        if (points->pointCount() != (uint64_t)reader->numValues()) {
          throw std::runtime_error("NumericPredicateQuery: points/column value count mismatch");
        }
        auto storedRanges = targetPool.make_span<PointsReader::OrdinalRange>(
            query.intervals().size());
        size_t rangeCount = fillPointRanges(*points, query, storedRanges);
        pointRanges = storedRanges.first(rangeCount);
        pointValueCount = pointCount(pointRanges);
        uint64_t docCost = pointValueCount;
        if (reader->multiValued()) {
          // Repeated docids make this a safe upper bound, not an exact count
          // of unique matching documents.
          docCost = std::min<uint64_t>(docCost, reader->docsWithValue());
        }
        cost = (int64_t)docCost;
        matchState = pointValueCount == 0
            ? Query::MatchState::EMPTY : Query::MatchState::NONEMPTY;
      } else {
        summary = reader->leafMeta().summarizeRange(query.getLo(), query.getHi());
        // An exact set's envelope proves rejection, never inclusion in gaps.
        if (query.isExactSet() && !allMatch) {
          summary->insideLower = summary->insideUpper = 0;
        }
        int64_t possible = reader->numValues() - summary->outside;
        int64_t estimate = query.isExactSet() ? possible
            : summary->insideLower + (possible - summary->insideLower + 1) / 2;
        cost = std::min<int64_t>(estimate, reader->docsWithValue());
        matchState = possible == 0 ? Query::MatchState::EMPTY
            : summary->insideLower > 0 ? Query::MatchState::NONEMPTY
                                     : Query::MatchState::UNKNOWN;
      }
      return targetPool.make<Supplier>(*this, targetPool, segment, *reader,
                                       summary, cost, matchState,
                                       allMatch, points, pointRanges,
                                       pointValueCount);
    }

    Query::Scorer* createScorer(MemPool& targetPool,
                                IndexReader::Segment& segment) override {
      auto* supplier = scorerSupplier(targetPool, segment);
      if (supplier == nullptr) return nullptr;
      Query::Demand demand = Query::Demand::fromLeadCost(
          std::numeric_limits<int64_t>::max());
      return supplier->resolve(
          targetPool, supplier->makePlanContext(demand))->build(targetPool);
    }

    Query::Scorer* createPointsScorerForTests(MemPool& targetPool,
                                              IndexReader::Segment& segment) {
      auto* supplier = static_cast<Supplier*>(scorerSupplier(targetPool, segment));
      return supplier == nullptr ? nullptr
          : supplier->createPointsScorerForTests(targetPool);
    }

    Query::Scorer* createComplementScorerForTests(
        MemPool& targetPool, IndexReader::Segment& segment) {
      auto* supplier = static_cast<Supplier*>(scorerSupplier(targetPool, segment));
      return supplier == nullptr ? nullptr
          : supplier->createComplementScorerForTests(targetPool);
    }

    // Test/benchmark baseline: the exact pre-change full block-decode scan.
    Query::Scorer* createFullScanScorerForTests(MemPool& targetPool,
                                                IndexReader::Segment& segment) {
      const auto* segInfo = segmentInfo(segment);
      if (!segInfo) return nullptr;
      auto* reader = targetPool.make<IntColReader>(segment.postingsReader(), *segInfo);
      if (reader->numValues() == 0) return nullptr;
      int64_t colMin = reader->getMin();
      int64_t colMax = reader->getMax();
      if (query.getHi() < query.getLo() || colMax < query.getLo()
          || query.getHi() < colMin) {
        return nullptr;
      }
      bool allMatch = query.covers(colMin, colMax);
      return targetPool.make<PredicateScorer<IntColReader::Iterator>>(
          *reader, query, allMatch, constantScore);
    }

    Query::Scorer* createZoneMapScorerForTests(MemPool& targetPool,
                                               IndexReader::Segment& segment,
                                               bool eager = true) {
      const auto* segInfo = segmentInfo(segment);
      if (!segInfo) return nullptr;
      auto* reader = targetPool.make<IntColReader>(segment.postingsReader(), *segInfo);
      if (reader->numValues() == 0) return nullptr;
      if (query.getHi() < query.getLo() || reader->getMax() < query.getLo()
          || query.getHi() < reader->getMin()) {
        return nullptr;
      }
      auto* plans = targetPool.make<BlockPlans>(targetPool, *reader, query);
      return targetPool.make<ZoneMapScorer>(
          targetPool, *reader, *plans, query,
          segment.maxDoc(), constantScore, eager);
    }

  };
};

} // namespace luxir
