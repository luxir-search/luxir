// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include "luxir/schema/FieldType.h"

#include "luxir/api/luxir_types.hpp"

namespace luxir {

void StrFieldType::normalize(std::string& value) const {
  if (normalizer) normalizer->createChain()->normalizeTerm(value);
}

TextFieldType::TextFieldType(std::string_view name, int flags, std::string_view tokenizer,
                             std::vector<std::string> filters)
    : FieldType(name, FieldType::TEXT, flags) {
  api::AnalyzerDef def;
  def.tokenizer.emplace().name = tokenizer;
  std::vector<api::AnalyzerComponent> components(filters.size());
  for (size_t i = 0; i < filters.size(); i++) components[i].name = filters[i];
  def.filters = std::span<const api::AnalyzerComponent>(components);
  analyzer_ = Analyzer::compile(def);  // copies what it keeps; the views die here
}

FeatureFieldType::FeatureFieldType(std::string_view name, int flags, Kind kind)
    : TextFieldType(name, flags, std::shared_ptr<const Analyzer>()), kind(kind) {
  // One compiled keyword chain serves every feature field.
  static const std::shared_ptr<const Analyzer> keyword = [] {
    api::AnalyzerDef def;
    def.tokenizer.emplace().name = "keyword";
    return Analyzer::compile(def);
  }();
  analyzer_ = keyword;
}

}  // namespace luxir
