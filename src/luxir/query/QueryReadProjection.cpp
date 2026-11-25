// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include "luxir/query/ReadDeclarer.h"

#include "luxir/query/BooleanQuery.h"
#include "luxir/query/CombinedFieldQuery.h"
#include "luxir/query/ConstantScoreQuery.h"
#include "luxir/query/DisMaxQuery.h"
#include "luxir/query/ForcePrepareQuery.h"
#include "luxir/query/MinScoreQuery.h"
#include "luxir/query/PhraseQuery.h"
#include "luxir/query/RescoreQuery.h"
#include "luxir/query/ToParentQuery.h"

namespace luxir {

void TermQuery::declareReads(ReadDeclarer& reads, PlanningContext& planning, int32_t flags, float) const {
  reads.term(planning, field, term, TERM_STATS | TERM_DOCS | ((flags & NEED_SCORES) ? FIELD_NORMS : 0));
}

void PhraseQuery::declareReads(ReadDeclarer& reads, PlanningContext& planning, int32_t flags, float) const {
  uint8_t products = TERM_STATS | TERM_DOCS | TERM_POSITIONS | ((flags & NEED_SCORES) ? FIELD_NORMS : 0);
  for (auto term : getTerms()) reads.term(planning, getField(), term, products);
}

void BoostQuery::declareReads(ReadDeclarer& reads, PlanningContext& planning, int32_t flags,
                              float multiplier) const {
  reads.child(*child, planning, flags, multiplier * boost);
}

void ForcePrepareQuery::declareReads(ReadDeclarer& reads, PlanningContext& planning, int32_t flags,
                                     float multiplier) const {
  reads.child(*child, planning, flags, multiplier);
}

void ConstantScoreQuery::declareReads(ReadDeclarer& reads, PlanningContext& planning, int32_t flags,
                                       float) const {
  reads.child(*child, planning, childFlags(flags));
}

void MinScoreQuery::declareReads(ReadDeclarer& reads, PlanningContext& planning, int32_t flags,
                                  float) const {
  reads.child(*child, planning, childFlags(flags));
}

void RescoreQuery::declareReads(ReadDeclarer& reads, PlanningContext& planning, int32_t flags,
                                 float) const {
  reads.child(*child, planning, childFlags(flags, childNeedsScore(flags)));
}

void ToParentQuery::declareReads(ReadDeclarer& reads, PlanningContext&, int32_t flags,
                                 float multiplier) const {
  auto effective = reduction;
  int32_t childFlags = childWeightMode(flags, effective, multiplier);
  reads.child(*child, source, childFlags);
}

void CombinedFieldQuery::declareReads(ReadDeclarer& reads, PlanningContext& planning, int32_t flags,
                                      float) const {
  if (flags & NEED_SCORES) {
    // A hit in any field wants all the lengths. This conservative field
    // union may over-fetch if every triggering term is absent.
    for (auto field : fields) reads.field(planning, field, FIELD_NORMS);
  }
  for (const auto& term : terms) reads.term(planning, fields[term.field], term.text, TERM_STATS | TERM_DOCS);
}

void DisMaxQuery::declareReads(ReadDeclarer& reads, PlanningContext& planning, int32_t flags,
                               float multiplier) const {
  if (auto* membership = membershipForWeight(flags)) {
    reads.child(*membership, planning, flags, multiplier);
  } else {
    for (auto* disjunct : disjuncts) reads.child(*disjunct, planning, flags, multiplier);
  }
}

void BooleanQuery::declareReads(ReadDeclarer& reads, PlanningContext& planning, int32_t flags,
                                 float multiplier) const {
  dispatchWeight(planning, [&](Query& child) {
    reads.child(child, planning, flags, multiplier);
  }, [&](const CompiledBoolean& plan) {
    bool cacheEnabled = mandatoryCacheEnabled(planning.topReader);
    for (auto* query : plan.mandatory) {
      int32_t childFlags = mandatoryWeightMode(*query, flags).flags;
      reads.child(*query, planning, childFlags, multiplier);
      // Only the built weight can settle whether a second cache weight is needed.
      if (cacheEnabled && (childFlags & (NEED_SCORES | ALLOW_PRUNING))) {
        reads.child(*query, planning, filterWeightFlags(flags));
      }
    }
    for (auto* query : plan.optionalForWeight(flags)) reads.child(*query, planning, flags, multiplier);
    for (auto* query : plan.prohibited) reads.child(*query, planning, exclusionWeightFlags(flags));
    for (auto* query : plan.filter) reads.child(*query, planning, filterWeightFlags(flags));
  });
}

} // namespace luxir
