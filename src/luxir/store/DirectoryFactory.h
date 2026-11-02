// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <map>
#include <mutex>
#include <set>
#include "Directory.h"
#include "FileLock.h"
#include "FSDirectory.h"

namespace luxir {

enum class StorageRoot { AUTHORITATIVE, NODE_PRIVATE };

class DirectoryFactory {
public:
  struct ViewOptions { std::shared_ptr<StorageMemory> account; };
  struct Child { std::string name; bool directory; };
  virtual ~DirectoryFactory() = default;
  // Opening a view creates no namespace entries. Paths are relative to the root.
  virtual std::shared_ptr<Directory> directory(StorageRoot root, std::string_view path, ViewOptions options = {}) = 0;
  virtual std::vector<Child> children(StorageRoot root, std::string_view path) = 0;
  // Removes a subtree durably. Missing subtrees are allowed; roots cannot be removed.
  virtual void removeTree(StorageRoot root, std::string_view path) = 0;
  virtual std::shared_ptr<StorageMemory> memory() { return {}; }
  virtual std::string describe() const = 0;
  // Local path for disk statistics, or empty for a backend without one.
  virtual std::string diskPath() const { return {}; }
  uint64_t storageBytes() { auto account = memory(); return account ? account->bytes() : 0; }

  static std::string checkedPath(StorageRoot root, std::string_view path) {
    if (path.find_first_of("\\\0", 0, 2) != std::string_view::npos) throw std::invalid_argument("invalid storage path");
    size_t start = 0;
    while (start < path.size()) {
      auto end = path.find('/', start);
      if (end == std::string_view::npos) end = path.size();
      auto part = path.substr(start, end - start);
      if (part.empty() || part == "." || part == ".." || (start == 0 && root == StorageRoot::AUTHORITATIVE && part == "node")) {
        throw std::invalid_argument("invalid storage path: " + std::string(path));
      }
      start = end + 1;
    }
    if (!path.empty() && path.back() == '/') throw std::invalid_argument("invalid storage path");
    return std::string(path);
  }
};

class RAMDirFactory : public DirectoryFactory {
  std::mutex mutex;
  std::map<std::pair<StorageRoot, std::string>, std::shared_ptr<RAMDir>> views;
  std::shared_ptr<StorageMemory> account;
public:
  explicit RAMDirFactory(uint64_t limit = 0, std::function<bool()> reclaim = {})
      : account(std::make_shared<StorageMemory>(limit, nullptr, std::move(reclaim))) {}
  std::string describe() const override { return "ram"; }
  std::shared_ptr<StorageMemory> memory() override { return account; }
  std::shared_ptr<Directory> directory(StorageRoot root, std::string_view path, ViewOptions options = {}) override {
    auto key = std::pair(root, checkedPath(root, path));
    std::lock_guard lock(mutex);
    auto& dir = views[key];
    if (!dir) dir = std::make_shared<RAMDir>(options.account ? std::move(options.account) : account);
    return dir;
  }
  std::vector<Child> children(StorageRoot root, std::string_view path) override {
    auto key = checkedPath(root, path);
    auto prefix = key.empty() ? key : key + '/';
    std::lock_guard lock(mutex);
    std::map<std::string, bool> found;
    for (const auto& [view, dir] : views) {
      if (view.first != root || !dir->exists()) continue;
      if (view.second == key) {
        std::vector<Directory::FileInfo> files;
        dir->listFiles(files);
        for (const auto& file : files) found.emplace(file.name, false);
      } else if (view.second.starts_with(prefix)) {
        auto rest = std::string_view(view.second).substr(prefix.size());
        found[std::string(rest.substr(0, rest.find('/')))] = true;
      }
    }
    std::vector<Child> result;
    for (const auto& [name, directory] : found) result.push_back({name, directory});
    return result;
  }
  void removeTree(StorageRoot root, std::string_view path) override {
    auto key = checkedPath(root, path);
    if (key.empty()) throw std::invalid_argument("cannot remove a storage root");
    std::lock_guard lock(mutex);
    std::erase_if(views, [&](const auto& view) {
      if (view.first.first != root || (view.first.second != key && !view.first.second.starts_with(key + '/'))) return false;
      view.second->clear();
      return true;
    });
  }
};

// Both roots use the configured data directory: node/ is exclusively node-private.
// An unowned open creates nothing and takes no lock. ReadOnlyDirFactory rejects writes.
class FSDirFactory : public DirectoryFactory {
  std::filesystem::path basePath;
  std::optional<FileLock> lock;
  bool unowned;
  std::mutex mutex;
  std::set<std::filesystem::path> cleaned;
  std::map<std::filesystem::path, std::shared_ptr<FSDirectory::ParentSync>> parentSync;

  std::shared_ptr<FSDirectory::ParentSync> parentsOf(const std::filesystem::path& path) {
    std::lock_guard guard(mutex);
    auto& state = parentSync[path];
    if (!state) state = std::make_shared<FSDirectory::ParentSync>();
    return state;
  }

  std::filesystem::path rootPath(StorageRoot root) const {
    return root == StorageRoot::AUTHORITATIVE ? basePath : basePath / "node";
  }
  std::filesystem::path pathOf(StorageRoot root, std::string_view relative) const {
    auto base = rootPath(root);
    auto key = checkedPath(root, relative);
    auto path = key.empty() ? base : base / key;
    auto within = std::filesystem::weakly_canonical(path).lexically_relative(base);
    if (within.empty() || *within.begin() == ".."
        || (root == StorageRoot::AUTHORITATIVE && *within.begin() == "node")) {
      throw std::invalid_argument("storage path escapes its root");
    }
    return path; // removeTree must unlink a final symlink, not remove its target.
  }
public:
  explicit FSDirFactory(std::filesystem::path path, bool unowned = false)
      : basePath(std::filesystem::weakly_canonical(std::filesystem::absolute(path))), unowned(unowned) {
    if (unowned) {
      if (!std::filesystem::is_directory(basePath)) throw ReadOnlyError("data directory does not exist: " + basePath.string());
    } else {
      if (std::filesystem::is_symlink(basePath / "node")) throw std::invalid_argument("node-private root must not be a symlink");
      FSDirectory(basePath / "node", {}, parentsOf(basePath / "node")).sync({});
      lock.emplace(basePath / "node" / "write.lock");
    }
  }
  std::string describe() const override { return "fs:" + basePath.string(); }
  std::string diskPath() const override { return basePath.string(); }
  std::shared_ptr<Directory> directory(StorageRoot root, std::string_view relative, ViewOptions options = {}) override {
    auto path = pathOf(root, relative);
    if (!unowned && std::filesystem::is_directory(path)) {
      std::lock_guard guard(mutex);
      if (cleaned.insert(path).second) FSDirectory::cleanPublicationTemps(path);
    }
    return std::make_shared<FSDirectory>(path, basePath, parentsOf(path));
  }
  std::vector<Child> children(StorageRoot root, std::string_view relative) override {
    auto path = pathOf(root, relative);
    if (!std::filesystem::exists(path)) return {};
    std::vector<Child> result;
    for (const auto& entry : std::filesystem::directory_iterator(path)) {
      auto name = entry.path().filename().string();
      if (FSDirectory::isPublishTemp(name) || (root == StorageRoot::AUTHORITATIVE && relative.empty() && name == "node")) continue;
      result.push_back({name, entry.symlink_status().type() == std::filesystem::file_type::directory});
    }
    std::ranges::sort(result, {}, &Child::name);
    return result;
  }
  void removeTree(StorageRoot root, std::string_view relative) override {
    if (relative.empty()) throw std::invalid_argument("cannot remove a storage root");
    auto path = pathOf(root, relative);
    std::vector<std::shared_ptr<FSDirectory::ParentSync>> forgotten;
    {
      std::lock_guard guard(mutex);
      auto removed = [&](const auto& item) { return item == path || item.string().starts_with(path.string() + '/'); };
      std::erase_if(cleaned, removed);
      std::erase_if(parentSync, [&](const auto& entry) {
        if (!removed(entry.first)) return false;
        forgotten.push_back(entry.second);
        return true;
      });
    }
    // Invalidate before removal, including views still held if removal fails.
    for (const auto& state : forgotten) {
      std::lock_guard guard(state->mutex);
      state->durable = false;
    }
    std::filesystem::remove_all(path);
    // An interrupted removal still needs its surviving ancestor synced.
    auto parent = path.parent_path();
    while (!std::filesystem::is_directory(parent)) parent = parent.parent_path();
    FSDirectory(parent).sync({});
  }
};

} // namespace luxir
