// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <map>
#include <mutex>

#include "luxir/store/DelegatingDirectory.h"
#include "luxir/store/StorageRegistry.h"

namespace luxir::example {

// Per-factory counts shared with its directory views. An empty path counts all views.
class StorageCounters {
  mutable std::mutex mutex;
  std::map<std::pair<std::string, std::string>, uint64_t> counts;
public:
  void add(std::string_view op, std::string_view path) {
    std::lock_guard lock(mutex);
    counts[{std::string(op), std::string(path)}]++;
    if (!path.empty()) counts[{std::string(op), ""}]++;
  }
  uint64_t count(std::string_view op, std::string_view path = {}) const {
    std::lock_guard lock(mutex);
    auto it = counts.find({std::string(op), std::string(path)});
    return it == counts.end() ? 0 : it->second;
  }
};

inline std::string viewLabel(StorageRoot root, std::string_view path) {
  return root == StorageRoot::NODE_PRIVATE ? "node/" + std::string(path) : std::string(path);
}

class CountingDirectory : public DelegatingDirectory {
  std::shared_ptr<StorageCounters> counters;
  std::string path;
public:
  CountingDirectory(std::shared_ptr<Directory> inner, std::shared_ptr<StorageCounters> counters, std::string path)
      : DelegatingDirectory(std::move(inner)), counters(std::move(counters)), path(std::move(path)) {}

  std::shared_ptr<InputFile> openFile(std::string_view name, bool expectSynced = false) override {
    counters->add("open", path);
    return delegate->openFile(name, expectSynced);
  }
  std::unique_ptr<File> createFile(std::string_view name) override {
    counters->add("create", path);
    return delegate->createFile(name);
  }
  std::unique_ptr<File> createFile(std::string_view name, FileCreateOptions options) override {
    counters->add("create", path);
    return delegate->createFile(name, std::move(options));
  }
};

class CountingLayer : public DelegatingDirFactory {
public:
  std::shared_ptr<StorageCounters> counters = std::make_shared<StorageCounters>();

  using DelegatingDirFactory::DelegatingDirFactory;
  bool exclusive() const override { return delegate->exclusive(); }
  std::shared_ptr<Directory> directory(StorageRoot root, std::string_view path, ViewOptions options = {}) override {
    return std::make_shared<CountingDirectory>(delegate->directory(root, path, std::move(options)), counters, viewLabel(root, path));
  }
  std::string describe() const override { return "counting(" + delegate->describe() + ")"; }
};

class CountingRamBackend : public DelegatingDirFactory {
public:
  std::shared_ptr<StorageCounters> counters = std::make_shared<StorageCounters>();

  explicit CountingRamBackend(const StorageContext& ctx)
      : DelegatingDirFactory(StorageRegistry::global().makeBackend("ram", ctx)) {}
  bool exclusive() const override { return true; }
  std::shared_ptr<Directory> directory(StorageRoot root, std::string_view path, ViewOptions options = {}) override {
    counters->add("open", viewLabel(root, path));
    return delegate->directory(root, path, std::move(options));
  }
  std::string describe() const override { return "counting_ram"; }
};

}  // namespace luxir::example
