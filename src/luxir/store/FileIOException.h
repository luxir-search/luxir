// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "luxir/util/ApiError.h"

namespace luxir {

// Output failures can leave append-only values partially written. Indexing
// must discard the in-progress segment instead of reusing its streams.
class FileIOException : public ApiError {
public:
  explicit FileIOException(const std::string& message)
      : ApiError(ErrorKind::INTERNAL, "internal", message) {}
  FileIOException(ErrorKind kind, std::string code, const std::string& message)
      : ApiError(kind, std::move(code), message) {}
};

// The kernel reported EIO: unlike resource or permission failures, this
// makes the file suspect even when no checksum could be computed.
class FileEIOError : public FileIOException {
public:
  explicit FileEIOError(const std::string& message)
      : FileIOException(ErrorKind::INTERNAL, "file_eio", message) {}
};

class StorageMemoryLimitError : public FileIOException {
public:
  StorageMemoryLimitError()
      : FileIOException(ErrorKind::RESOURCE_EXHAUSTED, "storage_memory_limit", "RAM storage memory limit exceeded") {}
};

}
