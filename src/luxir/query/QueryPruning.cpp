// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include "QueryPruning.h"

#include "Query.h"

namespace luxir {

SegmentMask QueryPruning::viableSegments(const Query& query, IndexReader& reader, MemPool& pool,
                                         std::vector<SegmentVerdict>* verdicts, bool explain) {
  auto segments = reader.segments();
  SegmentMask mask = SegmentMask::make(pool, segments.size(), true);
  if (verdicts != nullptr) {
    verdicts->clear();
    verdicts->reserve(segments.size());
  }
  for (const Segment& segment : segments) {
    size_t ord = (size_t)segment.ord;
    std::string reason;
    bool pruned = false;
    if (!disableForTests) {
      if (segment.numLive() == 0) {
        pruned = true;
        if (explain) reason = "live_docs=0";
      } else if (const SegmentSummary* summary = segment.summary()) {
        // Section-1 evidence first; payload probes only when nothing cheaper
        // rejected the segment.
        for (PruneTier tier : {PruneTier::BOUNDS, PruneTier::FILTER}) {
          SummaryView view(summary, segment.numLive(), tier, explain ? &reason : nullptr);
          if (query.mayMatch(view) == Query::MatchState::EMPTY) {
            pruned = true;
            break;
          }
        }
      }
    }
    if (pruned) mask.clear(ord);
    if (verdicts != nullptr) verdicts->push_back({segment.segInfo.seg_id, segment.ord, pruned, std::move(reason)});
  }
  return mask;
}

bool QueryPruning::collectionMayMatch(const Query& query, const SegmentSummary* aggregate, std::string* reason) {
  if (disableForTests || aggregate == nullptr || !aggregate->complete()) {
    if (reason != nullptr && aggregate == nullptr) reason->assign("summary_missing");
    else if (reason != nullptr && !aggregate->complete()) reason->assign("summary_incomplete");
    return true;
  }
  SummaryView view(aggregate, -1, PruneTier::FILTER, reason);
  return query.mayMatch(view) != Query::MatchState::EMPTY;
}

// PlanningContext memo, declared in Query.h.
const SegmentMask& Query::PlanningContext::viableSegments(const Query& query) {
  auto it = viableMasks.find(&query);
  if (it != viableMasks.end()) return it->second;
  SegmentMask mask = QueryPruning::viableSegments(query, topReader, pool);
  return viableMasks.emplace(&query, mask).first->second;
}

} // namespace luxir
