// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include "luxir/store/FileVerifier.h"
#include "luxir/store/CorruptionMarker.h"
#include <format>
#include <oneapi/tbb/parallel_for.h>

namespace luxir {

std::string checksumMismatch(const FileDescriptor& expected, const FileCheck& actual) {
  if (actual.matches(expected)) return {};
  return std::format("checksum mismatch: {}: expected size={} xxh3={:016x}, actual size={} xxh3={:016x}",
                     expected.name, expected.size, expected.xxh3, actual.size, actual.xxh3);
}

void FileVerification::open(Directory& dir, std::span<const FileDescriptor> files, bool full) {
  auto suspects = CorruptionMarker::read(dir);
  items.reserve(files.size());
  for (const auto& file : files) {
    bool direct = std::ranges::binary_search(suspects, file.name);
    if (!full && !direct) continue;
    std::shared_ptr<ChecksumReader> reader;
    try { reader = dir.openChecksum(file.name, direct); }
    catch (const FileEIOError& e) { throw ChecksumMismatchError("unreadable file: " + file.name + ": " + e.what()); }
    catch (const std::exception& e) { throw std::runtime_error("could not verify file: " + file.name + ": " + e.what()); }
    if (!reader) throw ChecksumMismatchError("missing file: " + file.name);
    items.push_back({file, std::move(reader), {}, false});
  }
}

void FileVerification::run() {
  // Largest first: a long tail of small files then fills in behind the big ones.
  std::ranges::sort(items, [](const auto& a, const auto& b) { return a.reader->size() > b.reader->size(); });
  oneapi::tbb::parallel_for((size_t)0, items.size(), [&](size_t i) {
    auto& item = items[i];
    try {
      item.problem = checksumMismatch(item.file, item.reader->checksum());
      item.corrupt = !item.problem.empty();
    } catch (const FileEIOError& e) {
      item.problem = "unreadable file: " + item.file.name + ": " + e.what();
      item.corrupt = true;
    } catch (const std::exception& e) {
      item.problem = "could not verify file: " + item.file.name + ": " + e.what();
    }
  }, oneapi::tbb::simple_partitioner());
  size_t failures = 0;
  const Item* first = nullptr;
  for (const auto& item : items) if (!item.problem.empty()) {
    if (!first || item.file.name < first->file.name) first = &item;
    failures++;
  }
  if (!first) return;
  auto message = first->problem;
  if (failures > 1) message += std::format(" (and {} more files)", failures - 1);
  if (first->corrupt) throw ChecksumMismatchError(message);
  throw std::runtime_error(message);
}

void verifyFiles(Directory& dir, std::span<const FileDescriptor> files, bool full) {
  FileVerification verification;
  verification.open(dir, files, full);
  verification.run();
}

}
