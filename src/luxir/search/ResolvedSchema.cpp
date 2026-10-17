// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include "luxir/search/ReaderManager.h"
#include "luxir/store/Manifest.h"
#include "luxir/api/build.h"
#include <glaze/glaze.hpp>

namespace luxir {

std::string ReaderManager::resolvedSchema() {
  auto current = snapshot();
  auto schema = current->schema;
  std::optional<uint64_t> oldest;
  std::pmr::monotonic_buffer_resource arena;
  auto info = Manifest::decode(current->bytes, arena);
  for (const auto& seg : info.segments) {
    if (seg.live_docs > 0) oldest = oldest ? std::min(*oldest, seg.schema_gen) : seg.schema_gen;
  }
  auto fields = schema->signatures();
  using Json = glz::generic_sorted_u64;
  Json result;
  result["generation"] = schema->gen_;
  result["fields"] = Json::object_t{};
  for (const auto& [name, signature] : fields) {
    auto& logical = result["fields"][signature.logicalName];
    logical["bindings"]["search"] = schema->resolveFor(signature.logicalName, OpClass::SEARCH).physicalName;
    logical["bindings"]["value"] = schema->resolveFor(signature.logicalName, OpClass::VALUE).physicalName;
    auto& rep = logical["representations"][signature.label];
    rep["name"] = name;
    for (const auto& [property, value] : signature.properties) {
      if (property == "posting_flags") continue;
      if (glz::read_json(rep[property], value)) throw std::runtime_error("Invalid signature JSON");
    }
    auto* type = schema->physical(name);
    rep["stored"] = type->isStored();
    if (type->isStored()) rep["stored_resource"] = type->storedResource_;
    if (!signature.properties.contains("analyzer")) rep["analyzer"] = nullptr;
    if (!signature.properties.contains("normalizer")) rep["normalizer"] = nullptr;
    uint64_t introduced = schema->introduction(name);
    rep["introduced_generation"] = introduced;
    // Include segments that LACK this physical name. Looking only at segments
    // containing it would hide exactly the coverage gap this view describes.
    if (oldest) rep["oldest_generation"] = *oldest;
    else rep["oldest_generation"] = nullptr;
    rep["coverage_complete"] = !oldest || *oldest >= introduced;
  }
  std::string json;
  if (glz::write_json(result, json)) throw std::runtime_error("Cannot serialize resolved schema");
  return json;
}

void ReaderManager::listFields(api::FieldsResponse& out, std::pmr::memory_resource& mr) {
  auto reader = getReader();
  auto catalog = reader->retrievableFields();
  api::FieldInfo* items = nullptr;
  if (!catalog.empty()) {
    items = (api::FieldInfo*)mr.allocate(sizeof(api::FieldInfo) * catalog.size(), alignof(api::FieldInfo));
  }
  for (size_t i = 0; i < catalog.size(); i++) {
    std::string_view type;
    switch (catalog[i].type->type()) {
      case FieldType::TEXT: type = "text"; break;
      case FieldType::STRING: type = "string"; break;
      case FieldType::ID: type = "id"; break;
      case FieldType::INT: type = "int"; break;
      case FieldType::FLOAT: type = "float"; break;
      case FieldType::DOUBLE: type = "double"; break;
      case FieldType::DATE: type = "date"; break;
      default: type = "other"; break;
    }
    new (&items[i]) api::FieldInfo{api::build::arenaStr(mr, catalog[i].name), type, catalog[i].derived};
  }
  out.fields = std::span<const api::FieldInfo>(items, catalog.size());
}

} // namespace luxir
