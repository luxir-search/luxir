#pragma once

#include <optional>
#include <string_view>

#include "MultiTermQuery.h"

namespace luxir {

// Matches documents whose field has an indexed term in [lower, upper] under
// byte order.  Either end may be open (nullopt) or exclusive.  Constant
// scoring via the shared multi-term machinery - this is PrefixQuery with
// range endpoints instead of a shared prefix.
//
// Endpoints are used exactly as provided (query build normalizes/coerces
// them per field type before constructing the query); "byte order" means the
// terms-dictionary order, not any collation.
class TermRangeQuery final : public MultiTermQuery {
  std::optional<std::string_view> lower;
  std::optional<std::string_view> upper;
  bool includeLower;
  bool includeUpper;

public:
  TermRangeQuery(std::string_view field, std::optional<std::string_view> lower,
                 bool includeLower, std::optional<std::string_view> upper,
                 bool includeUpper)
    : MultiTermQuery(QueryKind::TERM_RANGE, field), lower(lower), upper(upper),
      includeLower(includeLower), includeUpper(includeUpper) {}

  bool equals(const Query& other) const override {
    if (other.getKind() != kind) return false;
    const auto& rhs = static_cast<const TermRangeQuery&>(other);
    return field == rhs.field && lower == rhs.lower
        && upper == rhs.upper && includeLower == rhs.includeLower
        && includeUpper == rhs.includeUpper;
  }

  uint64_t hashImpl() const override {
    uint64_t value = mixHash(Query::hashImpl(), field);
    value = mixHash(value, lower.has_value());
    if (lower.has_value()) value = mixHash(value, *lower);
    value = mixHash(value, includeLower);
    value = mixHash(value, upper.has_value());
    if (upper.has_value()) value = mixHash(value, *upper);
    return mixHash(value, includeUpper);
  }

  const std::optional<std::string_view>& getLower() const { return lower; }
  const std::optional<std::string_view>& getUpper() const { return upper; }
  bool lowerInclusive() const { return includeLower; }
  bool upperInclusive() const { return includeUpper; }

  FilterKeyScope appendFilterKey(FilterKeyBuilder& out,
                                 const FilterKeyContext& ctx) const override {
    out.appendKind(kind);
    unused(ctx);
    out.appendString(field);
    out.appendOptionalTerm(lower);
    out.appendBool(includeLower);
    out.appendOptionalTerm(upper);
    out.appendBool(includeUpper);
    return FilterKeyScope::SEGMENT_STABLE;
  }

  FilteredTermsEnum* createFilteredEnum(MemPool& pool, TermsEnum& te) override {
    return pool.make<RangeTermsEnum>(te, lower, includeLower, upper, includeUpper);
  }
};

} // namespace luxir
