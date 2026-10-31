// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include "SourceMap.h"

#include <algorithm>
#include <cstring>
#include <stdexcept>

#include <xxhash.h>

#include "luxir/reader/Postings.h"
#include "luxir/store/OutputStream.h"

namespace luxir {

namespace {

constexpr uint64_t kMapFormat = 1;

template <class T>
T load(const char* p) {
  T v;
  memcpy(&v, p, sizeof(T));
  return v;
}

}  // namespace

uint32_t SourceMap::fileIndex(const SourceFileRef& ref) {
  auto it = std::ranges::find(files, ref.name, &SourceFileRef::name);
  if (it != files.end()) return (uint32_t)(it - files.begin());
  files.push_back(ref);
  return (uint32_t)files.size() - 1;
}

void SourceMap::addRun(int32_t docStart, int32_t count, uint32_t file, uint32_t ordinal) {
  if (count <= 0) return;
  if (!runs.empty()) {
    auto& last = runs.back();
    if (last.file == file && last.docStart + last.count == docStart && last.ordinal + (uint32_t)last.count == ordinal) {
      last.count += count;
      return;
    }
  }
  runs.push_back({docStart, count, file, ordinal});
}

bool SourceMap::locate(int32_t doc, uint32_t& file, uint32_t& ordinal) const {
  size_t hint = runs.size();
  return locate(doc, hint, file, ordinal);
}

bool SourceMap::locate(int32_t doc, size_t& hint, uint32_t& file, uint32_t& ordinal) const {
  auto found = [&](size_t i) {
    const auto& run = runs[i];
    if (doc < run.docStart || doc - run.docStart >= run.count) return false;
    hint = i;
    file = run.file;
    ordinal = run.ordinal + (uint32_t)(doc - run.docStart);
    return true;
  };
  for (size_t i = hint; i < hint + 2 && i < runs.size(); i++) {
    if (found(i)) return true;
  }
  auto it = std::ranges::upper_bound(runs, doc, {}, &SourceRun::docStart);
  if (it == runs.begin()) return false;
  return found((size_t)(it - runs.begin()) - 1);
}

void SourceMap::normalize() {
  std::ranges::sort(runs, {}, &SourceRun::docStart);
  std::vector<SourceRun> joined;
  joined.reserve(runs.size());
  for (const auto& run : runs) {
    if (!joined.empty()) {
      auto& last = joined.back();
      if (last.file == run.file && last.docStart + last.count == run.docStart &&
          last.ordinal + (uint32_t)last.count == run.ordinal) {
        last.count += run.count;
        continue;
      }
    }
    joined.push_back(run);
  }
  runs = std::move(joined);
}

int64_t SourceMap::coveredDocs() const {
  int64_t total = 0;
  for (const auto& run : runs) total += run.count;
  return total;
}

void SourceMap::write(Directory& dir, const std::string& name, std::vector<std::string>* names,
                      std::vector<FileDescriptor>* descriptors) const {
  std::string out(Postings::LUXIR_HEADER);
  auto put = [&](auto v) { out.append((const char*)&v, sizeof(v)); };
  put(kMapFormat);
  put((uint32_t)files.size());
  put((uint32_t)runs.size());
  for (const auto& f : files) {
    put((uint32_t)f.name.size());
    out.append(f.name);
    put(f.docs);
    put(f.bytes);
  }
  for (const auto& run : runs) {
    put((uint32_t)run.docStart);
    put((uint32_t)run.count);
    put(run.file);
    put(run.ordinal);
  }
  put((uint64_t)XXH3_64bits(out.data(), out.size()));

  Directory::FileCreateOptions options;
  options.expectedSize = out.size();
  auto file = dir.createFile(name, options);
  if (!file) throw std::runtime_error("cannot create source map " + name);
  OutputStream stream(file.get());
  stream.write(out.data(), out.size());
  stream.close();
  dir.finishFile(*file);
  if (names) names->push_back(name);
  if (descriptors) descriptors->push_back(file->descriptor());
}

SourceMap SourceMap::read(Directory& dir, const std::string& name, int32_t maxDoc) {
  auto bad = [&](const char* why) { return std::runtime_error("source map " + name + ": " + why); };
  auto file = dir.openFile(name);
  if (!file) throw bad("missing");
  std::string_view data = file->read();
  constexpr size_t fixed = 8 + 8 + 4 + 4;
  if (data.size() < fixed + 8 || data.substr(0, 8) != Postings::LUXIR_HEADER) throw bad("not a source map");
  if (load<uint64_t>(data.data() + data.size() - 8) != XXH3_64bits(data.data(), data.size() - 8)) {
    throw bad("checksum mismatch");
  }
  if (load<uint64_t>(data.data() + 8) != kMapFormat) throw bad("unsupported format");
  uint32_t nFiles = load<uint32_t>(data.data() + 16);
  uint32_t nRuns = load<uint32_t>(data.data() + 20);
  const char* p = data.data() + fixed;
  const char* end = data.data() + data.size() - 8;
  SourceMap map;
  for (uint32_t i = 0; i < nFiles; i++) {
    if (end - p < 4) throw bad("truncated");
    uint32_t len = load<uint32_t>(p);
    p += 4;
    if ((size_t)(end - p) < (size_t)len + 12) throw bad("truncated");
    SourceFileRef ref{std::string(p, len), load<uint32_t>(p + len), load<uint64_t>(p + len + 4)};
    map.files.push_back(std::move(ref));
    p += (size_t)len + 12;
  }
  if ((size_t)(end - p) != (size_t)nRuns * 16) throw bad("run table size mismatch");
  int64_t previousEnd = 0;
  for (uint32_t i = 0; i < nRuns; i++, p += 16) {
    SourceRun run{(int32_t)load<uint32_t>(p), (int32_t)load<uint32_t>(p + 4), load<uint32_t>(p + 8), load<uint32_t>(p + 12)};
    if (run.count <= 0 || run.docStart < previousEnd || (int64_t)run.docStart + run.count > maxDoc ||
        run.file >= nFiles) {
      throw bad("inconsistent run");
    }
    previousEnd = (int64_t)run.docStart + run.count;
    map.runs.push_back(run);
  }
  return map;
}

}  // namespace luxir
