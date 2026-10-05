// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <algorithm>
#include "DirectoryFactory.h"

namespace luxir {

/// Directory wrapper that serves reads and rejects every mutation.
///
/// This is the enforcement point for --read-only: the node opens a data
/// directory it does not own (no write.lock), so nothing may write to it.
/// Making that a wrapper rather than a flag inside each backend means the
/// guarantee holds for any Directory implementation, and a mutator added to
/// the interface later cannot silently escape it - the override is missing and
/// the compiler says so.
///
/// Requests are refused at the transport before reaching here; these throws are
/// the backstop for paths that hold a Directory directly (schema persistence,
/// merges, commit) rather than the expected way for a user to see an error.
class ReadOnlyDirectory : public Directory {
  std::shared_ptr<Directory> delegate_;
  std::string name_;

  [[noreturn]] void reject(std::string_view operation) const {
    throw ReadOnlyError("read-only directory '" + name_ + "': " + std::string(operation) +
                        " rejected");
  }

public:
  ReadOnlyDirectory(std::shared_ptr<Directory> delegate, std::string name)
      : delegate_(std::move(delegate)), name_(std::move(name)) {}

  void listFiles(std::vector<FileInfo>& target) override {
    delegate_->listFiles(target);
  }

  std::shared_ptr<InputFile> openFile(std::string_view name, bool expectSynced = false) override {
    return delegate_->openFile(name, expectSynced);
  }

  std::unique_ptr<File> createFile(std::string_view name) override {
    reject(std::string("createFile(") + std::string(name) + ")");
  }

  bool deleteFile(std::string_view name) override {
    reject(std::string("deleteFile(") + std::string(name) + ")");
  }

  void deletePrefix(std::string_view prefix) override {
    reject(std::string("deletePrefix(") + std::string(prefix) + ")");
  }

  void finishFile(File& file) override {
    reject(std::string("finishFile(") + std::string(file.name()) + ")");
  }

  void renameFile(std::string_view from, std::string_view to) override {
    reject(std::string("renameFile(") + std::string(from) + " -> " + std::string(to) + ")");
  }

  void sync(std::span<const std::string> filenames) override {
    unused(filenames);
    reject("sync");
  }

  Directory& underlying() override { return delegate_->underlying(); }
  void linkFile(Directory&, std::string_view) override { reject("linkFile"); }

  void clear() override {
    reject("clear");
  }
};


/// Factory wrapper that hands out ReadOnlyDirectory instances.
///
/// Pair it with a backend factory opened read-only (see FSDirFactory), which
/// suppresses the open-time side effects a wrapper cannot reach: taking
/// write.lock, creating the data directory, resetting trash/.
class ReadOnlyDirFactory : public DirectoryFactory {
  std::unique_ptr<DirectoryFactory> delegate_;

  [[noreturn]] static void refuse(std::string_view what, std::string_view name) {
    throw ReadOnlyError("read-only data directory: cannot " + std::string(what) + " '" + std::string(name) + "'");
  }

public:
  explicit ReadOnlyDirFactory(std::unique_ptr<DirectoryFactory> delegate)
      : delegate_(std::move(delegate)) {}

  uint64_t storageBytes(std::string_view collection = {}) override { return delegate_->storageBytes(collection); }
  std::vector<std::string> collections() override { return delegate_->collections(); }

  // Every namespace mutation is refused before it reaches the backend, and
  // opening never creates.
  std::shared_ptr<Directory> container(std::string_view name, bool create) override {
    if (create) refuse("create collection", name);
    return std::make_shared<ReadOnlyDirectory>(delegate_->container(name, false), std::string(name));
  }
  std::shared_ptr<Directory> incarnation(std::string_view name, std::string_view incarnation, bool create) override {
    if (create) refuse("create collection", name);
    return std::make_shared<ReadOnlyDirectory>(delegate_->incarnation(name, incarnation, false),
                                               std::string(name) + "/" + std::string(incarnation));
  }
  void createCollection(std::string_view name) override { refuse("create collection", name); }
  std::vector<std::string> incarnations(std::string_view name) override { return delegate_->incarnations(name); }
  void removeIncarnation(std::string_view name, std::string_view incarnation) override {
    unused(incarnation);
    refuse("remove an incarnation of", name);
  }
  void remove(std::string_view name) override { refuse("remove collection", name); }
};

} // namespace luxir
