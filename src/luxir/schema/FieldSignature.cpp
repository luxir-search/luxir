// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include "FieldSignature.h"
#include "FieldTypeRegistry.h"

namespace luxir {

FieldSignature::FieldSignature(std::string_view name, FieldType& type) {
  // A plugin field's derived fields are representations labelled "$<suffix>".
  if (auto dollar = name.find(CompositeFieldType::SEPARATOR); dollar != std::string_view::npos) {
    logicalName = name.substr(0, dollar);
    label = name.substr(dollar);
  } else {
    auto split = name.rfind("__");
    logicalName = name.substr(0, split);
    label = split == std::string_view::npos ? "self" : std::string(name.substr(split + 2));
  }
  if (type.type() == FieldType::NESTED) {
    type.signatureProps(properties);
    return;
  }
  properties["multi"] = !type.multiValued() ? "false"
      : type.multiArray() && !FieldType::arrayOnly(type.type()) ? "\"array\"" : "true";
  properties["index"] = type.rangeIndexed() ? "\"range\"" : type.indexed() ? "\"match\"" : "\"none\"";
  properties["column"] = type.hasColumn() ? "true" : "false";
  // Include the term recording and norm choices for hand-built field types too.
  properties["posting_flags"] = std::to_string(type.flags_ &
      (FieldType::INDEX_DOCS_FREQS_POSITIONS | FieldType::NUM_TOKENS_APPROX | FieldType::NUM_TOKENS_EXACT));
  type.signatureProps(properties);
  if (auto* composite = dynamic_cast<CompositeFieldType*>(&type)) {
    properties["type"] = "\"plugin\"";
    properties["plugin_type"] = composite->plugin->canonical;
  }
}

} // namespace luxir
