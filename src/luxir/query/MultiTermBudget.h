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
#include "luxir/reader/ScanAllowance.h"

namespace luxir {

// Server-side guards on term-dictionary expansion (prefix, wildcard, regex,
// string range, fuzzy). A multi-term query's cost grows with the field's
// dictionary, not with the request, so one short pattern on a
// high-cardinality field can hold a core for as long as the scan runs. These
// limits are operator opt-ins, all off by default. A request cannot raise or
// disable an operator limit; zero means unlimited.
struct MultiTermLimits {
  // Terms one segment of one query may expand to before the request is
  // rejected. For a prefix with max_expansions, this counts the selected
  // terms. Counts per segment, so a merge can change whether a query passes.
  int32_t maxExpansions = 0;
  // Dictionary terms all multi-term and fuzzy scans of one request may visit,
  // matching or not. Opt in to bound even a zero-hit `*x*` scan.
  int64_t maxTermsExamined = 0;
  // Estimate: summed docFreq of selected terms, not decoded postings.
  // Fuzzy scored rewrites do not count toward this limit.
  int64_t maxPostings = 0;
};

// Request-wide dictionary work allowance shared by every multi-term query in
// one request, across planning contexts, weights, and segments. Charges are
// atomic and report work after use. The outcome depends only on the total,
// not scheduling; work can pass a limit by at most one slice per concurrent scan.
class MultiTermBudget : public ScanAllowance {
  MultiTermLimits limits;
  std::atomic<int64_t> examined{0};
  std::atomic<int64_t> postings{0};

public:
  explicit MultiTermBudget(MultiTermLimits limits = {}) : limits(limits) {}

  const MultiTermLimits& getLimits() const noexcept { return limits; }

  // The per-segment cap on expanded terms, after any top-term selection.
  int32_t maxExpansions() const noexcept {
    return limits.maxExpansions > 0 ? limits.maxExpansions
                                    : std::numeric_limits<int32_t>::max();
  }

  int64_t termsExamined() const noexcept {
    return examined.load(std::memory_order_relaxed);
  }

  int64_t postingsCharged() const noexcept {
    return postings.load(std::memory_order_relaxed);
  }

  // Work can pass the limit by at most one slice per concurrent scan.
  // A future deadline/cancel poll gets a ~50 us interval.
  static constexpr int64_t SCAN_SLICE = 4096;

  ScanAllowance* scanAllowance() { return limits.maxTermsExamined > 0 ? this : nullptr; }

  int64_t charge(int64_t visited) override {
    int64_t total = examined.fetch_add(visited, std::memory_order_relaxed) + visited;
    if (total > limits.maxTermsExamined) return 0;
    // Avoid overflow when the configured limit is INT64_MAX.
    return std::min(SCAN_SLICE - 1, limits.maxTermsExamined - total) + 1;
  }

  static constexpr int64_t POSTINGS_SLICE = 1 << 20;

  [[nodiscard]] bool chargePostings(int64_t count) {
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
        std::format("{} exceeds the postings estimate limit of {}: summed docFreq "
                    "of selected terms across all multi-term expansions",
                    what, limits.maxPostings),
        "too_many_expansion_postings");
  }

  [[noreturn]] void throwTooManyExpansions(std::string_view what) const {
    throw ResourceExhaustedError(
        std::format("{} matches more than {} terms in one segment; narrow the "
                    "pattern, or give a prefix query max_expansions of at most {} "
                    "to keep only its most frequent terms",
                    what, maxExpansions(), maxExpansions()),
        "too_many_expansions");
  }
};

} // namespace luxir
