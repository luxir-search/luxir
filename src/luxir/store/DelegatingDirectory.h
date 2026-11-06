// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "DirectoryFactory.h"

namespace luxir {

/// A Directory that forwards every virtual to another one. Plugin layers
/// derive from it and override only what they change.
///
/// Core wrappers that refuse or track mutations implement the bare interface;
/// layers derive from the delegating bases. New virtuals must not silently
/// bypass the core wrappers' checks.
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
  std::optional<PublicationVersion> publishFile(std::string_view name, std::span<const std::byte> bytes, PublishCondition condition) override {
    return delegate->publishFile(name, bytes, condition);
  }
  std::optional<Publication> readPublished(std::string_view name) override { return delegate->readPublished(name); }
  void deleteDurably(std::span<const std::string> names) override { delegate->deleteDurably(names); }
  void sync(std::span<const std::string> filenames) override { delegate->sync(filenames); }
  void clear() override { delegate->clear(); }
  Directory& underlying() override { return delegate->underlying(); }
  void linkFile(Directory& source, std::string_view name) override { delegate->linkFile(source, name); }
  std::shared_ptr<ChecksumReader> openChecksum(std::string_view name, bool direct = false) override {
    return delegate->openChecksum(name, direct);
  }
  Capabilities capabilities() const override { return delegate->capabilities(); }
  std::string scratchPath() const override { return delegate->scratchPath(); }
};


/// A DirectoryFactory that forwards every virtual to another one. Layer
/// factories derive from it, override what they change (usually directory(),
/// to wrap the views they hand out) and inherit the rest.
class DelegatingDirFactory : public DirectoryFactory {
protected:
  std::unique_ptr<DirectoryFactory> delegate;

public:
  explicit DelegatingDirFactory(std::unique_ptr<DirectoryFactory> inner) : delegate(std::move(inner)) {}

  std::shared_ptr<Directory> directory(StorageRoot root, std::string_view path, ViewOptions options = {}) override {
    return delegate->directory(root, path, std::move(options));
  }
  std::vector<Child> children(StorageRoot root, std::string_view path) override { return delegate->children(root, path); }
  void removeTree(StorageRoot root, std::string_view path) override { delegate->removeTree(root, path); }
  std::shared_ptr<StorageMemory> memory() override { return delegate->memory(); }
  std::string describe() const override { return delegate->describe(); }
  std::string diskPath() const override { return delegate->diskPath(); }
  DirectoryFactory* inner() override { return delegate.get(); }
};

} // namespace luxir
