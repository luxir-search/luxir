// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <boost/unordered/unordered_flat_set.hpp>
#include "DelegatingDirectory.h"
#include "luxir/util/Signal.h"

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
  std::shared_ptr<Directory> delegate;
  CheckedDirMode mode_;
  bool verbose_;

  // Files that have been finishFile()'d but not yet sync()'d in this session.
  boost::unordered_flat_set<std::string> unsyncedFiles_;
  std::mutex mu_;

  void reportUnsynced(std::string_view operation, const std::string& name) {
    std::string msg = "CheckedDirectory: " + std::string(operation) + " of unsynced file: " + name;
    if (mode_ == CheckedDirMode::THROW) {
      throw std::runtime_error(msg);
    } else {
      LOG_WARN("{}", msg);
    }
  }

public:
  CheckedDirectory(std::shared_ptr<Directory> inner, CheckedDirMode mode, bool verbose = false)
      : delegate(std::move(inner)), mode_(mode), verbose_(verbose) {}

  void listFiles(std::vector<FileInfo>& target) override { delegate->listFiles(target); }
  Directory& underlying() override { return delegate->underlying(); }
  Capabilities capabilities() const override { return delegate->capabilities(); }
  std::shared_ptr<ChecksumReader> openChecksum(std::string_view name, bool direct = false) override {
    return delegate->openChecksum(name, direct);
  }

  std::shared_ptr<InputFile> openFile(std::string_view name, bool expectSynced = false) override {
    if (expectSynced) {
      std::lock_guard lock(mu_);
      auto it = unsyncedFiles_.find(std::string(name));
      if (it != unsyncedFiles_.end()) {
        reportUnsynced("expectSynced openFile", *it);
      }
    }
    if (verbose_) LOG_DEBUG("CheckedDirectory: openFile({}, expectSynced={})", name, expectSynced);
    return delegate->openFile(name, expectSynced);
  }

  // Readers share an open segment rather than reopening it, so most reads
  // never reach openFile(expectSynced): publication checks every file.
  void expectSynced(std::span<const FileDescriptor> files) override {
    std::lock_guard lock(mu_);
    for (const auto& file : files) {
      if (unsyncedFiles_.contains(file.name)) reportUnsynced("publication", file.name);
    }
  }

  void knownFiles(std::span<const FileDescriptor> files) override { delegate->knownFiles(files); }

  std::unique_ptr<File> createFile(std::string_view name) override {
    // A non-null test listener result injects a file creation failure.
    if (Signal::emit("checkedCreateFile", this, &name)) return nullptr;
    if (verbose_) LOG_DEBUG("CheckedDirectory: createFile({})", name);
    return delegate->createFile(name);
  }

  std::unique_ptr<File> createFile(
      std::string_view name, FileCreateOptions options) override {
    if (Signal::emit("checkedCreateFile", this, &name)) return nullptr;
    if (verbose_) LOG_DEBUG("CheckedDirectory: createFile({}, ramDelegating={})",
                            name, options.ramDelegating);
    return delegate->createFile(name, std::move(options));
  }

  bool deleteFile(std::string_view name) override {
    {
      std::lock_guard lock(mu_);
      unsyncedFiles_.erase(std::string(name));
    }
    if (verbose_) LOG_DEBUG("CheckedDirectory: deleteFile({})", name);
    return delegate->deleteFile(name);
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
    delegate->deletePrefix(prefix);
  }

  void finishFile(File& file) override {
    delegate->finishFile(file);
    {
      std::lock_guard lock(mu_);
      unsyncedFiles_.insert(std::string(file.name()));
    }
    if (verbose_) LOG_DEBUG("CheckedDirectory: finishFile({}) size={}", file.name(), file.size());
  }

  std::optional<PublicationVersion> publishFile(std::string_view name, std::span<const std::byte> bytes, PublishCondition condition) override {
    std::optional<PublicationVersion> applied;
    try { applied = delegate->publishFile(name, bytes, condition); }
    catch (...) {
      std::lock_guard lock(mu_);
      unsyncedFiles_.insert(std::string(name)); // installation may have preceded the failure
      throw;
    }
    if (applied) {
      std::lock_guard lock(mu_);
      unsyncedFiles_.erase(std::string(name));
    }
    if (verbose_) LOG_DEBUG("CheckedDirectory: publishFile({}, applied={})", name, applied.has_value());
    return applied;
  }

  std::optional<Publication> readPublished(std::string_view name) override { return delegate->readPublished(name); }

  void deleteDurably(std::span<const std::string> names) override {
    delegate->deleteDurably(names);
    std::lock_guard lock(mu_);
    for (const auto& name : names) unsyncedFiles_.erase(name);
  }

  void sync(std::span<const std::string> filenames) override {
    delegate->sync(filenames);
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
    delegate->clear();
  }

  void linkFile(Directory& source, std::string_view name) override {
    source.openFile(name, true);
    delegate->linkFile(source, name);
  }
};


/// Factory wrapper that creates CheckedDirectory instances around another factory's directories.
class CheckedDirFactory : public DelegatingDirFactory {
  CheckedDirMode mode_;
  bool verbose_;
  std::mutex mu_;
  // Reopening must not forget unsynced files until the namespace is removed.
  std::map<std::pair<StorageRoot, std::string>, std::shared_ptr<CheckedDirectory>> opened_;
public:
  CheckedDirFactory(std::unique_ptr<DirectoryFactory> inner, CheckedDirMode mode, bool verbose = false)
      : DelegatingDirFactory(std::move(inner)), mode_(mode), verbose_(verbose) {}
  std::string describe() const override { return "checked(" + delegate->describe() + ")"; }
  DirectoryFactory* inner() override { return delegate.get(); }
  std::shared_ptr<Directory> directory(StorageRoot root, std::string_view path, ViewOptions options = {}) override {
    auto key = std::pair(root, checkedPath(root, path));
    auto dir = delegate->directory(root, path, std::move(options));
    std::lock_guard lock(mu_);
    auto& checked = opened_[key];
    if (!checked) checked = std::make_shared<CheckedDirectory>(std::move(dir), mode_, verbose_);
    return checked;
  }
  void removeTree(StorageRoot root, std::string_view path) override {
    delegate->removeTree(root, path);
    std::lock_guard lock(mu_);
    std::erase_if(opened_, [&](const auto& entry) {
      return entry.first.first == root && (entry.first.second == path || entry.first.second.starts_with(std::string(path) + '/'));
    });
  }

};

} // namespace luxir
