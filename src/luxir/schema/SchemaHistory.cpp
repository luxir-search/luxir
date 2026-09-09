// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include "Schema.h"
#include "luxir/api/build.h"
#include "luxir/api/padded_input.h"

namespace luxir {

FieldSignatures Schema::signatures(bool includeTemplates) const {
  FieldSignatures result;
  for (const auto& [name, type] : fieldTypeMap) {
    if (dynamic_cast<StoredFieldType*>(type.get()) || type->isDerived()) continue;
    if (type->isAbstract() && !includeTemplates) continue;
    result.emplace(name, FieldSignature(name, *type));
    auto it = logicalFields.find(name);
    if (it != logicalFields.end() && it->second->primary == type) {
      for (const auto& [label, variant] : it->second->variants) {
        std::string physicalName = name + "__" + label;
        result.emplace(physicalName, FieldSignature(physicalName, *variant));
      }
    }
  }
  return result;
}

void Schema::addRootSignatures(std::string_view root, FieldSignatures& out) const {
  auto found = findRoot(root);
  if (!found.primary) return;
  out.try_emplace(std::string(root), root, **found.primary);
  if (found.owner) {
    for (const auto& [label, type] : found.owner->variants) {
      std::string name = std::string(root) + "__" + label;
      out.try_emplace(name, name, *type);
    }
  }
}

uint64_t Schema::introduction(std::string_view physicalName) const {
  auto it = introducedGen.find(physicalName);
  if (it != introducedGen.end()) return it->second;
  auto* type = getFieldTypePtr(physicalName);
  if (type) {
    it = introducedGen.find(type->name());
    if (it != introducedGen.end()) return it->second;
  }
  return gen_;
}

void Schema::inheritIntroductions(const Schema* previous, const FieldSignatures& materialized) {
  introducedGen.clear();
  auto current = signatures(true);
  for (const auto& [name, signature] : materialized) addRootSignatures(signature.logicalName, current);
  auto before = previous ? previous->signatures(true) : FieldSignatures{};
  // Removing an explicit root may expose a template representation that the
  // root previously suppressed. Its introduction is this publication, even
  // when the template itself is much older and the root has not flushed yet.
  for (const auto& [name, signature] : before) {
    if (validFieldName(signature.logicalName)) addRootSignatures(signature.logicalName, current);
  }
  if (previous) {
    for (const auto& [name, gen] : previous->introducedGen) {
      auto root = std::string_view(name).substr(0, name.rfind("__"));
      if (validFieldName(root)) addRootSignatures(root, current);
    }
  }
  for (const auto& [name, signature] : current) {
    uint64_t gen = gen_;
    if (previous) {
      auto old = before.find(name);
      auto* type = previous->getFieldTypePtr(name);
      // A concrete declaration may replace a compatible template instance.
      if (old != before.end()) {
        if (old->second.properties == signature.properties) gen = previous->introduction(name);
      } else if (type && FieldSignature(name, *type).properties == signature.properties) {
        gen = previous->introduction(name);
      }
    }
    introducedGen.emplace(name, gen);
  }
}

std::string Schema::encodeStored() const {
  api::SchemaInfo info;
  info.source_def = std::as_bytes(std::span(sourceDef_.data(), sourceDef_.size()));
  std::vector<std::pair<std::string_view, uint64_t>> introductions;
  introductions.reserve(introducedGen.size());
  for (const auto& [name, gen] : introducedGen) introductions.emplace_back(name, gen);
  info.introduced_gen = api::map_view<std::string_view, uint64_t>(introductions);
  std::vector<std::byte> bytes;
  if (!api::encode(info, bytes)) throw std::runtime_error("Cannot serialize schema history");
  return std::string((const char*)bytes.data(), bytes.size());
}

std::shared_ptr<Schema> Schema::decodeStored(std::span<const std::byte> bytes) {
  std::pmr::monotonic_buffer_resource arena;
  api::SchemaInfo info;
  if (!api::decode(info, api::copyToPaddedInput(bytes, arena), arena) || info.source_def.empty()) {
    throw std::runtime_error("Cannot decode schema history");
  }
  api::SchemaDef def;
  if (!api::decode(def, api::copyToPaddedInput(info.source_def, arena), arena)) {
    throw std::runtime_error("Cannot decode stored schema definition");
  }
  auto schema = fromProto(def);
  for (const auto& [name, gen] : info.introduced_gen) schema->introducedGen.emplace(name, gen);
  return schema;
}

} // namespace luxir
