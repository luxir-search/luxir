// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cassert>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "QueryPruning.h"

namespace luxir {

// Catalog evidence for one segment in a reader's row space, including deleted rows.
class CatalogSummaryView {
  QueryPruning& pruning;
  QueryPruning::FieldMemo& fields;
  int32_t ord;
  std::string* reason;

public:
  const Query::PlanningContext& planning;

  CatalogSummaryView(QueryPruning& pruning, const Query::PlanningContext& planning, int32_t ord,
                     std::string* reason = nullptr)
      : pruning(pruning), fields(pruning.fields[&planning.topReader]), ord(ord), reason(reason), planning(planning) {}

  // nullopt means unusable evidence. A null record is proven field absence.
  std::optional<const DecodedFieldRecord*> record(std::string_view field) const;

  // Whether a term membership overlay of the segment proves `term` absent from
  // `field`. Reads only the overlay's own bytes, never the segment's data.
  bool membershipExcludes(std::string_view field, std::string_view term) const;

  std::optional<CatalogSummaryView> inSpace(const Query::PlanningContext& space) const {
    assert(space.topReader.segments().size() == planning.topReader.segments().size());
    if (space.topReader.segments()[(size_t)ord].maxDoc() == 0) return std::nullopt;
    return CatalogSummaryView(pruning, space, ord, reason);
  }

  // The clause that completes an EMPTY verdict supplies its witness last.
  void prove(std::string_view why) const {
    if (reason != nullptr) reason->assign(why);
  }
};

} // namespace luxir
