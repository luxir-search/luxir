#pragma once

#include <algorithm>
#include <limits>
#include <memory>
#include <span>
#include <vector>
#include <boost/container/small_vector.hpp>

#include "Query.h"
#include "solux/search/DocSet.h"

namespace solux::QueryPrep {

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
    for (doc++; doc < maxDoc; doc++) {
      if (docs->get(doc)) return doc;
    }
    doc = PostingsReader::END;
    return doc;
  }

  int32_t advance(int32_t docid) override {
    assert(doc < docid);  // strict Scorer contract; callers guard
    if (docs->type == DocSet::ARRAY) {
      auto it = std::lower_bound(arrDocs.begin(), arrDocs.end(), docid);
      if (it == arrDocs.end()) {
        arrIdx = (int32_t)arrDocs.size();
        doc = PostingsReader::END;
      } else {
        arrIdx = (int32_t)(it - arrDocs.begin());
        doc = *it;
      }
      return doc;
    }
    doc = std::max(doc + 1, docid);
    while (doc < maxDoc) {
      if (docs->get(doc)) return doc;
      doc++;
    }
    doc = PostingsReader::END;
    return doc;
  }

  int32_t docId() override { return doc; }

  float score() override { return 0.0f; }
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

  Query::Scorer* get(MemPool& targetPool, int64_t leadCost) override {
    unused(leadCost);
    return createDocSetScorer(targetPool, docs, segment);
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

} // namespace solux::QueryPrep
