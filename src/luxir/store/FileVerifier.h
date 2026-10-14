// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <functional>
#include <span>
#include <stdexcept>
#include <string>
#include "luxir/store/Directory.h"

namespace luxir {

// A file whose bytes do not match the manifest descriptor that names it.
class ChecksumMismatchError : public std::runtime_error {
public:
  explicit ChecksumMismatchError(const std::string& message) : std::runtime_error(message) {}
};

// Size and xxh3 of a file's bytes, as a manifest FileDescriptor records them.
struct FileCheck {
  uint64_t size = 0;
  uint64_t xxh3 = 0;
};

// Hashes `file` in chunks. `progress`, when set, runs after each chunk with
// its byte count; it may sleep to pace the read or throw to abandon it.
FileCheck hashFile(InputFile& file, const std::function<void(size_t)>& progress = {});

// Empty when `actual` matches `expected`; otherwise a description naming the
// file with its expected and actual size and digest.
std::string checksumMismatch(const FileDescriptor& expected, const FileCheck& actual);

// Reads every file of `files` from `dir` and checks its size and xxh3 against
// its descriptor, on up to `threads` threads (0: one per core, at most 8).
// Throws ChecksumMismatchError naming the first failing file by name, with the
// number of others that also failed; a missing or unreadable file fails too.
void verifyFiles(Directory& dir, std::span<const FileDescriptor> files, unsigned threads = 0);

}
