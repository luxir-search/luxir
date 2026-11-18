// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "RowRelation.h"
#include "LeafOwnershipMappers.h"

namespace luxir {

class OwnershipRelation final : public RowRelation {
  LeafOwnershipMappers mappers;

  class Scorer;
  class Supplier;

public:
  OwnershipRelation(IndexReader& many, IndexReader& one, RequestMemTracker& memory);

  // Null when the leaf has no rows in the descendant space; binds on first use.
  const OwnershipMapper* mapper(size_t leaf) const { return mappers.get(leaf); }

  Query::ScorerSupplier* reduce(MemPool& pool, size_t leaf, Query::ScorerSupplier& input,
      DocSet* domain, RowReduction reduction, float multiplier) override;
  DomainHandle expand(size_t leaf, DomainHandle domain) override;
  DomainHandle project(size_t leaf, DomainHandle domain) override;
  FilterKeyScope appendFilterKey(FilterKeyBuilder& out, const FilterKeyContext& context) const override;
};

} // namespace luxir
