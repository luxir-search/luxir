// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <algorithm>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>
#include "Directory.h"
#include "FetchCountdown.h"

namespace luxir {

// Files secured by open handles (Directory::openChecksum): each handle keeps
// its bytes readable after the name is unlinked, and maps them later without
// reopening the name. A reader secures its snapshot's files this way when it
// opens and maps them only when a segment materializes, so commit-time
// deletion needs no knowledge of readers and cannot race a lazy open.
//
// As a Directory it serves openFile from the held handles only (a name it
// does not hold is absent) and refuses every mutation. Not thread safe: one
// materialization uses it at a time.
class HeldFiles final : public Directory {
  struct Held {
    std::string name;
    std::shared_ptr<ChecksumReader> handle;
    std::shared_ptr<InputFile> file;  // mapped by declareReads, handed out by openFile
  };
  std::vector<Held> handles;
  uint64_t mapped = 0;

  Held* find(std::string_view name) {
    for (auto& held : handles) {
      if (held.name == name) return &held;
    }
    return nullptr;
  }
  const Held* find(std::string_view name) const { return const_cast<HeldFiles*>(this)->find(name); }
  [[noreturn]] static void refuse(std::string_view operation) {
    throw std::logic_error("HeldFiles: " + std::string(operation) + " is not supported");
  }

public:
  // Opens a handle on `name` in `dir`; false when the file does not exist.
  bool hold(Directory& dir, std::string_view name) {
    if (find(name)) return true;
    auto handle = dir.openChecksum(name);
    if (!handle) return false;
    handles.push_back({std::string(name), std::move(handle), nullptr});
    return true;
  }
  // The mapping of a held file, made now if need be and kept for openFile,
  // so a fetch ahead of the open and the open read the same mapping. Null
  // when the name is not held or fails to map: the open maps it again and
  // reports the failure.
  std::shared_ptr<InputFile> mapping(std::string_view name) {
    auto* held = find(name);
    if (!held) return nullptr;
    if (!held->file) {
      try {
        held->file = held->handle->map();
      } catch (const std::exception&) {
        return nullptr;
      }
    }
    return held->file;
  }
  // Declares `ranges` of a held file, clipped to its size, for a fetch ahead
  // of the open; nothing when no range is left or the file does not map.
  void declare(std::string_view name, std::vector<ByteRange> ranges, ReadSet& reads) {
    auto file = mapping(name);
    if (!file) return;
    uint64_t size = (uint64_t)file->size();
    size_t kept = 0;
    for (const auto& range : ranges) {
      if (range.offset >= size) continue;
      ranges[kept++] = {range.offset, std::min(range.length, size - range.offset)};
    }
    ranges.resize(kept);
    if (!ranges.empty()) reads.push_back({std::move(file), std::move(ranges)});
  }
  // Declares all of a held file.
  void declareWhole(std::string_view name, ReadSet& reads) {
    if (auto file = mapping(name)) reads.push_back({file, {{0, (uint64_t)file->size()}}});
  }
  // Declares every held file whole: for parts whose open reads all of their
  // files (live docs, small overlays).
  void declareReads(ReadSet& reads) {
    for (auto& held : handles) declareWhole(held.name, reads);
  }
  // The names of the held files, valid while they are held.
  std::vector<std::string_view> names() const {
    std::vector<std::string_view> result;
    result.reserve(handles.size());
    for (const auto& held : handles) result.push_back(held.name);
    return result;
  }
  bool empty() const noexcept { return handles.empty(); }
  size_t size() const noexcept { return handles.size(); }
  // Bytes of the files mapped through openFile so far.
  uint64_t mappedBytes() const noexcept { return mapped; }

  std::shared_ptr<InputFile> openFile(std::string_view name, bool expectSynced = false) override {
    unused(expectSynced);  // checked when the files were secured
    auto* held = find(name);
    if (!held) return {};
    auto file = held->file ? std::exchange(held->file, nullptr) : held->handle->map();
    if (!file) return {};  // a backend that lost the bytes; the caller reports it
    mapped += (uint64_t)file->size();
    return file;
  }
  std::shared_ptr<ChecksumReader> openChecksum(std::string_view name, bool direct = false) override {
    unused(direct);
    const auto* held = find(name);
    return held ? held->handle : nullptr;
  }
  void listFiles(std::vector<FileInfo>& target) override {
    auto start = target.size();
    for (const auto& held : handles) target.push_back({held.name, held.handle->size()});
    std::sort(target.begin() + (int64_t)start, target.end(),
              [](const FileInfo& a, const FileInfo& b) { return a.name < b.name; });
  }

  std::unique_ptr<File> createFile(std::string_view) override { refuse("createFile"); }
  bool deleteFile(std::string_view) override { refuse("deleteFile"); }
  void deletePrefix(std::string_view) override { refuse("deletePrefix"); }
  void finishFile(File&) override { refuse("finishFile"); }
  void deleteDurably(std::span<const std::string>) override { refuse("deleteDurably"); }
  std::optional<PublicationVersion> publishFile(std::string_view, std::span<const std::byte>,
                                                PublishCondition) override {
    refuse("publishFile");
  }
  std::optional<Publication> readPublished(std::string_view) override { refuse("readPublished"); }
  void sync(std::span<const std::string>) override { refuse("sync"); }
  void clear() override { refuse("clear"); }
  void linkFile(Directory&, std::string_view) override { refuse("linkFile"); }
};

} // namespace luxir
