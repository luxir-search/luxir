// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <format>
#include <string>
#include <string_view>

#include "MultiTermQuery.h"

namespace luxir {

// Matches documents with a term in `field` starting with `prefix`.  The prefix
// is used verbatim; an empty prefix matches documents that have at least one
// indexed term in the field.
// A scored hit receives one constant point (a prefix match has no per-term
// ranking); BooleanQuery suppresses that automatic constant in required position.
// A positive maxExpansions keeps only that many terms per segment, those with
// the highest document frequency (see MultiTermQuery::maxExpansions). The kept
// set depends only on the segment, so filter keys stay segment-stable.
class PrefixQuery final : public MultiTermQuery {
  std::string_view prefix;

public:
  PrefixQuery(std::string_view field, std::string_view prefix,
              int32_t maxExpansions = 0)
    : MultiTermQuery(QueryKind::PREFIX, field, maxExpansions), prefix(prefix) {}

  bool equalsSameKind(const Query& other) const override {
    const auto& rhs = static_cast<const PrefixQuery&>(other);
    return field == rhs.field && prefix == rhs.prefix
        && maxExpansions == rhs.maxExpansions;
  }

  uint64_t hashImpl() const override {
    uint64_t value = Query::hashImpl();
    value = mixHash(value, field);
    value = mixHash(value, prefix);
    return maxExpansions > 0 ? mixHash(value, maxExpansions) : value;
  }

  std::string describe() const override {
    return std::format("prefix query '{}' on field '{}'", prefix, field);
  }

  std::string_view getPrefix() const { return prefix; }

  FilterKeyScope appendFilterKey(FilterKeyBuilder& out,
                                 const FilterKeyContext& ctx) const override {
    out.appendKind(kind);
    unused(ctx);
    out.appendString(field);
    out.appendTerm(prefix);
    out.appendInt32(maxExpansions);
    return FilterKeyScope::SEGMENT_STABLE;
  }

  FilteredTermsEnum* createFilteredEnum(MemPool& pool, TermsEnum& te) override;
};

} // namespace luxir
