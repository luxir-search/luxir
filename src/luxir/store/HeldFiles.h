// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <stdexcept>
#include <string>
#include <utility>
#include <vector>
#include "Directory.h"

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
  std::vector<std::pair<std::string, std::shared_ptr<ChecksumReader>>> handles;
  uint64_t mapped = 0;

  const std::shared_ptr<ChecksumReader>* find(std::string_view name) const {
    for (const auto& [held, handle] : handles) {
      if (held == name) return &handle;
    }
    return nullptr;
  }
  [[noreturn]] static void refuse(std::string_view operation) {
    throw std::logic_error("HeldFiles: " + std::string(operation) + " is not supported");
  }

public:
  // Opens a handle on `name` in `dir`; false when the file does not exist.
  bool hold(Directory& dir, std::string_view name) {
    if (find(name)) return true;
    auto handle = dir.openChecksum(name);
    if (!handle) return false;
    handles.emplace_back(std::string(name), std::move(handle));
    return true;
  }
  bool empty() const noexcept { return handles.empty(); }
  size_t size() const noexcept { return handles.size(); }
  // Bytes of the files mapped through openFile so far.
  uint64_t mappedBytes() const noexcept { return mapped; }

  std::shared_ptr<InputFile> openFile(std::string_view name, bool expectSynced = false) override {
    unused(expectSynced);  // checked when the files were secured
    const auto* handle = find(name);
    if (!handle) return {};
    auto file = (*handle)->map();
    if (!file) return {};  // a backend that lost the bytes; the caller reports it
    mapped += (uint64_t)file->size();
    return file;
  }
  std::shared_ptr<ChecksumReader> openChecksum(std::string_view name, bool direct = false) override {
    unused(direct);
    const auto* handle = find(name);
    return handle ? *handle : nullptr;
  }
  void listFiles(std::vector<FileInfo>& target) override {
    auto start = target.size();
    for (const auto& [name, handle] : handles) target.push_back({name, handle->size()});
    std::sort(target.begin() + (int64_t)start, target.end(),
              [](const FileInfo& a, const FileInfo& b) { return a.name < b.name; });
  }

  std::unique_ptr<File> createFile(std::string_view) override { refuse("createFile"); }
  bool deleteFile(std::string_view) override { refuse("deleteFile"); }
  void deletePrefix(std::string_view) override { refuse("deletePrefix"); }
  void finishFile(File&) override { refuse("finishFile"); }
  void renameFile(std::string_view, std::string_view) override { refuse("renameFile"); }
  void clear() override { refuse("clear"); }
  void linkFile(Directory&, std::string_view) override { refuse("linkFile"); }
};

} // namespace luxir
