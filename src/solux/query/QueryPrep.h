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
#include "DocSetBulkScorer.h"
#include "solux/search/DocSet.h"

namespace solux::QueryPrep {

inline bool disableDirectPostingsMaterializationForTests = false;
// A/B toggle: restore request-local sparse batch materialization on cache
// bypass instead of preserving the postings supplier as the gather feed.
inline bool disableSparseBatchPostingsFeedForTests = false;

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
  FilterCache::Use* cacheUse = nullptr;
  PreparedDomainDependence domainDependence =
      PreparedDomainDependence::QUERY_CANONICAL;

  Query::SegmentSource& segmentSource() const {
    if (prepared) return *prepared;
    return *weight;
  }

  void setPrepared(std::unique_ptr<Query::Weight::PreparedWeight> value) {
    prepared = std::move(value);
    domainDependence = prepared == nullptr
        ? PreparedDomainDependence::QUERY_CANONICAL
        : prepared->domainDependence();
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
      source.setPrepared(weight->prepare(ctx));
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

// ConstantScorer(0) base: exact zero score, max-score, and score bounds in
// one place, so any formation's bound math can trust this clause regardless
// of how it entered the plan. The base's min-competitive exhaust hint stays
// at its no-op default: a filter clause scores nothing, but its membership
// still gates the conjunction, so it must never self-exhaust.
class DocSetScorer final : public Query::ConstantScorer {
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
  DocSetScorer(DocSet* docs, int32_t maxDoc)
      : ConstantScorer(0.0f), docs(docs), maxDoc(maxDoc) {
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
      // Targets are monotonic for a Scorer, so only the suffix past the cursor
      // is live. Gallop adapts to the step size: a lead re-advancing past a
      // rejected candidate pays one nearby probe, while a non-lead driven by a
      // sparser clause pays O(log distance) instead of visiting every
      // intervening member.
      const int32_t* base = arrDocs.data();
      const int32_t* found = screaming::gallopLowerBound(
          base + arrIdx + 1, base + arrDocs.size(), docid);
      arrIdx = (int32_t) (found - base);
      doc = arrIdx < (int32_t) arrDocs.size() ? arrDocs[(size_t) arrIdx]
                                              : PostingsReader::END;
      return doc;
    }
    return seekBitSet(docid);
  }

  int32_t docId() override { return doc; }

  bool supportsWindowFilter() const override { return true; }

  // Window fills OR membership into caller-owned scratch; the caller clears
  // separate clause scratch and ANDs across conjunctive scorers. The array
  // resume cursor requires windows to arrive in nondecreasing order.
  void fillWindowBits(std::span<uint64_t> windowBits, int32_t windowStart,
                      int32_t windowEnd) override {
    assert(windowStart >= 0 && windowEnd >= windowStart && windowEnd <= maxDoc);
    if (windowEnd <= windowStart) return;
    if (docs->type == DocSet::ARRAY) {
      const int32_t* base = arrDocs.data();
      const int32_t* end = base + arrDocs.size();
      const int32_t* it = screaming::gallopLowerBound(
          base + windowArrIdx, end, windowStart);
      while (it != end && *it < windowEnd) {
        int32_t relative = *it - windowStart;
        windowBits[(size_t) (relative >> 6)]
            |= 1ULL << (relative & 63);
        ++it;
      }
      windowArrIdx = (int32_t) (it - base);
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

  DocSet* exactDocSet() override { return docs; }

  DocSet* docSet() const { return docs; }

  Query::Scorer* get(MemPool& targetPool, int64_t leadCost) override {
    unused(leadCost);
    return createDocSetScorer(targetPool, docs, segment);
  }

  BulkScorer* bulkScorer(MemPool& targetPool) override {
    if (docs == nullptr || docs->card() == 0) return nullptr;
    return targetPool.make<DocSetBulkScorer>(
        targetPool, docs, segment.maxDoc());
  }

  FilteredBulkResult filteredBulkScorer(
      MemPool& targetPool,
      const BulkScorerContext& bulkContext) override {
    if (bulkContext.requireFilterConsumption) {
      return {};
    }
    return {bulkScorer(targetPool), false};
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
      if (domain == nullptr
          && !disableDirectPostingsMaterializationForTests
          && bulk->appendDocs(builder)) {
        return builder.build();
      }
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
    if (domain == nullptr) {
      for (;;) {
        auto doc = scorer->next();
        if (doc == PostingsReader::END) break;
        builder.add(doc);
      }
    } else if (domain->type == DocSet::Type::BITSET) {
      const FixedBitSet& bits = ((BitDocSet*) domain)->bits();
      for (;;) {
        auto doc = scorer->next();
        if (doc == PostingsReader::END) break;
        if (bits.get(doc)) {
          builder.add(doc);
        }
      }
    } else {
      // Sparse array domain: drive from the array and advance the scorer,
      // galloping the array cursor to wherever the scorer lands.
      std::span<int32_t> docs = ((ArrDocSet*) domain)->docs();
      const int32_t* p = docs.data();
      const int32_t* end = p + docs.size();
      while (p != end) {
        int32_t target = *p;
        if (scorer->docId() < target && scorer->advance(target) == PostingsReader::END) {
          break;
        }
        int32_t landing = scorer->docId();
        if (landing == target) {
          builder.add(target);
          p++;
        } else {
          assert(landing > target);
          p = screaming::gallopLowerBound(p + 1, end, landing);
        }
      }
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
  PreparedDomainDependence domainDependence() const noexcept override {
    return PreparedDomainDependence::CANONICAL_READER_DOMAIN;
  }
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
    source.cacheUse = use;
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
        source.setPrepared(std::make_unique<ReaderStablePreparedWeight>(
            std::move(value)));
        out.emplace_back(std::move(source));
        continue;
      }
    }

    source.setPrepared(weight->prepare(ctx));
    out.emplace_back(std::move(source));
  }
  return out;
}

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
  // Pruned scored TOP_k keeps the calibrated density route: sparse filters
  // remain pull/WAND iterators and do not even touch the cache.
  DENSITY_ROUTED,
  // Exhaustive collection, scored or unscored, values exact filter cost and
  // docs-only iteration. Let an admitted cached DocSet become the required
  // clause at every density; conjunction ordering then chooses the route.
  EXHAUSTIVE_CLAUSE,
  // An exact filtered disjunction can batch sparse filter candidates from a
  // cached DocSet or uncached term postings while preserving the ordinary
  // dense cached-mask route. The caller supplies the sparse-density cutoff;
  // only the middle band stays uncached.
  SPARSE_BATCH
};

inline bool disableFilterOwnForTests = false;
// Request-local construction has to repay its postings gather in the same
// request. The 5M cold-cache sweep found no profitable EXHAUSTIVE_CLAUSE
// endpoint even at 0.11% density: direct postings conjunctions were already
// cheaper. Unscored SPARSE_BATCH repaid it at 0.11% but not 1.02%, giving
// COUNT a 1/256 cutoff. Scored SPARSE_BATCH uses its separately measured
// density inverse supplied by the caller.
inline constexpr int32_t kSparseBatchCountOwnDensityInverse = 256;

inline bool shouldOwnFilterValue(FilterSupplierMode mode, int64_t cost,
                                 int32_t maxDoc,
                                 int32_t sparseBatchDensityInverse) {
  if (disableFilterOwnForTests
      || cost > DocSetBuilder::arrayLimitFor(maxDoc)) {
    return false;
  }
  switch (mode) {
    case FilterSupplierMode::DENSITY_ROUTED:
    case FilterSupplierMode::EXHAUSTIVE_CLAUSE:
      return false;
    case FilterSupplierMode::SPARSE_BATCH:
      return sparseBatchDensityInverse > 0
          && cost <= maxDoc / sparseBatchDensityInverse;
  }
  std::unreachable();
}

inline Query::ScorerSupplier* filterSupplier(
    MemPool& targetPool, const PreparedSource& source,
    IndexReader& reader, IndexReader::Segment& segment,
    FilterSupplierMode mode = FilterSupplierMode::DENSITY_ROUTED,
    int32_t sparseBatchDensityInverse = 0) {
  auto& weight = *source.weight;
  auto* prepared = source.prepared.get();
  auto* use = source.cacheUse;
  Query::SegmentSource& segmentSource = source.segmentSource();
  if (source.domainDependence != PreparedDomainDependence::QUERY_CANONICAL
      && use != nullptr
      && use->scope() != FilterKeyScope::READER_STABLE) {
    use = nullptr;
  }
  if (use != nullptr && use->scope() == FilterKeyScope::READER_STABLE) {
    return segmentSource.scorerSupplier(targetPool, segment);
  }

  // Gate before cache traffic. DENSITY_ROUTED leaves sparse filters on pruned
  // pull/WAND (a cached mask measures 7-16% slower on the 5M sweep).
  // SPARSE_BATCH extends that policy in both directions: its measured sparse
  // side wants the DocSet candidate vector, while the dense side retains the
  // existing cached mask. Exhaustive mode bypasses the pruning-based density
  // policy so exact collection can use a cached DocSet in conjunction
  // planning. On cache bypass, SPARSE_BATCH retains postings for the batch
  // gatherer; admitted cache builds still publish a DocSet, and cache hits
  // borrow one. Sparse entries also populate through facet/domain consumers.
  auto* uncached = segmentSource.scorerSupplier(targetPool, segment);
  if (uncached == nullptr) return nullptr;
  int64_t cost = uncached->cost();
  if (use == nullptr
      || (mode == FilterSupplierMode::DENSITY_ROUTED
          && cost < segment.maxDoc() / kMaskFilterDensityInverse)
      || (mode == FilterSupplierMode::SPARSE_BATCH
          && (sparseBatchDensityInverse <= 0
              || cost > segment.maxDoc() / sparseBatchDensityInverse
                  && cost < segment.maxDoc() / kMaskFilterDensityInverse))) {
    return uncached;
  }

  auto probe = use->probe((size_t) segment.ord);
  if (probe.kind() == FilterCache::Probe::Kind::HIT) {
    DocSet* effective = use->effectiveDocSet(
        (size_t) segment.ord, reader);
    return targetPool.make<DocSetSupplier>(effective, segment);
  } else if (probe.kind() == FilterCache::Probe::Kind::BUILD) {
    auto buildStart = std::chrono::steady_clock::now();
    auto raw = materializeRawFilter(weight, prepared, segment);
    uint32_t buildCostMicros = elapsedBuildMicros(buildStart);
    use->publishRaw(
        (size_t) segment.ord, probe, std::move(raw), buildCostMicros);
  } else {
    if (mode == FilterSupplierMode::SPARSE_BATCH
        && !disableSparseBatchPostingsFeedForTests) {
      return uncached;
    }
    if (!shouldOwnFilterValue(
            mode, cost, segment.maxDoc(), sparseBatchDensityInverse)) {
      return uncached;
    }
    auto raw = materializeRawFilter(weight, prepared, segment);
    use->adoptOwnedRaw((size_t) segment.ord, probe, std::move(raw));
  }

  DocSet* effective = use->effectiveDocSet((size_t) segment.ord, reader);
  return targetPool.make<DocSetSupplier>(effective, segment);
}

inline Query::ScorerSupplier* filterSupplier(
    MemPool& targetPool, Query::Weight& weight, FilterCache::Use* use,
    IndexReader& reader, IndexReader::Segment& segment,
    FilterSupplierMode mode = FilterSupplierMode::DENSITY_ROUTED,
    int32_t sparseBatchDensityInverse = 0) {
  PreparedSource source;
  source.weight = &weight;
  source.cacheUse = use;
  return filterSupplier(
      targetPool, source, reader, segment, mode,
      sparseBatchDensityInverse);
}

struct ExactDomainSource {
  Query::Weight* weight = nullptr;
  FilterCache::Use* cacheUse = nullptr;
};

struct ExactDomainResult {
  bool available = false;
  DomainHandle docs;
};

// Try the separable exact-DocSet route for one segment. At the root, the
// ordinary exhaustive filter policy may admit/build a source. Beneath an
// explicit parent domain, only already-resident exact values are accepted:
// a miss falls back to streaming the complete effective query through that
// domain instead of materializing unrestricted source sets first.
inline ExactDomainResult tryExactDomain(
    MemPool& targetPool, std::span<const ExactDomainSource> sources,
    IndexReader& reader, IndexReader::Segment& segment, DocSet* domain) {
  if (domain != nullptr && domain->card() == 0) {
    return {true, DomainHandle::borrowed(domain)};
  }

  std::vector<DocSet*> sets;
  sets.reserve(sources.size() + (domain == nullptr ? 0 : 1));
  bool allAvailable = true;
  for (const auto& source : sources) {
    if (source.weight == nullptr || source.weight->needsPrepare()) {
      allAvailable = false;
      continue;
    }

    DocSet* exact = nullptr;
    bool matchesNone = false;
    if (domain != nullptr && source.cacheUse != nullptr) {
      if (source.cacheUse->scope() == FilterKeyScope::READER_STABLE) {
        allAvailable = false;
        continue;
      }
      auto probe = source.cacheUse->probe((size_t) segment.ord);
      if (probe.kind() != FilterCache::Probe::Kind::HIT) {
        allAvailable = false;
        continue;
      }
      exact = source.cacheUse->effectiveDocSet(
          (size_t) segment.ord, reader);
      if (exact == nullptr) {
        allAvailable = false;
        continue;
      }
    } else {
      auto* supplier = filterSupplier(
          targetPool, *source.weight, source.cacheUse,
          reader, segment, FilterSupplierMode::EXHAUSTIVE_CLAUSE);
      if (supplier == nullptr) {
        matchesNone = true;
      } else {
        exact = supplier->exactDocSet();
        if (exact == nullptr) {
          allAvailable = false;
          continue;
        }
      }
    }

    if (matchesNone) {
      DocSetBuilder empty(segment.maxDoc());
      return {true, DomainHandle(empty.build())};
    }
    sets.push_back(exact);
  }
  if (!allAvailable) return {};
  if (domain != nullptr) sets.push_back(domain);

  if (sets.empty()) return {true, DomainHandle()};
  if (sets.size() == 1) {
    return {true, DomainHandle::borrowed(sets[0])};
  }
  return {true, DomainHandle(DocSet::intersect(sets))};
}

class ExactDomainPlan {
  std::vector<ExactDomainSource> sources;

public:
  void add(Query::Weight& weight, FilterCache::Use* cacheUse) {
    sources.push_back({&weight, cacheUse});
  }

  bool empty() const { return sources.empty(); }

  ExactDomainResult produce(
      MemPool& targetPool, IndexReader& reader,
      IndexReader::Segment& segment, DocSet* domain) const {
    return tryExactDomain(
        targetPool, sources, reader, segment, domain);
  }
};

inline DomainHandle materializeEffectiveFilter(
    const PreparedSource& source, IndexReader& reader,
    IndexReader::Segment& segment, DocSet* domain) {
  auto& weight = *source.weight;
  auto* prepared = source.prepared.get();
  auto* use = source.cacheUse;
  if (source.domainDependence != PreparedDomainDependence::QUERY_CANONICAL
      && use != nullptr
      && use->scope() != FilterKeyScope::READER_STABLE) {
    use = nullptr;
  }
  if (use == nullptr) {
    return DomainHandle(materialize(weight, prepared, segment, domain));
  }

  if (use->scope() == FilterKeyScope::READER_STABLE) {
    if (auto* cached = dynamic_cast<ReaderStablePreparedWeight*>(prepared)) {
      DocSet* canonical = segment.liveDocs() == nullptr
          ? nullptr : &segment.liveDocs()->docset();
      assert(domain == nullptr || domain == canonical);
      return DomainHandle::borrowed(cached->docSet(segment));
    }
    return DomainHandle(materialize(weight, prepared, segment, domain));
  }

  auto probe = use->probe((size_t) segment.ord);
  if (probe.kind() == FilterCache::Probe::Kind::HIT) {
    return DomainHandle::borrowed(use->effectiveDocSet(
        (size_t) segment.ord, reader, domain));
  } else if (probe.kind() == FilterCache::Probe::Kind::BUILD) {
    auto buildStart = std::chrono::steady_clock::now();
    auto raw = materializeRawFilter(weight, prepared, segment);
    uint32_t buildCostMicros = elapsedBuildMicros(buildStart);
    use->publishRawByproduct(
        (size_t) segment.ord, probe, std::move(raw), buildCostMicros);
  } else {
    // A non-folded raw materialization is free by-product population. A
    // domain-composed set must never enter the raw cache.
    if (domain == nullptr
        && use->wasAdmitted()
        && !weight.needsScores() && !weight.allowsPruning()) {
      auto buildStart = std::chrono::steady_clock::now();
      auto raw = materializeRawFilter(weight, prepared, segment);
      uint32_t buildCostMicros = elapsedBuildMicros(buildStart);
      use->offerRaw(
          (size_t) segment.ord, std::move(raw), buildCostMicros);
      DocSet* docs = use->effectiveDocSet((size_t) segment.ord, reader);
      return DomainHandle::borrowed(docs);
    }
    if (!disableFilterOwnForTests
        && !weight.needsScores() && !weight.allowsPruning()) {
      auto raw = materializeRawFilter(weight, prepared, segment);
      use->adoptOwnedRaw((size_t) segment.ord, probe, std::move(raw));
      return DomainHandle::borrowed(use->effectiveDocSet(
          (size_t) segment.ord, reader, domain));
    }
    return DomainHandle(
        materialize(weight, prepared, segment, domain));
  }

  DocSet* effective = use->effectiveDocSet(
      (size_t) segment.ord, reader, domain);
  return DomainHandle::borrowed(effective);
}

inline DomainHandle materializeEffectiveFilter(
    Query::Weight& weight, FilterCache::Use* use, IndexReader& reader,
    IndexReader::Segment& segment, DocSet* domain) {
  PreparedSource source;
  source.weight = &weight;
  source.cacheUse = use;
  return materializeEffectiveFilter(source, reader, segment, domain);
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

inline DomainHandle materializeEffectiveIntersection(
    std::span<const PreparedSource> sources, IndexReader& reader,
    IndexReader::Segment& segment, DocSet* domain) {
  std::vector<DomainHandle> sets;
  std::vector<DocSet*> ptrs;
  sets.reserve(sources.size());
  ptrs.reserve(sources.size());
  for (auto& source : sources) {
    sets.push_back(materializeEffectiveFilter(
        source, reader, segment, domain));
    ptrs.push_back(sets.back().get());
  }
  if (ptrs.empty()) return {};
  if (ptrs.size() == 1) return std::move(sets[0]);
  return DomainHandle(DocSet::intersect(ptrs));
}

} // namespace solux::QueryPrep
