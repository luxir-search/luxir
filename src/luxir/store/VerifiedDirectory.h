// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <map>
#include "FileVerifier.h"

namespace luxir {

// After run() succeeds, opens checked files through the verified handles.
// Only the preparation borrows this view and its verification; the resulting
// readers retain InputFiles, which outlive both. Unchecked names use delegate.
class VerifiedDirectory : public Directory {
  Directory& delegate;
  std::map<std::string_view, const FileVerification::Item*, std::less<>> held;
public:
  VerifiedDirectory(Directory& delegate, const FileVerification& verification) : delegate(delegate) {
    for (const auto& item : verification.items) held.emplace(item.file.name, &item);
  }

  std::shared_ptr<InputFile> openFile(std::string_view name, bool expectSynced = false) override {
    auto it = held.find(name);
    if (it == held.end()) return delegate.openFile(name, expectSynced);
    const auto& item = *it->second;
    if (expectSynced) delegate.expectSynced(std::span(&item.file, 1));
    return item.reader->map();
  }

  void listFiles(std::vector<FileInfo>& target) override { delegate.listFiles(target); }
  std::shared_ptr<ChecksumReader> openChecksum(std::string_view name, bool direct = false) override {
    return delegate.openChecksum(name, direct);
  }
  void expectSynced(std::span<const FileDescriptor> files) override { delegate.expectSynced(files); }
  std::unique_ptr<File> createFile(std::string_view name) override { return delegate.createFile(name); }
  std::unique_ptr<File> createFile(std::string_view name, FileCreateOptions options) override {
    return delegate.createFile(name, std::move(options));
  }
  bool deleteFile(std::string_view name) override { return delegate.deleteFile(name); }
  void deletePrefix(std::string_view prefix) override { delegate.deletePrefix(prefix); }
  void finishFile(File& file) override { delegate.finishFile(file); }
  void renameFile(std::string_view from, std::string_view to) override { delegate.renameFile(from, to); }
  void sync(std::span<const std::string> names) override { delegate.sync(names); }
  void clear() override { delegate.clear(); }
  Directory& underlying() override { return delegate.underlying(); }
  void linkFile(Directory& source, std::string_view name) override { delegate.linkFile(source, name); }
  Capabilities capabilities() const override { return delegate.capabilities(); }
};

}
