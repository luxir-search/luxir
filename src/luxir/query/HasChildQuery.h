// Copyright 2026 Luxir contributors
// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "Query.h"

namespace luxir {
// One join over the reader's topology: which documents are candidates and
// which parent each maps to. Built once per prepare.
class JoinTopology {
public:
  virtual ~JoinTopology() = default;
  // The child documents of one segment eligible under the parent domain.
  virtual std::unique_ptr<DocSet> childDomain(IndexReader::Segment& segment, std::span<DocSet* const> parents) = 0;
  // The parent (segment, doc) of a child in segment.
  virtual std::pair<int32_t, int32_t> parentOf(int32_t segment, int32_t doc) = 0;
};

// Semijoin from child records to parents; subclasses supply the topology.
class JoinQuery : public Query {
public:
  Query* child;
  Query* filter;
  api::HasChildQuery spec;
  JoinQuery(Query* child, Query* filter, const api::HasChildQuery& spec)
      : Query(QueryKind::HAS_CHILD), child(child), filter(filter), spec(spec) {}
  virtual std::unique_ptr<JoinTopology> topology(IndexReader& reader) = 0;
  FilterKeyScope appendFilterKey(FilterKeyBuilder&, const FilterKeyContext&) const override {
    return FilterKeyScope::UNCACHEABLE;
  }
  void validateLogicalImpl(PlanningContext& context, float multiplier = 1.0f) const override {
    child->validateLogical(context, multiplier);
    if (filter) filter->validateLogical(context, 1.0f);
  }
  Weight* createWeight(Context& context, int32_t flags, float multiplier = 1.0f) override;
};

class HasChildQuery final : public JoinQuery {
public:
  using JoinQuery::JoinQuery;
  std::unique_ptr<JoinTopology> topology(IndexReader& reader) override;
};

// Block-join nested query: the child documents of path, mapped to their root
// by the segment's block layout.
class NestedQuery final : public JoinQuery {
public:
  std::string_view path;
  static api::HasChildQuery joinSpec(const api::NestedQuery& spec) {
    api::HasChildQuery join;
    join.relation = spec.path;
    join.score_mode = spec.score_mode.empty() ? std::string_view("avg") : spec.score_mode;
    join.inner_hits = spec.inner_hits;
    return join;
  }
  NestedQuery(Query* child, Query* filter, const api::NestedQuery& spec)
      : JoinQuery(child, filter, joinSpec(spec)), path(spec.path) {}
  std::unique_ptr<JoinTopology> topology(IndexReader& reader) override;
};
}
