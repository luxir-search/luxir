// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace luxir {

// Search work priced from index metadata, in abstract units of about one
// nanosecond on a reference core. The prices only need to be right to within
// a small factor: they choose between serial and parallel execution and never
// affect results. Measured on a 1M-document text corpus.
namespace work {
// Anything without a better price, per document it may visit.
inline constexpr double MATCH = 4.0;
// Per lead document, per required clause of a conjunction.
inline constexpr double CLAUSE = 3.0;
// Per term position a phrase reads.
inline constexpr double POSITION = 1.5;
// A facet, per document it counts and per segment it sets up.
inline constexpr double FACET_DOC = 0.7;
inline constexpr double FACET_SEGMENT = 8000.0;
// Per term of an explicit term list, per segment: a dictionary seek, hit or
// miss, and the postings setup of a hit. A 45,587-term list on a 6-segment,
// 11.4M-document keyword field ran 27.7 ms faster across segments than
// serially, about 120 ns per term and segment; rounded up for cold
// dictionaries.
inline constexpr double TERM_LOOKUP = 150.0;

// Reading a posting list. Dense lists decode in bulk; sparse ones pay per
// document.
inline double postings(int64_t docs, int64_t maxDoc) {
  if (docs <= 0 || maxDoc <= 0) return 0.0;
  double sparse = 1.0 - std::min(1.0, (double)docs / (double)maxDoc);
  return (double)docs * (0.7 + 3.5 * sparse * sparse);
}
}  // namespace work

// A request's estimated work, per segment. Required work is certain to run.
// Prunable work is an upper bound that execution may cut far short, as a
// top-k ranking does.
struct WorkQuote {
  std::vector<double> required;
  std::vector<double> prunable;

  explicit WorkQuote(size_t segments) : required(segments), prunable(segments) {}
};

}  // namespace luxir
