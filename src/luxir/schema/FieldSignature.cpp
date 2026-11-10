// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include "FieldSignature.h"
#include "FieldTypeRegistry.h"
#include <glaze/glaze.hpp>

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
  properties["type"] = "\"" + std::string(FieldType::typeName(type.type())) + "\"";
  if (auto* composite = dynamic_cast<CompositeFieldType*>(&type)) {
    properties["type"] = "\"plugin\"";
    properties["plugin_type"] = composite->plugin->canonical;
  }
  properties["multi"] = !type.multiValued() ? "false"
      : type.multiArray() && !FieldType::arrayOnly(type.type()) ? "\"array\"" : "true";
  properties["index"] = type.rangeIndexed() ? "\"range\"" : type.indexed() ? "\"match\"" : "\"none\"";
  properties["column"] = type.hasColumn() ? "true" : "false";
  if (type.type() == FieldType::TEXT || type.type() == FieldType::ID
      || (type.type() == FieldType::STRING && type.indexed())) {
    switch (type.longTerms) {
      case TermPolicy::HASH128: properties["long_terms"] = "\"hash128\""; break;
      case TermPolicy::TRUNCATE: properties["long_terms"] = "\"truncate\""; break;
      case TermPolicy::REJECT: properties["long_terms"] = "\"reject\""; break;
    }
  }
  // Include the term recording and norm choices for hand-built field types too.
  properties["posting_flags"] = std::to_string(type.flags_ &
      (FieldType::INDEX_DOCS_FREQS_POSITIONS | FieldType::NUM_TOKENS_APPROX | FieldType::NUM_TOKENS_EXACT));
  if (auto* text = dynamic_cast<TextFieldType*>(&type)) properties["analyzer"] = text->analyzer_->canonical;
  if (auto* str = dynamic_cast<StrFieldType*>(&type)) {
    properties["normalizer"] = "[]";
    if (str->normalizer) {
      glz::generic_sorted_u64 analyzer;
      if (glz::read_json(analyzer, str->normalizer->canonical) ||
          glz::write_json(analyzer["filters"], properties["normalizer"])) {
        throw std::runtime_error("Cannot serialize normalizer signature");
      }
    }
    // Keys of different definitions are not comparable, so the definition is
    // part of the field's identity: a change introduces a new representation.
    if (str->collation) properties["collation"] = str->collation->canonical;
  }
  if (auto* date = dynamic_cast<DateFieldType*>(&type)) {
    properties["unit"] = "\"" + std::string(dateUnitName(date->unit)) + "\"";
  }
  if (auto* vector = dynamic_cast<VectorFieldType*>(&type)) {
    properties["dims"] = std::to_string(vector->inputDims());
    if (vector->truncateDims() > 0) properties["truncate_dims"] = std::to_string(vector->truncateDims());
    static constexpr const char* metrics[] = {"none", "l2", "ip", "cosine"};
    properties["metric"] = "\"" + std::string(metrics[vector->metric()]) + "\"";
    properties["normalize_on_write"] = vector->normalizeOnWrite() ? "true" : "false";
    properties["normalized"] = vector->metric() == VectorFieldType::METRIC_COSINE && vector->normalized()
        ? "true" : "false";
  }
}

} // namespace luxir
