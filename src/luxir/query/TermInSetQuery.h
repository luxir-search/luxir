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
