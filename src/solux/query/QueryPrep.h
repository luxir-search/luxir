#pragma once

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <limits>
#include <memory>
#include <span>
#include <stdexcept>
#include <vector>
#include <boost/container/small_vector.hpp>

#include "Query.h"
#include "solux/search/DocSet.h"

namespace solux::QueryPrep {

inline uint32_t elapsedBuildMicros(
    std::chrono::steady_clock::time_point start) {
  auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
      std::chrono::steady_clock::now() - start).count();
  return (uint32_t) std::min<int64_t>(
      elapsed, std::numeric_limits<uint32_t>::max());
}

// Convenience path for callers that just need a scorer and are not doing
// parent-level planning. INT64_MAX means there is no external lead iterator
// constraining scorer construction.
inline Query::Scorer* createScorer(MemPool& targetPool,
                                   IndexReader::Segment& segment,
                                   Query::SegmentSource& source) {
  auto* supplier = source.scorerSupplier(targetPool, segment);
  if (supplier == nullptr) return nullptr;
  return supplier->get(targetPool, std::numeric_limits<int64_t>::max());
}

// Owning prepare result for a child query. If prepared is null, the original
// Weight remains usable through the same SegmentSource path.
struct PreparedSource {
  Query::Weight* weight = nullptr;
  std::unique_ptr<Query::Weight::PreparedWeight> prepared;

  Query::SegmentSource& segmentSource() const {
    if (prepared) return *prepared;
    return *weight;
  }
};

inline bool anyNeedsPrepare(std::span<Query::Weight*> weights) {
  for (auto* weight : weights) {
    if (weight->needsPrepare()) return true;
  }
  return false;
}

inline std::vector<PreparedSource> prepareSources(std::span<Query::Weight*> weights,
                                                  Query::Weight::PrepareContext& ctx) {
  std::vector<PreparedSource> out;
  out.reserve(weights.size());
  for (auto* weight : weights) {
    PreparedSource source;
    source.weight = weight;
    if (weight->needsPrepare()) {
      source.prepared = weight->prepare(ctx);
    }
    out.emplace_back(std::move(source));
  }
  return out;
}

inline std::span<const PreparedSource> preparedSpan(const std::vector<PreparedSource>& sources) {
  return {sources.data(), sources.size()};
}

struct CostedScorers {
  std::span<Query::Scorer*> scorers;
  std::span<int64_t> costs;
};

inline std::span<Query::SegmentSource*> liveSources(MemPool& targetPool,
                                                    std::span<Query::Weight*> weights) {
  if (weights.empty()) return {};
  auto sources = targetPool.make_span<Query::SegmentSource*>(weights.size());
  for (size_t i = 0; i < weights.size(); i++) {
    sources[i] = weights[i];
  }
  return sources;
}

inline std::span<Query::SegmentSource*> segmentSources(MemPool& targetPool,
                                                       std::span<const PreparedSource> preparedSources) {
  if (preparedSources.empty()) return {};
  auto sources = targetPool.make_span<Query::SegmentSource*>(preparedSources.size());
  for (size_t i = 0; i < preparedSources.size(); i++) {
    sources[i] = &preparedSources[i].segmentSource();
  }
  return sources;
}

inline std::span<Query::Scorer*> createScorers(MemPool& targetPool,
                                               IndexReader::Segment& segment,
                                               std::span<Query::SegmentSource* const> sources) {
  if (sources.empty()) return {};
  auto& scorers = *targetPool.make_vec<Query::Scorer*>();
  scorers.reserve(sources.size());
  for (auto* source : sources) {
    auto* scorer = createScorer(targetPool, segment, *source);
    if (scorer != nullptr) scorers.push_back(scorer);
  }
  return scorers;
}

inline CostedScorers createScorersWithCosts(
    MemPool& targetPool, IndexReader::Segment& segment,
    std::span<Query::SegmentSource* const> sources) {
  if (sources.empty()) return {};
  auto scorers = targetPool.make_span<Query::Scorer*>(sources.size());
  auto costs = targetPool.make_span<int64_t>(sources.size());
  size_t count = 0;
  for (auto* source : sources) {
    auto* supplier = source->scorerSupplier(targetPool, segment);
    if (supplier == nullptr) continue;
    int64_t cost = supplier->cost();
    auto* scorer = supplier->get(targetPool, std::numeric_limits<int64_t>::max());
    if (scorer == nullptr) continue;
    scorers[count] = scorer;
    costs[count++] = cost;
  }
  return {scorers.first(count), costs.first(count)};
}


// Like createScorers, but retains supplier costs and orders both parallel spans
// by ascending cost.
inline CostedScorers createScorersByCost(MemPool& targetPool,
                                         IndexReader::Segment& segment,
                                         std::span<Query::SegmentSource* const> sources) {
  if (sources.empty()) return {};

  struct CostedSupplier {
    int64_t cost;
    Query::ScorerSupplier* supplier;
  };

  // The (cost, supplier) scratch is only needed to sort before building scorers;
  // it does not outlive this call. A small_vector keeps it on the stack for the
  // usual handful of clauses.
  boost::container::small_vector<CostedSupplier, 16> costed;
  for (auto* source : sources) {
    auto* supplier = source->scorerSupplier(targetPool, segment);
    if (supplier != nullptr) costed.push_back({supplier->cost(), supplier});
  }
  std::sort(costed.begin(), costed.end(),
            [](const CostedSupplier& a, const CostedSupplier& b) { return a.cost < b.cost; });

  auto* scorers = targetPool.make_arr<Query::Scorer*>(sources.size());
  auto* costs = targetPool.make_arr<int64_t>(sources.size());
  size_t count = 0;
  for (auto& c : costed) {
    auto* scorer = c.supplier->get(targetPool, std::numeric_limits<int64_t>::max());
    if (scorer != nullptr) {
      scorers[count] = scorer;
      costs[count++] = c.cost;
    }
  }
  return {{scorers, count}, {costs, count}};
}

// Collect a ScorerSupplier for each source, keeping a 1:1 mapping with the input
// (a null entry means that source cannot match this segment). Lets a parent plan
// over child costs - and pick lead iterators - before any scorer is built.
inline std::span<Query::ScorerSupplier*> collectSuppliers(MemPool& targetPool,
                                                          IndexReader::Segment& segment,
                                                          std::span<Query::SegmentSource* const> sources) {
  if (sources.empty()) return {};
  auto* suppliers = targetPool.make_arr<Query::ScorerSupplier*>(sources.size());
  for (size_t i = 0; i < sources.size(); i++) {
    suppliers[i] = sources[i]->scorerSupplier(targetPool, segment);
  }
  return {suppliers, sources.size()};
}

class DocSetScorer final : public Query::Scorer {
  DocSet* docs;
  int32_t maxDoc;
  int32_t doc = -1;
  std::span<int32_t> arrDocs;
  int32_t arrIdx = -1;
  int32_t windowArrIdx = 0;

  int32_t seekBitSet(int32_t target) {
    if (target >= maxDoc) {
      return doc = PostingsReader::END;
    }
    const auto& bits = ((BitDocSet*) docs)->bits();
    int32_t found = bits.nextSetBit(target);
    return doc = found == FixedBitSet::MAX_INDEX
        ? PostingsReader::END : found;
  }

public:
  DocSetScorer(DocSet* docs, int32_t maxDoc) : docs(docs), maxDoc(maxDoc) {
    if (docs->type == DocSet::ARRAY) {
      arrDocs = ((ArrDocSet*)docs)->docs();
    }
  }

  int32_t next() override {
    if (docs->type == DocSet::ARRAY) {
      arrIdx++;
      doc = arrIdx < (int32_t)arrDocs.size() ? arrDocs[arrIdx] : PostingsReader::END;
      return doc;
    }
    assert(doc != PostingsReader::END);
    return seekBitSet(doc + 1);
  }

  int32_t advance(int32_t docid) override {
    assert(doc < docid);  // strict Scorer contract; callers guard
    if (docs->type == DocSet::ARRAY) {
      // Targets are monotonic for a Scorer. Consume the array cursor once
      // rather than binary-searching the shrinking suffix for every rejected
      // filter doc; a DocSet lead must remain O(cardinality) over its lifetime.
      do {
        arrIdx++;
      } while (arrIdx < (int32_t) arrDocs.size()
               && arrDocs[(size_t) arrIdx] < docid);
      if (arrIdx >= (int32_t) arrDocs.size()) {
        arrIdx = (int32_t)arrDocs.size();
        doc = PostingsReader::END;
      } else {
        doc = arrDocs[(size_t) arrIdx];
      }
      return doc;
    }
    return seekBitSet(docid);
  }

  int32_t docId() override { return doc; }

  float score() override { return 0.0f; }

  // Exact zero bounds: any formation's bound math can trust this clause
  // regardless of how it entered the plan.
  float getMaxScore(int32_t upTo) override { return 0.0f; }

  int32_t advanceShallow(int32_t target) override {
    return PostingsReader::END;
  }

  bool supportsWindowFilter() const override { return true; }

  // Window fills OR membership into caller-owned scratch; the caller clears
  // separate clause scratch and ANDs across conjunctive scorers. The array
  // resume cursor requires windows to arrive in nondecreasing order.
  void fillWindowBits(std::span<uint64_t> windowBits, int32_t windowStart,
                      int32_t windowEnd) override {
    assert(windowStart >= 0 && windowEnd >= windowStart && windowEnd <= maxDoc);
    if (windowEnd <= windowStart) return;
    if (docs->type == DocSet::ARRAY) {
      auto begin = arrDocs.begin() + windowArrIdx;
      auto it = std::lower_bound(begin, arrDocs.end(), windowStart);
      while (it != arrDocs.end() && *it < windowEnd) {
        int32_t relative = *it - windowStart;
        windowBits[(size_t) (relative >> 6)]
            |= 1ULL << (relative & 63);
        ++it;
      }
      windowArrIdx = (int32_t) (it - arrDocs.begin());
      return;
    }

    const auto& source = static_cast<BitDocSet*>(docs)->bits();
    int32_t bitCount = windowEnd - windowStart;
    int32_t words = (bitCount + 63) >> 6;
    int32_t sourceWord = windowStart >> 6;
    int32_t shift = windowStart & 63;
    int32_t sourceWords = (int32_t) FixedBitSet::sizeInWords(maxDoc);
    for (int32_t i = 0; i < words; i++) {
      uint64_t bits = source.words[sourceWord + i] >> shift;
      if (shift != 0 && sourceWord + i + 1 < sourceWords) {
        bits |= source.words[sourceWord + i + 1] << (64 - shift);
      }
      if (i + 1 == words && (bitCount & 63) != 0) {
        bits &= (1ULL << (bitCount & 63)) - 1ULL;
      }
      windowBits[(size_t) i] |= bits;
    }
  }
};

inline Query::Scorer* createDocSetScorer(MemPool& targetPool, DocSet* docs,
                                         IndexReader::Segment& segment) {
  if (docs == nullptr || docs->card() == 0) return nullptr;
  return targetPool.make<DocSetScorer>(docs, segment.maxDoc());
}

// Cache hits must retain a bulk implementation as well as pull/window
// membership; otherwise the same query would lose the dense COUNT route.
class DocSetBulkScorer final : public BulkScorer {
  static constexpr int32_t kWindowSize = DocsEnumMeta::L1_DOCS;
  static constexpr int32_t kWindowWords = (kWindowSize + 63) >> 6;

  DocSet* docs;
  int32_t maxDoc;
  std::span<int32_t> arrDocs;
  int32_t sourceArrIdx = 0;
  std::span<int32_t> outDocs;
  std::span<float> outScores;
  std::span<uint64_t> windowBits;

  int32_t endFor(int32_t min, int32_t max) const {
    int32_t requested = min + kWindowSize;
    if (requested < min) requested = max;
    return std::min({requested, max, maxDoc});
  }

  void fillSourceBits(int32_t min, int32_t end) {
    std::fill(windowBits.begin(), windowBits.end(), 0);
    skipCount(SkipStats::countBulkFillCalls);
    if (docs->type == DocSet::ARRAY) {
      // Windows arrive in nondecreasing order (BulkScorer contract), so
      // resume from the cursor instead of searching the whole array per
      // window.
      auto it = std::lower_bound(arrDocs.begin() + sourceArrIdx,
                                 arrDocs.end(), min);
      while (it != arrDocs.end() && *it < end) {
        int32_t relative = *it - min;
        windowBits[(size_t) (relative >> 6)] |= 1ULL << (relative & 63);
        ++it;
      }
      sourceArrIdx = (int32_t) (it - arrDocs.begin());
      return;
    }

    const FixedBitSet& source = ((BitDocSet*) docs)->bits();
    int32_t bitCount = end - min;
    int32_t words = (bitCount + 63) >> 6;
    int32_t sourceWord = min >> 6;
    int32_t shift = min & 63;
    int32_t sourceWords = (int32_t) FixedBitSet::sizeInWords(source.size());
    for (int32_t i = 0; i < words; i++) {
      uint64_t bits = source.words[sourceWord + i] >> shift;
      if (shift != 0 && sourceWord + i + 1 < sourceWords) {
        bits |= source.words[sourceWord + i + 1] << (64 - shift);
      }
      if (i + 1 == words && (bitCount & 63) != 0) {
        bits &= (1ULL << (bitCount & 63)) - 1ULL;
      }
      windowBits[(size_t) i] = bits;
    }
  }

  void intersectFilter(DocSet* filter, int32_t min, int32_t end) {
    if (filter == nullptr) return;
    int32_t bitCount = end - min;
    int32_t words = (bitCount + 63) >> 6;
    if (filter->type == DocSet::BITSET) {
      const FixedBitSet& source = ((BitDocSet*) filter)->bits();
      int32_t sourceWord = min >> 6;
      int32_t shift = min & 63;
      int32_t sourceWords = (int32_t) FixedBitSet::sizeInWords(source.size());
      for (int32_t i = 0; i < words; i++) {
        uint64_t bits = source.words[sourceWord + i] >> shift;
        if (shift != 0 && sourceWord + i + 1 < sourceWords) {
          bits |= source.words[sourceWord + i + 1] << (64 - shift);
        }
        windowBits[(size_t) i] &= bits;
      }
      return;
    }

    for (int32_t wordIdx = 0; wordIdx < words; wordIdx++) {
      uint64_t bits = windowBits[(size_t) wordIdx];
      while (bits != 0) {
        int32_t bit = (int32_t) std::countr_zero(bits);
        int32_t doc = min + (wordIdx << 6) + bit;
        if (doc >= end) break;
        if (!filter->get(doc)) {
          windowBits[(size_t) wordIdx] &= ~(1ULL << bit);
        }
        bits &= bits - 1;
      }
    }
  }

public:
  DocSetBulkScorer(MemPool& pool, DocSet* docs, int32_t maxDoc)
      : docs(docs), maxDoc(maxDoc),
        outDocs(pool.make_span<int32_t>((size_t) kWindowSize)),
        outScores(pool.make_span<float>((size_t) kWindowSize)),
        windowBits(pool.make_span<uint64_t>((size_t) kWindowWords)) {
    if (docs->type == DocSet::ARRAY) {
      arrDocs = ((ArrDocSet*) docs)->docs();
    }
  }

  bool willCountDense() const override { return true; }

  int32_t scoreNextWindow(ScoreWindow& out, DocSet* filter,
                          int32_t min, int32_t max,
                          float minCompetitiveScore) override {
    int32_t end = endFor(min, max);
    out = {.min = min, .max = end};
    if (min >= end) return PostingsReader::END;
    fillSourceBits(min, end);
    intersectFilter(filter, min, end);
    if (minCompetitiveScore <= 0.0f) {
      int32_t words = (end - min + 63) >> 6;
      for (int32_t wordIdx = 0; wordIdx < words; wordIdx++) {
        uint64_t bits = windowBits[(size_t) wordIdx];
        while (bits != 0) {
          int32_t bit = (int32_t) std::countr_zero(bits);
          int32_t doc = min + (wordIdx << 6) + bit;
          if (doc >= end) break;
          outDocs[(size_t) out.size] = doc;
          outScores[(size_t) out.size] = 0.0f;
          out.size++;
          bits &= bits - 1;
        }
      }
    }
    out.docs = outDocs.first((size_t) out.size);
    out.scores = outScores.first((size_t) out.size);
    return end >= max || end >= maxDoc ? PostingsReader::END : end;
  }

  int32_t countNextWindow(int64_t& count, DocSetBuilder* domainOut,
                          DocSet* filter, int32_t min, int32_t max) override {
    max = std::min(max, maxDoc);
    if (min >= max) return PostingsReader::END;
    if (min == 0 && max == maxDoc && filter == nullptr && domainOut == nullptr) {
      count += docs->card();
      return PostingsReader::END;
    }

    int32_t end = endFor(min, max);
    fillSourceBits(min, end);
    intersectFilter(filter, min, end);
    if (domainOut != nullptr) {
      skipCount(SkipStats::bulkDomainWindowsFed);
      domainOut->addWindowWords(windowBits.data(), min, end);
    }
    int32_t words = (end - min + 63) >> 6;
    for (int32_t i = 0; i < words; i++) {
      count += (int32_t) std::popcount(windowBits[(size_t) i]);
    }
    return end >= max ? PostingsReader::END : end;
  }
};

// Supplier over a pre-materialized filter domain (a prepared boolean's per-segment
// filter result). cost() is the exact domain cardinality, so a parent conjunction
// can order it against the other required clauses - a selective filter should lead
// the iteration rather than always being walked first by clause position.
class DocSetSupplier final : public Query::ScorerSupplier {
  DocSet* docs;
  IndexReader::Segment& segment;

public:
  DocSetSupplier(DocSet* docs, IndexReader::Segment& segment) : docs(docs), segment(segment) {}

  int64_t cost() override { return docs == nullptr ? 0 : (int64_t)docs->card(); }

  Query::Scorer* get(MemPool& targetPool, int64_t leadCost) override {
    unused(leadCost);
    return createDocSetScorer(targetPool, docs, segment);
  }

  BulkScorer* bulkScorer(MemPool& targetPool) override {
    if (docs == nullptr || docs->card() == 0) return nullptr;
    return targetPool.make<DocSetBulkScorer>(
        targetPool, docs, segment.maxDoc());
  }
};

inline std::unique_ptr<DocSet> materialize(Query::SegmentSource& source,
                                           IndexReader::Segment& segment,
                                           DocSet* domain) {
  // Used from prepare() paths, which can run deep in a work-stealing stack.
  // Use the thread-local pool (its inline buffer lives in TLS, not on this
  // stack frame) rather than a stack-resident MemPool, and keep scorer
  // temporaries out of Query::Context's shared request pool.
  auto guard = MemPool::threadLocalPoolGuard();
  MemPool& scratch = guard.pool();
  DocSetBuilder builder(segment.maxDoc());
  auto* supplier = source.scorerSupplier(scratch, segment);
  if (supplier != nullptr) {
    auto* bulk = supplier->bulkScorer(scratch);
    if (bulk != nullptr) {
      int64_t count = 0;
      for (int32_t cursor = 0; cursor != PostingsReader::END && cursor < segment.maxDoc(); ) {
        int32_t next = bulk->countNextWindow(count, &builder, domain, cursor, segment.maxDoc());
        if (next == PostingsReader::END) break;
        assert(next > cursor);
        cursor = next;
      }
      assert(count == builder.card());
      return builder.build();
    }

    auto* scorer = supplier->get(scratch, std::numeric_limits<int64_t>::max());
    if (scorer == nullptr) {
      return builder.build();
    }
    for (;;) {
      auto doc = scorer->next();
      if (doc == PostingsReader::END) break;
      if (domain && !domain->get(doc)) continue;
      builder.add(doc);
    }
  }
  return builder.build();
}

inline std::unique_ptr<DocSet> materialize(Query::Weight& weight,
                                           Query::Weight::PreparedWeight* prepared,
                                           IndexReader::Segment& segment,
                                           DocSet* domain) {
  Query::SegmentSource& source = prepared != nullptr
    ? static_cast<Query::SegmentSource&>(*prepared)
    : static_cast<Query::SegmentSource&>(weight);
  return materialize(source, segment, domain);
}

// Prepared source over one pinned whole-reader cache value. This is distinct
// from the raw segment cache path: its DocSets already include reader liveness,
// and callers borrow them directly without a liveDocs composition pass.
class ReaderStablePreparedWeight final
    : public Query::Weight::PreparedWeight {
  std::shared_ptr<const FilterCache::ReaderValue> value;

public:
  explicit ReaderStablePreparedWeight(
      std::shared_ptr<const FilterCache::ReaderValue> value)
    : value(std::move(value)) {}

  DocSet* docSet(IndexReader::Segment& segment) const {
    return value->docSet((size_t) segment.ord,
                         {segment.segInfo.seg_id, segment.maxDoc()});
  }

  Query::ScorerSupplier* scorerSupplier(
      MemPool& targetPool, IndexReader::Segment& segment) override {
    return targetPool.make<DocSetSupplier>(docSet(segment), segment);
  }

  Query::Scorer* createScorer(
      MemPool& targetPool, IndexReader::Segment& segment) override {
    return createDocSetScorer(targetPool, docSet(segment), segment);
  }

  bool outputIsSubsetOfDomain() const noexcept override { return true; }
};

// Filter-only pre-prepare seam. Reader-stable hits replace prepare() with a
// pinned DocSet source. A claimed miss prepares exactly once, materializes the
// complete canonical live domain, and publishes one whole-reader value. Gate
// failures and lost claims take the ordinary nonblocking prepare path.
inline std::vector<PreparedSource> prepareFilterSources(
    std::span<Query::Weight*> weights,
    std::span<FilterCache::Use* const> uses,
    Query::Weight::PrepareContext& ctx) {
  assert(uses.empty() || uses.size() == weights.size());
  std::vector<PreparedSource> out;
  out.reserve(weights.size());
  for (size_t i = 0; i < weights.size(); i++) {
    auto* weight = weights[i];
    auto* use = uses.empty() ? nullptr : uses[i];
    PreparedSource source;
    source.weight = weight;
    if (!weight->needsPrepare()) {
      out.emplace_back(std::move(source));
      continue;
    }

    if (use != nullptr && use->scope() == FilterKeyScope::READER_STABLE) {
      auto probe = use->probeReaderStable(ctx.reader, ctx.domainPerSeg);
      std::shared_ptr<const FilterCache::ReaderValue> value;
      if (probe.kind() == FilterCache::ReaderProbe::Kind::HIT) {
        value = probe.sharedValue();
      } else if (probe.kind() == FilterCache::ReaderProbe::Kind::BUILD) {
        auto buildStart = std::chrono::steady_clock::now();
        auto prepared = weight->prepare(ctx);
        Query::SegmentSource& segmentSource = prepared != nullptr
            ? static_cast<Query::SegmentSource&>(*prepared)
            : static_cast<Query::SegmentSource&>(*weight);
        std::vector<std::unique_ptr<DocSet>> liveExact;
        liveExact.reserve(ctx.reader.segments().size());
        for (size_t segmentOrd = 0;
             segmentOrd < ctx.reader.segments().size(); segmentOrd++) {
          DocSet* canonical = ctx.domainPerSeg.empty()
              ? nullptr : ctx.domainPerSeg[segmentOrd];
          liveExact.push_back(materialize(
              segmentSource, ctx.reader.segments()[segmentOrd], canonical));
        }
        uint32_t buildCostMicros = elapsedBuildMicros(buildStart);
        value = use->publishReaderStable(
            probe, std::move(liveExact), buildCostMicros);
      }
      if (value != nullptr) {
        source.prepared = std::make_unique<ReaderStablePreparedWeight>(
            std::move(value));
        out.emplace_back(std::move(source));
        continue;
      }
    }

    source.prepared = weight->prepare(ctx);
    out.emplace_back(std::move(source));
  }
  return out;
}

// An effective filter may borrow request-pinned cache state or own a freshly
// materialized/domain-composed set. Callers keep this handle, not a naked
// pointer, whenever the result must survive the current stack frame.
class MaterializedFilter {
  std::unique_ptr<DocSet> owned;
  DocSet* borrowed = nullptr;

public:
  MaterializedFilter() = default;
  explicit MaterializedFilter(std::unique_ptr<DocSet> docs)
    : owned(std::move(docs)) {}
  explicit MaterializedFilter(DocSet* docs) : borrowed(docs) {}
  MaterializedFilter(MaterializedFilter&&) noexcept = default;
  MaterializedFilter& operator=(MaterializedFilter&&) noexcept = default;
  MaterializedFilter(const MaterializedFilter&) = delete;
  MaterializedFilter& operator=(const MaterializedFilter&) = delete;

  DocSet* get() const { return owned != nullptr ? owned.get() : borrowed; }
};

inline std::unique_ptr<DocSet> materializeRawFilter(
    Query::Weight& weight, Query::Weight::PreparedWeight* prepared,
    IndexReader::Segment& segment) {
  if (weight.needsScores() || weight.allowsPruning()) {
    throw std::logic_error(
        "raw filter materialization requires NEED_SCORES and ALLOW_PRUNING off");
  }
  return materialize(weight, prepared, segment, nullptr);
}

enum class FilterSupplierMode : uint8_t {
  // Scored TOP_k keeps the calibrated density route: sparse filters remain
  // pull/WAND iterators and do not even touch the cache.
  DENSITY_ROUTED,
  // Exhaustive count/match/domain production values exact filter cost and
  // docs-only iteration. Let an admitted cached DocSet become the required
  // clause at every density; conjunction ordering then chooses the route.
  EXHAUSTIVE_CLAUSE
};

inline Query::ScorerSupplier* filterSupplier(
    MemPool& targetPool, Query::Weight& weight,
    Query::Weight::PreparedWeight* prepared, FilterCache::Use* use,
    IndexReader& reader, IndexReader::Segment& segment,
    FilterSupplierMode mode = FilterSupplierMode::DENSITY_ROUTED) {
  Query::SegmentSource& source = prepared != nullptr
    ? static_cast<Query::SegmentSource&>(*prepared)
    : static_cast<Query::SegmentSource&>(weight);
  if (use == nullptr) return source.scorerSupplier(targetPool, segment);
  if (use->scope() == FilterKeyScope::READER_STABLE) {
    return source.scorerSupplier(targetPool, segment);
  }

  // In density-routed mode, gate before any cache traffic: below the mask
  // crossover the scored pull/WAND formation owns the regime, and a cached
  // set displacing it measures 7-16% slower on the 5M sweep. Exhaustive mode
  // deliberately bypasses that scored policy so exact cardinality participates
  // in conjunction planning. Sparse entries also populate through facet/domain
  // consumers, which serve them at any density.
  auto* uncached = source.scorerSupplier(targetPool, segment);
  if (uncached == nullptr
      || (mode == FilterSupplierMode::DENSITY_ROUTED
          && uncached->cost()
              < segment.maxDoc() / kMaskFilterDensityInverse)) {
    return uncached;
  }

  auto probe = use->probe((size_t) segment.ord);
  std::shared_ptr<const FilterCache::SegmentValue> value;
  if (probe.kind() == FilterCache::Probe::Kind::HIT) {
    value = use->pinnedValue((size_t) segment.ord);
  } else if (probe.kind() == FilterCache::Probe::Kind::BUILD) {
    auto buildStart = std::chrono::steady_clock::now();
    auto raw = materializeRawFilter(weight, prepared, segment);
    uint32_t buildCostMicros = elapsedBuildMicros(buildStart);
    value = use->publishRaw(
        (size_t) segment.ord, probe, std::move(raw), buildCostMicros);
  } else {
    return uncached;
  }

  DocSet* effective = use->effectiveDocSet(
      (size_t) segment.ord, reader, value);
  return targetPool.make<DocSetSupplier>(effective, segment);
}

inline MaterializedFilter materializeEffectiveFilter(
    Query::Weight& weight, Query::Weight::PreparedWeight* prepared,
    FilterCache::Use* use, IndexReader& reader,
    IndexReader::Segment& segment, DocSet* domain) {
  if (use == nullptr) {
    return MaterializedFilter(materialize(weight, prepared, segment, domain));
  }

  if (use->scope() == FilterKeyScope::READER_STABLE) {
    if (auto* cached = dynamic_cast<ReaderStablePreparedWeight*>(prepared)) {
      DocSet* canonical = segment.liveDocs() == nullptr
          ? nullptr : &segment.liveDocs()->docset();
      assert(domain == nullptr || domain == canonical);
      return MaterializedFilter(cached->docSet(segment));
    }
    return MaterializedFilter(materialize(weight, prepared, segment, domain));
  }

  auto probe = use->probe((size_t) segment.ord);
  std::shared_ptr<const FilterCache::SegmentValue> value;
  if (probe.kind() == FilterCache::Probe::Kind::HIT) {
    value = use->pinnedValue((size_t) segment.ord);
  } else if (probe.kind() == FilterCache::Probe::Kind::BUILD) {
    auto buildStart = std::chrono::steady_clock::now();
    auto raw = materializeRawFilter(weight, prepared, segment);
    uint32_t buildCostMicros = elapsedBuildMicros(buildStart);
    value = use->publishRawByproduct(
        (size_t) segment.ord, probe, std::move(raw), buildCostMicros);
  } else {
    // A non-folded raw materialization is free by-product population. A
    // domain-composed set must never enter the raw cache.
    if (domain == nullptr
        && use->wasAdmitted()
        && !weight.needsScores() && !weight.allowsPruning()) {
      auto buildStart = std::chrono::steady_clock::now();
      auto effective = materialize(weight, prepared, segment, domain);
      uint32_t buildCostMicros = elapsedBuildMicros(buildStart);
      value = use->offerRaw(
          (size_t) segment.ord, std::move(effective), buildCostMicros);
      DocSet* docs = use->effectiveDocSet(
          (size_t) segment.ord, reader, value);
      return MaterializedFilter(docs);
    }
    return MaterializedFilter(
        materialize(weight, prepared, segment, domain));
  }

  DocSet* effective = use->effectiveDocSet(
      (size_t) segment.ord, reader, value, domain);
  return MaterializedFilter(effective);
}

inline std::unique_ptr<DocSet> intersectOwned(std::vector<std::unique_ptr<DocSet>>& sets) {
  if (sets.empty()) return nullptr;
  if (sets.size() == 1) return std::move(sets[0]);

  std::vector<DocSet*> ptrs;
  ptrs.reserve(sets.size());
  for (auto& set : sets) {
    ptrs.push_back(set.get());
  }
  return DocSet::intersect(ptrs);
}

inline std::unique_ptr<DocSet> materializeIntersection(std::span<const PreparedSource> sources,
                                                       IndexReader::Segment& segment,
                                                       DocSet* domain) {
  std::vector<std::unique_ptr<DocSet>> sets;
  sets.reserve(sources.size());
  for (auto& source : sources) {
    sets.push_back(materialize(source.segmentSource(), segment, domain));
  }
  return intersectOwned(sets);
}

inline MaterializedFilter materializeEffectiveIntersection(
    std::span<const PreparedSource> sources,
    std::span<FilterCache::Use* const> uses, IndexReader& reader,
    IndexReader::Segment& segment, DocSet* domain) {
  assert(uses.empty() || uses.size() == sources.size());
  std::vector<MaterializedFilter> sets;
  std::vector<DocSet*> ptrs;
  sets.reserve(sources.size());
  ptrs.reserve(sources.size());
  for (size_t i = 0; i < sources.size(); i++) {
    auto& source = sources[i];
    auto* use = uses.empty() ? nullptr : uses[i];
    sets.push_back(materializeEffectiveFilter(
        *source.weight, source.prepared.get(), use, reader, segment, domain));
    ptrs.push_back(sets.back().get());
  }
  if (ptrs.empty()) return {};
  if (ptrs.size() == 1) return std::move(sets[0]);
  return MaterializedFilter(DocSet::intersect(ptrs));
}

} // namespace solux::QueryPrep
