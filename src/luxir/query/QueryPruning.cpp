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

const SegmentMask& QueryPruning::membership(const Query& query, const Query::PlanningContext& planning) {
  auto found = membershipMemo.find({&planning, &query});
  if (found != membershipMemo.end()) return found->second;
  auto segments = planning.topReader.segments();
  SegmentMask mask(segments.size());
  if (!disableForTests) {
    bool describe = planning.proofNotes || planning.profiles;
    size_t root = membershipMemo.size() + 1;
    std::string reason;
    for (const auto& segment : segments) {
      reason.clear();
      CatalogSummaryView view(*this, planning, segment.ord, describe ? &reason : nullptr);
      if (query.mayMatch(view) != Query::MatchState::EMPTY) continue;
      size_t ord = (size_t)segment.ord;
      mask.exclude(ord);
      if (describe) {
        if (mask.reasons.empty()) mask.reasons.resize(segments.size());
        mask.reasons[ord] = std::format("root {}: {}", root, reason.empty() ? "proven empty" : reason);
      }
    }
    if (planning.proofNotes && !mask.reasons.empty()) {
      if (!planning.mayPrepare && mask.none()) {
        planning.proofNotes->push_back(std::format("op {}: empty weight for the collection (segment 0, {})",
            planning.opName, mask.reasons.front()));
      } else {
        for (const auto& segment : segments) {
          size_t ord = (size_t)segment.ord;
          if (!mask.test(ord)) planning.proofNotes->push_back(std::format(
              "op {}: pruned segment {} (ord {}): {}", planning.opName, segment.segInfo.seg_id, ord, mask.reasons[ord]));
        }
      }
    }
  }
  return membershipMemo.emplace(PlanKey{&planning, &query}, std::move(mask)).first->second;
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
  if (QueryPruning::verifyTermAbsenceForTests) {
    auto* terms = getTermsEnum(field, segnum);
    if (terms && terms->seek(term)) {
      throw std::logic_error(std::format("term '{}' of field '{}' found in segment {} proven to lack it",
                                         term, field.segInfos.name(), segnum));
    }
  }
  return true;
}

} // namespace luxir
