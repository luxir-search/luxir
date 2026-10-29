// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "luxir/query/ParseContext.h"
#include "luxir/query/PluginQueryBase.h"
#include "luxir/query/QueryRegistry.h"
#include "luxir/query/TermQuery.h"

namespace example {

// Two concrete query classes under one registered name, with the same value
// payload but different fields. Identity must include the concrete C++ type.
template <bool ById>
class FieldValueQuery final : public luxir::PluginQueryBase {
  luxir::TermQuery term;

  class Weight final : public luxir::Query::Weight {
    luxir::Query::Weight* child;

  public:
    Weight(Context& context, const FieldValueQuery& query, int32_t flags,
           luxir::Query::Weight* child)
      : luxir::Query::Weight(context, query, flags), child(child) {
      traits = child->getFlags();
    }

    ScorerSupplier* scorerSupplierImpl(
        luxir::MemPool& pool, luxir::IndexReader::Segment& segment,
        SupplierExecutionMode mode) override {
      return child->scorerSupplier(pool, segment, mode);
    }

    Scorer* createScorer(luxir::MemPool& pool, luxir::IndexReader::Segment& segment) override {
      return child->createScorer(pool, segment);
    }
  };

protected:
  bool equalsPayload(const PluginQueryBase& other) const override {
    return term.getTerm() == static_cast<const FieldValueQuery&>(other).term.getTerm();
  }

  uint64_t hashPayload() const override { return mixHash(0, term.getTerm()); }

  luxir::FilterKeyScope appendPayloadFilterKey(
      luxir::FilterKeyBuilder& out, const luxir::FilterKeyContext&) const override {
    out.appendString(term.getTerm());
    return luxir::FilterKeyScope::SEGMENT_STABLE;
  }

  void validateLogicalImpl(PlanningContext& context, float multiplier) const override {
    term.validateLogical(context, multiplier);
  }

public:
  FieldValueQuery(const luxir::PluginQueryContext& ctx, std::string_view value)
    : PluginQueryBase(ctx), term(ById ? "id" : "cat_s", value) {}

  bool recoverableAsPredicate() const noexcept override { return true; }

  luxir::Query::Weight* createWeight(Context& context, int32_t flags,
                                    float multiplier = 1.0f) override {
    return context.pool.make<Weight>(context, *this, flags, term.createWeight(context, flags, multiplier));
  }
};

using CategoryValueQuery = FieldValueQuery<false>;
using IdValueQuery = FieldValueQuery<true>;
static_assert(std::is_trivially_destructible_v<CategoryValueQuery>);
static_assert(std::is_trivially_destructible_v<IdValueQuery>);

inline luxir::PluginQueryDef fieldValueDef() {
  using Kind = luxir::PluginQueryArg::Kind;
  return {[](luxir::PluginQueryContext& ctx) -> luxir::Query* {
    auto value = ctx.params.requireString("value");
    bool byId = ctx.params.getBool("by_id", false);
    ctx.params.rejectUnknown();
    if (byId) return ctx.parse.pool.make<IdValueQuery>(ctx, value);
    return ctx.parse.pool.make<CategoryValueQuery>(ctx, value);
  }, {{"value", Kind::STRING}, {"by_id", Kind::BOOL}}, "value"};
}

// Deliberately omits the identity base to exercise the maker boundary.
class UnscopedQuery final : public luxir::Query {
  luxir::TermQuery term;

protected:
  void validateLogicalImpl(PlanningContext& context, float multiplier) const override {
    term.validateLogical(context, multiplier);
  }

public:
  explicit UnscopedQuery(std::string_view value) : Query(luxir::QueryKind::PLUGIN), term("id", value) {}

  luxir::FilterKeyScope appendFilterKey(
      luxir::FilterKeyBuilder& out, const luxir::FilterKeyContext& ctx) const override {
    return term.appendFilterKey(out, ctx);
  }

  luxir::Query::Weight* createWeight(Context& context, int32_t flags,
                                    float multiplier = 1.0f) override {
    return term.createWeight(context, flags, multiplier);
  }
};

inline luxir::PluginQueryDef unscopedDef() {
  using Kind = luxir::PluginQueryArg::Kind;
  return {[](luxir::PluginQueryContext& ctx) -> luxir::Query* {
    auto value = ctx.params.requireString("value");
    ctx.params.rejectUnknown();
    return ctx.parse.pool.make<UnscopedQuery>(value);
  }, {{"value", Kind::STRING}}, "value"};
}

}  // namespace example
