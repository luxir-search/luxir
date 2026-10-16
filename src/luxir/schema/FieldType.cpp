// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include "luxir/schema/FieldType.h"

#include "luxir/api/luxir_types.hpp"

#include <format>

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

std::string VectorFieldType::inputDimsError(size_t n) const {
  if (inputDims_ > 0 && n != (size_t)inputDims_) {
    return std::format("expected dims={}, got {}", inputDims_, n);
  }
  if (truncateDims_ > 0 && n < (size_t)truncateDims_) {
    return std::format("expected at least truncate_dims={} components, got {}", truncateDims_, n);
  }
  return {};
}

}  // namespace luxir
