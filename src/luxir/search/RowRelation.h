// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "luxir/query/Query.h"

namespace luxir {

// How a family's matching rows reduce to its owner: a score and an inclusive
// range for the number of matching rows. The first five kinds are the join's
// score modes. A weight resolves NONE before a relation sees it: FIXED scores
// every owner with the multiplier itself, and CONSTANT reads a uniform
// child's exact score once per leaf.
struct RowReduction {
  enum Kind { NONE, MAXIMUM, MINIMUM, SUM, AVERAGE, CONSTANT, FIXED };
  Kind kind = MAXIMUM;
  int32_t min = 1;
  int32_t max = std::numeric_limits<int32_t>::max();

  bool operator==(const RowReduction&) const = default;
  bool bounded() const { return min != 1 || max != std::numeric_limits<int32_t>::max(); }
  bool accepts(int64_t count) const { return count >= min && count <= max; }
  int64_t decidingCount() const {
    return max == std::numeric_limits<int32_t>::max() ? min : (int64_t)max + 1;
  }
};

// A leaf-aligned relation between views. Only the implementation knows whether
// rows arrive in order or share physical storage.
class RowRelation {
public:
  IndexReader& many;
  IndexReader& one;
  RequestMemTracker& memory;

  enum class Direction { EXPAND, PROJECT };

  struct Mapping {
    RowRelation* relation = nullptr;
    Direction direction = Direction::PROJECT;

    IndexReader& source() const {
      return direction == Direction::EXPAND ? relation->one : relation->many;
    }
    DomainHandle apply(size_t leaf, DomainHandle domain) const {
      return direction == Direction::EXPAND ? relation->expand(leaf, std::move(domain))
          : relation->project(leaf, std::move(domain));
    }
    DomainHandle unapply(size_t leaf, DomainHandle domain) const {
      return direction == Direction::EXPAND ? relation->project(leaf, std::move(domain))
          : relation->expand(leaf, std::move(domain));
    }
  };

  RowRelation(IndexReader& many, IndexReader& one, RequestMemTracker& memory)
      : many(many), one(one), memory(memory) {
    assert(many.segments().size() == one.segments().size());
  }
  virtual ~RowRelation() = default;

  // Construction dispatch happens once per leaf. The returned supplier owns the
  // concrete mapping algorithm; no mapping dispatch is added to row iteration.
  virtual Query::ScorerSupplier* reduce(MemPool& pool, size_t leaf, Query::ScorerSupplier& input,
      DocSet* domain, RowReduction reduction, float multiplier) = 0;
  virtual DomainHandle expand(size_t leaf, DomainHandle domain) = 0;
  virtual DomainHandle project(size_t leaf, DomainHandle domain) = 0;
  virtual FilterKeyScope appendFilterKey(FilterKeyBuilder& out, const FilterKeyContext& context) const = 0;
};

} // namespace luxir
