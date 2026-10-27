// Copyright 2026 Luxir contributors
// SPDX-License-Identifier: Apache-2.0
#include "luxir/query/HasChildQuery.h"
#include "luxir/query/KnnQuery.h"
#include "luxir/query/QueryPrep.h"
#include "luxir/search/RelationReader.h"
#include "luxir/search/SearchRequest.h"
#include <map>

namespace luxir {
namespace {
using Key = std::pair<int32_t, int32_t>;
// Keep allocation accounting alive through prepared-result streaming. Standalone
// engine callers without a SearchRequest retain ordinary allocator behavior.
struct JoinStorage {
  RequestMemTracker* tracker;
  std::optional<RequestMemResource> tracked;
  size_t reserved = 0;
  explicit JoinStorage(RequestMemTracker* tracker) : tracker(tracker) {
    if (tracker) tracked.emplace(*tracker, "parent scores and witnesses", "parent-child state");
  }
  ~JoinStorage() { if (tracker) tracker->release(reserved); }
  std::pmr::memory_resource* resource() {
    return tracked ? &*tracked : std::pmr::new_delete_resource();
  }
  void reserve(size_t bytes) {
    if (tracker) tracker->charge(bytes, "parent-child state", "domains and parent hits");
    reserved += bytes;
  }
};
struct Matches {
  int64_t count = 0;
  double sum = 0;
  float minimum = std::numeric_limits<float>::max();
  float maximum = std::numeric_limits<float>::lowest();
  std::pmr::vector<Query::ChildMatch> witnesses;
  explicit Matches(std::pmr::memory_resource* resource) : witnesses(resource) {}
};
class JoinedWeight final : public Query::Weight {
  HasChildQuery& join;
  Query::Weight* child;
  Query::Weight* filter;
  float multiplier;
  class Prepared final : public Query::Weight::PreparedWeight {
    std::shared_ptr<JoinStorage> storage;
    KnnQuery::Weight::KnnPreparedWeight hits;
    std::pmr::map<Key, Matches> matches;
    api::HasChildQuery spec;
  public:
    Prepared(std::shared_ptr<JoinStorage> storage, std::vector<std::vector<KnnQuery::Hit>>&& hits,
        std::pmr::map<Key, Matches>&& matches, const api::HasChildQuery& spec)
        : storage(std::move(storage)), hits(std::move(hits)), matches(std::move(matches)), spec(spec) {}
    Query::Scorer* createScorer(MemPool& pool, IndexReader::Segment& segment) override {
      return hits.createScorer(pool, segment);
    }
    Query::ScorerSupplier* scorerSupplierImpl(MemPool& pool, IndexReader::Segment& segment,
        Query::SupplierExecutionMode mode) override { return hits.scorerSupplier(pool, segment, mode); }
    bool outputIsSubsetOfDomain() const noexcept override { return true; }
    size_t retainedBytes() const noexcept override {
      if (storage->tracker) return 0; // already charged; reset scopes must not double-charge
      size_t bytes = sizeof(*this) + hits.retainedBytes();
      for (const auto& [key, match] : matches) bytes += sizeof(key) + sizeof(match) + 4 * sizeof(void*)
          + match.witnesses.capacity() * sizeof(Query::ChildMatch);
      return bytes;
    }
    void visitChildMatches(int32_t segment, int32_t doc, const Query::ChildMatchVisitor& visitor) const override {
      if (!spec.inner_hits || spec.inner_hits->limit.value_or(1) == 0) return;
      auto found = matches.find({segment, doc});
      if (found != matches.end()) visitor(*spec.inner_hits, spec.relation, found->second.witnesses, found->second.count);
    }
  };
public:
  JoinedWeight(Query::Context& context, HasChildQuery& join, int32_t flags, float multiplier)
      : Query::Weight(context, join, flags), join(join),
        child(join.child->createWeight(context, flags | Query::NEED_SCORES)),
        filter(join.filter ? join.filter->createWeight(context, 0) : nullptr), multiplier(multiplier) {
    traits |= NEEDS_PREPARE;
  }
  Query::Scorer* createScorer(MemPool&, IndexReader::Segment&) override { throw std::logic_error("has_child requires prepare"); }
  Query::ScorerSupplier* scorerSupplierImpl(MemPool&, IndexReader::Segment&, Query::SupplierExecutionMode) override {
    throw std::logic_error("has_child requires prepare");
  }
  void releaseSegmentState(IndexReader::Segment& segment) noexcept override {
    child->releaseSegmentState(segment);
    if (filter) filter->releaseSegmentState(segment);
  }
  bool hasReleasableSegmentState() const noexcept override {
    return child->hasReleasableSegmentState() || (filter && filter->hasReleasableSegmentState());
  }
  std::unique_ptr<PreparedWeight> prepare(PrepareContext& ctx) override {
    auto* request = dynamic_cast<SearchRequest*>(ctx.warnings);
    auto* tracker = request ? &request->memoryTracker : nullptr;
    auto storage = std::make_shared<JoinStorage>(tracker);
    JoinStorage scratch(tracker);
    auto& topology = ctx.reader.relations();
    auto ordinal = topology.relationOrdinals.find(join.spec.relation);
    uint32_t relationId = ordinal == topology.relationOrdinals.end() ? 0 : ordinal->second;
    std::vector<std::unique_ptr<DocSet>> owned;
    std::vector<DocSet*> domains;
    for (auto& segment : ctx.reader.segments()) {
      // Covers sparse builder growth, conversion to bits, and the overlapping
      // old/new domains while applying a child filter.
      scratch.reserve((((size_t)segment.maxDoc() + 63) / 64 * 8) * 4 + 512);
      auto& relation = topology.segments[segment.ord];
      DocSetBuilder builder(segment.maxDoc());
      for (int32_t doc : relation.children) {
        auto parent = relation.parents[doc];
        if (parent.segment < 0 || relation.relation[doc] != relationId) continue;
        auto* allowed = ctx.domainPerSeg.empty() ? nullptr : ctx.domainPerSeg[parent.segment];
        if (!allowed || allowed->get(parent.doc)) builder.add(doc);
      }
      owned.push_back(builder.build());
      domains.push_back(owned.back().get());
    }
    PrepareContext childContext(ctx.reader, domains, ctx.parallel, ctx.warnings);
    auto prepare = [&](Query::Weight* weight) {
      QueryPrep::PreparedSource source;
      source.weight = weight;
      if (weight->needsPrepare()) source.setPrepared(weight->prepare(childContext));
      return source;
    };
    if (filter) {
      auto source = prepare(filter);
      for (auto& segment : ctx.reader.segments()) {
        auto guard = MemPool::threadLocalPoolGuard();
        auto* scorer = QueryPrep::createScorer(guard.pool(), segment, source.segmentSource());
        DocSetBuilder builder(segment.maxDoc());
        if (scorer) for (int32_t doc; (doc = scorer->next()) != PostingsReader::END; )
          if (domains[segment.ord]->get(doc)) builder.add(doc);
        owned[segment.ord] = builder.build();
        domains[segment.ord] = owned[segment.ord].get();
        source.releaseSegmentState(segment);
      }
    }
    auto source = prepare(child);
    std::pmr::map<Key, Matches> matches(storage->resource());
    const size_t limit = join.spec.inner_hits ? (size_t)join.spec.inner_hits->limit.value_or(1) : 0;
    auto better = [](const Query::ChildMatch& a, const Query::ChildMatch& b) {
      if (a.score != b.score) return a.score > b.score;
      return Key(a.segment, a.doc) < Key(b.segment, b.doc);
    };
    for (auto& segment : ctx.reader.segments()) {
      auto guard = MemPool::threadLocalPoolGuard();
      auto* scorer = QueryPrep::createScorer(guard.pool(), segment, source.segmentSource());
      if (scorer) for (int32_t doc; (doc = scorer->next()) != PostingsReader::END; ) {
        if (!domains[segment.ord]->get(doc)) continue;
        auto parent = topology.segments[segment.ord].parents[doc];
        auto [entry, inserted] = matches.try_emplace(Key(parent.segment, parent.doc), storage->resource());
        auto& match = entry->second;
        float score = scorer->score();
        ++match.count;
        match.sum += score;
        match.minimum = std::min(match.minimum, score);
        match.maximum = std::max(match.maximum, score);
        if (limit) {
          Query::ChildMatch witness{segment.ord, doc, score};
          auto at = std::lower_bound(match.witnesses.begin(), match.witnesses.end(), witness, better);
          if ((size_t)(at - match.witnesses.begin()) < limit) {
            match.witnesses.insert(at, witness);
            if (match.witnesses.size() > limit) match.witnesses.pop_back();
          }
        }
      }
      source.releaseSegmentState(segment);
    }
    storage->reserve(matches.size() * sizeof(KnnQuery::Hit)
        + ctx.reader.segments().size() * sizeof(std::vector<KnnQuery::Hit>) + sizeof(Prepared));
    std::vector<std::vector<KnnQuery::Hit>> hits(ctx.reader.segments().size());
    std::vector<size_t> counts(hits.size());
    for (const auto& [key, match] : matches) ++counts[key.first];
    for (size_t i = 0; i < hits.size(); ++i) hits[i].reserve(counts[i]);
    for (auto it = matches.begin(); it != matches.end();) {
      const auto& [key, match] = *it;
      if (match.count < join.spec.min_children.value_or(1) || match.count > join.spec.max_children.value_or(INT32_MAX)) {
        it = matches.erase(it); continue;
      }
      auto mode = join.spec.score_mode;
      float score = mode == "none" ? 1.0f : mode == "sum" ? (float)match.sum : mode == "avg" ? (float)(match.sum / match.count)
          : mode == "min" ? match.minimum : match.maximum;
      if (!std::isfinite(score)) throw RequestError("has_child score aggregation exceeds the finite float range");
      float weighted = Query::checkedBoostProduct(multiplier, score);
      hits[key.first].push_back({key.second, (inputFlags & Query::NEED_SCORES) ? weighted : 0.0f});
      ++it;
    }
    if (!limit) matches.clear();
    return std::make_unique<Prepared>(std::move(storage), std::move(hits), std::move(matches), join.spec);
  }
};
}
Query::Weight* HasChildQuery::createWeight(Context& context, int32_t flags, float multiplier) {
  return context.pool.make<JoinedWeight>(context, *this, flags, multiplier);
}
}
