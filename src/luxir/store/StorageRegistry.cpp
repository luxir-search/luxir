// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include "luxir/store/StorageRegistry.h"

#include <stdexcept>

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

uint64_t StorageContext::optionBytes(const std::string& key, uint64_t fallback) const {
  auto value = option(key);
  if (!value) return fallback;
  const std::string& text = *value;
  size_t pos = 0;
  uint64_t number = 0;
  while (pos < text.size() && text[pos] >= '0' && text[pos] <= '9') {
    uint64_t digit = (uint64_t)(text[pos] - '0');
    if (number > (UINT64_MAX - digit) / 10) throw std::invalid_argument("store option '" + key + "' is too large");
    number = number * 10 + digit;
    pos++;
  }
  if (pos == 0) throw std::invalid_argument("store option '" + key + "' must be a byte count, got '" + text + "'");
  std::string suffix = text.substr(pos);
  uint64_t unit = 1;
  if (suffix.empty() || suffix == "B") unit = 1;
  else if (suffix == "K" || suffix == "KB" || suffix == "KiB") unit = 1ULL << 10;
  else if (suffix == "M" || suffix == "MB" || suffix == "MiB") unit = 1ULL << 20;
  else if (suffix == "G" || suffix == "GB" || suffix == "GiB") unit = 1ULL << 30;
  else throw std::invalid_argument("store option '" + key + "' has an unknown size suffix '" + suffix + "'");
  if (number > UINT64_MAX / unit) throw std::invalid_argument("store option '" + key + "' is too large");
  return number * unit;
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
    uint64_t limitMb = ctx.config.store.ram_limit_mb;
    if (limitMb > UINT64_MAX / (1024 * 1024)) throw std::invalid_argument("store.ram_limit_mb is too large");
    return std::make_unique<RAMDirFactory>(limitMb * 1024 * 1024, ctx.reclaim);
  });
  registry.addBackend("fs", [](const StorageContext& ctx) -> std::unique_ptr<DirectoryFactory> {
    return std::make_unique<FSDirFactory>(ctx.config.store.data_dir, ctx.config.read_only);
  });
}

}  // namespace luxir
