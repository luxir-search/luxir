// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include "ScratchFile.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <string>
#include <system_error>

#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

namespace luxir {

namespace {
[[noreturn]] void fail(const char* what) {
  throw std::system_error(errno, std::generic_category(), std::string("ScratchFile: ") + what);
}
}  // namespace

std::filesystem::path ScratchFile::directory() {
  return std::filesystem::temp_directory_path();
}

ScratchFile::ScratchFile(size_t bufferBytes) : buffer(bufferBytes == 0 ? 1 : bufferBytes) {
  std::string path = (directory() / "luxir-scratch-XXXXXX").string();
  fd = ::mkstemp(path.data());
  if (fd < 0) fail("mkstemp");
  ::unlink(path.c_str());
}

ScratchFile::~ScratchFile() {
  if (mapped != nullptr) ::munmap(mapped, mappedSize);
  if (fd >= 0) ::close(fd);
}

void ScratchFile::flush() {
  size_t done = 0;
  while (done < buffered) {
    ssize_t n = ::pwrite(fd, buffer.data() + done, buffered - done, (off_t)(written + done));
    if (n < 0) {
      if (errno == EINTR) continue;
      fail("pwrite");
    }
    done += (size_t)n;
  }
  written += buffered;
  buffered = 0;
}

void ScratchFile::append(const void* data, size_t bytes) {
  if (mapped != nullptr) throw std::logic_error("ScratchFile: append after map");
  auto* src = (const std::byte*)data;
  while (bytes > 0) {
    size_t take = std::min(bytes, buffer.size() - buffered);
    std::memcpy(buffer.data() + buffered, src, take);
    buffered += take;
    src += take;
    bytes -= take;
    if (buffered == buffer.size()) flush();
  }
}

std::span<const std::byte> ScratchFile::map() {
  if (mapped == nullptr) {
    flush();
    buffer = {};
    if (written == 0) return {};
    mappedSize = (size_t)written;
    void* p = ::mmap(nullptr, mappedSize, PROT_READ, MAP_SHARED, fd, 0);
    if (p == MAP_FAILED) fail("mmap");
    mapped = p;
  }
  return {(const std::byte*)mapped, mappedSize};
}

}  // namespace luxir
