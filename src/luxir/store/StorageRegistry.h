// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "luxir/LuxirConfig.h"
#include "luxir/store/DirectoryFactory.h"

namespace luxir {

/// What a backend or layer maker may read when the node builds its storage.
struct StorageContext {
  const LuxirConfig& config;                            // the whole node config (data_dir, read_only, ...)
  const std::map<std::string, std::string>& options;    // config.store.options
  std::function<bool()> reclaim;                        // RAM pressure callback; false when nothing was freed

  std::optional<std::string> option(const std::string& key) const {
    auto it = options.find(key);
    if (it == options.end()) return std::nullopt;
    return it->second;
  }
  std::string optionOr(const std::string& key, std::string fallback) const {
    auto value = option(key);
    return value ? *value : std::move(fallback);
  }
  // A byte count: digits with an optional K, M or G suffix (also KB/KiB, ...; all powers of 1024).
  // Throws std::invalid_argument naming the option when the value is malformed.
  uint64_t optionBytes(const std::string& key, uint64_t fallback) const;
};

/// Named storage backends and layers. A backend builds the root DirectoryFactory
/// from the config; a layer wraps the factory built so far (typically as a
/// DelegatingDirFactory). Filled during startup (initPlugins), then frozen.
class StorageRegistry {
public:
  using BackendMaker = std::function<std::unique_ptr<DirectoryFactory>(const StorageContext&)>;
  using LayerMaker = std::function<std::unique_ptr<DirectoryFactory>(std::unique_ptr<DirectoryFactory> inner,
                                                                    const StorageContext&)>;

private:
  std::map<std::string, BackendMaker, std::less<>> backends;
  std::map<std::string, LayerMaker, std::less<>> layers;
  bool isFrozen = false;

public:
  // Throw std::invalid_argument on a bad or duplicate name, std::logic_error after freeze.
  void addBackend(std::string name, BackendMaker maker);
  void addLayer(std::string name, LayerMaker maker);

  // Throw std::invalid_argument listing the registered names when `name` is unknown.
  std::unique_ptr<DirectoryFactory> makeBackend(std::string_view name, const StorageContext& ctx) const;
  std::unique_ptr<DirectoryFactory> makeLayer(std::string_view name, std::unique_ptr<DirectoryFactory> inner,
                                              const StorageContext& ctx) const;

  // Sorted.
  std::vector<std::string> backendNames() const;
  std::vector<std::string> layerNames() const;

  bool frozen() const { return isFrozen; }
  void freeze() { isFrozen = true; }
};

// Registers the built-in backends: ram, fs and ram_object. Called by initPlugins.
void registerBuiltinStorage(StorageRegistry& registry);

}  // namespace luxir
