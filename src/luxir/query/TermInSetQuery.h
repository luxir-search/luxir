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
    : MultiTermQuery(field), terms(terms) {}

  bool equals(const Query& other) const override {
    const auto* rhs = dynamic_cast<const TermInSetQuery*>(&other);
    return rhs != nullptr && field == rhs->field
        && terms.size() == rhs->terms.size()
        && std::equal(terms.begin(), terms.end(), rhs->terms.begin());
  }

  uint64_t hashImpl() const override {
    uint64_t value = mixHash(Query::hashImpl(), field);
    value = mixHash(value, terms.size());
    for (std::string_view term : terms) value = mixHash(value, term);
    return value;
  }

  std::span<const std::string_view> getTerms() const { return terms; }

  FilterKeyScope appendFilterKey(FilterKeyBuilder& out,
                                 const FilterKeyContext& ctx) const override {
    unused(ctx);
    out.appendTag(FilterKeyTag::ANY_OF);
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
