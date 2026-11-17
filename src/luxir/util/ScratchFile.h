// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <vector>

namespace luxir {

/// A node-local temporary file for merge workspaces too large to keep on the
/// heap. It is unlinked as soon as it is created, so nothing outlives the
/// process (or a crash), and it is not a Directory file: remote or RAM-backed
/// index stores never see it. Written append-only, then mapped read-only;
/// mapped pages are page cache the kernel can evict, not anonymous memory.
class ScratchFile {
  int fd = -1;
  uint64_t written = 0;
  std::vector<std::byte> buffer;
  size_t buffered = 0;
  void* mapped = nullptr;
  size_t mappedSize = 0;

  void flush();

public:
  static constexpr size_t DEFAULT_BUFFER_BYTES = 64 * 1024;

  /// Creates the file under directory() (TMPDIR or the system default).
  explicit ScratchFile(size_t bufferBytes = DEFAULT_BUFFER_BYTES);
  ~ScratchFile();
  ScratchFile(const ScratchFile&) = delete;
  ScratchFile& operator=(const ScratchFile&) = delete;

  void append(const void* data, size_t bytes);

  uint64_t size() const { return written + buffered; }

  /// Flushes and maps the whole file read-only. No appends afterwards.
  std::span<const std::byte> map();

  static std::filesystem::path directory();
};

}  // namespace luxir
