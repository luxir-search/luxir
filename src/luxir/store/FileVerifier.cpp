// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include "luxir/store/FileVerifier.h"
#include <format>
#include <oneapi/tbb/parallel_for.h>

namespace luxir {

std::string checksumMismatch(const FileDescriptor& expected, const FileCheck& actual) {
  if (actual.matches(expected)) return {};
  return std::format("checksum mismatch: {}: expected size={} xxh3={:016x}, actual size={} xxh3={:016x}",
                     expected.name, expected.size, expected.xxh3, actual.size, actual.xxh3);
}

void FileVerification::open(Directory& dir, std::span<const FileDescriptor> files, size_t group, bool direct) {
  items.reserve(items.size() + files.size());
  for (const auto& file : files) {
    Item item{group, file, nullptr, {}};
    try {
      item.reader = dir.openChecksum(file.name, direct);
      if (!item.reader) item.problem = "missing file: " + file.name;
    } catch (const std::exception& e) {
      item.problem = "unreadable file: " + file.name + ": " + e.what();
    }
    items.push_back(std::move(item));
  }
}

uint64_t FileVerification::bytes() const {
  uint64_t total = 0;
  for (const auto& item : items) if (item.reader) total += item.reader->size();
  return total;
}

void FileVerification::run(const std::function<void(size_t)>& progress) {
  // Largest first: a long tail of small files then fills in behind the big ones.
  std::vector<size_t> order(items.size());
  for (size_t i = 0; i < order.size(); i++) order[i] = i;
  std::ranges::sort(order, [&](size_t a, size_t b) {
    uint64_t sa = items[a].reader ? items[a].reader->size() : 0, sb = items[b].reader ? items[b].reader->size() : 0;
    return sa > sb;
  });
  oneapi::tbb::parallel_for(size_t(0), order.size(), [&](size_t i) {
    auto& item = items[order[i]];
    if (!item.reader) return;
    try {
      item.problem = checksumMismatch(item.file, item.reader->checksum(progress));
    } catch (const std::exception& e) {
      item.problem = "unreadable file: " + item.file.name + ": " + e.what();
    }
    item.reader.reset();
  }, oneapi::tbb::simple_partitioner());
}

std::string FileVerification::problem(size_t group) const {
  std::map<std::string_view, const std::string*> failures;
  for (const auto& item : items)
    if (item.group == group && !item.problem.empty()) failures.emplace(item.file.name, &item.problem);
  if (failures.empty()) return {};
  auto message = *failures.begin()->second;
  if (failures.size() > 1) message += std::format(" (and {} more files)", failures.size() - 1);
  return message;
}

void FileVerification::verified(size_t group, std::map<std::string, FileDescriptor>& out) const {
  for (const auto& item : items)
    if (item.group == group && item.problem.empty()) out.insert_or_assign(item.file.name, item.file);
}

void verifyFiles(Directory& dir, std::span<const FileDescriptor> files) {
  if (files.empty()) return;
  FileVerification verification;
  verification.open(dir, files);
  verification.run();
  if (auto problem = verification.problem(0); !problem.empty()) throw ChecksumMismatchError(problem);
}

}
