// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include "IndexWriter.h"
#include <glaze/glaze.hpp>

namespace luxir {

std::string IndexWriter::resolvedSchema() {
  auto schema = currentSchema.load();
  std::optional<uint64_t> oldest;
  {
    std::lock_guard<std::mutex> lock(indexMutex);
    oldest = oldestCommittedSchemaGen;
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

} // namespace luxir
