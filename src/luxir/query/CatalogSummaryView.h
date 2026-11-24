// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

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

  std::optional<CatalogSummaryView> inSpace(const Query::PlanningContext& space) const {
    if (space.topReader.segments()[(size_t)ord].maxDoc() == 0) return std::nullopt;
    return CatalogSummaryView(pruning, space, ord, reason);
  }

  // Records why a leaf proved the view empty; the first reason wins.
  void prove(std::string_view why) const {
    if (reason != nullptr && reason->empty()) reason->assign(why);
  }
};

} // namespace luxir
