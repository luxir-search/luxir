// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "luxir/index/SegmentSummary.h"

namespace luxir {

// Cheapest to most expensive summary evidence. A leaf consults a tier only
// when the view allows it, so the filter tier, the one that reads a
// sidecar's bulky payloads, runs only for segments nothing cheaper rejected.
enum class PruneTier : uint8_t { LIVE, PRESENCE, BOUNDS, EXACT, FILTER };

enum class FieldPresence : uint8_t { ABSENT, PRESENT, UNKNOWN };

// Read-only adapter over one segment's summary, or over a collection's
// aggregate, for Query::mayMatch. A null or incomplete summary proves no
// absence; its records may still carry bounds.
class SummaryView {
  const SegmentSummary* summary;
  int64_t liveDocs_;
  PruneTier maxTier;
  std::string* reason;

public:
  SummaryView(const SegmentSummary* summary, int64_t liveDocs, PruneTier maxTier = PruneTier::FILTER,
              std::string* reason = nullptr)
      : summary(summary), liveDocs_(liveDocs), maxTier(maxTier), reason(reason) {}

  // Every field of the data has a record: absence of a record is a proof.
  bool complete() const noexcept { return summary != nullptr && summary->complete(); }
  const SegmentSummary* summaryOrNull() const noexcept { return summary; }
  // Live documents, or -1 when unknown (an aggregate).
  int64_t liveDocs() const noexcept { return liveDocs_; }
  bool allows(PruneTier tier) const noexcept { return tier <= maxTier; }

  FieldPresence presence(std::string_view physicalField) const noexcept {
    if (!complete()) return FieldPresence::UNKNOWN;
    return summary->field(physicalField) != nullptr ? FieldPresence::PRESENT : FieldPresence::ABSENT;
  }

  // The field's record, when the summary has one; null is not a proof.
  const api::FieldSummary* record(std::string_view physicalField) const noexcept {
    return summary != nullptr ? summary->field(physicalField) : nullptr;
  }

  // Records why a leaf proved the segment empty; the first reason wins.
  void prove(std::string_view why) const {
    if (reason != nullptr && reason->empty()) reason->assign(why);
  }
};

} // namespace luxir
