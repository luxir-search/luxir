// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "luxir/query/Query.h"

namespace luxir {

// A leaf-aligned relation between views. Only the implementation knows whether
// rows arrive in order or share physical storage.
class RowRelation {
public:
  IndexReader& many;
  IndexReader& one;
  RequestMemTracker& memory;

  enum class Direction { EXPAND, PROJECT };
  // A uniform child without a construction-time score quote supplies its
  // exact per-leaf contribution before membership reduction starts.
  enum class Reduction { NONE, MAXIMUM, CONSTANT };

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
      DocSet* domain, Reduction reduction, float multiplier) = 0;
  virtual DomainHandle expand(size_t leaf, DomainHandle domain) = 0;
  virtual DomainHandle project(size_t leaf, DomainHandle domain) = 0;
  virtual FilterKeyScope appendFilterKey(FilterKeyBuilder& out, const FilterKeyContext& context) const = 0;
};

} // namespace luxir
