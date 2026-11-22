// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "luxir/index/SegmentSummary.h"

namespace luxir {

enum class FieldPresence : uint8_t { ABSENT, PRESENT, UNKNOWN };

// Read-only adapter over one segment's summary, or the collection summary
// standing in for one, for Query::mayMatch. A null or incomplete summary
// proves no absence; the records of an incomplete one may still carry bounds.
class SummaryView {
  const SegmentSummary* summary;
  std::string* reason;

public:
  explicit SummaryView(const SegmentSummary* summary, std::string* reason = nullptr)
      : summary(summary), reason(reason) {}

  // Every field of the segment has a record: absence of a record is a proof.
  bool complete() const noexcept { return summary != nullptr && summary->complete(); }

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

  // Whether `summary` proves `term` absent from the dictionary of `field`:
  // the field is absent, has no terms, or its term bounds exclude the term.
  // The segment then contributes zero document and term frequency for it.
  static bool provesTermAbsent(const SegmentSummary* summary, std::string_view field,
                               std::string_view term) noexcept {
    if (summary == nullptr) return false;
    const api::FieldSummary* record = summary->field(field);
    if (record == nullptr) return summary->complete();
    return SegmentSummary::boundsExcludeTerm(*record, term);
  }
};

} // namespace luxir
