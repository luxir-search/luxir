// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include "FieldSignature.h"

namespace luxir {

FieldSignature::FieldSignature(std::string_view name, FieldType& type) {
  auto split = name.rfind("__");
  logicalName = name.substr(0, split);
  label = split == std::string_view::npos ? "self" : std::string(name.substr(split + 2));
  properties["multi"] = !type.multiValued() ? "false"
      : type.multiArray() && !FieldType::arrayOnly(type.type()) ? "\"array\"" : "true";
  properties["index"] = type.rangeIndexed() ? "\"range\"" : type.indexed() ? "\"match\"" : "\"none\"";
  properties["column"] = type.hasColumn() ? "true" : "false";
  // Include the term recording and norm choices for hand-built field types too.
  properties["posting_flags"] = std::to_string(type.flags_ &
      (FieldType::INDEX_DOCS_FREQS_POSITIONS | FieldType::NUM_TOKENS_APPROX | FieldType::NUM_TOKENS_EXACT));
  type.signatureProps(properties);
}

} // namespace luxir
