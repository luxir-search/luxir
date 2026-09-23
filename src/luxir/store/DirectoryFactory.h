// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <map>
#include <mutex>
#include <optional>
#include "Directory.h"
#include "FileLock.h"
#include "FSDirectory.h"
#include "luxir/util/log.h"

namespace luxir {

/// Creates Directory instances for collections. Owns shared resources
/// (e.g. I/O threads, dictionaries) that live under the base path.
/// Lives on LuxirNode - one per instance.
class DirectoryFactory {
public:
  virtual ~DirectoryFactory() = default;

  /// Open or create storage. Exclusive creation fails if storage already exists.
  virtual std::shared_ptr<Directory> create(std::string_view collectionName, bool exclusive = false) = 0;

  /// Return the names of collections that already exist on disk.
  virtual std::vector<std::string> listCollections() = 0;

  /// Remove all storage for the named collection.
  virtual void remove(std::string_view collectionName) = 0;
};


/// Factory that creates RAMDir instances (one per collection).
class RAMDirFactory : public DirectoryFactory {
public:
  std::shared_ptr<Directory> create(std::string_view collectionName, bool exclusive = false) override {
    (void)collectionName;
    (void)exclusive;
    return std::make_shared<RAMDir>();
  }

  std::vector<std::string> listCollections() override {
    return {};  // RAM has no persistence
  }

  void remove(std::string_view collectionName) override {
    (void)collectionName;
  }
};


/// Factory that creates FSDirectory instances under basePath_/c/<name>.
/// Shared resources (dictionaries, etc.) live directly under basePath_.
///
/// `unowned` opens an existing data directory without claiming it: no
/// write.lock, no directory creation, no trash reset.  It suppresses only the
/// open-time side effects, which a Directory wrapper cannot reach because they
/// happen here in the constructor; rejecting mutations is ReadOnlyDirFactory's
/// job, and the two are meant to be composed (see LuxirNode::createSingletons).
class FSDirFactory : public DirectoryFactory {
  std::filesystem::path basePath_;
  std::filesystem::path collectionsPath_;  // basePath_/c
  std::filesystem::path trashPath_;  // basePath_/trash
  std::optional<FileLock> lock_;
  std::mutex removeMutex_;
  std::map<std::string, std::filesystem::path> pendingTrash_;
  uint64_t trashSequence_ = 0;

public:
  explicit FSDirFactory(std::filesystem::path path, bool unowned = false)
      : basePath_(std::move(path)),
        collectionsPath_(basePath_ / "c"),
        trashPath_(basePath_ / "trash") {
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
    std::filesystem::remove_all(trashPath_);
    std::filesystem::create_directories(trashPath_);
  }

  std::shared_ptr<Directory> create(std::string_view collectionName, bool exclusive = false) override {
    auto path = collectionsPath_ / collectionName;
    if (exclusive && !std::filesystem::create_directory(path)) {
      throw std::filesystem::filesystem_error("collection directory already exists", path,
          std::make_error_code(std::errc::file_exists));
    }
    try {
      return std::make_shared<FSDirectory>(path);
    } catch (...) {
      if (exclusive) std::filesystem::remove(path);
      throw;
    }
  }

  std::vector<std::string> listCollections() override {
    std::vector<std::string> result;
    for (const auto& entry : std::filesystem::directory_iterator(collectionsPath_)) {
      if (entry.is_directory()) {
        result.push_back(entry.path().filename().string());
      }
    }
    std::sort(result.begin(), result.end());
    return result;
  }

  void remove(std::string_view collectionName) override {
    const std::lock_guard<std::mutex> lock(removeMutex_);
    std::string name(collectionName);
    if (auto pending = pendingTrash_.find(name); pending != pendingTrash_.end()) {
      std::filesystem::remove_all(pending->second);
      pendingTrash_.erase(pending);
    }

    auto source = collectionsPath_ / collectionName;
    auto destination = trashPath_ / (name + "-" + std::to_string(trashSequence_++));
    pendingTrash_.emplace(name, destination);

    std::error_code ec;
    std::filesystem::rename(source, destination, ec);
    if (ec) {
      std::error_code existsError;
      bool sourceExists = std::filesystem::exists(source, existsError);
      if (!existsError && !sourceExists) {
        pendingTrash_.erase(name);
        return;
      }
      pendingTrash_.erase(name);
      throw std::filesystem::filesystem_error(
          "failed to move collection to trash", source, destination, ec);
    }
    std::filesystem::remove_all(destination);
    pendingTrash_.erase(name);
  }
};


} // namespace luxir
