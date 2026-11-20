// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <string_view>

namespace luxir {
class Query;

class QueryVisitor {
public:
  enum class Occur : uint8_t { MUST, SHOULD, FILTER, MUST_NOT };
  // Indexed term, with the product of path boosts and term/field weights.
  // Meaningful only through scoring occurrences. Combined-field weights
  // affect frequency before saturation, not the final score linearly.
  // Views are borrowed from the query/context pool.
  virtual void term(std::string_view field, std::string_view text, float boost) = 0;
  // Select the visitor for children under this occurrence, or null to skip.
  // The default visits everything except MUST_NOT.
  virtual QueryVisitor* sub(Occur occur, const Query& /*parent*/) {
    return occur == Occur::MUST_NOT ? nullptr : this;
  }
  // Terms cannot be enumerated request-wide; inspection is incomplete.
  virtual void leaf(const Query& /*query*/) {}
protected:
  ~QueryVisitor() = default;
};
} // namespace luxir
