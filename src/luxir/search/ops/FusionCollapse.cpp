// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include "FusionOp.h"
#include "CollapsedPage.h"

namespace luxir {
namespace {

class FusionMembers final : public Query::SegmentSource {
public:
  using Hit = std::pair<segdoc, float>;
  RequestMemResource resource;
  std::pmr::vector<Hit> hits;
  std::pmr::vector<std::span<const Hit>> segments;

  explicit FusionMembers(RequestMemTracker& tracker)
      : resource(tracker, "fused evidence", "fusion collapse"),
        hits(&resource), segments(&resource) {}

  class Scorer final : public Query::Scorer {
    std::span<const Hit> hits;
    int64_t position = -1;
  public:
    explicit Scorer(std::span<const Hit> hits) : hits(hits) {}
    int32_t docId() override {
      return position < 0 ? -1 : position == (int64_t)hits.size()
          ? PostingsReader::END : hits[(size_t)position].first.docId();
    }
    int32_t next() override {
      if (position < (int64_t)hits.size()) ++position;
      return docId();
    }
    int32_t advance(int32_t target) override {
      assert(target > docId());
      position = screaming::gallopLowerBound(position + 1, (int64_t)hits.size(), target,
          [&](int64_t i) { return hits[(size_t)i].first.docId(); });
      return docId();
    }
    float score() override { return hits[(size_t)position].second; }
  };

  class Supplier final : public Query::ScorerSupplier {
    std::span<const Hit> hits;
    class Plan final : public Query::ScorerPlan {
      std::span<const Hit> hits;
      Query::Scorer* buildScorer(MemPool& pool) override { return pool.make<Scorer>(hits); }
    public:
      Plan(const Query::PlanContext& context, std::span<const Hit> hits)
          : Query::ScorerPlan(context, {}, (int64_t)hits.size()), hits(hits) {}
    };
  public:
    explicit Supplier(std::span<const Hit> hits) : hits(hits) {}
    int64_t cost() override { return (int64_t)hits.size(); }
    Query::ScorerPlan* resolve(MemPool& pool, const Query::PlanContext& context) override {
      return pool.make<Plan>(context, hits);
    }
  };

  Query::ScorerSupplier* scorerSupplierImpl(MemPool& pool, IndexReader::Segment& segment,
      Query::SupplierExecutionMode mode) override {
    unused(mode);
    auto part = segments[(size_t)segment.ord];
    return part.empty() ? nullptr : pool.make<Supplier>(part);
  }
  Query::Scorer* createScorer(MemPool& pool, IndexReader::Segment& segment) override {
    auto part = segments[(size_t)segment.ord];
    return part.empty() ? nullptr : pool.make<Scorer>(part);
  }
};

class DiverseFusionCalc final : public FusionOp::Calc {
public:
  std::unique_ptr<ScoreCollector<true>> groups;
  std::unique_ptr<FusionMembers> members;
  api::GroupTable* pageGroups = nullptr;
  SortPlan rankSort;

  using FusionOp::Calc::Calc;

  void doFusion() override { doFusionImpl<true>(); }

  api::Val* getTargetForSub(SearchResponse* response, Calculator* child) override {
    auto& children = thisOp().groupOps;
    auto found = children.find(child->getOp().name);
    if (found != children.end() && found->second == &child->getOp()) {
      assert(pageGroups != nullptr && response == op.req.lastResponse);
      return build::opsSlot(pageGroups->ops, children.size(), child->getOp().name, response->mr);
    }
    return FusionOp::Calc::getTargetForSub(response, child);
  }

  void finishCollapsedFusion() override {
    auto& op = thisOp();
    groups = std::make_unique<ScoreCollector<true>>(op.topCount, op.collapse);
    members = std::make_unique<FusionMembers>(op.req.memoryTracker);
    members->hits.assign(fusedList.begin(), fusedList.end());
    std::sort(members->hits.begin(), members->hits.end(),
        [](const auto& a, const auto& b) { return a.first < b.first; });
    const auto& segments = op.req.reader->segments();
    auto cursor = members->hits.begin();
    for (int32_t segment = 0; segment < (int32_t)segments.size(); segment++) {
      auto begin = cursor;
      DocSetBuilder builder(segments[(size_t)segment].maxDoc());
      groups->setSegment(segment);
      while (cursor != members->hits.end() && cursor->first.segment() == segment) {
        auto [doc, score] = *cursor++;
        builder.add(doc.docId());
        groups->collect(segment, doc.docId(), score);
      }
      members->segments.emplace_back(begin, cursor);
      auto domain = builder.build();
      auto charge = std::make_shared<DomainVariantPlan::DomainCharge>(
          op.req.memoryTracker, domain->ramBytesUsed(), "fused domain", "fusion collapse");
      fusedDomains.push_back(DomainHandle(std::move(domain)).chargedWith(std::move(charge)));
    }
    // Ordinary fusion children retain the entire evidence union, including
    // documents excluded from the returned groups by missing: drop.
    task_group_fanout(fusedDomains.empty() ? nullptr : taskGroup, subCalcs.size(), [&](size_t i) {
      auto* child = subCalcs[i].get();
      return [this, child]() { child->calcAll(taskGroup, fusedDomains); };
    });
    bool enrich = op.fusionProto.collapse->get_number || op.headSort || !op.groupOps.empty();
    CollapsedPageSettings settings{op, *op.collapse, *op.fusionProto.collapse, rankSort,
        op.headSort, op.groupOps, op.fusionProto.fields, op.fusionProto.batch_size,
        op.fusionProto.offset, op.fusionProto.get_number, op.fusionProto.get_scores,
        op.fusionProto.document_format, enrich ? members.get() : nullptr,
        fusedDomains, pageGroups, true};
    settings.namedQueries = op.namedQueries;
    emitCollapsedPage(settings, *this, *groups);
  }
};

} // namespace

void FusionOp::Calc::appendCollapsedSource(MergeableCollector& collector, std::vector<RankedHead>& heads) {
  auto append = [&](auto& ranked) {
    auto rows = ranked.sort();
    heads.reserve(rows.size());
    for (const auto& row : rows) heads.push_back({row.doc, row.score});
  };
  if (collector.diverseFieldCollector) append(*collector.diverseFieldCollector);
  else if (collector.diverseScoreCollector) append(*collector.diverseScoreCollector);
  else if (collector.useFieldSort) append(*collector.groupFieldCollector);
  else append(*collector.groupScoreCollector);
}

void FusionOp::Calc::finishCollapsedFusion() { std::unreachable(); }

SearchOp::Calculator* FusionOp::createDiverseCalculator(
    Calculator* parent, int64_t slot, int64_t numSlots, const StructuralConstraint* structural) {
  return new DiverseFusionCalc(*this, parent, slot, numSlots, structural);
}

size_t FusionOp::collapsedResidentBytes() const {
  size_t candidates = 0;
  for (auto* source : sources) candidates = saturatingAdd(candidates, (size_t)source->topCount);
  candidates = std::min(candidates, (size_t)req.reader->maxDoc());
  size_t bytes = saturatingMultiply(candidates, sizeof(FusionMembers::Hit) + sizeof(int32_t));
  // The same bounded group heap as top_docs, plus exact candidate-union totals.
  bytes = saturatingAdd(bytes, saturatingMultiply(
      std::min(candidates, (size_t)topCount), 2 * (sizeof(CollapseKey) + sizeof(size_t))));
  for (const auto& [name, child] : groupOps) {
    unused(name);
    bytes = saturatingAdd(bytes, child->facetBucketResidentBytes());
  }
  return bytes;
}

} // namespace luxir
