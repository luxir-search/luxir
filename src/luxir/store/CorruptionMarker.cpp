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
  auto name = std::string(kPrefix) + newUuid();
  Directory::FileCreateOptions options; options.expectedSize = text.size();
  auto file = dir.createFile(name, options);
  OutputStream out(file.get());
  out.write(text.data(), text.size()); out.close();
  dir.finishFile(*file);
  std::array<std::string, 2> synced{name, "."};
  dir.sync(synced);
}

std::vector<std::string> CorruptionMarker::read(Directory& dir) {
  std::vector<Directory::FileInfo> listing;
  dir.listFiles(listing);
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
  bool removed = false;
  for (const auto& entry : listing) if (isMarker(entry.name)) removed = dir.deleteFile(entry.name) || removed;
  if (!removed) return;
  std::array<std::string, 1> directory{"."};
  dir.sync(directory);
}

}
