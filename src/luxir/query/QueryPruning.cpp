// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include "luxir/query/QueryPruning.h"

#include "luxir/query/Query.h"
#include "luxir/query/SummaryView.h"

namespace luxir {

bool QueryPruning::collectionEmpty(const Query& query) {
  if (disableForTests) return false;
  const SegmentSummary* aggregate = reader.collectionSummary();
  if (aggregate == nullptr) return false;
  auto found = collectionMemo.find(&query);
  if (found != collectionMemo.end()) return found->second;
  bool empty = query.mayMatch(SummaryView(aggregate)) == Query::MatchState::EMPTY;
  collectionMemo.emplace(&query, empty);
  return empty;
}

std::string QueryPruning::collectionReason(const Query& query) const {
  const SegmentSummary* aggregate = reader.collectionSummary();
  if (disableForTests || aggregate == nullptr) return {};
  std::string why;
  if (query.mayMatch(SummaryView(aggregate, &why)) != Query::MatchState::EMPTY) return {};
  return why.empty() ? std::string("proven empty") : why;
}

const SegmentMask& QueryPruning::membership(const Query& query) {
  auto found = membershipMemo.find(&query);
  if (found != membershipMemo.end()) return found->second;
  auto segments = reader.segments();
  if (collectionEmpty(query)) {
    return membershipMemo.emplace(&query, SegmentMask(segments.size(), false)).first->second;
  }
  SegmentMask mask(segments.size(), true);
  if (!disableForTests) {
    for (const auto& segment : segments) {
      SummaryView view(segment.summary());
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
      if (segment.numLive() == 0) mask.clear((size_t)segment.ord);
    }
  }
  return memo.emplace(&query, std::move(mask)).first->second;
}

std::string QueryPruning::reason(const Query& query, size_t ord) const {
  const auto& segment = reader.segments()[ord];
  if (auto collection = collectionReason(query); !collection.empty()) return "collection summary: " + collection;
  if (segment.numLive() == 0) return "live_docs=0";
  std::string why;
  SummaryView view(segment.summary(), &why);
  if (query.mayMatch(view) != Query::MatchState::EMPTY) return {};
  return why.empty() ? std::string("proven empty") : why;
}

bool QueryPruning::termAbsent(const IndexReader::Segment& segment, std::string_view field, std::string_view term) {
  return !disableForTests && SummaryView::provesTermAbsent(segment.summary(), field, term);
}

} // namespace luxir
