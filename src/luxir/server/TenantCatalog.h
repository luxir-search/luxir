// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "luxir/store/DirectoryFactory.h"
#include "luxir/store/CollectionId.h"

namespace luxir {

// Engine-owned names and incarnation lifecycles. A factory owns only paths.
class TenantCatalog {
public:
  struct Entry {
    std::string id;
    std::optional<std::string> name;
    std::string state;
  };
  struct State {
    uint64_t rev = 0;
    uint64_t high = 0;
    std::string op; // distinguishes identical competing mutations after an unknown outcome
    std::vector<Entry> entries;
  };
private:
  DirectoryFactory& factory;
  std::string tenant;
  std::mutex mutex;
  std::shared_ptr<StorageMemory> tenantAccount;
  std::string path;
  struct Snapshot { State state; std::optional<Directory::Publication> publication; };

  Snapshot snapshot();
  void mutate(const std::function<bool(State&)>& mutation);
  std::shared_ptr<StorageMemory> account(std::string_view name);
public:
  TenantCatalog(DirectoryFactory& factory, std::string_view tenant = CollectionId::kDefaultTenant);
  static void initialize(DirectoryFactory& factory, bool writable);
  static std::vector<CollectionId> collections(DirectoryFactory& factory);
  static void recover(DirectoryFactory& factory, bool follower);
  static bool empty(DirectoryFactory& factory);
  static uint64_t incarnationNumber(std::string_view id);

  State read();
  std::optional<std::string> current(std::string_view name);
  std::vector<std::string> incarnations(std::string_view name);
  // A supplied source id resumes a follower candidate or its active incarnation.
  std::string reserve(std::string_view name, std::string_view sourceId = {}, bool exclusive = false);
  void activate(std::string_view name, std::string_view id);
  void retire(std::string_view name);
  void remove(std::string_view name);
  void removeIncarnation(std::string_view id);
  void cleanup(bool follower);
  std::shared_ptr<Directory> directory(std::string_view name, std::string_view id);
  uint64_t bytes(std::string_view name);
};

} // namespace luxir
