// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <span>
#include <string_view>

#include "MultiTermQuery.h"

namespace luxir {

// Constant-score membership in an explicit sorted unique indexed-term set.
// The provided bytes are already field-native and are never analyzed or
// normalized here.
class TermInSetQuery final : public MultiTermQuery {
  std::span<const std::string_view> terms;

public:
  TermInSetQuery(std::string_view field,
                 std::span<const std::string_view> terms)
    : MultiTermQuery(QueryKind::TERM_IN_SET, field), terms(terms) {}

  bool equalsSameKind(const Query& other) const override {
    const auto& rhs = static_cast<const TermInSetQuery&>(other);
    return field == rhs.field && terms.size() == rhs.terms.size()
        && std::equal(terms.begin(), terms.end(), rhs.terms.begin());
  }

  uint64_t hashImpl() const override {
    uint64_t value = mixHash(Query::hashImpl(), field);
    return mixSampledSequence(
        value, terms,
        [](uint64_t seed, std::string_view term) {
          return mixHash(seed, term);
        });
  }

  std::span<const std::string_view> getTerms() const { return terms; }

  FilterKeyScope appendFilterKey(FilterKeyBuilder& out,
                                 const FilterKeyContext& ctx) const override {
    out.appendKind(kind);
    unused(ctx);
    out.appendString(field);
    out.appendSize(terms.size());
    for (std::string_view term : terms) out.appendTerm(term);
    return FilterKeyScope::SEGMENT_STABLE;
  }

  FilteredTermsEnum* createFilteredEnum(MemPool& pool,
                                        TermsEnum& te) override {
    return pool.make<ExplicitTermsEnum>(te, terms);
  }
};

} // namespace luxir
