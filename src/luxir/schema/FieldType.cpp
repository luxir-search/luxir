// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include "luxir/schema/FieldType.h"

#include "luxir/api/luxir_types.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <format>
#include <glaze/glaze.hpp>

namespace luxir {

void FieldType::signatureProps(SignatureProps& out) const {
  out["type"] = "\"" + std::string(typeName(type_)) + "\"";
}

static void termSignatureProps(const FieldType& type, FieldType::SignatureProps& out) {
  switch (type.longTerms) {
    case TermPolicy::HASH128: out["long_terms"] = "\"hash128\""; break;
    case TermPolicy::TRUNCATE: out["long_terms"] = "\"truncate\""; break;
    case TermPolicy::REJECT: out["long_terms"] = "\"reject\""; break;
  }
}

void TextFieldType::signatureProps(SignatureProps& out) const {
  FieldType::signatureProps(out);
  termSignatureProps(*this, out);
  out["analyzer"] = analyzer_->canonical;
}

void StrFieldType::signatureProps(SignatureProps& out) const {
  FieldType::signatureProps(out);
  if (flags_ & INDEX_DOCS) termSignatureProps(*this, out);
  out["normalizer"] = "[]";
  if (normalizer) {
    glz::generic_sorted_u64 analyzer;
    if (glz::read_json(analyzer, normalizer->canonical) || glz::write_json(analyzer["filters"], out["normalizer"])) {
      throw std::runtime_error("Cannot serialize normalizer signature");
    }
  }
}

void IdFieldType::signatureProps(SignatureProps& out) const {
  FieldType::signatureProps(out);
  termSignatureProps(*this, out);
}

void DateFieldType::signatureProps(SignatureProps& out) const {
  FieldType::signatureProps(out);
  out["unit"] = "\"" + std::string(dateUnitName(unit)) + "\"";
}

void DecimalFieldType::signatureProps(SignatureProps& out) const {
  FieldType::signatureProps(out);
  out["scale"] = std::to_string(scale);
}

void VectorFieldType::signatureProps(SignatureProps& out) const {
  FieldType::signatureProps(out);
  out["dims"] = std::to_string(inputDims());
  if (truncateDims() > 0) out["truncate_dims"] = std::to_string(truncateDims());
  static constexpr const char* metrics[] = {"none", "l2", "ip", "cosine"};
  out["metric"] = "\"" + std::string(metrics[metric()]) + "\"";
  out["normalize_on_write"] = normalizeOnWrite() ? "true" : "false";
  out["normalized"] = metric() == METRIC_COSINE && normalized() ? "true" : "false";
}

std::string_view FieldType::typeName(Type type) {
  static constexpr std::string_view names[] = {
    "none", "string", "text", "bin", "int", "float", "double", "id", "vector", "date", "geo_point", "boolean", "decimal", "ip"
  };
  return (unsigned)type < std::size(names) ? names[(unsigned)type] : "unknown";
}

uint32_t FieldType::capabilities() const {
  switch (storage(type_)) {
    case Storage::TERMS:
      return MATCH | ANY_OF | PREFIX | PATTERN | FUZZY | FREE_TEXT | TERM_FILTER
          | RANGE | SORT | FIELD_FACET | COLLAPSE | VALUE_EXPR;
    case Storage::TEXT:
      return MATCH | ANY_OF | PREFIX | PATTERN | FUZZY | FREE_TEXT | RANGE | FIELD_FACET;
    case Storage::INT64:
      return MATCH | ANY_OF | RANGE | SORT | FIELD_FACET | RANGE_FACET | COLLAPSE | VALUE_EXPR;
    default: return 0;
  }
}

void FieldType::requireCapability(uint32_t required, std::string_view operation,
                                   std::string_view fieldName) const {
  if (!supports(required)) {
    auto message = std::format("{} is not supported on {} field '{}'", operation, typeName(type_), fieldName);
    auto remedy = capabilityRemedy(required);
    if (!remedy.empty()) message.append("; ").append(remedy);
    throw RequestError(message);
  }
}

uint32_t FieldType::typeParameters(Type type, uint32_t typeFlags) {
  switch (type) {
    case DECIMAL:
      if (typeFlags <= DecimalFieldType::MAX_SCALE) return typeFlags;
      throw std::runtime_error(std::format("Unknown decimal scale {}", typeFlags));
    case DATE:
      DateFieldType::unitFromTypeFlags(typeFlags);
      return typeFlags;
    case NONE: case STRING: case TEXT: case BIN: case INT: case FLOAT:
    case DOUBLE: case ID: case VECTOR: case GEO_POINT: case BOOLEAN: case IP:
      if (typeFlags == 0) return 0;
      throw std::runtime_error(std::format(
          "Unknown type flags 0x{:x} for persisted field type '{}'", typeFlags, typeName(type)));
  }
  throw std::runtime_error(std::format("Unknown persisted field type {}", (int)type));
}

std::string FieldType::describeType(Type type, uint32_t typeFlags) {
  if (type == DATE) {
    return std::format("type 'date' unit '{}'", dateUnitName(DateFieldType::unitFromTypeFlags(typeFlags)));
  }
  typeParameters(type, typeFlags);
  if (type == DECIMAL) return std::format("type 'decimal' scale {}", typeFlags);
  return std::format("type '{}'", typeName(type));
}

uint32_t DateFieldType::typeFlagsForUnit(DateUnit unit) {
  switch (unit) {
    case DateUnit::MILLIS: return 0;
    case DateUnit::NANOS: return 1;
  }
  throw std::invalid_argument("Unknown date unit");
}

DateUnit DateFieldType::unitFromTypeFlags(uint32_t typeFlags) {
  if (typeFlags & ~1u) {
    throw std::runtime_error(std::format("Unknown type flags 0x{:x} for persisted field type 'date'", typeFlags));
  }
  return typeFlags & 1u ? DateUnit::NANOS : DateUnit::MILLIS;
}

void StrFieldType::normalize(std::string& value) const {
  if (normalizer) normalizer->createChain()->normalizeTerm(value);
}

TextFieldType::TextFieldType(std::string_view name, int flags, std::string_view tokenizer,
                             std::vector<std::string> filters)
    : FieldType(name, FieldType::TEXT, flags, true) {
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
  // Every element, including a tail truncation drops, must be finite and at
  // most MAX_ELEMENT_MAGNITUDE in magnitude. Without the sign bit, float bits
  // order like magnitudes, with infinity and NaN above every finite value, so
  // one branch-free max checks both; only a failure goes back for the first
  // bad index.
  constexpr uint32_t limitBits = std::bit_cast<uint32_t>(MAX_ELEMENT_MAGNITUDE);
  uint32_t maxBits = 0;
  for (float x : vec) maxBits = std::max(maxBits, std::bit_cast<uint32_t>(x) & 0x7fffffffu);
  if (maxBits > limitBits) [[unlikely]] {
    for (size_t i = 0; i < n; i++) {
      if (!std::isfinite(vec[i])) return std::format("element {} is not a finite float32", i);
      if (std::fabs(vec[i]) > MAX_ELEMENT_MAGNITUDE) {
        return std::format("element {} ({}) exceeds the magnitude limit 2^{}", i, vec[i],
                           std::ilogb(MAX_ELEMENT_MAGNITUDE));
      }
    }
  }
  return {};
}

}  // namespace luxir
