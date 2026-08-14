#pragma once

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <limits>
#include <span>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <vector>

#include "luxir/query/Query.h"
#include "luxir/query/PointsMaterialize.h"
#include "luxir/reader/IntColReader.h"
#include "luxir/reader/PointsReader.h"

namespace luxir {

// Numeric range query over the doc-order numeric column. Every numeric field
// type stores an order-preserving encoded int64 (INT raw, FLOAT/DOUBLE Lucene
// sortable bits, DATE epoch millis), so all pruning and comparisons happen in
// encoded space. A document matches when any value is in inclusive [lo, hi].
class NumericRangeQuery final : public Query {
  std::string_view field;
  int64_t lo;
  int64_t hi;

public:
  static inline bool disableShapesForTests = false;

  NumericRangeQuery(std::string_view field, int64_t lo, int64_t hi)
    : field(field), lo(lo), hi(hi) {}

  std::string_view getField() const { return field; }
  int64_t getLo() const { return lo; }
  int64_t getHi() const { return hi; }
  ScoreProfile scoreProfile() const override {
    return ScoreProfile::automatic(1.0f);
  }

  FilterKeyScope appendFilterKey(FilterKeyBuilder& out,
                                 const FilterKeyContext& ctx) const override {
    unused(ctx);
    out.appendTag(FilterKeyTag::NUMERIC_RANGE);
    out.appendString(field);
    out.appendInt64(lo);
    out.appendInt64(hi);
    return FilterKeyScope::SEGMENT_STABLE;
  }

  Query::Weight* createWeight(Context& context, int32_t flags,
                              float multiplier = 1.0f) override {
    float score = constantWhenScored(flags, multiplier);
    return context.pool.make<Weight>(context, *this, flags, score);
  }

  // RangeScorer is the sparse two-phase verifier and, when instantiated with
  // IntColReader::Iterator, the benchmark's full column-scan baseline.
  template <class ColIter>
  class RangeScorer final : public Query::ConstantScorer {
    IntColReader& reader;
    ColIter iter;
    int64_t lo;
    int64_t hi;
    bool allMatch;
    bool multi;
    int32_t docid = -1;

    bool valueInRange() {
      if (docid == PostingsReader::END) return false;
      if (!multi) {
        if (allMatch) return true;
        int64_t v = iter.value();
        return lo <= v && v <= hi;
      }
      auto [start, end] = reader.getStartEndValueRank(iter.rank());
      if (allMatch) return start < end;
      for (int64_t r = start; r < end; r++) {
        int64_t v = iter.values().valueAt(r);
        if (lo <= v && v <= hi) return true;
      }
      return false;
    }

  public:
    RangeScorer(IntColReader& reader, int64_t lo, int64_t hi, bool allMatch,
                float constantScore)
      : Query::ConstantScorer(constantScore), reader(reader), iter(reader),
        lo(lo), hi(hi), allMatch(allMatch), multi(reader.multiValued()) {}

    int32_t approximationNext() override {
      docid = iter.next();
      return docid;
    }
    int32_t approximationAdvance(int32_t target) override {
      docid = iter.advance(target);
      return docid;
    }
    int32_t approximationDocId() override { return docid; }
    bool matches() override { return valueInRange(); }
    float matchCost() override {
      if (allMatch) return 0.0f;
      if (!multi) return 1.0f;
      int64_t docs = reader.docsWithValue();
      return docs > 0 ? (float)reader.numValues() / (float)docs : 1.0f;
    }

    int32_t next() override {
      for (;;) {
        docid = iter.next();
        if (docid == PostingsReader::END || valueInRange()) return docid;
      }
    }
    int32_t advance(int32_t target) override {
      assert(docid < target);
      docid = iter.advance(target);
      while (docid != PostingsReader::END && !valueInRange()) {
        docid = iter.next();
      }
      return docid;
    }
    int32_t docId() override { return docid; }
  };

  enum class BlockRelation : uint8_t {
    OUTSIDE,
    INSIDE,
    CROSSES
  };

  struct BlockPlan {
    BlockRelation relation = BlockRelation::OUTSIDE;
    uint32_t lowerResidual = 0;
    uint32_t upperResidual = 0;
  };

  static BlockPlan classifyBlock(const IntColReader::NumericBlockInfo& block,
                                 const NumBlockZone& zone,
                                 int64_t lo, int64_t hi) {
    BlockPlan plan;
    if (zone.max < lo || hi < zone.min) {
      return plan;
    }
    if (lo <= zone.min && zone.max <= hi) {
      plan.relation = BlockRelation::INSIDE;
      return plan;
    }

    plan.relation = BlockRelation::CROSSES;
    if (block.bits() > 32 || block.scaledSlope != 0) {
      return plan;
    }

    // CROSSES guarantees every subtraction below represents a non-negative
    // signed-order distance. Unsigned subtraction makes the full INT64 span
    // exact, and quotient+remainder implements ceil without overflow.
    uint64_t gcd = block.gcd;
    assert(gcd != 0);
    uint64_t lower = 0;
    if (lo > zone.min) {
      uint64_t delta = (uint64_t)lo - (uint64_t)zone.min;
      lower = delta / gcd + (delta % gcd != 0);
    }
    uint64_t upper;
    if (hi >= zone.max) {
      upper = ((uint64_t)zone.max - (uint64_t)zone.min) / gcd;
    } else {
      upper = ((uint64_t)hi - (uint64_t)zone.min) / gcd;
    }
    // A compressed block's maximum persisted residual fits its format<=32.
    // Saturation keeps a defensive release build correct at both signed
    // extremes even if future metadata admits a wider residual.
    plan.lowerResidual = lower > UINT32_MAX ? UINT32_MAX : (uint32_t)lower;
    plan.upperResidual = upper > UINT32_MAX ? UINT32_MAX : (uint32_t)upper;
    return plan;
  }

  class CrossingValues {
    IntColReader& reader;
    int64_t residualStart = -1;
    int64_t rawStart = -1;
    uint32_t residualCount = 0;
    uint32_t rawCount = 0;
    uint32_t residuals[128];
    int64_t raw[128];

  public:
    explicit CrossingValues(IntColReader& reader) : reader(reader) {}

    bool matches(int64_t valueRank, const BlockPlan& plan, int64_t lo, int64_t hi) {
      int64_t blockNum = valueRank / IntColReader::BLOCK_SIZE;
      auto block = reader.blockInfo(blockNum);
      if (block.bits() <= 32 && block.scaledSlope == 0) {
        if (valueRank < residualStart
            || valueRank >= residualStart + (int64_t)residualCount) {
          residualStart = reader.decodeResidualSubBlock(valueRank, residuals,
                                                        residualCount);
        }
        uint32_t v = residuals[valueRank - residualStart];
        return plan.lowerResidual <= v && v <= plan.upperResidual;
      }
      if (valueRank < rawStart || valueRank >= rawStart + (int64_t)rawCount) {
        rawStart = reader.decodeValueSubBlock(valueRank, raw, rawCount);
      }
      int64_t v = raw[valueRank - rawStart];
      return lo <= v && v <= hi;
    }
  };

  class ZoneMapScorer final : public Query::ConstantScorer {
    static constexpr int32_t ITER_WINDOW_SIZE = 4096;
    static constexpr int32_t ITER_WINDOW_WORDS = ITER_WINDOW_SIZE / 64;

    IntColReader& reader;
    std::span<const BlockPlan> plans;
    screaming::BitSet::Selector* selector = nullptr;
    CrossingValues crossing;
    std::span<uint64_t> iterBits;
    int64_t lo;
    int64_t hi;
    int32_t maxDoc;
    int32_t docid = -1;
    int32_t iterWindowStart = 0;
    int32_t iterWindowEnd = 0;

    static void setBit(std::span<uint64_t> words, int32_t windowStart,
                       int32_t doc) {
      int32_t index = doc - windowStart;
      words[(size_t)(index >> 6)] |= 1ULL << (index & 63);
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

    int32_t docForRank(int32_t rank) const {
      return selector == nullptr ? rank : selector->select(rank);
    }

    std::pair<int32_t, int32_t> firstDocRank(int32_t min) const {
      if (reader.denseDocsWithValue()) {
        if (min >= reader.docsWithValue()) return {PostingsReader::END, 0};
        return {min, min};
      }
      screaming::BitSet::Iterator iter(reader.docsWithValueBitSet());
      int32_t doc = iter.advance(min);
      if (doc == screaming::BitSet::END) return {PostingsReader::END, 0};
      return {doc, iter.rank()};
    }

    void fillSingle(std::span<uint64_t> words, int32_t min, int32_t max) {
      auto [doc, rank] = firstDocRank(min);
      int64_t nvals = reader.numValues();
      while (doc < max && rank < nvals) {
        int64_t blockNum = rank / IntColReader::BLOCK_SIZE;
        int64_t blockEnd = std::min<int64_t>(nvals,
            (blockNum + 1) * (int64_t)IntColReader::BLOCK_SIZE);
        const BlockPlan& plan = plans[(size_t)blockNum];
        if (plan.relation == BlockRelation::OUTSIDE) {
          rank = (int32_t)blockEnd;
          if (rank >= nvals) break;
          doc = docForRank(rank);
          continue;
        }
        if (plan.relation == BlockRelation::INSIDE
            && reader.denseDocsWithValue()) {
          int32_t runEnd = (int32_t)std::min<int64_t>(blockEnd, max);
          setRun(words, min, doc, runEnd);
          rank = runEnd;
          doc = runEnd;
          continue;
        }

        int64_t limit = blockEnd;
        while (rank < limit) {
          doc = docForRank(rank);
          if (doc >= max) return;
          if (plan.relation == BlockRelation::INSIDE
              || crossing.matches(rank, plan, lo, hi)) {
            setBit(words, min, doc);
          }
          rank++;
        }
        if (rank < nvals) doc = docForRank(rank);
      }
    }

    bool docValuesMatch(int64_t start, int64_t end) {
      int64_t rank = start;
      while (rank < end) {
        int64_t blockNum = rank / IntColReader::BLOCK_SIZE;
        int64_t blockEnd = std::min<int64_t>(end,
            (blockNum + 1) * (int64_t)IntColReader::BLOCK_SIZE);
        const BlockPlan& plan = plans[(size_t)blockNum];
        if (plan.relation == BlockRelation::INSIDE) return true;
        if (plan.relation == BlockRelation::CROSSES) {
          while (rank < blockEnd) {
            if (crossing.matches(rank, plan, lo, hi)) return true;
            rank++;
          }
        } else {
          rank = blockEnd;
        }
      }
      return false;
    }

    void fillMulti(std::span<uint64_t> words, int32_t min, int32_t max) {
      auto [doc, docRank] = firstDocRank(min);
      while (doc < max && docRank < reader.docsWithValue()) {
        auto [start, end] = reader.getStartEndValueRank(docRank);
        if (start < end && docValuesMatch(start, end)) {
          setBit(words, min, doc);
        }
        docRank++;
        if (docRank >= reader.docsWithValue()) break;
        doc = docForRank(docRank);
      }
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
    ZoneMapScorer(MemPool& pool, IntColReader& reader,
                  std::span<const BlockPlan> plans, int64_t lo, int64_t hi,
                  int32_t maxDoc, float constantScore)
        : Query::ConstantScorer(constantScore), reader(reader), plans(plans),
          crossing(reader),
          iterBits(pool.make_arr<uint64_t>(ITER_WINDOW_WORDS), ITER_WINDOW_WORDS),
          lo(lo), hi(hi), maxDoc(maxDoc) {
      if (!reader.denseDocsWithValue()) {
        const auto& bits = reader.docsWithValueBitSet();
        selector = pool.make<screaming::BitSet::Selector>(
            bits, pool.make_span<int32_t>((size_t)bits.nBuckets + 1));
      }
      std::fill(iterBits.begin(), iterBits.end(), 0);
    }

    void fillWindowBits(std::span<uint64_t> words, int32_t min,
                        int32_t max) override {
      assert(min >= 0 && min <= max && max <= maxDoc);
      if (reader.multiValued()) {
        fillMulti(words, min, max);
      } else {
        fillSingle(words, min, max);
      }
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

  protected:
    // The current iteration window is behind docid, so emptying the doc
    // range is enough; no per-call test on the seek path.
    void exhaust() override {
      maxDoc = 0;
      iterWindowStart = 0;
      iterWindowEnd = 0;
    }
  };

  class RangeBulkScorer final : public BulkScorer {
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
    RangeBulkScorer(MemPool& pool, ZoneMapScorer* scorer, int32_t maxDoc,
                    float constantScore)
        : scorer(scorer),
          windowBits(pool.make_arr<uint64_t>(WINDOW_WORDS), WINDOW_WORDS),
          outDocs(pool.make_arr<int32_t>(WINDOW_SIZE), WINDOW_SIZE),
          outScores(pool.make_arr<float>(WINDOW_SIZE), WINDOW_SIZE),
          maxDoc(maxDoc), constantScore(constantScore) {}

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
    NumericRangeQuery& query;
    std::span<SegFieldInfo*> segInfos;
    float constantScore;

    static int64_t estimateCost(IntColReader& reader,
                                std::span<const BlockPlan> plans) {
      int64_t estimate = 0;
      for (int64_t blockNum = 0; blockNum < reader.numBlocks(); blockNum++) {
        int64_t count = reader.valuesInBlock(blockNum);
        switch (plans[(size_t)blockNum].relation) {
          case BlockRelation::INSIDE:
            estimate += count;
            break;
          case BlockRelation::CROSSES:
            estimate += (count + 1) / 2;
            break;
          case BlockRelation::OUTSIDE:
            break;
        }
      }
      return std::min<int64_t>(estimate, reader.docsWithValue());
    }

    static std::pair<uint64_t, uint64_t> exactPointPositions(
        PointsReader& points, const PointsReader::FenceRange& fence,
        int64_t lo, int64_t hi, std::span<uint32_t> residualScratch,
        std::span<int64_t> rawScratch) {
      auto first = points.valueBounds(fence.firstLeaf, lo, hi,
                                      residualScratch, rawScratch);
      uint64_t loPos = points.leafOrdinalStart(fence.firstLeaf) + first.lower;
      if (fence.firstLeaf == fence.lastLeaf) {
        return {loPos, points.leafOrdinalStart(fence.firstLeaf) + first.upper};
      }
      auto last = points.valueBounds(fence.lastLeaf, lo, hi,
                                     residualScratch, rawScratch);
      return {loPos, points.leafOrdinalStart(fence.lastLeaf) + last.upper};
    }

    bool segmentInfo(IndexReader::Segment& segment, SegFieldInfo*& segInfo) const {
      if (segInfos.empty()) return false;
      segInfo = segInfos[segment.ord];
      return segInfo != nullptr && segInfo->columnLoc.offset() > 0;
    }

  public:
    Weight(Context& context, NumericRangeQuery& query, int32_t flags,
           float constantScore)
        : Query::Weight(context, flags), query(query),
          constantScore(constantScore) {
      traits |= IS_CONSTANT_SCORING;
      segInfos = context.readSegInfos(query.getField());
    }

    class Supplier final : public Query::ScorerSupplier {
      // Crossing values cost about 2x a dense scan. Require at least
      // half the values to be structurally prunable before using zone maps.
      // Measured with gcc-release on 2026-07-10 on a hybrid-core laptop, the
      // least representative hardware described by the tuning caveat.
      static constexpr int64_t ZONE_MAP_MIN_PRUNABLE_FRACTION_DENOMINATOR = 2;
      // The direct points arm beat the best scan arm at every measured
      // selectivity and field shape. Measured with gcc-release on 2026-07-10
      // on a hybrid-core laptop per the tuning caveat.

      using Materialized = PointsMaterialize::Materialized;

      enum class ScorerArm : uint8_t {
        SPARSE_VERIFY,
        POINTS,
        ZONE_MAP,
        SCAN,
      };

      NumericRangeQuery::Weight& weight;
      MemPool& planPool;
      IndexReader::Segment& segment;
      IntColReader& reader;
      PointsReader* points;
      std::span<const BlockPlan> plans;
      PointsReader::FenceRange fence;
      int64_t estimatedCost;
      bool allMatch;
      bool useZoneMap;
      bool exactReady = false;
      uint64_t loPos = 0;
      uint64_t hiPos = 0;

      std::pair<uint64_t, uint64_t> exactPositions(MemPool& pool) {
        assert(points != nullptr);
        if (!exactReady) {
          auto scratchGuard = pool.rewindScopeGuard();
          auto residuals = pool.make_span<uint32_t>(points->maxPointsPerLeaf());
          auto raw = pool.make_span<int64_t>(points->maxPointsPerLeaf());
          std::tie(loPos, hiPos) = Weight::exactPointPositions(
              *points, fence, weight.query.getLo(), weight.query.getHi(),
              residuals, raw);
          exactReady = true;
        }
        return {loPos, hiPos};
      }

      Materialized materializePoints(MemPool& pool, uint64_t begin,
                                     uint64_t end) {
        assert(points != nullptr && begin <= end);
        uint64_t expected = end - begin;
        auto docScratch = pool.make_span<uint32_t>(points->maxPointsPerLeaf());
        if (PointsMaterialize::useBitset(expected, segment.maxDoc())) {
          auto words = pool.make_span<uint64_t>(
              FixedBitSet::sizeInWords(segment.maxDoc()));
          std::fill(words.begin(), words.end(), 0);
          FixedBitSet bits(words.data(), segment.maxDoc());
          points->emitOrdinalRange(begin, end, docScratch,
              [&bits](int32_t doc) { bits.set(doc); },
              [&bits](int32_t runBegin, int32_t runEnd) {
                PointsMaterialize::setRun(bits, runBegin, runEnd);
              },
              [&bits](int32_t wordIndex, uint64_t word) {
                bits.words[wordIndex] |= word;
              });
          return {{}, words.data()};
        }

        auto docs = pool.make_span<int32_t>((size_t)expected);
        size_t size = 0;
        points->emitOrdinalRange(begin, end, docScratch,
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
        assert(size == expected);
        boost::sort::spreadsort::integer_sort(docs.begin(), docs.end());
        if (reader.multiValued()) {
          size = (size_t)(std::unique(docs.begin(), docs.end()) - docs.begin());
        }
        return {docs.first(size), nullptr};
      }

      Materialized materializeComplement(MemPool& pool, uint64_t begin,
                                         uint64_t end) {
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
        points->emitOrdinalRange(
            0, begin, docScratch, clearDoc, clearDocRun, clearWord);
        points->emitOrdinalRange(end, points->pointCount(), docScratch,
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

      BulkScorer* phaseOneBulkScorer(MemPool& pool) {
        if (!useZoneMap) return nullptr;
        skipCount(SkipStats::numericRangeZoneArms);
        auto* scorer = pool.make<ZoneMapScorer>(
            pool, reader, plans, weight.query.getLo(), weight.query.getHi(),
            segment.maxDoc(), weight.constantScore);
        return pool.make<RangeBulkScorer>(
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

      ScorerArm selectScorerArm(const Query::Demand& demand) const {
        // Sparse verification pays for the consumer's possible fill span;
        // materialize once that span can cross the range's estimate fence.
        if (demand.span < estimatedCost) {
          return ScorerArm::SPARSE_VERIFY;
        }
        if (points != nullptr) return ScorerArm::POINTS;
        return useZoneMap ? ScorerArm::ZONE_MAP : ScorerArm::SCAN;
      }

      static Query::ScorerShape shapeFor(ScorerArm arm) {
        bool twoPhase = arm == ScorerArm::SPARSE_VERIFY
            || arm == ScorerArm::SCAN;
        return {
          .matchState = Query::MatchState::NONEMPTY,
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
        };
      }

      Query::Scorer* buildScorer(
          MemPool& targetPool, ScorerArm arm, uint64_t begin,
          uint64_t end, bool complement) {
        switch (arm) {
          case ScorerArm::SPARSE_VERIFY:
            skipCount(SkipStats::numericRangeSparseVerifyArms);
            return targetPool.make<
                RangeScorer<IntColReader::SparseIterator>>(
                    reader, weight.query.getLo(), weight.query.getHi(),
                    allMatch, weight.constantScore);
          case ScorerArm::POINTS:
            if (complement) {
              skipCount(SkipStats::numericRangeComplementArms);
              return scorerFor(targetPool,
                  materializeComplement(targetPool, begin, end));
            }
            skipCount(SkipStats::numericRangePointsArms);
            return scorerFor(targetPool,
                materializePoints(targetPool, begin, end));
          case ScorerArm::ZONE_MAP:
            skipCount(SkipStats::numericRangeZoneArms);
            return targetPool.make<ZoneMapScorer>(
                targetPool, reader, plans, weight.query.getLo(),
                weight.query.getHi(), segment.maxDoc(), weight.constantScore);
          case ScorerArm::SCAN:
            return targetPool.make<RangeScorer<IntColReader::Iterator>>(
                reader, weight.query.getLo(), weight.query.getHi(), allMatch,
                weight.constantScore);
        }
        std::unreachable();
      }

      class Plan final : public Query::ScorerPlan {
        Supplier& supplier;
        ScorerArm arm;
        uint64_t loPos;
        uint64_t hiPos;
        bool complement;

      protected:
        Query::Scorer* buildScorer(MemPool& targetPool) override {
          return supplier.buildScorer(
              targetPool, arm, loPos, hiPos, complement);
        }

      public:
        Plan(Supplier& supplier, const Query::PlanContext& planContext,
             const Query::ScorerShape& shape, int64_t cost, ScorerArm arm,
             uint64_t loPos, uint64_t hiPos, bool complement)
          : Query::ScorerPlan(planContext, shape, cost),
            supplier(supplier), arm(arm), loPos(loPos), hiPos(hiPos),
            complement(complement) {}
      };

      struct BulkState final : Query::ScorerSupplier::BulkBuildState {
        enum class Arm : uint8_t {
          POINTS,
          ZONE_MAP,
        };

        Arm arm;
        uint64_t begin = 0;
        uint64_t end = 0;
        bool complement = false;

        BulkState(const Supplier* owner, Arm arm)
          : arm(arm) {
          this->owner = owner;
        }
      };

      int64_t exactCount() {
        if (points != nullptr && !reader.multiValued()) {
          auto [begin, end] = exactPositions(planPool);
          return (int64_t)(end - begin);
        }
        CrossingValues crossing(reader);
        if (!reader.multiValued()) {
          int64_t count = 0;
          for (int64_t blockNum = 0;
               blockNum < reader.numBlocks(); blockNum++) {
            const BlockPlan& plan = plans[(size_t)blockNum];
            int64_t start =
                blockNum * (int64_t)IntColReader::BLOCK_SIZE;
            int64_t end = start + reader.valuesInBlock(blockNum);
            if (plan.relation == BlockRelation::INSIDE) {
              count += end - start;
            } else if (plan.relation == BlockRelation::CROSSES) {
              for (int64_t rank = start; rank < end; rank++) {
                count += crossing.matches(
                    rank, plan, weight.query.getLo(),
                    weight.query.getHi());
              }
            }
          }
          return count;
        }

        int64_t count = 0;
        for (int32_t docRank = 0;
             docRank < reader.docsWithValue(); docRank++) {
          auto [start, end] = reader.getStartEndValueRank(docRank);
          int64_t rank = start;
          bool matched = false;
          while (rank < end && !matched) {
            int64_t blockNum = rank / IntColReader::BLOCK_SIZE;
            int64_t blockEnd = std::min<int64_t>(
                end, (blockNum + 1)
                    * (int64_t)IntColReader::BLOCK_SIZE);
            const BlockPlan& plan = plans[(size_t)blockNum];
            if (plan.relation == BlockRelation::INSIDE) {
              matched = true;
            } else if (plan.relation == BlockRelation::CROSSES) {
              while (rank < blockEnd && !matched) {
                matched = crossing.matches(
                    rank, plan, weight.query.getLo(),
                    weight.query.getHi());
                rank++;
              }
            } else {
              rank = blockEnd;
            }
          }
          count += matched;
        }
        return count;
      }

    public:
      Supplier(NumericRangeQuery::Weight& weight, MemPool& planPool,
               IndexReader::Segment& segment,
               IntColReader& reader, std::span<const BlockPlan> plans,
               int64_t estimatedCost, bool allMatch, PointsReader* points,
               PointsReader::FenceRange fence)
          : weight(weight), planPool(planPool), segment(segment),
            reader(reader), points(points),
            plans(plans), fence(fence), estimatedCost(estimatedCost),
            allMatch(allMatch) {
        int64_t prunableValues = 0;
        for (int64_t blockNum = 0; blockNum < reader.numBlocks(); blockNum++) {
          if (plans[(size_t)blockNum].relation != BlockRelation::CROSSES) {
            prunableValues += reader.valuesInBlock(blockNum);
          }
        }
        int64_t denominator = ZONE_MAP_MIN_PRUNABLE_FRACTION_DENOMINATOR;
        int64_t minPrunable = reader.numValues() / denominator
            + (reader.numValues() % denominator != 0);
        useZoneMap = prunableValues >= minPrunable;
      }

      int64_t cost() override { return estimatedCost; }

      Query::ScorerShape describeScorer(
          const Query::PlanContext& buildContext) const override {
        if (buildContext.numericRangeDisableShapesForTests) return {};
        ScorerArm arm = selectScorerArm(buildContext.demand);
        return shapeFor(arm);
      }

      Query::UnresolvedSupplierCause unresolvedScorerCause(
          const Query::PlanContext& buildContext) const override {
        return buildContext.numericRangeDisableShapesForTests
            ? Query::UnresolvedSupplierCause::NUMERIC_GEO
            : Query::UnresolvedSupplierCause::NONE;
      }

      Query::Scorer* createPointsScorerForTests(MemPool& targetPool) {
        if (points == nullptr) return nullptr;
        auto [begin, end] = exactPositions(targetPool);
        return scorerFor(targetPool,
            materializePoints(targetPool, begin, end));
      }

      Query::Scorer* createComplementScorerForTests(MemPool& targetPool) {
        if (points == nullptr || reader.multiValued()) return nullptr;
        auto [begin, end] = exactPositions(targetPool);
        return scorerFor(targetPool,
            materializeComplement(targetPool, begin, end));
      }

      Query::ScorerPlan* resolve(
          MemPool& planPool,
          const Query::PlanContext& planContext) override {
        ScorerArm arm = selectScorerArm(planContext.demand);
        uint64_t begin = 0;
        uint64_t end = 0;
        bool complement = false;
        if (arm == ScorerArm::POINTS) {
          std::tie(begin, end) = exactPositions(planPool);
          complement = useComplement(end - begin);
        }
        return planPool.make<Plan>(
            *this, planContext, shapeFor(arm), cost(), arm,
            begin, end, complement);
      }

      Query::PlanContext makePlanContext(
          const Query::Demand& demand) const override {
        Query::PlanContext context;
        context.demand = demand;
        context.numericRangeDisableShapesForTests =
            NumericRangeQuery::disableShapesForTests;
        return context;
      }

      BulkPlan planBulk(
          BulkUse use, const BulkScorerContext& bulkContext) override {
        unused(use);
        if (bulkContext.requireConstantCount) {
          if (segment.liveDocs() != nullptr) {
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
        if (points != nullptr) {
          auto [begin, end] = exactPositions(planPool);
          auto* state = planPool.make<BulkState>(
              this, BulkState::Arm::POINTS);
          state->begin = begin;
          state->end = end;
          state->complement = useComplement(end - begin);
          return {
            BulkAnswer::YES, BulkAnswer::YES, BulkAnswer::NO,
            BulkAnswer::NO, state,
          };
        }
        if (useZoneMap) {
          auto* state = planPool.make<BulkState>(
              this, BulkState::Arm::ZONE_MAP);
          return {
            BulkAnswer::YES, BulkAnswer::YES, BulkAnswer::NO,
            BulkAnswer::NO, state,
          };
        }
        return {
          BulkAnswer::NO, BulkAnswer::NO, BulkAnswer::NO,
          BulkAnswer::NO,
        };
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
          return phaseOneBulkScorer(targetPool);
        }
        assert(points != nullptr);
        if (state.complement) {
          skipCount(SkipStats::numericRangeComplementArms);
          return bulkFor(targetPool,
              materializeComplement(targetPool, state.begin, state.end));
        }
        skipCount(SkipStats::numericRangePointsArms);
        return bulkFor(targetPool,
            materializePoints(targetPool, state.begin, state.end));
      }

    };

    Query::ScorerSupplier* scorerSupplier(MemPool& targetPool,
                                           IndexReader::Segment& segment) override {
      SegFieldInfo* segInfo = nullptr;
      if (!segmentInfo(segment, segInfo)) return nullptr;
      auto* reader = targetPool.make<IntColReader>(segment.postingsReader(), *segInfo);
      if (reader->numValues() == 0) return nullptr;
      int64_t colMin = reader->getMin();
      int64_t colMax = reader->getMax();
      if (colMax < query.getLo() || query.getHi() < colMin) return nullptr;

      auto plans = targetPool.make_span<BlockPlan>((size_t)reader->numBlocks());
      for (int64_t i = 0; i < reader->numBlocks(); i++) {
        plans[(size_t)i] = classifyBlock(
            reader->blockInfo(i), reader->blockZone(i), query.getLo(),
            query.getHi());
      }
      bool allMatch = query.getLo() <= colMin && colMax <= query.getHi();
      int64_t cost = estimateCost(*reader, plans);
      PointsReader* points = nullptr;
      PointsReader::FenceRange fence;
      if (segInfo->pointsMetaOff != 0) {
        points = targetPool.make<PointsReader>(segment.postingsReader(), *segInfo);
        if (points->pointCount() != (uint64_t)reader->numValues()) {
          throw std::runtime_error("NumericRangeQuery: points/column value count mismatch");
        }
        fence = points->fenceRange(query.getLo(), query.getHi());
        if (fence.empty) return nullptr;
        uint64_t fenceCount = fence.endOrdinal - fence.firstOrdinal;
        if (reader->multiValued()) {
          // Repeated docids make this a safe upper bound, not an exact count
          // of unique matching documents.
          fenceCount = std::min<uint64_t>(fenceCount, reader->docsWithValue());
        }
        cost = (int64_t)fenceCount;
      }
      return targetPool.make<Supplier>(*this, targetPool, segment, *reader,
                                       plans, cost,
                                       allMatch, points, fence);
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
      SegFieldInfo* segInfo = nullptr;
      if (!segmentInfo(segment, segInfo)) return nullptr;
      auto* reader = targetPool.make<IntColReader>(segment.postingsReader(), *segInfo);
      if (reader->numValues() == 0) return nullptr;
      int64_t colMin = reader->getMin();
      int64_t colMax = reader->getMax();
      if (colMax < query.getLo() || query.getHi() < colMin) return nullptr;
      bool allMatch = query.getLo() <= colMin && colMax <= query.getHi();
      return targetPool.make<RangeScorer<IntColReader::Iterator>>(
          *reader, query.getLo(), query.getHi(), allMatch, constantScore);
    }

    Query::Scorer* createZoneMapScorerForTests(MemPool& targetPool,
                                               IndexReader::Segment& segment) {
      SegFieldInfo* segInfo = nullptr;
      if (!segmentInfo(segment, segInfo)) return nullptr;
      auto* reader = targetPool.make<IntColReader>(segment.postingsReader(), *segInfo);
      if (reader->numValues() == 0) return nullptr;
      if (reader->getMax() < query.getLo() || query.getHi() < reader->getMin()) {
        return nullptr;
      }
      auto plans = targetPool.make_span<BlockPlan>((size_t)reader->numBlocks());
      for (int64_t i = 0; i < reader->numBlocks(); i++) {
        plans[(size_t)i] = classifyBlock(
            reader->blockInfo(i), reader->blockZone(i), query.getLo(),
            query.getHi());
      }
      return targetPool.make<ZoneMapScorer>(
          targetPool, *reader, plans, query.getLo(), query.getHi(),
          segment.maxDoc(), constantScore);
    }

  };
};

} // namespace luxir
