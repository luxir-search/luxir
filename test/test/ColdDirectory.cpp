// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include "ColdDirectory.h"

#include <cstring>

namespace luxir::test {

ColdStats::FileStats ColdStats::fileStats(std::string_view name) const {
  std::lock_guard lock(mutex);
  auto it = files.find(name);
  return it == files.end() ? FileStats{} : it->second;
}

std::optional<KnownFile> ColdStats::knownFile(std::string_view name) const {
  std::lock_guard lock(mutex);
  auto it = known.find(name);
  if (it == known.end()) return std::nullopt;
  return it->second;
}

void ColdStats::fetched(std::string_view name, uint64_t bytes) {
  std::lock_guard lock(mutex);
  files[std::string(name)].fetchedBytes += bytes;
}

void ColdStats::acquired(std::string_view name, ByteRange range, uint64_t fileSize) {
  std::lock_guard lock(mutex);
  auto& file = files[std::string(name)];
  ++file.acquires;
  if (range.readClass == ReadClass::METADATA) ++file.metadataAcquires;
  if (range.offset == 0) ++file.firstPageReads;
  if (range.offset == 0 && range.length == fileSize) ++file.wholeReads;
}

void ColdStats::noteKnown(std::span<const KnownFile> files) {
  std::lock_guard lock(mutex);
  for (const auto& file : files) known.insert_or_assign(file.file.name, file);
}

namespace {

class ColdFile : public InputFile {
  static constexpr uint64_t PAGE = 4096;
  std::shared_ptr<ColdStats> stats;
  std::shared_ptr<InputFile> inner;
  std::string name;
  uint64_t bytes;
  std::unique_ptr<char[]> data;  // stable for the file's lifetime, as a mapping is
  std::mutex mutex;
  std::vector<bool> pages;

  void ensure(ByteRange range) {
    checkRange(range, bytes);
    if (range.length == 0) return;
    uint64_t fetched = 0;
    {
      std::lock_guard lock(mutex);
      for (uint64_t page = range.offset / PAGE; page <= (range.offset + range.length - 1) / PAGE; page++) {
        if (pages[page]) continue;
        uint64_t begin = page * PAGE, size = std::min(PAGE, bytes - begin);
        auto hold = inner->acquire({begin, size, range.readClass});
        std::memcpy(data.get() + begin, hold.getData(), size);
        pages[page] = true;
        fetched += size;
      }
    }
    if (fetched) stats->fetched(name, fetched);
  }

public:
  ColdFile(std::shared_ptr<ColdStats> stats, std::shared_ptr<InputFile> inner, std::string_view name)
      : stats(std::move(stats)), inner(std::move(inner)), name(name) {
    bytes = this->inner->size();
    data = std::make_unique_for_overwrite<char[]>(bytes + kReadSlack);
    std::memset(data.get(), 0xCC, bytes + kReadSlack);
    pages.assign((bytes + PAGE - 1) / PAGE, false);
  }

  size_t size() override { return bytes; }
  bool lazy() const override { return true; }

  HeldRange acquire(ByteRange range) override {
    ensure(range);
    stats->acquired(name, range, bytes);
    return {data.get() + range.offset, range.length};
  }

  void prefetch(size_t offset, size_t length) override {
    if (offset < bytes) ensure({offset, std::min<uint64_t>(length, bytes - offset)});
  }

  void prefetchRanges(std::span<const ByteRange> ranges) override {
    for (const auto& range : ranges) ensure(range);
  }

  void fetchRanges(std::span<const ByteRange> ranges, ReadDeadline, FetchDone done) override {
    std::exception_ptr error;
    try {
      for (const auto& range : ranges) ensure(range);
    } catch (...) {
      error = std::current_exception();
    }
    done(error);
  }
};

} // namespace

std::shared_ptr<InputFile> ColdDirectory::openFile(std::string_view name, bool expectSynced) {
  auto file = delegate->openFile(name, expectSynced);
  if (!file) return file;
  return std::make_shared<ColdFile>(stats, std::move(file), name);
}

void ColdDirectory::knownFiles(std::span<const KnownFile> files) {
  stats->noteKnown(files);
  delegate->knownFiles(files);
}

} // namespace luxir::test
