// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "RowRelation.h"
#include "luxir/reader/OwnershipMapper.h"

namespace luxir {

class OwnershipRelation final : public RowRelation {
  std::vector<std::optional<OwnershipMapper>> mappers;

  class Scorer;
  class Supplier;

public:
  OwnershipRelation(IndexReader& many, IndexReader& one, RequestMemTracker& memory);

  Query::ScorerSupplier* reduce(MemPool& pool, size_t leaf, Query::ScorerSupplier& input,
      DocSet* domain, Reduction reduction, float multiplier) override;
  DomainHandle expand(size_t leaf, DomainHandle domain) override;
  DomainHandle project(size_t leaf, DomainHandle domain) override;
  FilterKeyScope appendFilterKey(FilterKeyBuilder& out, const FilterKeyContext& context) const override;
};

} // namespace luxir
