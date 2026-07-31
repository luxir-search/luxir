#pragma once

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <limits>
#include <string_view>

#include "solux/search/SearchOverrides.h"

namespace solux {

enum class StrFacetCountKind : uint8_t {
  TOP_TERMS,
  COLUMN_DOMAIN,
  COLUMN_COMPLEMENT,
  TERM_DRIVEN
};

struct StrFacetCountInputs {
  int32_t maxDoc;
  int32_t domainCardinality;
  int32_t complementCardinality;
  int64_t segmentTerms;
  int64_t sumDocFreq;
  int64_t docsWithField;
  int64_t limit;
  int64_t topTermsAvailable;
  bool domainHasBitset;
  bool missingRequested;
  bool allowTopTerms;
  bool hasGlobalOrdMap;
  bool readerHasNoDeletes;
};

struct StrFacetCountPlan {
  // Column-read-equivalents, fit to the forced-strategy facet grid. These
  // compare whole strategies under the counter representation each uses.
  // DF_COST covers the per-term docFreq walk and local-to-global drain.
  static constexpr double DF_COST = 2.25;
  static constexpr double POSTINGS_SETUP_COST = 30.0;
  static constexpr double POSTINGS_ADVANCE_COST = 4.0;
  // Complement must win decisively. Near-tie complement choices measured
  // slower under load, while genuine complement wins clear this margin.
  static constexpr double COMPLEMENT_MARGIN = 0.9;

  StrFacetCountKind strategy = StrFacetCountKind::COLUMN_DOMAIN;
  double columnWork = std::numeric_limits<double>::infinity();
  double complementWork = std::numeric_limits<double>::infinity();
  double termWork = std::numeric_limits<double>::infinity();
  std::string_view forcedFallback;

  static StrFacetCountPlan select(
      const StrFacetCountInputs& in, StrFacetStrategy forced) {
    assert(in.domainCardinality >= 0);
    assert(in.complementCardinality >= 0);
    assert(in.domainCardinality + in.complementCardinality == in.maxDoc);

    StrFacetCountPlan plan;
    plan.columnWork = (double)in.domainCardinality;
    bool termsAvailable = in.segmentTerms > 0;
    if (termsAvailable) {
      int64_t bitsetBuildWords = in.domainHasBitset
          ? 0 : ((int64_t)in.maxDoc + 63) >> 6;
      plan.complementWork = (double)in.complementCardinality
          + (double)in.segmentTerms * DF_COST
          + (double)bitsetBuildWords;
      int32_t advanceSide = in.domainHasBitset
          ? std::min(in.domainCardinality, in.complementCardinality)
          : in.domainCardinality;
      plan.termWork = (double)in.segmentTerms * POSTINGS_SETUP_COST
          + std::min((double)in.sumDocFreq,
                     (double)in.segmentTerms * POSTINGS_ADVANCE_COST
                         * (double)advanceSide);
      if (in.missingRequested && in.docsWithField != in.maxDoc) {
        plan.termWork += (double)in.docsWithField;
      }
    }

    bool topTermsLegal = in.allowTopTerms
        && in.hasGlobalOrdMap
        && in.complementCardinality == 0
        && in.readerHasNoDeletes
        && in.limit >= 0
        && in.topTermsAvailable >= in.limit;

    if (forced == StrFacetStrategy::TOP_TERMS) {
      if (topTermsLegal) {
        plan.strategy = StrFacetCountKind::TOP_TERMS;
      } else {
        plan.forcedFallback =
            "forced top terms unavailable: domain or certificate";
      }
      return plan;
    }
    if (forced == StrFacetStrategy::COLUMN_DOMAIN) {
      return plan;
    }
    if (forced == StrFacetStrategy::COLUMN_COMPLEMENT) {
      if (termsAvailable) {
        plan.strategy = StrFacetCountKind::COLUMN_COMPLEMENT;
      } else {
        plan.forcedFallback =
            "forced complement unavailable: no terms dictionary";
      }
      return plan;
    }
    if (forced == StrFacetStrategy::TERM_DRIVEN) {
      if (termsAvailable) {
        plan.strategy = StrFacetCountKind::TERM_DRIVEN;
      } else {
        plan.forcedFallback =
            "forced term unavailable: no terms dictionary";
      }
      return plan;
    }

    assert(forced == StrFacetStrategy::AUTO);
    if (topTermsLegal) {
      plan.strategy = StrFacetCountKind::TOP_TERMS;
    } else if (termsAvailable && in.complementCardinality == 0) {
      plan.strategy = StrFacetCountKind::COLUMN_COMPLEMENT;
    } else if (plan.complementWork < plan.columnWork * COMPLEMENT_MARGIN
               && plan.complementWork <= plan.termWork) {
      plan.strategy = StrFacetCountKind::COLUMN_COMPLEMENT;
    } else if (plan.termWork < plan.columnWork) {
      plan.strategy = StrFacetCountKind::TERM_DRIVEN;
    }
    return plan;
  }
};

} // namespace solux
