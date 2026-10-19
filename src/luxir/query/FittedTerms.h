// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <format>
#include <string_view>

#include "Query.h"
#include "luxir/schema/FieldType.h"
#include "luxir/util/StrRef.h"

namespace luxir {

// Values over PackedTerm::MAX_LEN bytes are indexed in a fitted form (a
// prefix plus hash, or a truncated prefix). Wildcard, regex and fuzzy queries
// compare those indexed bytes, so one that can reach a term of
// PackedTerm::MIN_FITTED_LEN bytes may miss or falsely match a long value.
inline constexpr std::string_view FITTED_TERMS_WARNING = "fitted_terms";

// Warns once per request and field when a segment may hold a fitted term and
// canReach() says the query can reach one. canReach runs only for a flagged
// field, so the common case is a single cached lookup.
template <typename CanReach>
void warnIfFittedTermsReachable(Query::PlanningContext& context, std::string_view field,
                                TermPolicy policy, CanReach canReach) {
  if (context.warnings == nullptr) return;
  if ((context.topReader.field(field).flagsUnion() & FieldType::MAY_HAVE_FITTED) == 0) return;
  if (!canReach()) return;
  context.warn(FITTED_TERMS_WARNING, std::format(
      "field {}: values over {} bytes are indexed as {}; wildcard, regex and fuzzy queries "
      "may miss or falsely match them",
      field, PackedTerm::MAX_LEN,
      policy == TermPolicy::HASH128 ? "a prefix plus hash" : "a truncated prefix"));
}

} // namespace luxir
