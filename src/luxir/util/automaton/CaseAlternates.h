// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cstdint>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

namespace luxir::automaton {

struct CodepointFolder;

// Simple (one-to-one) Unicode case equivalence from the vendored uni-algo
// tables: two scalars are case alternates when their simple case folds agree,
// which covers lower, upper and title forms (k, K and KELVIN SIGN; sigma and
// final sigma). Full mappings that change length (sharp s vs "SS") are not
// alternates.

// The sorted class of `cp`, `cp` included, or an empty span when `cp` has no
// other case form.
std::span<const int32_t> caseAlternates(int32_t cp);

// Appends a single-codepoint range for every case alternate of every scalar in
// [lo, hi]. Cost is proportional to the cased scalars in the range.
void appendCaseAlternates(int32_t lo, int32_t hi, std::vector<std::pair<int32_t, int32_t>>& ranges);

// True when `folder` maps every case alternate of every codepoint in the UTF-8
// `term` to the same sequence as the codepoint itself, so the field already
// erases case on this term. False on invalid UTF-8.
bool caseStable(std::string_view term, const CodepointFolder& folder);

} // namespace luxir::automaton
