// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include "FieldFacetPlanning.h"
#include <algorithm>
#include <cassert>

namespace luxir {

FieldFacetCountPlan FieldFacetCountPlan::select(const FieldFacetCountInputs& in, FieldFacetStrategy forced) {
  using Kind = FieldFacetCountKind;
  assert(in.domainCardinality >= 0 && in.complementCardinality >= 0);
  assert(in.domainCardinality + in.complementCardinality == in.maxDoc);
  bool hasTerms = in.segmentTerms > 0;
  switch (forced) {
    case FieldFacetStrategy::TOP_TERMS:
      if (in.topTermsLegal) return {Kind::TOP_TERMS, {}};
      return {Kind::COLUMN_DOMAIN, "forced top terms unavailable: domain or certificate"};
    case FieldFacetStrategy::COLUMN_DOMAIN:
      return {Kind::COLUMN_DOMAIN, {}};
    case FieldFacetStrategy::COLUMN_COMPLEMENT:
      if (hasTerms) return {Kind::COLUMN_COMPLEMENT, {}};
      return {Kind::COLUMN_DOMAIN, "forced complement unavailable: no terms dictionary"};
    case FieldFacetStrategy::TERM_DRIVEN:
      if (hasTerms) return {Kind::TERM_DRIVEN, {}};
      return {Kind::COLUMN_DOMAIN, "forced term unavailable: no terms dictionary"};
    case FieldFacetStrategy::AUTO:
      break;
  }

  if (in.topTermsLegal) return {Kind::TOP_TERMS, {}};
  if (!hasTerms) return {Kind::COLUMN_DOMAIN, {}};
  if (in.complementCardinality == 0) {
    return {in.segmentTerms * in.walkTermCost < in.columnWork
                ? Kind::COLUMN_COMPLEMENT : Kind::COLUMN_DOMAIN, {}};
  }
  // Postings setup and advancing relative to a column read, from the forced
  // string strategy grid. Keep the complement margin measured under load.
  constexpr double POSTINGS_SETUP_COST = 30;
  constexpr double POSTINGS_ADVANCE_COST = 4;
  constexpr double COMPLEMENT_MARGIN = 0.9;
  int32_t advanceSide = in.domainHasBitset
      ? std::min(in.domainCardinality, in.complementCardinality) : in.domainCardinality;
  double termWork = in.segmentTerms * POSTINGS_SETUP_COST
      + std::min((double)in.sumDocFreq, in.segmentTerms * POSTINGS_ADVANCE_COST * advanceSide);
  if (in.missingRequested && in.docsWithField != in.maxDoc) termWork += in.docsWithField;
  double complementWork = in.complementValueWork + in.segmentTerms * in.complementTermCost
      + in.complementSetupWork;
  if (complementWork < in.columnWork * COMPLEMENT_MARGIN && complementWork <= termWork) {
    return {Kind::COLUMN_COMPLEMENT, {}};
  }
  return {termWork < in.columnWork ? Kind::TERM_DRIVEN : Kind::COLUMN_DOMAIN, {}};
}

} // namespace luxir
