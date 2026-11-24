// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include "luxir/query/QueryPruning.h"

#include <format>
#include <stdexcept>

#include "luxir/query/CatalogPredicates.h"
#include "luxir/query/CatalogSummaryView.h"
#include "luxir/query/Query.h"

namespace luxir {

bool QueryPruning::collectionEmpty(const Query& query) {
  return membership(query).none();
}

const SegmentMask& QueryPruning::membership(const Query& query) {
  auto found = membershipMemo.find(&query);
  if (found != membershipMemo.end()) return found->second;
  auto segments = reader.segments();
  SegmentMask mask(segments.size(), true);
  if (!disableForTests) {
    for (const auto& segment : segments) {
      CatalogSummaryView view(reader, segment.ord);
      if (query.mayMatch(view) == Query::MatchState::EMPTY) mask.clear((size_t)segment.ord);
    }
  }
  return membershipMemo.emplace(&query, std::move(mask)).first->second;
}

const SegmentMask& QueryPruning::viable(const Query& query) {
  auto found = memo.find(&query);
  if (found != memo.end()) return found->second;
  SegmentMask mask = membership(query);
  if (!disableForTests) {
    for (const auto& segment : reader.segments()) {
      if (segment.liveCountBounds().second == 0) mask.clear((size_t)segment.ord);
    }
  }
  return memo.emplace(&query, std::move(mask)).first->second;
}

std::string QueryPruning::reason(const Query& query, size_t ord) {
  const auto& segment = reader.segments()[ord];
  std::string why;
  CatalogSummaryView view(reader, (int32_t)ord, &why);
  if (query.mayMatch(view) != Query::MatchState::EMPTY) {
    return segment.liveCountBounds().second == 0 ? "live_docs=0" : "";
  }
  return why.empty() ? std::string("proven empty") : why;
}

bool QueryPruning::termAbsent(const IndexReader::FieldSegments& field, size_t segment, std::string_view term) {
  return !disableForTests && field.exists() && proof::recordExcludesTerm(field.at(segment), term);
}

void QueryPruning::crossCheckFailed(std::string_view what) {
  throw std::logic_error(std::format("pruning cross-check: {}", what));
}

} // namespace luxir
