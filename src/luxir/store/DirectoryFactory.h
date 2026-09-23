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

/// Opens directories relative to c/; CURRENT selects c/name/incarnation.
/// Owns shared resources
/// (e.g. I/O threads, dictionaries) that live under the base path.
/// Lives on LuxirNode - one per instance.
class DirectoryFactory {
public:
  struct Selection {
    std::string incarnation;
  };

  static Selection current(Directory& container) {
    auto file = container.openFile("CURRENT");
    if (!file) return {};
    Selection result;
    if (glz::read_json(result, file->read()) || result.incarnation.empty()
        || result.incarnation.find_first_of("/\\") != std::string::npos
        || result.incarnation == "." || result.incarnation == "..") {
      throw std::runtime_error("Invalid collection CURRENT");
    }
    return result;
  }

  void select(std::string_view name, const Selection& selection) {
    auto container = create(name);
    auto bytes = glz::write_json(selection).value();
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
    create("")->sync(directory); // persist the collection parent itself
  }

  // Build a durable copy with a fresh identity, without changing CURRENT yet.
  Selection copySnapshot(std::string_view name, const Selection& from, const Manifest& manifest) {
    auto old = create(std::string(name) + "/" + from.incarnation);
    Selection selection{newUuid()};
    auto next = create(std::string(name) + "/" + selection.incarnation);
    std::pmr::monotonic_buffer_resource arena;
    auto info = Manifest::decode(manifest.bytes, arena);
    auto files = filesOf(info);
    next->reuseFiles(*old, files, files);
    std::array<std::string, 1> directory{"."};
    next->sync(directory);
    info.incarnation = selection.incarnation;
    info.index_gen = manifest.highestGeneration + 1;
    info.commit_time++;
    std::vector<std::byte> bytes;
    if (!api::encode(info, bytes)) throw std::runtime_error("Failed to encode copied snapshot");
    Manifest::write(*next, info.index_gen, bytes);
    std::array<std::string, 1> root{Manifest::name(info.index_gen)};
    next->sync(root); next->sync(directory);
    return selection;
  }

  virtual uint64_t storageBytes(std::string_view collection = {}) { unused(collection); return 0; }

  virtual ~DirectoryFactory() = default;

  /// Open or create a directory relative to c/. Exclusive creation fails if it exists.
  virtual std::shared_ptr<Directory> create(std::string_view collectionName, bool exclusive = false) = 0;

  /// List child directories, defaulting to the collection names under c/.
  virtual std::vector<std::string> listDirectories(std::string_view parent = {}) = 0;

  /// Remove all storage for the named collection.
  virtual void remove(std::string_view collectionName) = 0;
};


/// Factory that retains RAMDir instances by their collection-relative paths.
class RAMDirFactory : public DirectoryFactory {
  std::mutex mutex;
  std::map<std::string, std::shared_ptr<Directory>, std::less<>> directories;
  std::shared_ptr<StorageMemory> memory;
  std::map<std::string, std::weak_ptr<StorageMemory>, std::less<>> collections;
public:
  explicit RAMDirFactory(uint64_t limit = 0, std::function<bool()> reclaim = {})
      : memory(std::make_shared<StorageMemory>(limit, nullptr, std::move(reclaim))) {}
  uint64_t storageBytes(std::string_view collection = {}) override {
    std::lock_guard lock(mutex);
    std::erase_if(collections, [](const auto& entry) { return entry.second.expired(); });
    if (collection.empty()) return memory->bytes();
    auto it = collections.find(collection);
    auto account = it == collections.end() ? nullptr : it->second.lock();
    return account ? account->bytes() : 0;
  }
  std::shared_ptr<Directory> create(std::string_view collectionName, bool exclusive = false) override {
    std::lock_guard lock(mutex);
    auto [it, inserted] = directories.try_emplace(std::string(collectionName));
    if (!inserted && exclusive) throw std::runtime_error("collection directory already exists");
    if (inserted) {
      auto name = collectionName.substr(0, collectionName.find('/'));
      auto& weak = collections[std::string(name)];
      auto account = weak.lock();
      if (!account) { account = std::make_shared<StorageMemory>(0, memory); weak = account; }
      it->second = std::make_shared<RAMDir>(std::move(account));
    }
    return it->second;
  }

  std::vector<std::string> listDirectories(std::string_view parent = {}) override {
    std::lock_guard lock(mutex);
    std::vector<std::string> names;
    std::string prefix = parent.empty() ? "" : std::string(parent) + "/";
    for (const auto& [name, dir] : directories) {
      if (!name.starts_with(prefix)) continue;
      auto child = name.substr(prefix.size());
      if (!child.empty() && child.find('/') == std::string::npos) names.push_back(child);
    }
    return names;
  }

  void remove(std::string_view collectionName) override {
    std::lock_guard lock(mutex);
    std::string prefix = std::string(collectionName) + "/";
    std::erase_if(directories, [&](const auto& entry) { return entry.first == collectionName || entry.first.starts_with(prefix); });
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

  std::shared_ptr<Directory> create(std::string_view collectionName, bool exclusive = false) override {
    auto path = collectionsPath_ / collectionName;
    if (unowned && !std::filesystem::is_directory(path)) throw ReadOnlyError("collection directory does not exist: " + path.string());
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

  std::vector<std::string> listDirectories(std::string_view parent = {}) override {
    std::vector<std::string> result;
    for (const auto& entry : std::filesystem::directory_iterator(collectionsPath_ / parent)) {
      if (entry.is_directory()) {
        result.push_back(entry.path().filename().string());
      }
    }
    std::sort(result.begin(), result.end());
    return result;
  }

  void remove(std::string_view collectionName) override {
    auto path = collectionsPath_ / collectionName;
    if (std::filesystem::exists(path / "CURRENT")) {
      FSDirectory container(path);
      container.deleteFile("CURRENT");
      std::array<std::string, 1> directory{"."};
      container.sync(directory);
    }
    std::filesystem::remove_all(path);
    FSDirectory collections(collectionsPath_);
    std::array<std::string, 1> directory{"."};
    collections.sync(directory);
  }
};


} // namespace luxir
