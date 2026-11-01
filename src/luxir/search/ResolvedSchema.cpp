// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include "luxir/search/ReaderManager.h"
#include "luxir/schema/FieldTypeRegistry.h"
#include "luxir/store/Manifest.h"
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
    for (auto [name, op] : {std::pair{"search", OpClass::SEARCH}, {"value", OpClass::VALUE}}) {
      try {
        logical["bindings"][name] = schema->resolveFor(signature.logicalName, op).physicalName;
      } catch (const RequestError& e) {
        if (e.code != "retrieval_only") throw;
        logical["bindings"][name] = nullptr;
      }
    }
    auto& rep = logical["representations"][signature.label];
    rep["name"] = name;
    for (const auto& [property, value] : signature.properties) {
      if (property == "posting_flags") continue;
      if (glz::read_json(rep[property], value)) throw std::runtime_error("Invalid signature JSON");
    }
    auto* type = schema->physical(name);
    rep["stored"] = type->isStored();
    if (type->isStored()) rep["stored_resource"] = type->storedResource_;
    // Ingest-time options: kept out of the signature, since changing them does
    // not change what existing segments hold.
    if (auto* geo = dynamic_cast<const GeoPointFieldType*>(type)) {
      rep["ignore_malformed"] = geo->ignoreMalformed;
      rep["ignore_z_value"] = geo->ignoreZValue;
    }
    if (auto* composite = dynamic_cast<const CompositeFieldType*>(type)) {
      rep["ignore_malformed"] = composite->ignoreMalformed;
    }
    // The analyzer property is always the expanded definition; a named analyzer
    // additionally reports the name it came from.
    if (auto* text = dynamic_cast<const TextFieldType*>(type); text && !text->analyzer_->presetName.empty()) {
      rep["analyzer_preset"] = text->analyzer_->presetName;
    }
    if (auto* vector = dynamic_cast<const VectorFieldType*>(type)) {
      const auto& config = vector->ann();
      auto& ann = rep["ann"];
      ann["codec"] = config.codec == VectorAnnConfig::PQ ? "pq" : "rabitq";
      ann["nlist"] = config.nlist;
      if (config.codec == VectorAnnConfig::PQ) {
        ann["pq"]["m"] = config.pqM;
        ann["pq"]["bits"] = config.pqBits;
      } else {
        ann["rabitq"]["bits"] = config.rabitqBits;
        ann["rabitq"]["query_bits"] = config.rabitqQueryBits;
      }
    }
    if (!signature.properties.contains("analyzer")) rep["analyzer"] = nullptr;
    if (!signature.properties.contains("normalizer")) rep["normalizer"] = nullptr;
    if (!signature.properties.contains("collation")) rep["collation"] = nullptr;
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
