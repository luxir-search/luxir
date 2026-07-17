#pragma once

#include <optional>
#include <string_view>

#include "MultiTermQuery.h"

namespace solux {

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
    : MultiTermQuery(field), lower(lower), upper(upper),
      includeLower(includeLower), includeUpper(includeUpper) {}

  const std::optional<std::string_view>& getLower() const { return lower; }
  const std::optional<std::string_view>& getUpper() const { return upper; }
  bool lowerInclusive() const { return includeLower; }
  bool upperInclusive() const { return includeUpper; }

  FilteredTermsEnum* createFilteredEnum(MemPool& pool, TermsEnum& te) override {
    return pool.make<RangeTermsEnum>(te, lower, includeLower, upper, includeUpper);
  }
};

} // namespace solux
