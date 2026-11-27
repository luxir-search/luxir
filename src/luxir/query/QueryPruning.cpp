// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include "luxir/query/QueryPruning.h"

#include <format>
#include <stdexcept>

#include "luxir/query/CatalogPredicates.h"
#include "luxir/query/MatchNoDocsQuery.h"
#include "luxir/query/CatalogSummaryView.h"
#include "luxir/query/Query.h"
#include "luxir/query/ReadDeclarer.h"

namespace luxir {

Query::Weight* Query::Context::createRootWeight(Query& query, int32_t flags, float multiplier) {
  if (!planning.pruning) planning.pruning = std::make_shared<QueryPruning>();
  auto& membership = planning.pruning->membership(query, planning);
  const auto& mask = membership.mask;
  bool firstWeight = !std::exchange(membership.weightAttempted, true);
  bool mayPrepare = query.mayRequirePreparation();
  bool emptyWeight = !mayPrepare && mask.none();
#ifndef NDEBUG
  ReadVerification::RootScope verify(query, planning, flags, multiplier, emptyWeight);
#endif
  auto* weight = emptyWeight ? pool.make<MatchNoDocsQuery::Weight>(*this, query, flags)
                             : query.buildWeight(*this, flags, multiplier);
  assert(!weight->needsPrepare() || mayPrepare);
  if (mask.prunesAny()) {
    weight->membership = &mask;
    if (firstWeight && planning.proofNotes) {
      auto segments = planning.topReader.segments();
      if (emptyWeight) {
        planning.proofNotes->push_back(std::format("op {}: empty weight for the collection (segment 0, {})",
            planning.opName, weight->proofReason(segments.front())));
      } else {
        for (const auto& segment : segments) {
          if (weight->provenEmpty(segment)) planning.proofNotes->push_back(std::format(
              "op {}: pruned segment {} (ord {}): {}", planning.opName, segment.segInfo.seg_id,
              segment.ord, weight->proofReason(segment)));
        }
      }
    }
  }
  return weight;
}

QueryPruning::Membership& QueryPruning::membership(const Query& query, const Query::PlanningContext& planning) {
  auto found = membershipMemo.find({&planning, &query});
  if (found != membershipMemo.end()) return found->second;
  auto segments = planning.topReader.segments();
  SegmentMask mask(segments.size());
  if (!disableForTests) {
    bool describe = planning.proofNotes || planning.profiles;
    std::string reason;
    for (const auto& segment : segments) {
      reason.clear();
      CatalogSummaryView view(*this, planning, segment.ord, describe ? &reason : nullptr);
      if (query.mayMatch(view) != Query::MatchState::EMPTY) continue;
      size_t ord = (size_t)segment.ord;
      mask.exclude(ord);
      if (describe) {
        if (mask.reasons.empty()) mask.reasons.resize(segments.size());
        mask.reasons[ord] = reason;
      }
    }
  }
  return membershipMemo.emplace(PlanKey{&planning, &query}, Membership{std::move(mask)}).first->second;
}

bool CatalogSummaryView::membershipExcludes(std::string_view field, std::string_view term) const {
  const auto& segment = planning.topReader.segments()[(size_t)ord];
  if (QueryPruning::disableForTests || !segment.hasTermMembership() || !record(field).value_or(nullptr)) return false;
  // record() resolved the field, so its memo holds the physical name. A term is
  // asked about once per segment and field for the request.
  std::string key(fields.find(field)->second->name());
  key.append(1, '\0').append(term).append(1, '\0').append((const char*)&ord, sizeof(ord));
  auto [entry, inserted] = pruning.membershipProofs.try_emplace(std::move(key));
  if (inserted) {
    std::string label;
    entry->second.absent = segment.membershipProvesAbsent(fields.find(field)->second->name(), term, &label);
    entry->second.label = std::move(label);
  }
  if (entry->second.absent && reason != nullptr) reason->assign(entry->second.label);
  return entry->second.absent;
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

bool TermFactsCache::termAbsent(CachedFieldInfo& field, size_t segnum, std::string_view term, CachedTermInfo* node) {
  if (QueryPruning::disableForTests) return false;
  if (!proof::recordExcludesTerm(field.segInfos.at(segnum), term)) {
    const auto& segment = reader.segments()[segnum];
    if (!segment.hasTermMembership()) return false;
    uint8_t* memo = nullptr;
    if (node != nullptr) {
      // Allocated while the term entry is still below any savepoint that may be rewound.
      if (node->absence.empty()) node->absence = pool.make_span_zeroed<uint8_t>(numSegments());
      memo = &node->absence[segnum];
      if (*memo != 0) return *memo == 1;
    }
    bool proven = segment.membershipProvesAbsent(field.segInfos.name(), term);
    if (memo != nullptr) *memo = proven ? 1 : 2;
    if (!proven) return false;
  }
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
