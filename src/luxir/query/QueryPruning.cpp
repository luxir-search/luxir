// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include "luxir/query/QueryPruning.h"

#include <format>
#include <stdexcept>

#include "luxir/query/CatalogPredicates.h"
#include "luxir/query/MatchNoDocsQuery.h"
#include "luxir/query/CatalogSummaryView.h"
#include "luxir/query/Query.h"

namespace luxir {

Query::Weight* Query::Context::createRootWeight(Query& query, int32_t flags, float multiplier) {
  if (!planning.pruning) planning.pruning = std::make_shared<QueryPruning>();
  const auto& mask = planning.pruning->membership(query, planning);
  auto* weight = !planning.mayPrepare && mask.none()
      ? pool.make<MatchNoDocsQuery::Weight>(*this, query, flags)
      : query.createWeight(*this, flags, multiplier);
  if (mask.prunesAny()) weight->membership = &mask;
  return weight;
}

bool QueryPruning::collectionEmpty(const Query& query, const Query::PlanningContext& planning) {
  return membership(query, planning).none();
}

const SegmentMask& QueryPruning::membership(const Query& query, const Query::PlanningContext& planning) {
  auto found = membershipMemo.find({&planning, &query});
  if (found != membershipMemo.end()) return found->second;
  auto segments = planning.topReader.segments();
  SegmentMask mask(segments.size(), true);
  if (!disableForTests) {
    for (const auto& segment : segments) {
      CatalogSummaryView view(*this, planning, segment.ord);
      if (query.mayMatch(view) == Query::MatchState::EMPTY) mask.clear((size_t)segment.ord);
    }
  }
  return membershipMemo.emplace(PlanKey{&planning, &query}, std::move(mask)).first->second;
}

const SegmentMask& QueryPruning::viable(const Query& query, const Query::PlanningContext& planning) {
  auto found = memo.find({&planning, &query});
  if (found != memo.end()) return found->second;
  SegmentMask mask = membership(query, planning);
  if (!disableForTests) {
    for (const auto& segment : planning.topReader.segments()) {
      if (segment.liveCountBounds().second == 0) mask.clear((size_t)segment.ord);
    }
  }
  return memo.emplace(PlanKey{&planning, &query}, std::move(mask)).first->second;
}

std::string QueryPruning::reason(const Query& query, const Query::PlanningContext& planning, size_t ord) {
  const auto& segment = planning.topReader.segments()[ord];
  std::string why;
  CatalogSummaryView view(*this, planning, (int32_t)ord, &why);
  if (query.mayMatch(view) != Query::MatchState::EMPTY) {
    return segment.liveCountBounds().second == 0 ? "live_docs=0" : "";
  }
  return why.empty() ? std::string("proven empty") : why;
}

std::optional<const DecodedFieldRecord*> CatalogSummaryView::record(std::string_view field) const {
  auto [entry, inserted] = fields.try_emplace(field);
  try {
    if (inserted) {
      auto lookup = planning.topReader.lookupField(field);
      if (!lookup.mismatchedSchemaType) entry->second = lookup.segments;
    }
    if (entry->second) return entry->second->at((size_t)ord);
  } catch (const std::runtime_error&) {
    // An unreadable catalog offers no proof. Execution reports its own errors.
  }
  return std::nullopt;
}

bool Query::Context::termAbsent(CachedFieldInfo& field, size_t segnum, std::string_view term) {
  if (QueryPruning::disableForTests || !proof::recordExcludesTerm(field.segInfos.at(segnum), term)) return false;
  if (QueryPruning::crossCheck) {
    auto* terms = getTermsEnum(field, segnum);
    if (terms && terms->seek(term)) {
      QueryPruning::crossCheckFailed(std::format("term '{}' of field '{}' found in segment {} proven to lack it",
                                                 term, field.segInfos.name(), segnum));
    }
  }
  return true;
}

void QueryPruning::crossCheckFailed(std::string_view what) {
  throw std::logic_error(std::format("pruning cross-check: {}", what));
}

} // namespace luxir
