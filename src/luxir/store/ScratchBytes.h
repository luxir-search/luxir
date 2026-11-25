// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <algorithm>
#include <cassert>
#include <cerrno>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#include "FileIOException.h"

namespace luxir {

/// Append-only scratch bytes for one task: held in RAM up to ramLimit, then
/// spilled to an unlinked file in `dir` (an empty dir keeps everything in
/// RAM, as a RAM directory's index is). Append returns each record's offset;
/// after finish(), view() reads any appended range. Nothing outlives the
/// object: the file has no name and its mapping is released on destruction.
class ScratchBytes {
  std::string dir;
  size_t ramLimit;
  std::vector<char> ram;
  std::vector<char> buffer;  // pending file writes
  int fd = -1;
  uint64_t total = 0;
  char* mapped = nullptr;
  bool finished = false;

  [[noreturn]] void fail(const char* what) const {
    throw FileIOException(std::string("scratch file ") + what + " in " + dir + ": " + strerror(errno));
  }

  void open() {
#ifdef O_TMPFILE
    fd = ::open(dir.c_str(), O_TMPFILE | O_RDWR | O_CLOEXEC, 0600);
#endif
    if (fd < 0) {
      std::string path = dir + "/.luxir-scratch-XXXXXX";
      fd = mkostemp(path.data(), O_CLOEXEC);
      if (fd < 0) fail("create");
      ::unlink(path.c_str());
    }
  }

  void writeAll(const char* data, size_t length) {
    while (length > 0) {
      ssize_t written = ::write(fd, data, length);
      if (written < 0) {
        if (errno == EINTR) continue;
        fail("write");
      }
      data += written;
      length -= (size_t)written;
    }
  }

  void flush() {
    writeAll(buffer.data(), buffer.size());
    buffer.clear();
  }

public:
  static constexpr size_t WRITE_BUFFER_BYTES = 1 << 20;

  ScratchBytes(std::string dir, size_t ramLimit) : dir(std::move(dir)), ramLimit(ramLimit) {}
  ScratchBytes(const ScratchBytes&) = delete;
  ScratchBytes& operator=(const ScratchBytes&) = delete;
  ~ScratchBytes() {
    if (mapped != nullptr) ::munmap(mapped, total);
    if (fd >= 0) ::close(fd);
  }

  uint64_t size() const { return total; }
  // RAM held: the in-RAM bytes' capacity, or the write buffer once spilled.
  // At most max(ramLimit, WRITE_BUFFER_BYTES) when dir is set; unbounded
  // with an empty dir, so a caller charging RAM tracks it.
  size_t ramBytes() const { return ram.capacity() + buffer.capacity(); }
  bool spilled() const { return fd >= 0; }

  uint64_t append(std::string_view bytes) {
    assert(!finished);
    uint64_t offset = total;
    if (fd < 0 && (dir.empty() || ram.size() + bytes.size() <= ramLimit)) {
      // Geometric growth, capped at the limit when the bytes can spill, so
      // the RAM held stays within ramLimit (see ramBytes()).
      size_t needed = ram.size() + bytes.size();
      if (needed > ram.capacity()) {
        size_t grown = std::max(needed, std::max<size_t>(4096, 2 * ram.capacity()));
        ram.reserve(dir.empty() ? grown : std::min(grown, ramLimit));
      }
      ram.insert(ram.end(), bytes.begin(), bytes.end());
    } else {
      if (fd < 0) {
        open();
        writeAll(ram.data(), ram.size());
        std::vector<char>().swap(ram);  // release the RAM, not just its size
        buffer.reserve(WRITE_BUFFER_BYTES);
      }
      if (buffer.size() + bytes.size() > WRITE_BUFFER_BYTES) flush();
      if (bytes.size() >= WRITE_BUFFER_BYTES) {
        writeAll(bytes.data(), bytes.size());
      } else {
        buffer.insert(buffer.end(), bytes.begin(), bytes.end());
      }
    }
    total += bytes.size();
    return offset;
  }

  void finish() {
    finished = true;
    if (fd < 0 || total == 0) return;
    flush();
    std::vector<char>().swap(buffer);
    void* map = ::mmap(nullptr, total, PROT_READ, MAP_SHARED, fd, 0);
    if (map == MAP_FAILED) fail("map");
    mapped = (char*)map;
  }

  std::string_view view(uint64_t offset, size_t length) const {
    assert(finished && offset + length <= total);
    return {(fd >= 0 ? mapped : ram.data()) + offset, length};
  }
};

}  // namespace luxir
