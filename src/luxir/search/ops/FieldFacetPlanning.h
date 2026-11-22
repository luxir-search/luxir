// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cstdint>
#include <string_view>

#include "luxir/search/SearchOverrides.h"

namespace luxir {

// Available parent bucket count strategies.
enum class FieldFacetCountKind : uint8_t {
  TOP_TERMS,
  COLUMN_DOMAIN,
  COLUMN_COMPLEMENT,
  TERM_DRIVEN
};

// Domain and counter costs for choosing one segment's count strategy.
struct FieldFacetCountInputs {
  int32_t maxDoc;
  int32_t domainCardinality;
  int32_t complementCardinality;
  int64_t segmentTerms;  // Distinct indexed values in this segment.
  int64_t sumDocFreq;
  int64_t docsWithField;
  bool domainHasBitset;
  bool missingRequested;
  bool topTermsLegal;  // The request can use the dictionary's certified top terms.
  // In column-read equivalents. Column and complement value work already
  // include the estimated number of updates to their respective counters.
  double columnWork;
  double walkTermCost;  // Cost per term in an all-docs walk.
  double complementTermCost;  // Cost per term when subtracting the staged complement.
  double complementValueWork;  // Column scan and counter updates for excluded docs.
  double complementSetupWork;  // Bitset and staging allocation work.
};

// Chosen strategy and optional reason a forced strategy could not run.
struct FieldFacetCountPlan {
  FieldFacetCountKind strategy;
  std::string_view forcedFallback;

  static FieldFacetCountPlan select(const FieldFacetCountInputs& in, FieldFacetStrategy forced);
};

} // namespace luxir
