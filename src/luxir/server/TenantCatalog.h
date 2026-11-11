// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <atomic>
#include "luxir/store/DirectoryFactory.h"
#include "luxir/store/CollectionId.h"
#include "luxir/store/IncarnationState.h"

namespace luxir {

// Engine-owned names and incarnation lifecycles. A factory owns only paths.
class TenantCatalog {
public:
  using Clock = std::chrono::system_clock;
  using Now = std::function<Clock::time_point()>;
  struct Entry {
    std::string id;
    std::optional<std::string> name;
    std::string state;
    std::string node;
    // Absent: never active. Zero: retirement visible, timestamp not yet finalized.
    std::optional<int64_t> retired_ms = {};
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
  std::string node;
  std::mutex mutex;
  std::recursive_mutex lifecycle;
  std::shared_ptr<StorageMemory> tenantAccount;
  std::string path;
  Now now;
  // Nonzero means pending. Count observations so a sweep cannot clear a new one.
  std::atomic<uint64_t> pendingRetirements{0};
  struct Snapshot { State state; std::optional<Directory::Publication> publication; };

  Snapshot snapshot();
  State mutate(const std::function<bool(State&)>& mutation);
  State update(const std::function<bool(State&)>& mutation);
  bool hasRetirements(const State& state) const;
  void observe(const State& state);
  static void retiring(Entry& entry);
  bool removalDue(const Entry& entry);
  std::shared_ptr<StorageMemory> account(std::string_view id);
  void requireOwner(const Entry& entry) const;
public:
  TenantCatalog(DirectoryFactory& factory, std::string_view tenant = CollectionId::kDefaultTenant,
                std::string_view node = {}, Now now = Clock::now);
  static void initialize(DirectoryFactory& factory, bool writable);
  static std::vector<CollectionId> collections(DirectoryFactory& factory);
  static void recover(DirectoryFactory& factory, bool follower, std::string_view node);
  static std::vector<std::string> tenants(DirectoryFactory& factory);
  static bool empty(DirectoryFactory& factory);
  static uint64_t incarnationNumber(std::string_view id);

  std::unique_lock<std::recursive_mutex> transition() { return std::unique_lock(lifecycle); }
  State read();
  bool hasPendingRetirements() const { return pendingRetirements.load(std::memory_order_relaxed) != 0; }
  void recover(bool follower);
  void adopt();
  bool assign(std::string_view id, std::string_view expectedNode, std::string_view node);
  std::optional<Entry> find(std::string_view name);
  Entry owned(std::string_view id);
  // Read STATE before confirming the entry, then claim only that version.
  IncarnationState claim(std::string_view name, std::string_view id, std::string_view state);
  // Captures a directory view, so a retained writer does not borrow this catalog.
  std::function<void()> supersededLogger(std::string id);
  uint64_t rename(std::string_view from, std::string_view to);
  uint64_t swap(std::string_view a, std::string_view b);
  // Installs ready bindings and reserves desired incarnations still being staged.
  uint64_t apply(const std::map<std::string, std::string>& names,
                 const std::map<std::string, std::string>& desired);
  std::optional<std::string> current(std::string_view name);
  std::vector<std::string> incarnations(std::string_view name);
  // A supplied source id resumes a follower candidate or its active incarnation.
  std::string reserve(std::string_view name, std::string_view sourceId = {}, bool exclusive = false);
  void activate(std::string_view name, std::string_view id, std::string_view replacing = {});
  void retire(std::string_view name, std::string_view id = {});
  void remove(std::string_view name);
  void removeIncarnation(std::string_view id);
  void expire(const std::function<bool(const Entry&)>& canRemove = {});
  void cleanup(bool follower);
  std::shared_ptr<Directory> directory(std::string_view id);
  uint64_t bytes(std::string_view id);
};

} // namespace luxir
