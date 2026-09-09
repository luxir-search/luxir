// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include "IndexWriter.h"
#include "luxir/util/Signal.h"
#include <glaze/glaze.hpp>
#include <set>

namespace luxir {

// Additions have no old interpretation to drain. Changes and removals do:
// otherwise an old message could survive removal and conflict with a re-add.
// Expand concrete roots on both sides, including declarations that replace
// template instances (and representations suppressed by those declarations).
static std::set<std::string> changedNames(const Schema& previous, const Schema& candidate) {
  auto before = previous.signatures(true);
  auto after = candidate.signatures(true);
  std::set<std::string> roots;
  for (const auto* definitions : {&before, &after}) {
    for (const auto& [name, signature] : *definitions) {
      if (Schema::validFieldName(signature.logicalName)) roots.insert(signature.logicalName);
    }
  }
  for (const auto& root : roots) {
    previous.addRootSignatures(root, before);
    candidate.addRootSignatures(root, after);
  }
  std::set<std::string> changed;
  for (const auto& [name, signature] : before) {
    auto it = after.find(name);
    if (it == after.end() || it->second.properties != signature.properties) changed.insert(name);
  }
  return changed;
}

void IndexWriter::awaitSchemaAdmission(std::unique_lock<std::mutex>& lock) {
  // startUpdateBody runs on a concurrency-1 node: at most one update-graph
  // worker blocks here, while already admitted messages bypass this gate.
  // Publication waits on its request thread, outside the update graph. These
  // are the execution invariants for blocking without task suspension.
  if (schemaPublishing) Signal::emit("schemaAdmissionWait", this);
  schemaCondition.wait(lock, [&] { return !schemaPublishing || closed.load(std::memory_order_relaxed); });
  if (closed.load(std::memory_order_relaxed)) throw IndexWriterClosedError("index writer is closed");
}

void IndexWriter::publishSchema(Schema& candidate, const Schema* previous,
                                const std::function<void()>& publish) {
  std::unique_lock<std::mutex> lock(indexMutex);
  awaitSchemaAdmission(lock);
  auto changed = previous ? changedNames(*previous, candidate) : std::set<std::string>{};
  auto validateMaterialized = [&] {
    auto definitions = candidate.signatures(true);
    auto materialized = fieldSignatures;
    // These registries are quiescent: flush visits handlers but never adds them.
    // An idle/flushing inverter can contribute only fields it already resolved.
    for (const auto* inverters : {&idleInverters, &flushingInverters}) {
      for (const auto& [inverter, owned] : *inverters) {
        for (const auto& [name, handler] : inverter->indexHandlers) {
          materialized.try_emplace(std::string(name), name, *inverter->schema->physical(name));
        }
      }
    }
    for (const auto& [name, signature] : materialized) {
      candidate.addRootSignatures(signature.logicalName, definitions);
    }
    for (const auto& [name, signature] : materialized) {
      auto it = definitions.find(name);
      if (it != definitions.end()) signature.checkCompatible(name, it->second);
    }
    return materialized;
  };
  // Reject existing data conflicts immediately, before closing admission.
  auto materialized = validateMaterialized();
  auto reopenAdmission = scope_guard([&] {
    if (schemaPublishing) {
      schemaPublishing = false;
      schemaCondition.notify_all();
    }
  });
  if (!changed.empty()) {
    schemaPublishing = true;
    Signal::emit("schemaQuiesce", this);
    // This blocks the schema request's thread (HTTP task-arena or gRPC), never
    // an update-graph node. The concurrency-1 start node limits gated graph
    // workers to one; admitted messages remain free to finish under their pin.
    // Keep the transaction on its original thread, including thread-local state.
    schemaCondition.wait(lock, [&] {
      return closed.load(std::memory_order_relaxed) || (admittedSchemas.empty() && busyInverters.empty());
    });
    if (closed.load(std::memory_order_relaxed)) throw IndexWriterClosedError("index writer is closed");
    // A finishing message may have materialized a conflicting name. Released
    // inverters flush while the gate is closed; either their fixed handlers or
    // their registered signatures protect that name, so no flush wait is needed.
    materialized = validateMaterialized();
  }
  candidate.inheritIntroductions(previous, materialized);
  // The durable callback still runs under indexMutex, with admission closed
  // when quiescing. The guard also reopens admission on rejection or I/O error.
  publish();
}

std::string IndexWriter::resolvedSchema() {
  std::shared_ptr<Schema> schema;
  FieldSignatures materialized;
  std::optional<uint64_t> oldest;
  {
    std::lock_guard<std::mutex> lock(indexMutex);
    schema = schemaProvider_();
    materialized = fieldSignatures;
    oldest = oldestCommittedSchemaGen;
  }
  auto fields = schema->signatures();
  for (const auto& [name, signature] : materialized) schema->addRootSignatures(signature.logicalName, fields);
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
