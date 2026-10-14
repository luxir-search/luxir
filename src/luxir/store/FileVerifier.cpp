// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include "FileVerifier.h"
#include <algorithm>
#include <atomic>
#include <format>
#include <map>
#include <mutex>
#include <thread>
#include <vector>
#define XXH_STATIC_LINKING_ONLY
#include <xxhash.h>

namespace luxir {

FileCheck hashFile(InputFile& file, const std::function<void(size_t)>& progress) {
  constexpr size_t kChunk = 1 << 20;
  auto data = file.read();
  XXH3_state_t state;
  XXH3_64bits_reset(&state);
  for (size_t offset = 0; offset < data.size(); offset += kChunk) {
    size_t length = std::min(kChunk, data.size() - offset);
    XXH3_64bits_update(&state, data.data() + offset, length);
    if (progress) progress(length);
  }
  return {(uint64_t)data.size(), XXH3_64bits_digest(&state)};
}

std::string checksumMismatch(const FileDescriptor& expected, const FileCheck& actual) {
  if (actual.size == expected.size && actual.xxh3 == expected.xxh3) return {};
  return std::format("checksum mismatch: {}: expected size={} xxh3={:016x}, actual size={} xxh3={:016x}",
                     expected.name, expected.size, expected.xxh3, actual.size, actual.xxh3);
}

std::vector<FileDescriptor> Directory::reuseFiles(Directory& source, std::span<const FileDescriptor> available,
                                                  std::span<const FileDescriptor> wanted, std::vector<std::string>* corrupt) {
  std::map<std::string_view, const FileDescriptor*> byName;
  for (const auto& file : available) byName.emplace(file.name, &file);
  std::vector<FileDescriptor> reused;
  for (const auto& file : wanted) {
    auto it = byName.find(file.name);
    if (it == byName.end() || *it->second != file) continue;
    // An existing (even unreadable) candidate must be verified by its caller.
    bool present = true;
    try { present = openFile(file.name) != nullptr; } catch (const std::exception&) {}
    if (present) continue;
    linkFile(source, file.name);
    std::string problem;
    try {
      auto linked = openFile(file.name);
      problem = linked ? checksumMismatch(file, hashFile(*linked)) : "checksum mismatch: " + file.name + ": missing";
    } catch (const std::exception& e) {
      problem = "checksum mismatch: " + file.name + ": unreadable: " + e.what();
    }
    if (problem.empty()) { reused.push_back(file); continue; }
    LOG_ERROR("Not reusing local file: {}", problem);
    deleteFile(file.name);
    if (corrupt) corrupt->push_back(std::move(problem));
  }
  return reused;
}

void verifyFiles(Directory& dir, std::span<const FileDescriptor> files, unsigned threads) {
  if (files.empty()) return;
  if (!threads) threads = std::clamp(std::thread::hardware_concurrency(), 1u, 8u);
  threads = (unsigned)std::min<size_t>(threads, files.size());
  std::atomic<size_t> next{0};
  std::mutex mutex;
  std::map<std::string, std::string> failures; // by file name
  auto work = [&] {
    for (size_t i; (i = next.fetch_add(1)) < files.size();) {
      const auto& file = files[i];
      std::string problem;
      try {
        auto input = dir.openFile(file.name, true);
        if (!input) problem = "checksum mismatch: " + file.name + ": missing";
        else problem = checksumMismatch(file, hashFile(*input));
      } catch (const std::exception& e) {
        problem = "checksum mismatch: " + file.name + ": unreadable: " + e.what();
      }
      if (problem.empty()) continue;
      std::lock_guard lock(mutex);
      failures.emplace(file.name, std::move(problem));
    }
  };
  {
    std::vector<std::jthread> pool;
    for (unsigned i = 1; i < threads; i++) pool.emplace_back(work);
    work();
  }
  if (failures.empty()) return;
  auto message = failures.begin()->second;
  if (failures.size() > 1) message += std::format(" (and {} more files)", failures.size() - 1);
  throw ChecksumMismatchError(message);
}

}
