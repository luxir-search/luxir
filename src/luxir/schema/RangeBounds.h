// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "luxir/schema/FieldType.h"
#include "luxir/schema/ValCoerce.h"

#include <algorithm>
#include <array>

namespace luxir {

inline std::array<const api::Val*, 4> rangeBoundLiterals(
    const api::Val* gte, const api::Val* gt, const api::Val* lte, const api::Val* lt,
    std::string_view field) {
  std::array<const api::Val*, 4> bounds{gte, gt, lte, lt};
  for (auto*& bound : bounds) if (bound && coerce::isNull(*bound)) bound = nullptr;
  if (bounds[0] && bounds[1]) {
    throw RequestError(fmt::format("Range on '{}' sets both gte and gt", field), "invalid_value");
  }
  if (bounds[2] && bounds[3]) {
    throw RequestError(fmt::format("Range on '{}' sets both lte and lt", field), "invalid_value");
  }
  return bounds;
}

// Coerce every supplied bound before deciding emptiness. Each coercer returns
// an inclusive half-line and the fold retains its date-math diagnostics.
template <typename Coerce>
FieldType::IntWindow foldRangeBounds(
    const std::array<const api::Val*, 4>& bounds, FieldType::IntWindow domain,
    Coerce coerce) {
  using Use = FieldType::LiteralUse;
  constexpr Use uses[] = {Use::GTE, Use::GT, Use::LTE, Use::LT};
  constexpr std::string_view names[] = {"gte", "gt", "lte", "lt"};
  for (size_t i = 0; i < bounds.size(); i++) {
    if (!bounds[i]) continue;
    FieldType::IntWindow bound;
    try {
      bound = coerce(*bounds[i], uses[i]);
    } catch (const RequestError& e) {
      throw RequestError(fmt::format("{} (bound '{}')", e.what(), names[i]), e.code);
    }
    domain.lo = std::max(domain.lo, bound.lo);
    domain.hi = std::min(domain.hi, bound.hi);
    domain.granuleSkipped |= bound.granuleSkipped;
    domain.unroundedNow |= bound.unroundedNow;
  }
  return domain;
}

} // namespace luxir
