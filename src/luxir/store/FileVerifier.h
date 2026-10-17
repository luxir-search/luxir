// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <functional>
#include <map>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>
#include "luxir/store/Directory.h"

namespace luxir {

// A file whose bytes do not match the manifest descriptor that names it, or
// that cannot be read at all.
class ChecksumMismatchError : public std::runtime_error {
public:
  explicit ChecksumMismatchError(const std::string& message) : std::runtime_error(message) {}
};

// Empty when `actual` matches `expected`; otherwise "checksum mismatch: <file>:
// expected size=.. xxh3=.., actual size=.. xxh3=..".
std::string checksumMismatch(const FileDescriptor& expected, const FileCheck& actual);

// A batch of files to check against their descriptors, possibly from several
// directories. open() takes a handle on every file up front, the same short
// window as opening a reader, so a file retired by a concurrent publication
// still verifies from its handle instead of failing as missing. run() hashes
// the handles in parallel on the calling thread's TBB arena.
class FileVerification {
public:
  struct Item {
    size_t group;
    FileDescriptor file;
    std::shared_ptr<ChecksumReader> reader; // null: missing
    std::string problem;                    // empty until run() finds one
  };
private:
  std::vector<Item> items;
public:
  // Opens `files` of `dir` under `group`. A missing file is a problem at once;
  // an unreadable one too.
  void open(Directory& dir, std::span<const FileDescriptor> files, size_t group = 0, bool direct = false);
  size_t size() const { return items.size(); }
  uint64_t bytes() const;
  // Hashes every opened file. `progress` runs on the hashing threads after
  // each chunk with its byte count.
  void run(const std::function<void(size_t)>& progress = {});
  // After run(): the first problem of `group` by file name, with how many
  // other files of the group also failed; empty when all verified.
  std::string problem(size_t group) const;
  // After run(): the descriptors of `group` that verified, by name.
  void verified(size_t group, std::map<std::string, FileDescriptor>& out) const;
  const std::vector<Item>& all() const { return items; }
};

// Reads every file of `files` from `dir` and checks its size and xxh3 against
// its descriptor, in parallel. Throws ChecksumMismatchError naming the first
// failing file by name, with the number of others that also failed; a missing
// or unreadable file fails too.
void verifyFiles(Directory& dir, std::span<const FileDescriptor> files);

}
