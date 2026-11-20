// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cstdint>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

namespace luxir::automaton {

class Budget;
struct CodepointFolder;

// Simple (one-to-one) Unicode case equivalence from the vendored uni-algo
// tables: two scalars are case alternates when their simple case folds agree,
// which covers lower, upper and title forms (k, K and KELVIN SIGN; sigma and
// final sigma). Full mappings that change length (sharp s vs "SS") are not
// alternates.

// The sorted class of `cp`, `cp` included, or an empty span when `cp` has no
// other case form.
std::span<const int32_t> caseAlternates(int32_t cp);

// True if every scalar's case class folds back to that scalar. This is a
// sufficient test for term-wide case erasure by scalar folding chains; callers
// must exclude context-dependent character mappings.
bool caseAlternatesFoldToSelf(std::string_view term, const CodepointFolder& folder);

// Input ranges must be sorted and disjoint. Appends unsorted single-codepoint
// ranges for case alternates. Cased-scalar visits and member scans consume budget.
void appendCaseAlternates(std::vector<std::pair<int32_t, int32_t>>& ranges, Budget& budget);

} // namespace luxir::automaton
