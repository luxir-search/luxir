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
/// guarantee holds for any Directory implementation. Core wrappers that refuse
/// or track mutations implement the bare interface; layers derive from the
/// delegating bases. A new pure virtual requires an explicit implementation here;
/// a default implementation calls this wrapper's own virtuals.
///
/// Requests are refused at the transport before reaching here; these throws are
/// the backstop for paths that hold a Directory directly (schema persistence,
/// merges, commit) rather than the expected way for a user to see an error.
class ReadOnlyDirectory : public Directory {
  std::shared_ptr<Directory> delegate;
  std::string name_;

  [[noreturn]] void reject(std::string_view operation) const {
    throw ReadOnlyError("read-only directory '" + name_ + "': " + std::string(operation) +
                        " rejected");
  }

public:
  ReadOnlyDirectory(std::shared_ptr<Directory> inner, std::string name)
      : delegate(std::move(inner)), name_(std::move(name)) {}

  void listFiles(std::vector<FileInfo>& target) override { delegate->listFiles(target); }
  std::shared_ptr<InputFile> openFile(std::string_view name, bool expectSynced = false) override {
    return delegate->openFile(name, expectSynced);
  }
  void expectSynced(std::span<const FileDescriptor> files) override { delegate->expectSynced(files); }
  std::shared_ptr<ChecksumReader> openChecksum(std::string_view name, bool direct = false) override {
    return delegate->openChecksum(name, direct);
  }
  Directory& underlying() override { return delegate->underlying(); }
  Capabilities capabilities() const override { return delegate->capabilities(); }

  std::unique_ptr<File> createFile(std::string_view name) override {
    reject(std::string("createFile(") + std::string(name) + ")");
  }

  std::unique_ptr<File> createFile(std::string_view name, FileCreateOptions) override {
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

  std::optional<PublicationVersion> publishFile(std::string_view name, std::span<const std::byte>, PublishCondition) override {
    reject(std::string("publishFile(") + std::string(name) + ")");
  }

  std::optional<Publication> readPublished(std::string_view name) override { return delegate->readPublished(name); }

  void deleteDurably(std::span<const std::string>) override { reject("deleteDurably"); }

  void sync(std::span<const std::string> filenames) override {
    unused(filenames);
    reject("sync");
  }

  void linkFile(Directory&, std::string_view) override { reject("linkFile"); }

  void clear() override {
    reject("clear");
  }
};


/// Factory wrapper that hands out ReadOnlyDirectory instances.
///
/// Pair it with a backend factory opened read-only (see FSDirFactory), which
/// suppresses the open-time side effects a wrapper cannot reach: taking
/// write.lock, creating the data directory, and cleaning publication temps.
class ReadOnlyDirFactory : public DirectoryFactory {
  std::unique_ptr<DirectoryFactory> delegate;

public:
  explicit ReadOnlyDirFactory(std::unique_ptr<DirectoryFactory> inner) : delegate(std::move(inner)) {}
  std::string describe() const override { return "read_only(" + delegate->describe() + ")"; }
  std::string diskPath() const override { return delegate->diskPath(); }
  DirectoryFactory* inner() override { return delegate.get(); }
  std::shared_ptr<StorageMemory> memory() override { return delegate->memory(); }
  std::vector<Child> children(StorageRoot root, std::string_view path) override { return delegate->children(root, path); }
  std::shared_ptr<Directory> directory(StorageRoot root, std::string_view path, ViewOptions options = {}) override {
    return std::make_shared<ReadOnlyDirectory>(delegate->directory(root, path, std::move(options)), std::string(path));
  }
  void removeTree(StorageRoot, std::string_view) override { throw ReadOnlyError("read-only data directory: cannot remove a subtree"); }

};

} // namespace luxir
