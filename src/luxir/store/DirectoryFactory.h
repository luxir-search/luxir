// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <map>
#include <mutex>
#include <optional>
#include "Directory.h"
#include "FileLock.h"
#include "FSDirectory.h"
#include "Manifest.h"
#include "luxir/util/Uuid.h"
#include "luxir/util/log.h"
#include <glaze/glaze.hpp>

namespace luxir {

class DirectoryFactory;

/// One collection's storage: a container directory whose CURRENT selects one of
/// its incarnation directories. A cheap handle; the factory must outlive it.
class CollectionStorage {
  DirectoryFactory* factory;
  std::string name_;
  struct Selection {
    std::string incarnation;
  };
public:
  CollectionStorage(DirectoryFactory& factory, std::string name) : factory(&factory), name_(std::move(name)) {}
  const std::string& name() const { return name_; }

  // The incarnation CURRENT selects in `container`, or nullopt without CURRENT.
  static std::optional<std::string> current(Directory& container) {
    auto file = container.openFile("CURRENT");
    if (!file) return std::nullopt;
    Selection result;
    if (glz::read_json(result, file->read()) || result.incarnation.empty()
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
  // Creates the collection and incarnation directories as needed. Their entries
  // are durable on return, so a later durable root inside is reachable.
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

/// Opens collection storage under c/: c/name holds CURRENT, which selects
/// c/name/incarnation. Lives on LuxirNode - one per instance.
/// The virtual members are backend primitives behind CollectionStorage; wrapping
/// factories override every one of them.
class DirectoryFactory {
public:
  virtual ~DirectoryFactory() = default;
  CollectionStorage collection(std::string name) { return {*this, std::move(name)}; }

  virtual std::vector<std::string> collections() = 0;
  virtual uint64_t storageBytes(std::string_view collection = {}) { unused(collection); return 0; }

  // Opens c/name. Without `create` a missing directory is an error and nothing
  // is created; with it, a created directory's entry is durable on return.
  virtual std::shared_ptr<Directory> container(std::string_view name, bool create) = 0;
  // As container(), for c/name/incarnation; creation includes the container.
  virtual std::shared_ptr<Directory> incarnation(std::string_view name, std::string_view incarnation, bool create) = 0;
  // Exclusive, durable creation of c/name; throws if it already exists.
  virtual void createCollection(std::string_view name) = 0;
  virtual std::vector<std::string> incarnations(std::string_view name) = 0;
  virtual void removeIncarnation(std::string_view name, std::string_view incarnation) = 0;
  /// Remove all storage for the named collection.
  virtual void remove(std::string_view name) = 0;
};

inline std::optional<std::string> CollectionStorage::current() {
  return current(*factory->container(name_, false));
}

inline void CollectionStorage::select(std::string_view incarnation) {
  auto container = factory->container(name_, false);
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

inline void CollectionStorage::createExclusive() { factory->createCollection(name_); }
inline std::shared_ptr<Directory> CollectionStorage::create(std::string_view incarnation) {
  return factory->incarnation(name_, incarnation, true);
}
inline std::shared_ptr<Directory> CollectionStorage::open(std::string_view incarnation) {
  return factory->incarnation(name_, incarnation, false);
}
inline std::vector<std::string> CollectionStorage::incarnations() { return factory->incarnations(name_); }
inline bool CollectionStorage::hasFiles() {
  std::vector<Directory::FileInfo> files;
  factory->container(name_, false)->listFiles(files);
  return !files.empty();
}
inline void CollectionStorage::removeIncarnation(std::string_view incarnation) {
  factory->removeIncarnation(name_, incarnation);
}
inline void CollectionStorage::retainOnly(std::string_view incarnation) noexcept {
  try {
    for (const auto& other : incarnations()) {
      if (other == incarnation) continue;
      try { removeIncarnation(other); }
      catch (const std::exception& e) { LOG_WARN("Incarnation cleanup '{}/{}' failed: {}", name_, other, e.what()); }
    }
  } catch (const std::exception& e) { LOG_WARN("Incarnation cleanup '{}' failed: {}", name_, e.what()); }
}
inline void CollectionStorage::remove() { factory->remove(name_); }
inline uint64_t CollectionStorage::bytes() { return factory->storageBytes(name_); }


/// Factory that retains RAMDir instances. One storage account per collection
/// name is shared by its container and incarnations, and outlives removal while
/// buffers it charged are still referenced.
class RAMDirFactory : public DirectoryFactory {
  struct Entry {
    std::shared_ptr<Directory> container;
    std::map<std::string, std::shared_ptr<Directory>, std::less<>> incarnations;
  };
  std::mutex mutex;
  std::map<std::string, Entry, std::less<>> entries;
  std::shared_ptr<StorageMemory> memory;
  std::map<std::string, std::weak_ptr<StorageMemory>, std::less<>> accounts;

  std::shared_ptr<Directory> makeDir(std::string_view name) {
    auto& weak = accounts[std::string(name)];
    auto account = weak.lock();
    if (!account) { account = std::make_shared<StorageMemory>(0, memory); weak = account; }
    return std::make_shared<RAMDir>(std::move(account));
  }
  Entry& entry(std::string_view name, bool create) {
    auto it = entries.find(name);
    if (it != entries.end()) return it->second;
    if (!create) throw std::runtime_error("collection directory does not exist: " + std::string(name));
    auto& added = entries[std::string(name)];
    added.container = makeDir(name);
    return added;
  }
public:
  explicit RAMDirFactory(uint64_t limit = 0, std::function<bool()> reclaim = {})
      : memory(std::make_shared<StorageMemory>(limit, nullptr, std::move(reclaim))) {}
  uint64_t storageBytes(std::string_view collection = {}) override {
    std::lock_guard lock(mutex);
    std::erase_if(accounts, [](const auto& entry) { return entry.second.expired(); });
    if (collection.empty()) return memory->bytes();
    auto it = accounts.find(collection);
    auto account = it == accounts.end() ? nullptr : it->second.lock();
    return account ? account->bytes() : 0;
  }
  std::vector<std::string> collections() override {
    std::lock_guard lock(mutex);
    std::vector<std::string> names;
    for (const auto& [name, entry] : entries) names.push_back(name);
    return names;
  }
  std::shared_ptr<Directory> container(std::string_view name, bool create) override {
    std::lock_guard lock(mutex);
    return entry(name, create).container;
  }
  std::shared_ptr<Directory> incarnation(std::string_view name, std::string_view incarnation, bool create) override {
    std::lock_guard lock(mutex);
    auto& owner = entry(name, create);
    auto it = owner.incarnations.find(incarnation);
    if (it != owner.incarnations.end()) return it->second;
    if (!create) throw std::runtime_error("incarnation directory does not exist: " + std::string(name) + "/" + std::string(incarnation));
    return owner.incarnations[std::string(incarnation)] = makeDir(name);
  }
  void createCollection(std::string_view name) override {
    std::lock_guard lock(mutex);
    if (entries.contains(name)) throw std::runtime_error("collection directory already exists");
    entry(name, true);
  }
  std::vector<std::string> incarnations(std::string_view name) override {
    std::lock_guard lock(mutex);
    std::vector<std::string> names;
    for (const auto& [incarnation, dir] : entry(name, false).incarnations) names.push_back(incarnation);
    return names;
  }
  void removeIncarnation(std::string_view name, std::string_view incarnation) override {
    std::lock_guard lock(mutex);
    if (auto it = entries.find(name); it != entries.end()) {
      if (auto dir = it->second.incarnations.find(incarnation); dir != it->second.incarnations.end()) it->second.incarnations.erase(dir);
    }
  }
  void remove(std::string_view name) override {
    std::lock_guard lock(mutex);
    if (auto it = entries.find(name); it != entries.end()) entries.erase(it);
  }
};


/// Factory that opens FSDirectory instances relative to basePath_/c/.
/// Shared resources (dictionaries, etc.) live directly under basePath_.
///
/// `unowned` opens an existing data directory without claiming it: no
/// write.lock or directory creation.  It suppresses only the
/// open-time side effects, which a Directory wrapper cannot reach because they
/// happen here in the constructor; rejecting mutations is ReadOnlyDirFactory's
/// job, and the two are meant to be composed (see LuxirNode::createSingletons).
class FSDirFactory : public DirectoryFactory {
  std::filesystem::path basePath_;
  std::filesystem::path collectionsPath_;  // basePath_/c
  std::optional<FileLock> lock_;
  bool unowned;

public:
  explicit FSDirFactory(std::filesystem::path path, bool unowned = false)
      : basePath_(std::move(path)),
        collectionsPath_(basePath_ / "c"), unowned(unowned) {
    if (unowned) {
      if (!std::filesystem::is_directory(collectionsPath_)) {
        throw ReadOnlyError("data directory does not exist (or holds no collections): " +
                            basePath_.string());
      }
      LOG_INFO("Using existing data directory unowned (no write lock): {}", basePath_.string());
      return;
    }

    bool created = std::filesystem::create_directories(collectionsPath_);
    if (created) {
      LOG_INFO("Created data directory: {}", basePath_.string());
    } else {
      LOG_INFO("Using existing data directory: {}", basePath_.string());
    }
    lock_.emplace(basePath_ / "write.lock");
    if (created) {
      FSDirectory base(basePath_);
      std::array<std::string, 1> directory{"."};
      base.sync(directory);
    }
  }

  std::vector<std::string> collections() override { return directoriesIn(collectionsPath_); }

  std::shared_ptr<Directory> container(std::string_view name, bool create) override {
    auto path = collectionsPath_ / name;
    if (create) makeDirectory(path);
    else if (!std::filesystem::is_directory(path)) throw std::runtime_error("collection directory does not exist: " + path.string());
    return std::make_shared<FSDirectory>(path);
  }

  std::shared_ptr<Directory> incarnation(std::string_view name, std::string_view incarnation, bool create) override {
    auto path = collectionsPath_ / name / incarnation;
    if (create) { makeDirectory(path.parent_path()); makeDirectory(path); }
    else if (!std::filesystem::is_directory(path)) throw std::runtime_error("incarnation directory does not exist: " + path.string());
    return std::make_shared<FSDirectory>(path);
  }

  void createCollection(std::string_view name) override {
    auto path = collectionsPath_ / name;
    if (unowned) throw ReadOnlyError("cannot create a collection in an unowned data directory");
    if (!std::filesystem::create_directory(path)) {
      throw std::filesystem::filesystem_error("collection directory already exists", path,
          std::make_error_code(std::errc::file_exists));
    }
    syncDirectory(collectionsPath_);
  }

  std::vector<std::string> incarnations(std::string_view name) override { return directoriesIn(collectionsPath_ / name); }

  void removeIncarnation(std::string_view name, std::string_view incarnation) override {
    std::filesystem::remove_all(collectionsPath_ / name / incarnation);
    syncDirectory(collectionsPath_ / name);
  }

  void remove(std::string_view name) override {
    auto path = collectionsPath_ / name;
    if (std::filesystem::exists(path / "CURRENT")) {
      FSDirectory container(path);
      container.deleteFile("CURRENT");
      syncDirectory(path);
    }
    std::filesystem::remove_all(path);
    syncDirectory(collectionsPath_);
  }

private:
  static void syncDirectory(const std::filesystem::path& path) {
    std::array<std::string, 1> directory{"."};
    FSDirectory(path).sync(directory);
  }
  // Creates `path` if missing and makes its entry in the parent durable.
  void makeDirectory(const std::filesystem::path& path) {
    if (std::filesystem::is_directory(path)) return;
    if (unowned) throw ReadOnlyError("directory does not exist: " + path.string());
    if (std::filesystem::create_directory(path)) syncDirectory(path.parent_path());
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
