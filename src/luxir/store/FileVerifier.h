// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <span>
#include <stdexcept>
#include <string>
#include <vector>
#include "luxir/store/Directory.h"

namespace luxir {

// A checksum mismatch, missing file, or EIO during verification.
class ChecksumMismatchError : public std::runtime_error {
public:
  explicit ChecksumMismatchError(const std::string& message) : std::runtime_error(message) {}
};

std::string checksumMismatch(const FileDescriptor& expected, const FileCheck& actual);

// Holds a collection's checksum handles before any checked file is mapped.
// run() hashes largest first on the calling TBB arena, retaining the handles
// so VerifiedDirectory can build a reader from the same files afterwards.
class FileVerification {
  friend class VerifiedDirectory;
  struct Item {
    FileDescriptor file;
    std::shared_ptr<ChecksumReader> reader;
    std::string problem;
    bool corrupt = false;
  };
  std::vector<Item> items;
public:
  // All files under full, otherwise only marker-named files. Suspect files
  // always use direct reads, including under full.
  void open(Directory& dir, std::span<const FileDescriptor> files, bool full = true);
  // Throws ChecksumMismatchError for corruption, runtime_error for other
  // failures to verify. Neither writes or clears markers.
  void run();
};

void verifyFiles(Directory& dir, std::span<const FileDescriptor> files, bool full = true);

}
