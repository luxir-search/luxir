// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include "FieldSignature.h"
#include <glaze/glaze.hpp>

namespace luxir {

FieldSignature::FieldSignature(std::string_view name, FieldType& type) {
  auto split = name.rfind("__");
  logicalName = name.substr(0, split);
  label = split == std::string_view::npos ? "self" : std::string(name.substr(split + 2));
  static constexpr const char* types[] = {
    "none", "string", "text", "bin", "int", "float", "double", "id", "vector", "date", "geo_point"
  };
  properties["type"] = "\"" + std::string(types[type.type()]) + "\"";
  properties["multi"] = type.multiValued() ? "true" : "false";
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
  }
  if (auto* vector = dynamic_cast<VectorFieldType*>(&type)) {
    properties["dims"] = std::to_string(vector->dims());
    static constexpr const char* metrics[] = {"none", "l2", "ip", "cosine"};
    properties["metric"] = "\"" + std::string(metrics[vector->metric()]) + "\"";
    properties["normalize_on_write"] = vector->normalizeOnWrite() ? "true" : "false";
    properties["normalized"] = vector->metric() == VectorFieldType::METRIC_COSINE && vector->normalized()
        ? "true" : "false";
  }
}

} // namespace luxir
