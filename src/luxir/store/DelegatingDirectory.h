// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "DirectoryFactory.h"

namespace luxir {

/// A Directory that forwards every virtual to another one. Wrappers (layers)
/// derive from it and override only what they change.
///
/// Forwarding is total, including the createFile overload with options,
/// expectSynced, knownFiles, deletePrefix, sync, underlying, linkFile,
/// openChecksum, clear and capabilities. A wrapper that must not let a new virtual through (see
/// ReadOnlyDirectory) overrides it explicitly.
class DelegatingDirectory : public Directory {
protected:
  std::shared_ptr<Directory> delegate;

public:
  explicit DelegatingDirectory(std::shared_ptr<Directory> inner) : delegate(std::move(inner)) {}

  void listFiles(std::vector<FileInfo>& target) override { delegate->listFiles(target); }
  std::shared_ptr<InputFile> openFile(std::string_view name, bool expectSynced = false) override {
    return delegate->openFile(name, expectSynced);
  }
  void expectSynced(std::span<const FileDescriptor> files) override { delegate->expectSynced(files); }
  void knownFiles(std::span<const FileDescriptor> files) override { delegate->knownFiles(files); }
  std::unique_ptr<File> createFile(std::string_view name) override { return delegate->createFile(name); }
  std::unique_ptr<File> createFile(std::string_view name, FileCreateOptions options) override {
    return delegate->createFile(name, std::move(options));
  }
  bool deleteFile(std::string_view name) override { return delegate->deleteFile(name); }
  void deletePrefix(std::string_view prefix) override { delegate->deletePrefix(prefix); }
  void finishFile(File& file) override { delegate->finishFile(file); }
  void renameFile(std::string_view from, std::string_view to) override { delegate->renameFile(from, to); }
  void sync(std::span<const std::string> filenames) override { delegate->sync(filenames); }
  void clear() override { delegate->clear(); }
  Directory& underlying() override { return delegate->underlying(); }
  void linkFile(Directory& source, std::string_view name) override { delegate->linkFile(source, name); }
  std::shared_ptr<ChecksumReader> openChecksum(std::string_view name, bool direct = false) override {
    return delegate->openChecksum(name, direct);
  }
  Capabilities capabilities() const override { return delegate->capabilities(); }
};


/// A DirectoryFactory that forwards every virtual to another one. Layer
/// factories derive from it, override what they change (usually container() and
/// incarnation(), to wrap the directories they hand out) and inherit the rest.
class DelegatingDirFactory : public DirectoryFactory {
protected:
  std::unique_ptr<DirectoryFactory> delegate;

public:
  explicit DelegatingDirFactory(std::unique_ptr<DirectoryFactory> inner) : delegate(std::move(inner)) {}

  std::vector<CollectionId> collections() override { return delegate->collections(); }
  uint64_t storageBytes() override { return delegate->storageBytes(); }
  uint64_t collectionBytes(const CollectionId& id) override { return delegate->collectionBytes(id); }
  std::shared_ptr<Directory> container(const CollectionId& id, bool create) override {
    return delegate->container(id, create);
  }
  std::shared_ptr<Directory> incarnation(const CollectionId& id, std::string_view incarnation, bool create) override {
    return delegate->incarnation(id, incarnation, create);
  }
  void createCollection(const CollectionId& id) override { delegate->createCollection(id); }
  std::vector<std::string> incarnations(const CollectionId& id) override { return delegate->incarnations(id); }
  void removeIncarnation(const CollectionId& id, std::string_view incarnation) override {
    delegate->removeIncarnation(id, incarnation);
  }
  void remove(const CollectionId& id) override { delegate->remove(id); }
  std::shared_ptr<Directory> nodeDirectory() override { return delegate->nodeDirectory(); }
  bool holdsNoCollections() override { return delegate->holdsNoCollections(); }
  std::string describe() const override { return delegate->describe(); }
  std::string diskPath() const override { return delegate->diskPath(); }
  uint64_t memoryLimitBytes() const override { return delegate->memoryLimitBytes(); }
};

} // namespace luxir
