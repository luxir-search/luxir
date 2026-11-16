// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include "OwnershipRelation.h"
#include "DomainVariantPlan.h"
#include "luxir/query/QueryPrep.h"

namespace luxir {

class OwnershipRelation::Scorer final : public Query::Scorer {
  Query::Scorer& child;
  const OwnershipMapper& mapper;
  OwnershipMapper::Cursor cursor;
  QueryPrep::DocSetScorer* domain;
  int32_t ownerCount;
  bool maximum;
  float multiplier;
  int32_t childDoc;
  int32_t current = -1;
  float value = 0;
  std::span<uint64_t> childBits;
  std::span<int64_t> boundaries;

  void fillOwnerWindow(std::span<uint64_t> words, int32_t start, int32_t end) {
    auto run = boundaries.first((size_t)(end - start) + 1);
    mapper.readBoundaries(start, end, run);
    int32_t row = (int32_t)run.front();
    int32_t last = (int32_t)run.back();
    // ends[i] is one past the last child row of owner start + i.
    const int64_t* ends = run.data() + 1;
    uint64_t* out = words.data();
    auto* eligible = domain;
    size_t owner = 0;
    // Marks the owner of a matching child row and returns the end of its
    // family. Owners only move forward within the window.
    auto familyEnd = [&](int32_t match) {
      while (ends[owner] <= match) owner++;
      int32_t doc = start + (int32_t)owner;
      if (!eligible || eligible->advanceWindow(doc, doc + 1) == doc) out[owner >> 6] |= 1ULL << (owner & 63);
      return (int32_t)ends[owner++];
    };
    if (childBits.empty()) {
      while ((row = child.advanceWindow(row, last)) < last) row = familyEnd(row);
      return;
    }
    uint64_t* bits = childBits.data();
    while (row < last) {
      int32_t limit = row + std::min(DocsEnumMeta::L1_DOCS, last - row);
      size_t wordCount = (size_t)(limit - row + 63) / 64;
      std::fill(bits, bits + wordCount, 0);
      child.fillWindowBits({bits, wordCount}, row, limit);
      int32_t next = row;
      size_t word = 0;
      uint64_t matches = bits[0];
      for (;;) {
        while (!matches && ++word < wordCount) matches = bits[word];
        if (!matches) break;
        int32_t match = row + (int32_t)(word * 64) + (int32_t)std::countr_zero(matches);
        // One matching child proves the family. Skip its remaining bits,
        // including any later child windows it spans.
        next = familyEnd(match);
        if (next >= limit) break;
        size_t offset = (size_t)(next - row);
        if ((offset >> 6) == word) {
          matches &= ~0ULL << (offset & 63);
        } else {
          word = offset >> 6;
          matches = bits[word] & (~0ULL << (offset & 63));
        }
      }
      // Only an empty fill needs a seek across a gap. Dense fills retain
      // the child's bulk decoding without a point probe before each one.
      row = next == row ? child.advanceWindow(limit, last) : std::max(limit, next);
    }
  }

public:
  Scorer(MemPool& pool, Query::Scorer& child, const OwnershipMapper& mapper,
         DocSet* domain, int32_t ownerCount, bool maximum, float multiplier,
         const Query::ScorerShape& childShape)
      : child(child), mapper(mapper), cursor(mapper, pool),
        domain(domain ? pool.make<QueryPrep::DocSetScorer>(domain, ownerCount) : nullptr),
        ownerCount(ownerCount), maximum(maximum), multiplier(multiplier), childDoc(child.docId()),
        childBits(!maximum && childShape.windowFillClause == Query::ClauseShape::DIRECT
            ? pool.make_span<uint64_t>(DocsEnumMeta::L1_DOCS / 64) : std::span<uint64_t>{}),
        boundaries(!maximum ? pool.make_span<int64_t>((size_t)std::min(ownerCount, DocsEnumMeta::L1_DOCS) + 1)
            : std::span<int64_t>{}) {}

  int32_t docId() override { return current; }
  float score() override { return value; }

  void setMinCompetitiveScore(float floor) override {
    if (maximum && multiplier > 0.0f) {
      child.setMinCompetitiveScore(Query::unscaleScoreFloor(floor, multiplier));
    }
  }

  int32_t next() override {
    return current == PostingsReader::END ? current : advance(current + 1);
  }

  int32_t advance(int32_t target) override {
    assert(target > current);
    if (domain) {
      target = domain->docId() < target ? domain->advance(target) : domain->docId();
    }
    if (target >= ownerCount) return current = PostingsReader::END;
    auto family = cursor.seek(target);
    int32_t doc = childDoc;
    if (doc < family.first) doc = child.advance(family.first);
    while (doc != PostingsReader::END) {
      // A child inside the sought family needs no lookup.
      if (doc >= family.end) family = cursor.owner(doc);
      int32_t owner = family.owner;
      if (domain) {
        int32_t eligible = domain->docId() < owner ? domain->advance(owner) : domain->docId();
        if (eligible != owner) {
          if (eligible >= ownerCount) return current = PostingsReader::END;
          family = cursor.seek(eligible);
          doc = child.advance(family.first);
          continue;
        }
      }
      value = maximum ? child.score() : 1.0f;
      if (maximum) {
        while ((doc = child.next()) < family.end) value = std::max(value, child.score());
      }
      value *= multiplier;
      // A competitive next() can finish without moving the postings cursor.
      childDoc = doc;
      return current = owner;
    }
    return current = PostingsReader::END;
  }

  int32_t advanceWindow(int32_t min, int32_t max) override {
    if (maximum) return Query::Scorer::advanceWindow(min, max);
    if (min >= max) return max;
    if (min >= ownerCount) return current = PostingsReader::END;
    max = std::min(max, ownerCount);
    if (domain) min = domain->advanceWindow(min, max);
    if (min >= max) return current = min;
    int32_t end = cursor.firstChild(max);
    int32_t row = cursor.firstChild(min);
    while ((row = child.advanceWindow(row, end)) < end) {
      auto family = cursor.owner(row);
      if (!domain || domain->advanceWindow(family.owner, family.owner + 1) == family.owner) {
        value = multiplier;
        return current = family.owner;
      }
      int32_t owner = domain->advanceWindow(family.owner + 1, max);
      if (owner >= max) return current = owner;
      row = cursor.firstChild(owner);
    }
    return current = max == ownerCount ? PostingsReader::END : max;
  }

  void fillWindowBits(std::span<uint64_t> words, int32_t start, int32_t end) override {
    if (maximum) return Query::Scorer::fillWindowBits(words, start, end);
    assert(start >= 0 && start <= end && end <= ownerCount);
    skipCount(SkipStats::countBulkFillCalls);
    if (start == end) return;
    current = end == ownerCount ? PostingsReader::END : end;
    for (int32_t first = start; first < end;) {
      int32_t limit = first + std::min(DocsEnumMeta::L1_DOCS, end - first);
      fillOwnerWindow(words.subspan((size_t)(first - start) / 64), first, limit);
      first = limit;
    }
  }

  [[gnu::cold]] void prepareMembership() override {
    if (!maximum) child.prepareMembership();
  }
};

class OwnershipRelation::Supplier final : public Query::ScorerSupplier {
  Query::ScorerSupplier& child;
  const OwnershipMapper& mapper;
  DocSet* domain;
  int32_t ownerCount;
  int32_t childCount;
  bool maximum;
  float multiplier;

  // hugin, 5M structured roots, 2026-11-16: mapping a filled owner costs
  // 4.4 ns; pulling costs 45-90 ns per body candidate. Use a nominal four
  // instructions/ns to compare with MembershipWork, charging ranked probes
  // at the upper end and count probes at the lower end. Child production
  // is additional, so expensive children move the crossover toward pull.
  static constexpr double OWNER_FILL_INSTRUCTIONS = 18;
  static constexpr double RANKED_PROBE_INSTRUCTIONS = 360;
  static constexpr double COUNT_PROBE_INSTRUCTIONS = 180;

  class Plan final : public Query::ScorerPlan {
    Supplier& supplier;
    Query::ScorerPlan& child;

    Query::Scorer* buildScorer(MemPool& pool) override {
      auto* scorer = child.build(pool);
      return scorer ? pool.make<Scorer>(pool, *scorer, supplier.mapper, supplier.domain,
          supplier.ownerCount, supplier.maximum, supplier.multiplier, child.shape()) : nullptr;
    }

  public:
    Plan(Supplier& supplier, const Query::PlanContext& context, const Query::ScorerShape& shape,
         Query::ScorerPlan& child)
        : Query::ScorerPlan(context, shape, supplier.cost()),
          supplier(supplier), child(child) {}
  };

  // The child is built for the windows this scorer walks, whatever the
  // owner side consumes.
  static Query::PlanContext childContext(Query::PlanContext context) {
    context.membershipConsumption = Query::MembershipConsumption::WINDOWS;
    return context;
  }

  // An exact scorer exactly when the child has one.
  //
  // The join picks its own arm here, from the demand its consumer quotes,
  // because the Boolean's filter gates compare cardinalities and do not read
  // what a filter costs to produce: a join advertised as a filler
  // unconditionally is filled over the whole span beside a selective body.
  // NumericPredicateQuery's supplier selects its arm the same way.
  //
  // Known limits of deciding on this side:
  //  - The probe constants are the consumer's cost per candidate, guessed
  //    from here.
  //  - The candidate estimate is the body's cost, not what survives pruning,
  //    so ranked bodies that prune hard (a required clause with boosts, an
  //    intersection of unions) can fill where probing would be cheaper.
  //  - Consumers quote span differently (an exclusion quotes its candidates,
  //    a window fill quotes windows), which the exposure estimate below
  //    papers over.
  //  - The choice is only filler or not; what the consumer does with a
  //    non-filler is not this supplier's to pick.
  // The decision belongs in the consumer once it prices fillers: this
  // supplier would then quote fill and probe work and select nothing.
  Query::ScorerShape selectScorer(const Query::PlanContext& context) const {
    Query::ScorerShape shape;
    shape.matchState = child.describeScorer(childContext(context)).matchState;
    shape.directKind = Query::DirectScorerKind::OTHER;
    shape.reportedTwoPhase = Query::ReportedTwoPhase::NO;
    shape.windowFillClause = Query::ClauseShape::NONE;
    shape.termDisjunctionClause = shape.termConjunctionClause = Query::ClauseShape::NONE;
    shape.independentTerm = Query::IndependentTermAccess::UNSUPPORTED;
    shape.docsOnly = Query::DocsOnlyAccess::UNSUPPORTED;
    shape.directDocSet = Query::DirectDocSetAccess::UNSUPPORTED;
    shape.membershipConsumption = context.membershipConsumption == Query::MembershipConsumption::EITHER
        ? Query::MembershipConsumption::WINDOWS : context.membershipConsumption;
    if (maximum || ownerCount == 0) return shape;
    const auto& demand = context.demand;
    double candidates = demand.expectedCandidates;
    if (candidates < 0) {
      if (demand.candidates == std::numeric_limits<int64_t>::max()) return shape;
      candidates = (double)demand.candidates;
    }
    if (!std::isfinite(candidates)) return shape;
    candidates = std::clamp(candidates, 0.0, (double)std::min<int64_t>(demand.candidates, ownerCount));
    // The context has a span, not its positions. Estimate the mapped child
    // exposure at the same fraction; never inspect ownership during planning.
    // A fill covers whole windows, and each opportunity can expose one,
    // whatever span the consumer quoted for its own cursor.
    int64_t exposed = std::min<int64_t>(demand.candidates, ownerCount) * DocsEnumMeta::L1_DOCS;
    double owners = (double)std::clamp<int64_t>(
        std::min(std::max(demand.span, exposed), context.expectedSpan), 0, ownerCount);
    double production = child.membershipWork(childCount).produce;
    double fill = OWNER_FILL_INSTRUCTIONS * owners + production * owners / ownerCount;
    double probe = candidates * (demand.horizon == Query::ExecutionUse::COUNT_WINDOWS
        ? COUNT_PROBE_INSTRUCTIONS : RANKED_PROBE_INSTRUCTIONS);
    if (fill < probe) shape.windowFillClause = Query::ClauseShape::DIRECT;
    return shape;
  }

public:
  Supplier(Query::ScorerSupplier& child, const OwnershipMapper& mapper,
           DocSet* domain, int32_t ownerCount, int32_t childCount, bool maximum, float multiplier)
      : child(child), mapper(mapper), domain(domain), ownerCount(ownerCount), childCount(childCount),
        maximum(maximum), multiplier(multiplier) {}

  int64_t cost() override { return std::min(child.cost(), (int64_t)ownerCount); }

  Query::ScorerShape describeScorer(const Query::PlanContext& context) const override {
    return selectScorer(context);
  }
  Query::UnresolvedSupplierCause unresolvedScorerCause(const Query::PlanContext& context) const override {
    return child.unresolvedScorerCause(childContext(context));
  }
  bool fillExpansionMemo(const Query::PlanContext& context) override {
    return child.fillExpansionMemo(childContext(context));
  }
  Query::ScorerPlan* resolve(MemPool& pool, const Query::PlanContext& context) override {
    return pool.make<Plan>(*this, context, selectScorer(context),
        *child.resolve(pool, childContext(context)));
  }
};

OwnershipRelation::OwnershipRelation(IndexReader& many, IndexReader& one, RequestMemTracker& memory)
    : RowRelation(many, one, memory) {
  auto path = many.path();
  if (path == one.path()) throw RequestError("domain transform requires different spaces");
  while (!path.empty() && path != one.path()) path = spaceParentPath(path);
  if (path != one.path()) throw RequestError("domain transform requires ancestor and descendant paths");
  mappers.reserve(many.segments().size());
  for (size_t leaf = 0; leaf < many.segments().size(); leaf++) {
    auto& child = many.segments()[leaf];
    assert(&child.segmentCore() == &one.segments()[leaf].segmentCore());
    if (child.hasSpace()) mappers.emplace_back(std::in_place, child.segmentCore(), one.path(), many.path());
    else mappers.emplace_back(std::nullopt);
  }
}

Query::ScorerSupplier* OwnershipRelation::reduce(MemPool& pool, size_t leaf,
    Query::ScorerSupplier& input, DocSet* domain, bool maximum, float multiplier) {
  return mappers[leaf] ? pool.make<Supplier>(input, *mappers[leaf], domain,
      one.segments()[leaf].maxDoc(), many.segments()[leaf].maxDoc(), maximum, multiplier) : nullptr;
}

FilterKeyScope OwnershipRelation::appendFilterKey(FilterKeyBuilder& out, const FilterKeyContext&) const {
  out.appendByte(0); // ownership relation
  out.appendString(many.path());
  out.appendString(one.path());
  return FilterKeyScope::SEGMENT_STABLE;
}

DomainHandle OwnershipRelation::expand(size_t leaf, DomainHandle domain) {
  auto& source = one.segments()[leaf];
  auto& destination = many.segments()[leaf];
  domain.assertScope(source);
  const auto& mapper = mappers[leaf];
  if (mapper) {
    if (!domain.get()) return DomainHandle().scoped(destination);
    if (domain.get() == source.liveDocSet()) {
      return DomainHandle::pinned(destination.liveDocSet(), destination.liveDocsShared()).scoped(destination);
    }
  }
  DocSetBuilder builder(destination.maxDoc());
  if (mapper && domain.get()) {
    domain.get()->forEach([&](int32_t row) {
      auto [first, end] = mapper->range(row);
      builder.addRange(first, end);
    });
  }
  return DomainVariantPlan::account(DomainHandle(builder.build()).scoped(destination), memory, "mapped domain");
}

DomainHandle OwnershipRelation::project(size_t leaf, DomainHandle domain) {
  auto& source = many.segments()[leaf];
  auto& destination = one.segments()[leaf];
  domain.assertScope(source);
  const auto& mapper = mappers[leaf];
  DocSetBuilder builder(destination.maxDoc());
  if (mapper && source.maxDoc()) {
    MemPool pool;
    OwnershipMapper::Cursor cursor(*mapper, pool);
    if (domain.get()) {
      int32_t lastOwner = -1;
      domain.get()->forEach([&](int32_t row) {
        int32_t owner = cursor.owner(row).owner;
        if (owner != lastOwner) builder.add(owner);
        lastOwner = owner;
      });
    } else {
      // All children still project to owners with nonempty families only.
      for (int32_t child = 0; child < source.maxDoc();) {
        auto family = cursor.owner(child);
        builder.add(family.owner);
        child = family.end;
      }
    }
  }
  return DomainVariantPlan::account(DomainHandle(builder.build()).scoped(destination), memory, "mapped domain");
}

} // namespace luxir
