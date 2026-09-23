// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include "Schema.h"
#include "luxir/api/build.h"
#include "luxir/api/luxir_index.hpp"
#include "luxir/api/padded_input.h"

namespace luxir {

std::shared_ptr<Schema> Schema::withHistory(uint64_t gen, const Schema* previous) const {
  auto copy = std::make_shared<Schema>(*this);
  copy->gen_ = gen;
  copy->inheritIntroductions(previous);
  return copy;
}

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

void Schema::inheritIntroductions(const Schema* previous) {
  introducedGen.clear();
  auto current = signatures(true);
  auto before = previous ? previous->signatures(true) : FieldSignatures{};
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

api::SchemaInfo Schema::storedInfo(std::pmr::memory_resource& arena) const {
  api::SchemaInfo info;
  info.source_def = std::as_bytes(std::span(sourceDef_.data(), sourceDef_.size()));
  auto* introductions = api::build::allocArray(info.introduced_gen, introducedGen.size(), arena);
  size_t i = 0;
  for (const auto& [name, gen] : introducedGen) introductions[i++] = {name, gen};
  return info;
}

std::shared_ptr<Schema> Schema::fromStored(const api::SchemaInfo& info, uint64_t gen) {
  std::pmr::monotonic_buffer_resource arena;
  api::SchemaDef def;
  if (!api::decode(def, api::copyToPaddedInput(info.source_def, arena), arena)) {
    throw std::runtime_error("Cannot decode stored schema definition");
  }
  auto schema = fromProto(def);
  schema->gen_ = gen;
  for (const auto& [name, gen] : info.introduced_gen) schema->introducedGen.emplace(name, gen);
  return schema;
}

} // namespace luxir
