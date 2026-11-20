// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include "luxir/store/StorageRegistry.h"

#include <stdexcept>

#include "luxir/api/build.h"

namespace luxir {

namespace {

void checkName(std::string_view kind, std::string_view name) {
  if (name.empty()) throw std::invalid_argument("storage " + std::string(kind) + " name must not be empty");
  for (char c : name) {
    if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_')) {
      throw std::invalid_argument("invalid storage " + std::string(kind) + " name '" + std::string(name) +
                                  "'; use [a-z0-9_]+");
    }
  }
}

template <class Map>
void add(Map& map, bool frozen, std::string_view kind, std::string name, typename Map::mapped_type maker) {
  if (frozen) throw std::logic_error("storage registry is frozen; register during startup");
  checkName(kind, name);
  if (!maker) throw std::invalid_argument("null storage " + std::string(kind) + " maker '" + name + "'");
  if (map.contains(name)) throw std::invalid_argument("duplicate storage " + std::string(kind) + " '" + name + "'");
  map.emplace(std::move(name), std::move(maker));
}

template <class Map>
std::vector<std::string> names(const Map& map) {
  std::vector<std::string> result;
  for (const auto& entry : map) result.push_back(entry.first);
  return result;
}

std::string joined(const std::vector<std::string>& list) {
  std::string out;
  for (const auto& name : list) {
    if (!out.empty()) out += ", ";
    out += name;
  }
  return out;
}

}  // namespace

StorageOptions::StorageOptions(const StoreConfig& config) {
  for (const auto& [component, options] : config.options) {
    auto& map = components[component];
    for (const auto& [key, text] : options) {
      auto* val = api::build::mapSlot(map.fields, options.size(), api::build::arenaStr(arena, key), arena);
      bool parsed = api::read_json(*val, text, arena);
      if (!parsed || !(std::holds_alternative<int64_t>(val->kind) || std::holds_alternative<double>(val->kind) ||
                       std::holds_alternative<bool>(val->kind) || std::holds_alternative<std::string_view>(val->kind))) {
        val->kind = api::build::arenaStr(arena, text);
      }
    }
  }
}

ParamReader StorageOptions::reader(std::string_view component, std::string owner) const {
  auto it = components.find(component);
  return ParamReader(std::move(owner), it == components.end() ? ParamReader::Entries{} : it->second.fields);
}

void StorageRegistry::addBackend(std::string name, BackendMaker maker) {
  add(backends, isFrozen, "backend", std::move(name), std::move(maker));
}

void StorageRegistry::addLayer(std::string name, LayerMaker maker) {
  add(layers, isFrozen, "layer", std::move(name), std::move(maker));
}

std::vector<std::string> StorageRegistry::backendNames() const { return names(backends); }
std::vector<std::string> StorageRegistry::layerNames() const { return names(layers); }

std::unique_ptr<DirectoryFactory> StorageRegistry::makeBackend(std::string_view name, const StorageContext& ctx) const {
  auto it = backends.find(name);
  if (it == backends.end()) {
    throw std::invalid_argument("unknown storage backend '" + std::string(name) +
                                "'; valid backends: " + joined(backendNames()));
  }
  return it->second(ctx);
}

std::unique_ptr<DirectoryFactory> StorageRegistry::makeLayer(std::string_view name, std::unique_ptr<DirectoryFactory> inner,
                                                             const StorageContext& ctx) const {
  auto it = layers.find(name);
  if (it == layers.end()) {
    std::string valid = joined(layerNames());
    throw std::invalid_argument("unknown storage layer '" + std::string(name) +
                                "'; valid layers: " + (valid.empty() ? "none registered" : valid));
  }
  return it->second(std::move(inner), ctx);
}

void registerBuiltinStorage(StorageRegistry& registry) {
  registry.addBackend("ram", [](const StorageContext& ctx) -> std::unique_ptr<DirectoryFactory> {
    if (ctx.config.read_only) {
      throw std::runtime_error("--read-only requires a persistent --store.backend such as fs; there is no "
                               "existing data to serve with backend=" + ctx.config.store.backend);
    }
    uint64_t limitMb = ctx.config.store.ram_limit_mb;
    if (limitMb > UINT64_MAX / (1024 * 1024)) throw std::invalid_argument("store.ram_limit_mb is too large");
    return std::make_unique<RAMDirFactory>(limitMb * 1024 * 1024, ctx.reclaim);
  });
  registry.addBackend("fs", [](const StorageContext& ctx) -> std::unique_ptr<DirectoryFactory> {
    return std::make_unique<FSDirFactory>(ctx.config.store.data_dir, ctx.config.read_only);
  });
  registry.addBackend("shared_fs", [](const StorageContext& ctx) -> std::unique_ptr<DirectoryFactory> {
    if (!ctx.config.replication.source.empty()) throw std::invalid_argument("shared_fs cannot run as a follower");
    return std::make_unique<FSDirFactory>(ctx.config.store.data_dir, ctx.config.read_only,
                                        FSDirFactory::Mode::SHARED, ctx.config.node.id);
  });
}

StorageRegistry& StorageRegistry::global() {
  static StorageRegistry instance = [] {
    StorageRegistry registry;
    registerBuiltinStorage(registry);
    return registry;
  }();
  return instance;
}

}  // namespace luxir
