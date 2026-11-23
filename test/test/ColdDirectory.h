// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <map>
#include <optional>
#include <span>
#include <mutex>
#include <string>
#include <vector>

#include "luxir/store/DelegatingDirectory.h"

namespace luxir::test {

// Shared by the views of a ColdLayer, keyed by file name.
class ColdStats {
public:
  struct FileStats {
    uint64_t fetchedBytes = 0;     // pages fetched from the inner directory
    uint64_t acquires = 0;
    uint64_t metadataAcquires = 0; // acquires of ReadClass::METADATA ranges
    uint64_t firstPageReads = 0;   // acquires that start at offset 0 (a header)
    uint64_t wholeReads = 0;       // acquires of the whole file
  };

private:
  mutable std::mutex mutex;
  std::map<std::string, FileStats, std::less<>> files;
  std::map<std::string, KnownFile, std::less<>> known;

public:
  FileStats fileStats(std::string_view name) const;
  // The latest knownFiles entry for `name`, if any directory got one.
  std::optional<KnownFile> knownFile(std::string_view name) const;

  void fetched(std::string_view name, uint64_t bytes);
  void acquired(std::string_view name, ByteRange range, uint64_t fileSize);
  void noteKnown(std::span<const KnownFile> files);
};

// A directory whose bytes are fetched over a network, as an object store
// backend presents them: REMOTE, and every file it opens is lazy and starts
// cold. A file fetches 4 KiB pages from the inner directory on first touch and
// counts what it fetched and how it was asked; bytes never fetched read as
// 0xCC, so a reader that skips a fetch fails loudly.
class ColdDirectory : public DelegatingDirectory {
  std::shared_ptr<ColdStats> stats;

public:
  explicit ColdDirectory(std::shared_ptr<Directory> inner, std::shared_ptr<ColdStats> stats = std::make_shared<ColdStats>())
      : DelegatingDirectory(std::move(inner)), stats(std::move(stats)) {}

  ColdStats& counters() { return *stats; }
  Capabilities capabilities() const override { return (delegate->capabilities() | REMOTE) & ~MEMORY_MAPPED; }
  std::shared_ptr<InputFile> openFile(std::string_view name, bool expectSynced = false) override;
  void knownFiles(std::span<const KnownFile> files) override;
};

// Storage layer "cold" (registered by the example plugin): every directory a
// node opens is a ColdDirectory over the inner one.
class ColdLayer : public DelegatingDirFactory {
public:
  const std::shared_ptr<ColdStats> stats = std::make_shared<ColdStats>();

  explicit ColdLayer(std::unique_ptr<DirectoryFactory> inner) : DelegatingDirFactory(std::move(inner)) {}

  bool exclusive() const override { return delegate->exclusive(); }
  std::shared_ptr<Directory> directory(StorageRoot root, std::string_view path, ViewOptions options = {}) override {
    return std::make_shared<ColdDirectory>(delegate->directory(root, path, std::move(options)), stats);
  }
  std::string describe() const override { return "cold(" + delegate->describe() + ")"; }
};

} // namespace luxir::test
