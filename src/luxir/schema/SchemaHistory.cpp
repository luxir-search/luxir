// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include "Schema.h"
#include <set>
#include "FieldTypeRegistry.h"
#include "luxir/api/build.h"
#include "luxir/api/luxir_index.hpp"
#include "luxir/api/padded_input.h"

namespace luxir {

std::shared_ptr<Schema> Schema::withHistory(SchemaBinding binding, const Schema* previous) const {
  auto copy = std::make_shared<Schema>(*this);
  copy->binding = std::move(binding);
  copy->gen_ = copy->binding.generation();
  copy->file = {};
  copy->inheritIntroductions(previous);
  return copy;
}

FieldSignatures Schema::signatures(bool includeTemplates) const {
  FieldSignatures result;
  for (const auto& [name, type] : fieldTypeMap) {
    // Stored resources are not fields: each segment records the codec and
    // chunk limits it was written with, so changing them must not start a new
    // introduction for the fields stored there.
    if (dynamic_cast<StoredFieldType*>(type.get()) || type->isDerived()) continue;
    if (type->isAbstract() && !includeTemplates) continue;
    result.emplace(name, FieldSignature(name, *type));
    auto it = logicalFields.find(name);
    if (it != logicalFields.end() && it->second->primary == type) {
      for (const auto& [label, variant] : it->second->variants) {
        std::string physicalName = name + "__" + label;
        result.emplace(physicalName, FieldSignature(physicalName, *variant));
      }
      // What a derived field holds depends on the plugin type's definition.
      if (auto* composite = dynamic_cast<CompositeFieldType*>(type.get())) {
        for (const auto& [suffix, derived] : it->second->derived) {
          std::string physicalName = CompositeFieldType::derivedName(name, suffix);
          auto& signature = result.emplace(physicalName, FieldSignature(physicalName, *derived)).first->second;
          signature.properties["plugin_type"] = composite->plugin->canonical;
        }
      }
    }
  }
  return result;
}

api::SchemaVersion Schema::introductionVersion(std::string_view physicalName) const {
  auto it = introducedGen.find(physicalName);
  if (it != introducedGen.end()) return it->second;
  auto* type = getFieldTypePtr(physicalName);
  if (type) {
    it = introducedGen.find(type->name());
    if (it != introducedGen.end()) return it->second;
  }
  return binding.version;
}

void Schema::inheritIntroductions(const Schema* previous) {
  introducedGen.clear();
  auto current = signatures(true);
  auto before = previous ? previous->signatures(true) : FieldSignatures{};
  for (const auto& [name, signature] : current) {
    auto gen = binding.version;
    if (previous) {
      auto old = before.find(name);
      auto* type = previous->getFieldTypePtr(name);
      // A concrete declaration may replace a compatible template instance.
      if (old != before.end()) {
        if (old->second.properties == signature.properties) gen = previous->introductionVersion(name);
      } else if (type && FieldSignature(name, *type).properties == signature.properties) {
        gen = previous->introductionVersion(name);
      }
    }
    introducedGen.emplace(name, gen);
  }
}

api::SchemaInfo Schema::storedInfo(std::pmr::memory_resource& arena) const {
  api::SchemaInfo info;
  info.source_def = std::as_bytes(std::span(sourceDef_.data(), sourceDef_.size()));
  auto* introductions = api::build::allocArray(info.introduced, introducedGen.size(), arena);
  size_t i = 0;
  for (const auto& [name, gen] : introducedGen) introductions[i++] = {name, gen};
  return info;
}

std::shared_ptr<Schema> Schema::fromStored(const api::SchemaInfo& info, SchemaBinding binding) {
  std::pmr::monotonic_buffer_resource arena;
  api::SchemaDef def;
  if (!api::decode(def, api::copyToPaddedInput(info.source_def, arena), arena)) {
    throw std::runtime_error("Cannot decode stored schema definition");
  }
  auto schema = compile(def, nullptr, true);
  schema->binding = std::move(binding);
  schema->gen_ = schema->binding.generation();
  for (const auto& [name, gen] : info.introduced) schema->introducedGen.emplace(name, gen);
  return schema;
}

// Payload estimate, not allocator/RSS accounting. Shared field/analyzer objects
// are counted once within a schema, conservatively once per schema version.
// Factory-private resources and container allocator overhead are excluded.
size_t Schema::estimatedBytes() const {
  size_t bytes = sizeof(*this) + sourceDef_.capacity() + file.name.capacity()
      + binding.scope.capacity() + binding.name.capacity();
  bytes += fieldTypeMap.bucket_count() * sizeof(map_type::value_type);
  bytes += logicalFields.bucket_count() * sizeof(logical_map::value_type);
  for (const auto& [name, version] : introducedGen)
    bytes += sizeof(decltype(introducedGen)::value_type) + 3 * sizeof(void*) + name.capacity();
  std::set<const FieldType*> types;
  std::set<const Analyzer*> analyzers;
  auto field = [&](const std::shared_ptr<FieldType>& type) {
    if (!types.insert(type.get()).second) return;
    // The largest concrete type bounds the fixed part, without adding a
    // virtual accounting method to every field implementation.
    bytes += std::max({sizeof(TextFieldType), sizeof(StrFieldType), sizeof(IntFieldType), sizeof(FloatFieldType),
        sizeof(DoubleFieldType), sizeof(DateFieldType), sizeof(GeoPointFieldType), sizeof(IdFieldType),
        sizeof(VectorFieldType), sizeof(StoredFieldType)})
        + type->name_.capacity() + type->storedResource_.capacity();
    if (auto* text = dynamic_cast<TextFieldType*>(type.get())) analyzers.insert(text->analyzer_.get());
    if (auto* str = dynamic_cast<StrFieldType*>(type.get()); str && str->normalizer) analyzers.insert(str->normalizer.get());
  };
  for (const auto& [name, type] : fieldTypeMap) { bytes += name.capacity(); field(type); }
  for (const auto& [name, logical] : logicalFields) {
    bytes += name.capacity() + sizeof(LogicalField) + logical->name.capacity()
        + logical->search.capacity() + logical->value.capacity();
    field(logical->primary);
    for (const auto& [label, type] : logical->variants) {
      bytes += sizeof(decltype(logical->variants)::value_type) + 3 * sizeof(void*) + label.capacity();
      field(type);
    }
  }
  for (const auto* analyzer : analyzers) if (analyzer) {
    bytes += sizeof(Analyzer) + analyzer->canonical.capacity() + analyzer->presetName.capacity()
        + analyzer->filters.capacity() * sizeof(void*) + analyzer->charFilters.capacity() * sizeof(void*);
  }
  return bytes;
}

} // namespace luxir
