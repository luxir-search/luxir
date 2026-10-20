// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

#include "Query.h"

namespace luxir {

// match_phrase_prefix: a phrase whose last slot is a prefix. At weight
// creation the prefix expands to the first maxExpansions indexed terms in
// term order (merged across segments), and the query rewrites to a
// PhraseQuery whose last slot holds that term set, so the phrase is matched
// once over a union of the expansions' postings rather than once per
// expansion. A single-token input (no leading terms) rewrites to a
// disjunction of the expansions, and an empty expansion to no match.
class PhrasePrefixQuery final : public Query {
  std::string_view field;
  std::span<std::string_view> terms;   // the leading (non-prefix) terms
  std::span<const int32_t> positions;  // terms' positions, then the prefix's
  std::string_view prefix;
  int32_t slop;
  int32_t maxExpansions;

public:
  // Default expansion cap (Elasticsearch parity).
  static constexpr int32_t DEFAULT_MAX_EXPANSIONS = 50;

  struct Expansion {
    std::string_view term;
    Similarity::TermStats termStats = {};
  };

  PhrasePrefixQuery(std::string_view field, std::span<std::string_view> terms,
                    std::span<const int32_t> positions, std::string_view prefix,
                    int32_t slop, int32_t maxExpansions)
      : Query(QueryKind::PHRASE_PREFIX), field(field), terms(terms),
        positions(positions), prefix(prefix), slop(slop),
        maxExpansions(maxExpansions) {
    assert(positions.size() == terms.size() + 1);
    assert(slop >= 0);
    assert(maxExpansions > 0);
  }

  bool recoverableAsPredicate() const noexcept override { return true; }

  bool equalsSameKind(const Query& other) const override {
    const auto& rhs = static_cast<const PhrasePrefixQuery&>(other);
    return field == rhs.field && prefix == rhs.prefix && slop == rhs.slop
        && maxExpansions == rhs.maxExpansions
        && std::ranges::equal(terms, rhs.terms)
        && std::ranges::equal(positions, rhs.positions);
  }

  uint64_t hashImpl() const override {
    uint64_t value = mixHash(Query::hashImpl(), field);
    value = mixHash(value, prefix);
    value = mixHash(value, slop);
    value = mixHash(value, maxExpansions);
    value = mixSampledSequence(
        value, terms,
        [](uint64_t seed, std::string_view term) {
          return mixHash(seed, term);
        });
    return mixSampledSequence(
        value, positions,
        [](uint64_t seed, int32_t position) {
          return mixHash(seed, position);
        });
  }

  [[nodiscard]] std::string_view getField() const { return field; }
  [[nodiscard]] std::span<std::string_view> getTerms() const { return terms; }
  [[nodiscard]] std::span<const int32_t> getPositions() const { return positions; }
  [[nodiscard]] std::string_view getPrefix() const { return prefix; }
  [[nodiscard]] int32_t getSlop() const { return slop; }
  [[nodiscard]] int32_t getMaxExpansions() const { return maxExpansions; }

  // The expansion set depends on every segment's dictionary, so the key is
  // scoped to the reader core, like FuzzyQuery's.
  FilterKeyScope appendFilterKey(FilterKeyBuilder& out,
                                 const FilterKeyContext& ctx) const override {
    out.appendKind(kind);
    unused(ctx);
    out.appendString(field);
    out.appendSize(terms.size());
    for (std::string_view term : terms) out.appendTerm(term);
    out.appendSize(positions.size());
    for (int32_t position : positions) out.appendInt32(position);
    out.appendTerm(prefix);
    out.appendInt32(slop);
    out.appendInt32(maxExpansions);
    return FilterKeyScope::CORE_STABLE;
  }

  // The first maxExpansions indexed terms starting with the prefix, in term
  // order and deduplicated across segments, with their summed stats. Term
  // bytes live in `pool`.
  std::vector<Expansion> expand(Context& context, MemPool& pool) const;

  // The executable form; see the class comment.
  Query* rewrite(Context& context) const;

  Query::Weight* createWeight(Context& context, int32_t flags,
                              float multiplier = 1.0f) override;
};

} // namespace luxir
