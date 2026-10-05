// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <boost/unordered/unordered_flat_set.hpp>
#include "DirectoryFactory.h"

namespace luxir {

enum class CheckedDirMode {
  WARN,  // Log a warning (suitable for production)
  THROW  // Throw an exception (suitable for tests)
};

/// Directory wrapper that validates fsync correctness and optionally logs operations.
/// Tracks files that have been finished via finishFile() but not yet synced via sync().
/// When openFile() is called with expectSynced=true on such a file, it reports the
/// gap - either as a log warning (production) or an exception (tests).
///
/// Files that already existed in the directory before this wrapper was created
/// (e.g. from a previous session) are assumed to have been properly synced.
class CheckedDirectory : public Directory {
  std::shared_ptr<Directory> delegate_;
  CheckedDirMode mode_;
  bool verbose_;

  // Files that have been finishFile()'d but not yet sync()'d in this session.
  boost::unordered_flat_set<std::string> unsyncedFiles_;
  std::mutex mu_;

  void reportUnsyncedRead(const std::string& name) {
    std::string msg = "CheckedDirectory: expectSynced openFile on unsynced file: " + name;
    if (mode_ == CheckedDirMode::THROW) {
      throw std::runtime_error(msg);
    } else {
      LOG_WARN("{}", msg);
    }
  }

public:
  CheckedDirectory(std::shared_ptr<Directory> delegate, CheckedDirMode mode, bool verbose = false)
      : delegate_(std::move(delegate)), mode_(mode), verbose_(verbose) {}

  void listFiles(std::vector<FileInfo>& target) override {
    delegate_->listFiles(target);
  }

  std::shared_ptr<InputFile> openFile(std::string_view name, bool expectSynced = false) override {
    if (expectSynced) {
      std::lock_guard lock(mu_);
      auto it = unsyncedFiles_.find(std::string(name));
      if (it != unsyncedFiles_.end()) {
        reportUnsyncedRead(*it);
      }
    }
    if (verbose_) LOG_DEBUG("CheckedDirectory: openFile({}, expectSynced={})", name, expectSynced);
    return delegate_->openFile(name, expectSynced);
  }

  std::unique_ptr<File> createFile(std::string_view name) override {
    if (verbose_) LOG_DEBUG("CheckedDirectory: createFile({})", name);
    return delegate_->createFile(name);
  }

  std::unique_ptr<File> createFile(
      std::string_view name, FileCreateOptions options) override {
    if (verbose_) LOG_DEBUG("CheckedDirectory: createFile({}, ramDelegating={})",
                            name, options.ramDelegating);
    return delegate_->createFile(name, std::move(options));
  }

  bool deleteFile(std::string_view name) override {
    {
      std::lock_guard lock(mu_);
      unsyncedFiles_.erase(std::string(name));
    }
    if (verbose_) LOG_DEBUG("CheckedDirectory: deleteFile({})", name);
    return delegate_->deleteFile(name);
  }

  void deletePrefix(std::string_view prefix) override {
    {
      std::lock_guard lock(mu_);
      for (auto it = unsyncedFiles_.begin(); it != unsyncedFiles_.end();) {
        if (it->starts_with(prefix)) {
          it = unsyncedFiles_.erase(it);
        } else {
          ++it;
        }
      }
    }
    if (verbose_) LOG_DEBUG("CheckedDirectory: deletePrefix({})", prefix);
    delegate_->deletePrefix(prefix);
  }

  void finishFile(File& file) override {
    delegate_->finishFile(file);
    {
      std::lock_guard lock(mu_);
      unsyncedFiles_.insert(std::string(file.name()));
    }
    if (verbose_) LOG_DEBUG("CheckedDirectory: finishFile({}) size={}", file.name(), file.size());
  }

  void renameFile(std::string_view from, std::string_view to) override {
    delegate_->renameFile(from, to);
    std::lock_guard lock(mu_);
    bool unsynced = unsyncedFiles_.erase(std::string(from));
    unsyncedFiles_.erase(std::string(to));
    if (unsynced) unsyncedFiles_.insert(std::string(to));
    if (verbose_) LOG_DEBUG("CheckedDirectory: renameFile({} -> {})", from, to);
  }

  void sync(std::span<const std::string> filenames) override {
    delegate_->sync(filenames);
    {
      std::lock_guard lock(mu_);
      for (auto& name : filenames) {
        unsyncedFiles_.erase(name);
      }
    }
    if (verbose_) LOG_DEBUG("CheckedDirectory: sync({} files)", filenames.size());
  }

  void clear() override {
    {
      std::lock_guard lock(mu_);
      unsyncedFiles_.clear();
    }
    if (verbose_) LOG_DEBUG("CheckedDirectory: clear()");
    delegate_->clear();
  }

  /// Access the underlying directory.
  Directory& underlying() override { return delegate_->underlying(); }
  void linkFile(Directory& source, std::string_view name) override {
    source.openFile(name, true);
    delegate_->linkFile(source, name);
  }
};


/// Factory wrapper that creates CheckedDirectory instances around another factory's directories.
class CheckedDirFactory : public DirectoryFactory {
  std::unique_ptr<DirectoryFactory> delegate_;
  CheckedDirMode mode_;
  bool verbose_;
  std::mutex mu_;
  // Reopening a directory must not forget which of its files are unsynced, so
  // wrappers live until their namespace is removed. Keys are collection or
  // collection/incarnation.
  std::map<std::string, std::shared_ptr<CheckedDirectory>, std::less<>> opened_;

  std::shared_ptr<Directory> wrap(std::string key, std::shared_ptr<Directory> dir) {
    std::lock_guard lock(mu_);
    auto& checked = opened_[std::move(key)];
    if (!checked) checked = std::make_shared<CheckedDirectory>(std::move(dir), mode_, verbose_);
    return checked;
  }

public:
  CheckedDirFactory(std::unique_ptr<DirectoryFactory> delegate, CheckedDirMode mode, bool verbose = false)
      : delegate_(std::move(delegate)), mode_(mode), verbose_(verbose) {}

  uint64_t storageBytes(std::string_view collection = {}) override { return delegate_->storageBytes(collection); }
  std::vector<std::string> collections() override { return delegate_->collections(); }
  std::shared_ptr<Directory> container(std::string_view name, bool create) override {
    return wrap(std::string(name), delegate_->container(name, create));
  }
  std::shared_ptr<Directory> incarnation(std::string_view name, std::string_view incarnation, bool create) override {
    return wrap(std::string(name) + "/" + std::string(incarnation), delegate_->incarnation(name, incarnation, create));
  }
  void createCollection(std::string_view name) override { delegate_->createCollection(name); }
  std::vector<std::string> incarnations(std::string_view name) override { return delegate_->incarnations(name); }
  void removeIncarnation(std::string_view name, std::string_view incarnation) override {
    delegate_->removeIncarnation(name, incarnation);
    std::lock_guard lock(mu_);
    opened_.erase(std::string(name) + "/" + std::string(incarnation));
  }
  void remove(std::string_view name) override {
    delegate_->remove(name);
    std::lock_guard lock(mu_);
    std::erase_if(opened_, [&](const auto& entry) {
      return entry.first == name || (entry.first.starts_with(name) && entry.first[name.size()] == '/');
    });
  }
};

} // namespace luxir
