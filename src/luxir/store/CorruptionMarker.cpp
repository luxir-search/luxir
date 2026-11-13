// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include "luxir/store/CorruptionMarker.h"
#include "luxir/util/Uuid.h"
#include <set>

namespace luxir {

void CorruptionMarker::write(Directory& dir, std::span<const std::string> files, std::string_view reason) {
  std::string text = "# ";
  text += reason;
  text += '\n';
  for (const auto& file : files) { text += file; text += '\n'; }
  // Each report adds a marker; an existing one must not lose its suspects.
  while (!dir.publishFile(std::string(kPrefix) + newUuid(), std::as_bytes(std::span(text)),
                         Directory::PublishMode::CREATE_ONLY)) {}
}

std::vector<std::string> CorruptionMarker::read(Directory& dir) {
  std::vector<Directory::FileInfo> listing;
  dir.listFiles(listing);
  return read(dir, listing);
}

std::vector<std::string> CorruptionMarker::read(Directory& dir, std::span<const Directory::FileInfo> listing) {
  std::set<std::string> names;
  for (const auto& entry : listing) {
    if (!isMarker(entry.name)) continue;
    auto file = dir.openFile(entry.name);
    if (!file) continue;
    auto text = file->read();
    for (size_t start = 0; start < text.size();) {
      auto end = text.find('\n', start);
      if (end == std::string_view::npos) end = text.size();
      auto line = text.substr(start, end - start);
      start = end + 1;
      if (line.empty() || line.front() == '#') continue;
      names.emplace(line);
    }
  }
  return {names.begin(), names.end()};
}

bool CorruptionMarker::present(Directory& dir) {
  std::vector<Directory::FileInfo> listing;
  dir.listFiles(listing);
  return std::ranges::any_of(listing, [](const auto& entry) { return isMarker(entry.name); });
}

void CorruptionMarker::clear(Directory& dir) {
  std::vector<Directory::FileInfo> listing;
  dir.listFiles(listing);
  clear(dir, listing);
}

void CorruptionMarker::clear(Directory& dir, std::span<const Directory::FileInfo> listing) {
  std::vector<std::string> names;
  for (const auto& entry : listing) if (isMarker(entry.name)) names.push_back(entry.name);
  dir.deleteDurably(names);
}

}
