// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <map>
#include <mutex>
#include <set>

#include "luxir/store/DelegatingDirectory.h"

namespace luxir::test {

enum class StrictWholeFiles { COUNT, DENY };

// Shared by the views of a factory. Parts and accesses are keyed by file name.
class StrictStorageCounters {
public:
  struct FileStats {
    uint64_t acquires = 0;
    uint64_t metadataAcquires = 0;
    uint64_t acquiredBytes = 0;
    uint64_t wholeFileAccesses = 0;
    uint64_t sizeRequests = 0;  // InputFile::size() calls from readers.
    std::set<std::pair<uint64_t, uint64_t>> parts;
    std::set<std::pair<uint64_t, uint64_t>> metadataParts;
  };

  struct LiveStats {
    uint64_t holds = 0;
    uint64_t bytes = 0;
    uint64_t peakHolds = 0;
    uint64_t peakBytes = 0;
  };

private:
  mutable std::mutex mutex;
  std::map<std::string, FileStats, std::less<>> files;
  LiveStats live;
  uint8_t pattern = 0;

public:
  FileStats fileStats(std::string_view name) const;
  LiveStats liveStats() const;
  void logSummary() const;

  void wholeFile(std::string_view name);
  void sizeRequested(std::string_view name);
  uint8_t acquired(std::string_view name, ByteRange range);
  void released(uint64_t length) noexcept;
};

class StrictDirectory : public DelegatingDirectory {
  std::shared_ptr<StrictStorageCounters> counters;
  StrictWholeFiles mode;

public:
  StrictDirectory(std::shared_ptr<Directory> inner, std::shared_ptr<StrictStorageCounters> counters,
                  StrictWholeFiles mode = StrictWholeFiles::COUNT)
      : DelegatingDirectory(std::move(inner)), counters(std::move(counters)), mode(mode) {}

  std::shared_ptr<InputFile> openFile(std::string_view name, bool expectSynced = false, uint64_t size = 0) override;
  std::shared_ptr<ChecksumReader> openChecksum(std::string_view name, bool direct = false) override;
  Capabilities capabilities() const override { return delegate->capabilities() & ~HARD_LINK; }
};

class StrictLayer : public DelegatingDirFactory {
  StrictWholeFiles mode;

public:
  const std::shared_ptr<StrictStorageCounters> counters = std::make_shared<StrictStorageCounters>();

  StrictLayer(std::unique_ptr<DirectoryFactory> inner, StrictWholeFiles mode)
      : DelegatingDirFactory(std::move(inner)), mode(mode) {}
  ~StrictLayer() override;

  bool exclusive() const override { return delegate->exclusive(); }
  std::shared_ptr<Directory> wrapDirectory(std::shared_ptr<Directory> dir) const {
    return std::make_shared<StrictDirectory>(std::move(dir), counters, mode);
  }
  std::shared_ptr<Directory> directory(StorageRoot root, std::string_view path, ViewOptions options = {}) override {
    return wrapDirectory(delegate->directory(root, path, std::move(options)));
  }
  std::string describe() const override { return "strict(" + delegate->describe() + ")"; }
};

// Borrows dir for a reader open using the test run's configured storage layer.
std::shared_ptr<Directory> strictReadDirectory(Directory& dir);

} // namespace luxir::test
