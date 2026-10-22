// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include "luxir/schema/FieldType.h"

#include "luxir/api/luxir_types.hpp"

#include <bit>
#include <cmath>
#include <format>

namespace luxir {

std::string_view FieldType::typeName(Type type) {
  static constexpr std::string_view names[] = {
    "none", "string", "text", "bin", "int", "float", "double", "id", "vector", "date", "geo_point"
  };
  return (unsigned)type < std::size(names) ? names[(unsigned)type] : "unknown";
}

uint32_t FieldType::typeParameters(Type type, uint32_t typeFlags) {
  switch (type) {
    case NONE: case STRING: case TEXT: case BIN: case INT: case FLOAT:
    case DOUBLE: case ID: case VECTOR: case DATE: case GEO_POINT:
      if (typeFlags == 0) return 0;
      throw std::runtime_error(std::format(
          "Unknown type flags 0x{:x} for persisted field type '{}'", typeFlags, typeName(type)));
  }
  throw std::runtime_error(std::format("Unknown persisted field type {}", (int)type));
}

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

std::string VectorFieldType::inputError(std::span<const float> vec) const {
  size_t n = vec.size();
  if (inputDims_ != 0 && n != (size_t)inputDims_ && (truncateDims_ == 0 || n != (size_t)truncateDims_)) {
    return truncateDims_ == 0
        ? std::format("expected dims={}, got {}", inputDims_, n)
        : std::format("expected dims={} or truncate_dims={}, got {}", inputDims_, truncateDims_, n);
  }
  // Every element, including a tail truncation drops, must be finite. One
  // branch-free pass over the exponent bits; only a failure goes back for the
  // first bad index.
  uint32_t nonFinite = 0;
  for (float x : vec) nonFinite |= (uint32_t)((std::bit_cast<uint32_t>(x) & 0x7f800000u) == 0x7f800000u);
  if (nonFinite) [[unlikely]] {
    for (size_t i = 0; i < n; i++) {
      if (!std::isfinite(vec[i])) return std::format("element {} is not a finite float32", i);
    }
  }
  return {};
}

}  // namespace luxir
