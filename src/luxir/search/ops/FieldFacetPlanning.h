// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cstdint>
#include <string_view>

#include "luxir/search/SearchOverrides.h"

namespace luxir {

enum class FieldFacetCountKind : uint8_t {
  TOP_TERMS,
  COLUMN_DOMAIN,
  COLUMN_COMPLEMENT,
  TERM_DRIVEN
};

struct FieldFacetCountInputs {
  int32_t maxDoc;
  int32_t domainCardinality;
  int32_t complementCardinality;
  int64_t segmentTerms;
  int64_t sumDocFreq;
  int64_t docsWithField;
  bool domainHasBitset;
  bool missingRequested;
  bool topTermsLegal;
  // In column-read equivalents. Column and complement value work already
  // include the estimated number of updates to their respective counters.
  double columnWork;
  double walkTermCost;
  double complementTermCost;
  double complementValueWork;
  double complementSetupWork;
};

struct FieldFacetCountPlan {
  FieldFacetCountKind strategy;
  std::string_view forcedFallback;

  static FieldFacetCountPlan select(const FieldFacetCountInputs& in, FieldFacetStrategy forced);
};

} // namespace luxir
