// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "PostingsReader.h"

namespace luxir {

// Sequential field enumeration with an uncached decode for callers
// that only need a temporary field.
class FieldCursor {
  PostingsReader* reader;
  int32_t current = -1;

public:
  explicit FieldCursor(PostingsReader& reader) : reader(&reader) {}

  [[nodiscard]] bool next() {
    if (current + 1 >= reader->numFields()) return false;
    ++current;
    return true;
  }

  int32_t ordinal() const noexcept { return current; }

  FieldRecordPtr decode() const {
    // Call after next() succeeds.
    assert(current >= 0 && current < reader->numFields());
    return reader->decodeField(current);
  }

  PackedTerm name() const noexcept { return reader->fieldName(current); }
};

} // namespace luxir
