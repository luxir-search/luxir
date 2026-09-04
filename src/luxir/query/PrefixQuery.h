#pragma once

#include <string_view>

#include "MultiTermQuery.h"

namespace luxir {

// Matches documents with a term in `field` starting with `prefix`.  The prefix
// is used verbatim; an empty prefix matches documents that have at least one
// indexed term in the field.
// A scored hit receives one constant point (a prefix match has no per-term
// ranking); BooleanQuery suppresses that automatic constant in required position.
class PrefixQuery final : public MultiTermQuery {
  std::string_view prefix;

public:
  PrefixQuery(std::string_view field, std::string_view prefix)
    : MultiTermQuery(QueryKind::PREFIX, field), prefix(prefix) {}

  bool equalsSameKind(const Query& other) const override {
    const auto& rhs = static_cast<const PrefixQuery&>(other);
    return field == rhs.field && prefix == rhs.prefix;
  }

  uint64_t hashImpl() const override {
    uint64_t value = Query::hashImpl();
    value = mixHash(value, field);
    return mixHash(value, prefix);
  }

  std::string_view getPrefix() const { return prefix; }

  FilterKeyScope appendFilterKey(FilterKeyBuilder& out,
                                 const FilterKeyContext& ctx) const override {
    out.appendKind(kind);
    unused(ctx);
    out.appendString(field);
    out.appendTerm(prefix);
    return FilterKeyScope::SEGMENT_STABLE;
  }

  FilteredTermsEnum* createFilteredEnum(MemPool& pool, TermsEnum& te) override {
    return pool.make<PrefixTermsEnum>(te, prefix);
  }
};

} // namespace luxir
