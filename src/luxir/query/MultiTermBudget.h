// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <format>
#include <limits>
#include <string>
#include <string_view>

#include "luxir/util/ApiError.h"

namespace luxir {

// Server-side guards on term-dictionary expansion (prefix, wildcard, regex,
// string range, fuzzy). A multi-term query's cost grows with the field's
// dictionary, not with the request, so one short pattern on a
// high-cardinality field can hold a core for as long as the scan runs. These
// limits bound that work per request and a request cannot raise them. Zero in
// any field means unlimited (an operator decision; a request can never ask
// for it).
struct MultiTermLimits {
  // Terms one segment of one query may expand to before the request is
  // rejected. A prefix query with a positive max_expansions keeps only its
  // top terms (at most MultiTermQuery::Weight::MAX_LAZY_TERMS), so it never
  // reaches this cap; every other expansion is complete and is held to it.
  int32_t maxExpansions = 65'536;
  // Dictionary terms all multi-term and fuzzy scans of one request may visit,
  // matching or not. This is what bounds a zero-hit `*x*` scan. HARDWARE
  // SENSITIVE: a leading-wildcard scan visits ~11 ns/term on a laptop
  // (BM_AutomatonEnum), so the default is ~100 ms of one core.
  int64_t maxTermsExamined = 1 << 23;
  // Postings all multi-term expansions of one request may union (summed
  // docFreq of the accepted terms).
  int64_t maxPostings = 1 << 30;
  // A pattern with an unbounded language and fewer literal leading bytes than
  // this is costed as a scan of its whole literal-prefix range, and rejected
  // before scanning when that range is larger than the remaining budget.
  int32_t minPrefixLength = 1;
};

// Request-wide dictionary work allowance shared by every multi-term query in
// one request, across planning contexts, weights, and segments. Charges are
// atomic so concurrent segment tasks may share it; a scan reads the remaining
// allowance when it starts, so concurrent scans can overshoot by at most one
// scan each.
class MultiTermBudget {
  static constexpr int64_t UNLIMITED = std::numeric_limits<int64_t>::max();

  MultiTermLimits limits;
  std::atomic<int64_t> examined{0};
  std::atomic<int64_t> postings{0};

  static int64_t remaining(int64_t limit, const std::atomic<int64_t>& used) {
    if (limit <= 0) return UNLIMITED;
    return std::max<int64_t>(0, limit - used.load(std::memory_order_relaxed));
  }

public:
  explicit MultiTermBudget(MultiTermLimits limits = {}) : limits(limits) {}

  const MultiTermLimits& getLimits() const noexcept { return limits; }

  // The per-segment cap on a complete expansion.
  int32_t maxExpansions() const noexcept {
    return limits.maxExpansions > 0 ? limits.maxExpansions
                                    : std::numeric_limits<int32_t>::max();
  }

  int64_t remainingTermsExamined() const noexcept {
    return remaining(limits.maxTermsExamined, examined);
  }

  int64_t remainingPostings() const noexcept {
    return remaining(limits.maxPostings, postings);
  }

  int64_t termsExamined() const noexcept {
    return examined.load(std::memory_order_relaxed);
  }

  int64_t postingsCharged() const noexcept {
    return postings.load(std::memory_order_relaxed);
  }

  // Record a scan's visited terms or accepted postings. False when the
  // request total passed its limit; the caller throws with its description.
  [[nodiscard]] bool chargeTermsExamined(int64_t terms) noexcept {
    int64_t total = examined.fetch_add(terms, std::memory_order_relaxed) + terms;
    return limits.maxTermsExamined <= 0 || total <= limits.maxTermsExamined;
  }

  [[nodiscard]] bool chargePostings(int64_t count) noexcept {
    int64_t total = postings.fetch_add(count, std::memory_order_relaxed) + count;
    return limits.maxPostings <= 0 || total <= limits.maxPostings;
  }

  [[noreturn]] void throwTermsExamined(std::string_view what) const {
    throw ResourceExhaustedError(
        std::format("{} visits too many dictionary terms: the request may "
                    "examine at most {} terms across all prefix, wildcard, "
                    "regex, range, and fuzzy expansions; use a longer literal "
                    "prefix or a narrower pattern",
                    what, limits.maxTermsExamined),
        "too_many_terms_examined");
  }

  [[noreturn]] void throwPostings(std::string_view what) const {
    throw ResourceExhaustedError(
        std::format("{} expands to too many postings: the request may union "
                    "at most {} postings across all multi-term expansions",
                    what, limits.maxPostings),
        "too_many_expansion_postings");
  }

  [[noreturn]] void throwTooManyExpansions(std::string_view what) const {
    throw ResourceExhaustedError(
        std::format("{} matches more than {} terms in one segment; narrow the "
                    "pattern, or give a prefix query max_expansions to keep "
                    "only its most frequent terms",
                    what, maxExpansions()),
        "too_many_expansions");
  }

  [[noreturn]] void throwShortPrefix(
      std::string_view what, size_t prefixLength, int64_t rangeTerms) const {
    throw ResourceExhaustedError(
        std::format("{} is open-ended with a {}-byte literal prefix (under "
                    "the {}-byte minimum), so it must scan {} dictionary "
                    "terms, more than the {} the request has left; start the "
                    "pattern with a longer literal prefix",
                    what, prefixLength, limits.minPrefixLength, rangeTerms,
                    remainingTermsExamined()),
        "pattern_prefix_too_short");
  }
};

} // namespace luxir
