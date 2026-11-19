// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "luxir/store/DelegatingDirectory.h"

namespace luxir::test {

// A directory whose bytes are fetched over a network, as an object store
// backend presents them: REMOTE, and every file it opens is lazy and starts
// cold. A file fetches 4 KiB pages on first touch and counts what it fetched;
// bytes read without a fetch (through unfetchedStream) are garbage, so a
// reader that skips one fails loudly.
class ColdDirectory : public DelegatingDirectory {
public:
  struct FileStats {
    uint64_t fetchedBytes = 0;
    uint64_t wholeReads = 0;
  };

private:
  std::mutex mutex;
  std::map<std::string, FileStats, std::less<>> stats;

  class File : public InputFile {
    static constexpr uint64_t PAGE = 4096;
    ColdDirectory& owner;
    std::shared_ptr<InputFile> inner;
    std::string name;
    uint64_t bytes;
    std::unique_ptr<char[]> data;
    std::mutex pagesMutex;
    std::vector<bool> pages;

    void ensure(uint64_t offset, uint64_t length) {
      checkRange(offset, length, bytes);
      if (length == 0) return;
      uint64_t fetched = 0;
      {
        std::lock_guard lock(pagesMutex);
        for (uint64_t page = offset / PAGE; page <= (offset + length - 1) / PAGE; page++) {
          if (pages[page]) continue;
          uint64_t begin = page * PAGE;
          uint64_t size = std::min(PAGE, bytes - begin);
          std::memcpy(data.get() + begin, inner->rangeStream(begin, size).ptr(), size);
          pages[page] = true;
          fetched += size;
        }
      }
      if (fetched) {
        std::lock_guard lock(owner.mutex);
        owner.stats[name].fetchedBytes += fetched;
      }
    }

    void whole() {
      {
        std::lock_guard lock(owner.mutex);
        owner.stats[name].wholeReads++;
      }
      ensure(0, bytes);
    }

  public:
    File(ColdDirectory& owner, std::shared_ptr<InputFile> inner, std::string name)
        : owner(owner), inner(std::move(inner)), name(std::move(name)) {
      bytes = this->inner->size();
      data = std::make_unique<char[]>(bytes + 1);
      std::memset(data.get(), 0xCC, bytes + 1);
      pages.assign((bytes + PAGE - 1) / PAGE, false);
    }

    size_t size() override { return bytes; }
    std::string_view read() override {
      whole();
      return {data.get(), bytes};
    }
    InputStream getInputStream() override {
      whole();
      return InputStream(data.get(), data.get() + bytes);
    }
    InputStream rangeStream(uint64_t offset, uint64_t length) override {
      ensure(offset, length);
      return InputStream(data.get() + offset, data.get() + offset + length);
    }
    void fetchRanges(std::span<const ByteRange> ranges, ReadDeadline, FetchDone done) override {
      std::exception_ptr error;
      try {
        for (const auto& range : ranges) ensure(range.offset, range.length);
      } catch (...) {
        error = std::current_exception();
      }
      done(error);
    }
    void prefetchRanges(std::span<const ByteRange> ranges) override {
      for (const auto& range : ranges) ensure(range.offset, std::min(range.length, bytes - std::min(bytes, range.offset)));
    }
    void prefetch(size_t offset, size_t length) override {
      if (offset < bytes) ensure(offset, std::min<uint64_t>(length, bytes - offset));
    }
    bool lazy() const override { return true; }
    bool resident(uint64_t offset, uint64_t length) const override {
      if (length == 0) return true;
      std::lock_guard lock(const_cast<std::mutex&>(pagesMutex));
      for (uint64_t page = offset / PAGE; page <= (offset + length - 1) / PAGE; page++) {
        if (page >= pages.size() || !pages[page]) return false;
      }
      return true;
    }
    InputStream unfetchedStream() override { return InputStream(data.get(), data.get() + bytes); }
  };

public:
  using DelegatingDirectory::DelegatingDirectory;

  Capabilities capabilities() const override { return delegate->capabilities() | REMOTE; }

  std::shared_ptr<InputFile> openFile(std::string_view name, bool expectSynced = false) override {
    auto file = delegate->openFile(name, expectSynced);
    if (!file) return file;
    return std::make_shared<File>(*this, std::move(file), std::string(name));
  }

  FileStats statsOf(std::string_view name) {
    std::lock_guard lock(mutex);
    auto it = stats.find(name);
    return it == stats.end() ? FileStats{} : it->second;
  }
};

} // namespace luxir::test
