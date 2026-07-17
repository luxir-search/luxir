#pragma once

#include <string_view>

#include "MultiTermQuery.h"

namespace solux {

// Matches documents with a term in `field` starting with `prefix`.  The prefix
// is used verbatim; an empty prefix matches documents that have at least one
// indexed term in the field.
// A scored hit receives one constant point (a prefix match has no per-term
// ranking); BooleanQuery suppresses that automatic constant in required position.
class PrefixQuery final : public MultiTermQuery {
  std::string_view prefix;

public:
  PrefixQuery(std::string_view field, std::string_view prefix)
    : MultiTermQuery(field), prefix(prefix) {}

  std::string_view getPrefix() const { return prefix; }

  FilteredTermsEnum* createFilteredEnum(MemPool& pool, TermsEnum& te) override {
    return pool.make<PrefixTermsEnum>(te, prefix);
  }
};

} // namespace solux
