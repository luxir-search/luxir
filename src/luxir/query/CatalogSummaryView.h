// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "luxir/reader/FieldRecordCodec.h"
#include "luxir/search/IndexReader.h"

namespace luxir {

// Catalog evidence for one segment in a reader's row space, including deleted rows.
class CatalogSummaryView {
  IndexReader& reader;
  int32_t ord;
  std::string* reason;

public:
  CatalogSummaryView(IndexReader& reader, int32_t ord, std::string* reason = nullptr)
      : reader(reader), ord(ord), reason(reason) {}

  const DecodedFieldRecord* record(std::string_view field) const {
    return reader.field(field).at((size_t)ord);
  }

  std::optional<CatalogSummaryView> inSpace(IndexReader& space) const {
    if (space.segments()[(size_t)ord].maxDoc() == 0) return std::nullopt;
    return CatalogSummaryView(space, ord, reason);
  }

  // Records why a leaf proved the view empty; the first reason wins.
  void prove(std::string_view why) const {
    if (reason != nullptr && reason->empty()) reason->assign(why);
  }
};

} // namespace luxir
