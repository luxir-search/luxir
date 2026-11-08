// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <memory_resource>
#include <string>
#include <string_view>
#include <vector>

#include "luxir/LuxirConfig.h"
#include "luxir/store/DirectoryFactory.h"
#include "luxir/util/ParamReader.h"

namespace luxir {

// Owns the typed CLI values behind maker readers, for the duration of stack construction.
class StorageOptions {
  std::pmr::monotonic_buffer_resource arena;
  std::map<std::string, api::Map, std::less<>> components;
public:
  explicit StorageOptions(const StoreConfig& config);
  ParamReader reader(std::string_view component, std::string owner) const;
};

/// What a backend or layer maker may read when the node builds its storage.
/// References are valid only for the duration of the maker call.
struct StorageContext {
  const LuxirConfig& config;                            // the whole node config (data_dir, read_only, ...)
  const ParamReader& params;                            // only this component's options
  std::function<bool()> reclaim;                        // RAM pressure callback; false when nothing was freed
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
  // The process-wide registry, seeded with the built-in backends (ram, fs).
  static StorageRegistry& global();

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

// Registers the built-in backends: ram and fs. Called when the global registry is built.
void registerBuiltinStorage(StorageRegistry& registry);

}  // namespace luxir
