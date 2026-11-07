// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once
// The collection summary stored in the manifest root
// (IndexInfo.field_summaries, IndexInfo.summary_complete): the conservative
// union of a snapshot's segment summaries, fitted to a size budget. The wire
// type api::FieldSummary is non-owning, so the writer keeps the aggregate in
// owned records and converts at the wire boundary, like AuxInfo. See
// docs/design/segment-summaries.md.

#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <span>
#include <string>
#include <vector>

#include "luxir/api/luxir_index.hpp"
#include "SegmentSummary.h"

namespace luxir {

struct OwnedFieldSummary {
  std::string fieldname;
  std::string termMin;
  std::string termMax;
  uint64_t docsWithField = 0;
  uint64_t numValues = 0;
  uint64_t sumDocFreq = 0;
  uint64_t sumTotalTermFreq = 0;
  uint64_t nTerms = 0;
  int64_t numMin = 0;
  int64_t numMax = 0;
  uint32_t type = 0;
  uint32_t summaryFlags = 0;
  uint32_t flags = 0;
  uint32_t typeFlags = 0;
  uint32_t segmentsWithField = 0;
  int32_t geoMinLat = 0;
  int32_t geoMaxLat = 0;
  int32_t geoMinLon = 0;
  int32_t geoMaxLon = 0;

  bool operator==(const OwnedFieldSummary&) const = default;
};

OwnedFieldSummary fromWire(const api::FieldSummary& wire);
// A view over `owned`, which outlives it.
api::FieldSummary toWire(const OwnedFieldSummary& owned);
// Views over `records`, the array allocated in `mr`; both outlive them.
std::span<const api::FieldSummary> summariesToWire(std::span<const OwnedFieldSummary> records,
                                                   std::pmr::memory_resource& mr);
std::vector<OwnedFieldSummary> summariesFromWire(std::span<const api::FieldSummary> records);

class CollectionSummary {
public:
  // What the stored aggregate keeps, from most to least. Only the first
  // three are complete: an incomplete aggregate is stored without records
  // and proves nothing.
  enum class Tier : uint8_t {
    FULL,            // every record with every bound
    NO_TERM_BOUNDS,  // term bounds dropped to fit the budget
    PRESENCE,        // every bound dropped: presence, types and counts only
    OVER_BUDGET,     // presence records alone exceed the budget
    INCOMPLETE,      // some segment has no readable, complete summary
  };

  std::vector<OwnedFieldSummary> fields;
  bool complete = false;
  Tier tier = Tier::INCOMPLETE;

  // The union of `summaries`, one per segment (null when a segment has no
  // readable summary): a record per physical field present in any segment,
  // sorted by name. Counts sum (saturating) and flags OR. A bound survives
  // only when every non-empty segment holding the field has it and every
  // segment agrees on the field's type and type flags. Any null or
  // incomplete summary makes the aggregate incomplete, without records.
  static CollectionSummary aggregate(std::span<const SegmentSummary* const> summaries);

  // Fits the aggregate to `maxBytes` of encoded manifest fields: drops term
  // bounds first, then every other bound, and when the presence records
  // alone are still over, stores it incomplete without records. A presence
  // record is never dropped from a complete aggregate.
  void fit(size_t maxBytes);

  // Encoded bytes of the manifest fields this aggregate becomes.
  size_t encodedBytes() const;

  // Copies the stored form of a manifest's aggregate.
  static CollectionSummary fromManifest(std::span<const api::FieldSummary> fields, bool complete);
};

} // namespace luxir
