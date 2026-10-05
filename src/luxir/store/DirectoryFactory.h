// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <map>
#include <mutex>
#include <optional>
#include <fstream>
#include "Directory.h"
#include "FileLock.h"
#include "FSDirectory.h"
#include "Manifest.h"
#include "CollectionId.h"
#include "luxir/util/Uuid.h"
#include "luxir/util/log.h"
#include <glaze/glaze.hpp>

namespace luxir {

class DirectoryFactory;

/// One collection's storage: a container directory whose CURRENT selects one of
/// its incarnation directories. A cheap handle; the factory must outlive it.
class CollectionStorage {
  DirectoryFactory* factory;
  CollectionId id_;
  struct Selection {
    std::string incarnation;
  };
public:
  CollectionStorage(DirectoryFactory& factory, CollectionId id) : factory(&factory), id_(std::move(id)) {}
  const CollectionId& id() const { return id_; }

  // The incarnation CURRENT selects in `container`, or nullopt without CURRENT.
  static std::optional<std::string> current(Directory& container) {
    auto file = container.openFile("CURRENT");
    if (!file) return std::nullopt;
    Selection result;
    std::string text(file->read()); // the parser reads up to a terminator
    if (glz::read_json(result, text) || result.incarnation.empty()
        || result.incarnation.find_first_of("/\\") != std::string::npos
        || result.incarnation == "." || result.incarnation == "..") {
      throw std::runtime_error("Invalid collection CURRENT");
    }
    return std::move(result.incarnation);
  }
  std::optional<std::string> current();
  // Durably switch CURRENT to an existing incarnation.
  void select(std::string_view incarnation);
  // Exclusively creates the collection; throws if its directory already exists.
  void createExclusive();
  // Creates the tenant, collection and incarnation directories as needed. Their
  // entries are durable on return, so a later durable root inside is reachable.
  std::shared_ptr<Directory> create(std::string_view incarnation);
  // Opens an existing incarnation without creating anything.
  std::shared_ptr<Directory> open(std::string_view incarnation);
  std::vector<std::string> incarnations();
  // Files directly in the collection directory, CURRENT included.
  bool hasFiles();
  void removeIncarnation(std::string_view incarnation);
  // Best-effort removal of every other incarnation directory.
  void retainOnly(std::string_view incarnation) noexcept;
  void remove();
  uint64_t bytes();
};

/// Opens collection storage under t/: t/tenant/name holds CURRENT, which
/// selects t/tenant/name/incarnation. Lives on LuxirNode - one per instance.
/// The virtual members are backend primitives behind CollectionStorage; wrapping
/// factories override every one of them.
class DirectoryFactory {
public:
  virtual ~DirectoryFactory() = default;
  CollectionStorage collection(CollectionId id) { return {*this, std::move(id)}; }

  // Every tenant's collections, ordered.
  virtual std::vector<CollectionId> collections() = 0;
  virtual uint64_t storageBytes() { return 0; }
  virtual uint64_t collectionBytes(const CollectionId& id) { unused(id); return 0; }

  // Opens t/tenant/name. Without `create` a missing directory is an error and
  // nothing is created; with it, created directory entries are durable on return.
  virtual std::shared_ptr<Directory> container(const CollectionId& id, bool create) = 0;
  // As container(), for t/tenant/name/incarnation; creation includes the container.
  virtual std::shared_ptr<Directory> incarnation(const CollectionId& id, std::string_view incarnation, bool create) = 0;
  // Exclusive, durable creation of t/tenant/name; throws if it already exists.
  virtual void createCollection(const CollectionId& id) = 0;
  virtual std::vector<std::string> incarnations(const CollectionId& id) = 0;
  virtual void removeIncarnation(const CollectionId& id, std::string_view incarnation) = 0;
  /// Remove all storage for the collection.
  virtual void remove(const CollectionId& id) = 0;
};

inline std::optional<std::string> CollectionStorage::current() {
  return current(*factory->container(id_, false));
}

inline void CollectionStorage::select(std::string_view incarnation) {
  auto container = factory->container(id_, false);
  auto bytes = glz::write_json(Selection{std::string(incarnation)}).value();
  Directory::FileCreateOptions options; options.expectedSize = bytes.size();
  auto file = container->createFile("CURRENT.pending", options);
  OutputStream out(file.get());
  out.write(bytes.data(), bytes.size()); out.close();
  container->finishFile(*file);
  std::array<std::string, 1> pending{"CURRENT.pending"};
  container->sync(pending);
  container->renameFile("CURRENT.pending", "CURRENT");
  std::array<std::string, 1> directory{"."};
  container->sync(directory);
}

inline void CollectionStorage::createExclusive() { factory->createCollection(id_); }
inline std::shared_ptr<Directory> CollectionStorage::create(std::string_view incarnation) {
  return factory->incarnation(id_, incarnation, true);
}
inline std::shared_ptr<Directory> CollectionStorage::open(std::string_view incarnation) {
  return factory->incarnation(id_, incarnation, false);
}
inline std::vector<std::string> CollectionStorage::incarnations() { return factory->incarnations(id_); }
inline bool CollectionStorage::hasFiles() {
  std::vector<Directory::FileInfo> files;
  factory->container(id_, false)->listFiles(files);
  return !files.empty();
}
inline void CollectionStorage::removeIncarnation(std::string_view incarnation) {
  factory->removeIncarnation(id_, incarnation);
}
inline void CollectionStorage::retainOnly(std::string_view incarnation) noexcept {
  try {
    for (const auto& other : incarnations()) {
      if (other == incarnation) continue;
      try { removeIncarnation(other); }
      catch (const std::exception& e) { LOG_WARN("Incarnation cleanup '{}/{}' failed: {}", id_.label(), other, e.what()); }
    }
  } catch (const std::exception& e) { LOG_WARN("Incarnation cleanup '{}' failed: {}", id_.label(), e.what()); }
}
inline void CollectionStorage::remove() { factory->remove(id_); }
inline uint64_t CollectionStorage::bytes() { return factory->collectionBytes(id_); }


/// Factory that retains RAMDir instances. Storage accounts form a tree: node,
/// tenant, collection. A collection's container and incarnations share its
/// account, which outlives removal while buffers it charged are referenced.
class RAMDirFactory : public DirectoryFactory {
  struct Entry {
    std::shared_ptr<Directory> container;
    std::map<std::string, std::shared_ptr<Directory>, std::less<>> incarnations;
  };
  std::mutex mutex;
  std::map<CollectionId, Entry> entries;
  std::shared_ptr<StorageMemory> memory;
  std::map<std::string, std::weak_ptr<StorageMemory>, std::less<>> tenantAccounts;
  std::map<CollectionId, std::weak_ptr<StorageMemory>> accounts;

  std::shared_ptr<StorageMemory> account(const CollectionId& id) {
    auto& weakTenant = tenantAccounts[id.tenant];
    auto tenant = weakTenant.lock();
    if (!tenant) { tenant = std::make_shared<StorageMemory>(0, memory); weakTenant = tenant; }
    auto& weak = accounts[id];
    auto result = weak.lock();
    if (!result) { result = std::make_shared<StorageMemory>(0, std::move(tenant)); weak = result; }
    return result;
  }
  Entry& entry(const CollectionId& id, bool create) {
    auto it = entries.find(id);
    if (it != entries.end()) return it->second;
    if (!create) throw std::runtime_error("collection directory does not exist: " + id.label());
    auto& added = entries[id];
    added.container = std::make_shared<RAMDir>(account(id));
    return added;
  }
public:
  explicit RAMDirFactory(uint64_t limit = 0, std::function<bool()> reclaim = {})
      : memory(std::make_shared<StorageMemory>(limit, nullptr, std::move(reclaim))) {}
  uint64_t storageBytes() override { return memory->bytes(); }
  uint64_t collectionBytes(const CollectionId& id) override {
    std::lock_guard lock(mutex);
    std::erase_if(accounts, [](const auto& entry) { return entry.second.expired(); });
    std::erase_if(tenantAccounts, [](const auto& entry) { return entry.second.expired(); });
    auto it = accounts.find(id);
    auto account = it == accounts.end() ? nullptr : it->second.lock();
    return account ? account->bytes() : 0;
  }
  std::vector<CollectionId> collections() override {
    std::lock_guard lock(mutex);
    std::vector<CollectionId> ids;
    for (const auto& [id, entry] : entries) ids.push_back(id);
    return ids;
  }
  std::shared_ptr<Directory> container(const CollectionId& id, bool create) override {
    std::lock_guard lock(mutex);
    return entry(id, create).container;
  }
  std::shared_ptr<Directory> incarnation(const CollectionId& id, std::string_view incarnation, bool create) override {
    std::lock_guard lock(mutex);
    auto& owner = entry(id, create);
    auto it = owner.incarnations.find(incarnation);
    if (it != owner.incarnations.end()) return it->second;
    if (!create) throw std::runtime_error("incarnation directory does not exist: " + id.label() + "/" + std::string(incarnation));
    return owner.incarnations[std::string(incarnation)] = std::make_shared<RAMDir>(account(id));
  }
  void createCollection(const CollectionId& id) override {
    std::lock_guard lock(mutex);
    if (entries.contains(id)) throw std::runtime_error("collection directory already exists");
    entry(id, true);
  }
  std::vector<std::string> incarnations(const CollectionId& id) override {
    std::lock_guard lock(mutex);
    std::vector<std::string> names;
    for (const auto& [incarnation, dir] : entry(id, false).incarnations) names.push_back(incarnation);
    return names;
  }
  void removeIncarnation(const CollectionId& id, std::string_view incarnation) override {
    std::lock_guard lock(mutex);
    if (auto it = entries.find(id); it != entries.end()) {
      if (auto dir = it->second.incarnations.find(incarnation); dir != it->second.incarnations.end()) it->second.incarnations.erase(dir);
    }
  }
  void remove(const CollectionId& id) override {
    std::lock_guard lock(mutex);
    if (auto it = entries.find(id); it != entries.end()) entries.erase(it);
  }
};


/// Factory that opens FSDirectory instances under basePath_/t/tenant/.
/// Shared resources (dictionaries, etc.) live directly under basePath_.
///
/// `unowned` opens an existing data directory without claiming it: no
/// write.lock or directory creation.  It suppresses only the
/// open-time side effects, which a Directory wrapper cannot reach because they
/// happen here in the constructor; rejecting mutations is ReadOnlyDirFactory's
/// job, and the two are meant to be composed (see Collections).
class FSDirFactory : public DirectoryFactory {
  std::filesystem::path basePath_;
  std::filesystem::path collectionsPath_;  // basePath_/t
  std::optional<FileLock> lock_;
  bool unowned;

  std::filesystem::path pathOf(const CollectionId& id) const { return collectionsPath_ / id.tenant / id.name; }

  // t/LAYOUT names the directory layout, so data from an incompatible layout is
  // refused as a whole instead of misread collection by collection. Collections
  // of earlier layouts lived under c/.
  static constexpr std::string_view kLayoutFile = "LAYOUT";
  static constexpr std::string_view kLayout = "tenant/collection/incarnation 1\n";
  void checkLayout() {
    auto marker = collectionsPath_ / kLayoutFile;
    if (std::filesystem::exists(marker)) {
      std::ifstream in(marker, std::ios::binary);
      std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
      if (text != kLayout) throw std::runtime_error("unsupported data directory layout in " + marker.string() + "; reindex it");
      return;
    }
    if (!holdsNoCollections(collectionsPath_) || std::filesystem::exists(basePath_ / "c")) {
      throw std::runtime_error("data directory " + basePath_.string() + " predates the tenant layout; reindex it");
    }
    if (unowned) return;
    FSDirectory collections(collectionsPath_, false);
    auto pending = std::string(kLayoutFile) + ".pending";
    auto file = collections.createFile(pending);
    OutputStream out(file.get());
    out.write(kLayout.data(), kLayout.size()); out.close();
    collections.finishFile(*file);
    std::array<std::string, 1> written{pending};
    collections.sync(written);
    collections.renameFile(pending, std::string(kLayoutFile));
    syncDirectory(collectionsPath_);
  }

public:
  explicit FSDirFactory(std::filesystem::path path, bool unowned = false)
      : basePath_(std::move(path)),
        collectionsPath_(basePath_ / "t"), unowned(unowned) {
    if (unowned) {
      if (!std::filesystem::is_directory(collectionsPath_)) {
        if (std::filesystem::exists(basePath_ / "c")) {
          throw std::runtime_error("data directory " + basePath_.string() + " predates the tenant layout; reindex it");
        }
        throw ReadOnlyError("data directory does not exist (or holds no collections): " +
                            basePath_.string());
      }
      LOG_INFO("Using existing data directory unowned (no write lock): {}", basePath_.string());
      checkLayout();
      return;
    }

    bool baseExisted = std::filesystem::is_directory(basePath_);
    bool created = std::filesystem::create_directories(collectionsPath_);
    if (created) {
      LOG_INFO("Created data directory: {}", basePath_.string());
    } else {
      LOG_INFO("Using existing data directory: {}", basePath_.string());
    }
    lock_.emplace(basePath_ / "write.lock");
    // Every owned open re-syncs the entry for t/: an earlier creation may have
    // failed after mkdir, before the entry was durable.
    if (!baseExisted && basePath_.has_parent_path()) syncDirectory(basePath_.parent_path());
    syncDirectory(basePath_);
    checkLayout();
  }

  // An empty collections directory, apart from its layout marker and what an
  // interrupted marker write leaves behind (LAYOUT.pending and its staging).
  static bool holdsNoCollections(const std::filesystem::path& collections) {
    for (const auto& entry : std::filesystem::directory_iterator(collections)) {
      if (!entry.is_regular_file() || !entry.path().filename().string().starts_with(kLayoutFile)) return false;
    }
    return true;
  }

  std::vector<CollectionId> collections() override {
    std::vector<CollectionId> ids;
    for (const auto& tenant : directoriesIn(collectionsPath_)) {
      for (auto& name : directoriesIn(collectionsPath_ / tenant)) ids.emplace_back(tenant, std::move(name));
    }
    return ids;
  }

  std::shared_ptr<Directory> container(const CollectionId& id, bool create) override {
    auto path = pathOf(id);
    if (create) { ensureDirectory(path.parent_path()); ensureDirectory(path); }
    return std::make_shared<FSDirectory>(path, false);
  }

  std::shared_ptr<Directory> incarnation(const CollectionId& id, std::string_view incarnation, bool create) override {
    auto path = pathOf(id) / incarnation;
    if (create) { ensureDirectory(path.parent_path().parent_path()); ensureDirectory(path.parent_path()); ensureDirectory(path); }
    return std::make_shared<FSDirectory>(path, false);
  }

  void createCollection(const CollectionId& id) override {
    auto path = pathOf(id);
    if (unowned) throw ReadOnlyError("cannot create a collection in an unowned data directory");
    ensureDirectory(path.parent_path());
    if (!std::filesystem::create_directory(path)) {
      throw std::filesystem::filesystem_error("collection directory already exists", path,
          std::make_error_code(std::errc::file_exists));
    }
    try { syncDirectory(path.parent_path()); }
    catch (...) {
      std::error_code ignored;
      std::filesystem::remove(path, ignored); // not created, as far as the caller knows
      throw;
    }
  }

  std::vector<std::string> incarnations(const CollectionId& id) override { return directoriesIn(pathOf(id)); }

  void removeIncarnation(const CollectionId& id, std::string_view incarnation) override {
    std::filesystem::remove_all(pathOf(id) / incarnation);
    syncDirectory(pathOf(id));
  }

  // The tenant directory stays: removing it would race a concurrent creation.
  void remove(const CollectionId& id) override {
    auto path = pathOf(id);
    if (std::filesystem::exists(path / "CURRENT")) {
      FSDirectory container(path, false);
      container.deleteFile("CURRENT");
      syncDirectory(path);
    }
    std::filesystem::remove_all(path);
    if (std::filesystem::is_directory(path.parent_path())) syncDirectory(path.parent_path());
  }

private:
  static void syncDirectory(const std::filesystem::path& path) {
    std::array<std::string, 1> directory{"."};
    FSDirectory(path, false).sync(directory);
  }
  // Creates `path` if missing and makes its entry in the parent durable. The
  // parent is synced even for an existing entry: an earlier creation may have
  // failed after mkdir, before its entry was durable.
  void ensureDirectory(const std::filesystem::path& path) {
    if (unowned) {
      if (std::filesystem::is_directory(path)) return;
      throw ReadOnlyError("directory does not exist: " + path.string());
    }
    std::filesystem::create_directory(path);
    syncDirectory(path.parent_path());
  }
  static std::vector<std::string> directoriesIn(const std::filesystem::path& path) {
    std::vector<std::string> result;
    for (const auto& entry : std::filesystem::directory_iterator(path)) {
      if (entry.is_directory()) result.push_back(entry.path().filename().string());
    }
    std::sort(result.begin(), result.end());
    return result;
  }
};


} // namespace luxir
