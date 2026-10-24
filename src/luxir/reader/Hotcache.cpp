// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include "luxir/reader/Hotcache.h"

#include <algorithm>
#include <cstring>
#include <stdexcept>

#define XXH_STATIC_LINKING_ONLY
#include <xxhash.h>

#include "luxir/reader/Postings.h"
#include "luxir/reader/PostingsReader.h"
#include "luxir/util/log.h"

namespace luxir {

namespace {

constexpr uint64_t kFormatVersion = 1;

void putVlong(std::string& out, uint64_t value) {
  while (value > 0x7f) {
    out.push_back((char)(value | 0x80));
    value >>= 7;
  }
  out.push_back((char)value);
}

uint64_t getVlong(const char*& pos, const char* end) {
  uint64_t value = 0;
  for (int shift = 0; shift < 64; shift += 7) {
    if (pos >= end) throw std::runtime_error("truncated hotcache");
    auto byte = (uint8_t)*pos++;
    value |= (uint64_t)(byte & 0x7f) << shift;
    if (!(byte & 0x80)) return value;
  }
  throw std::runtime_error("invalid hotcache varint");
}

}  // namespace

std::string Hotcache::fileName(std::string_view segStr) {
  return std::string(Postings::PREFIX_FNAME).append(segStr).append("__hot");
}

std::string Hotcache::fileName(uint64_t segId) { return fileName(Postings::getSortableString(segId)); }

std::optional<Hotcache::DataFile> Hotcache::parseDataFile(std::string_view name) {
  // s<segStr>_<filenum>, where both parts are sortable strings: one underscore
  // only (liveDocs "__L" and overlays "__" and the hotcache "__hot" have two).
  if (!name.starts_with(Postings::PREFIX_FNAME)) return std::nullopt;
  auto underscore = name.find('_');
  if (underscore == std::string_view::npos || underscore < 3) return std::nullopt;
  std::string_view segStr = name.substr(Postings::PREFIX_FNAME.size(), underscore - Postings::PREFIX_FNAME.size());
  std::string_view fileStr = name.substr(underscore + 1);
  if (fileStr.size() < 2 || fileStr.find('_') != std::string_view::npos) return std::nullopt;
  auto isDigit36 = [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z'); };
  if (!std::ranges::all_of(segStr, isDigit36) || !std::ranges::all_of(fileStr, isDigit36)) return std::nullopt;
  uint64_t fileNum = Postings::parseSortableString(fileStr);
  if (fileNum > UINT32_MAX || Postings::getSortableString(fileNum) != fileStr) return std::nullopt;
  return DataFile{segStr, (uint32_t)fileNum};
}

std::string Hotcache::encode() const {
  std::string out(kMagic);
  putVlong(out, kFormatVersion);
  putVlong(out, entries.size());
  for (const auto& entry : entries) {
    putVlong(out, entry.fileNum);
    putVlong(out, entry.range.offset);
    putVlong(out, entry.range.length);
  }
  uint64_t hash = XXH3_64bits(out.data(), out.size());
  for (int i = 0; i < 8; i++) out.push_back((char)(hash >> (8 * i)));
  return out;
}

Hotcache Hotcache::decode(std::string_view bytes) {
  if (bytes.size() < kMagic.size() + 2 + 8 || bytes.substr(0, kMagic.size()) != kMagic) {
    throw std::runtime_error("hotcache has invalid magic");
  }
  size_t bodySize = bytes.size() - 8;
  uint64_t expected = 0;
  for (int i = 0; i < 8; i++) expected |= (uint64_t)(uint8_t)bytes[bodySize + (size_t)i] << (8 * i);
  if (XXH3_64bits(bytes.data(), bodySize) != expected) throw std::runtime_error("hotcache checksum mismatch");
  const char* pos = bytes.data() + kMagic.size();
  const char* end = bytes.data() + bodySize;
  if (getVlong(pos, end) != kFormatVersion) throw std::runtime_error("unsupported hotcache version");
  uint64_t count = getVlong(pos, end);
  if (count > bodySize) throw std::runtime_error("invalid hotcache entry count");
  Hotcache cache;
  cache.entries.reserve((size_t)count);
  for (uint64_t i = 0; i < count; i++) {
    uint64_t fileNum = getVlong(pos, end);
    uint64_t offset = getVlong(pos, end);
    uint64_t length = getVlong(pos, end);
    if (fileNum > UINT32_MAX || offset > UINT64_MAX - length) throw std::runtime_error("invalid hotcache entry");
    cache.entries.push_back({(uint32_t)fileNum, {offset, length}});
  }
  if (pos != end) throw std::runtime_error("trailing bytes in hotcache");
  return cache;
}

std::vector<ByteRange> Hotcache::rangesOf(uint32_t fileNum) const {
  std::vector<ByteRange> result;
  for (const auto& entry : entries) {
    if (entry.fileNum == fileNum) result.push_back(entry.range);
  }
  return result;
}


class RecordingDirectory::File : public InputFile {
  RecordingDirectory& owner;
  std::shared_ptr<InputFile> inner;
  std::string name;

  void whole() { owner.add(name, {0, (uint64_t)inner->size()}); }

public:
  File(RecordingDirectory& owner, std::shared_ptr<InputFile> inner, std::string name)
      : owner(owner), inner(std::move(inner)), name(std::move(name)) {}

  size_t size() override { return inner->size(); }
  std::string_view read() override { whole(); return inner->read(); }
  InputStream getInputStream() override { whole(); return inner->getInputStream(); }
  InputStream rangeStream(uint64_t offset, uint64_t length) override {
    owner.add(name, {offset, length});
    return inner->rangeStream(offset, length);
  }
  void prefetchRanges(std::span<const ByteRange> ranges) override {
    for (const auto& range : ranges) owner.add(name, range);
    inner->prefetchRanges(ranges);
  }
  void prefetch(size_t offset, size_t length) override {
    owner.add(name, {offset, length});
    inner->prefetch(offset, length);
  }
  // Records as a lazy file would be read: a reader must not take the whole-file
  // stream eagerly, which would record the whole file as touched.
  bool lazy() const override { return true; }
};

RecordingDirectory::RecordingDirectory(Directory& inner)
    : DelegatingDirectory(std::shared_ptr<Directory>(std::shared_ptr<void>(), &inner)) {}

std::shared_ptr<InputFile> RecordingDirectory::openFile(std::string_view name, bool expectSynced) {
  auto file = delegate->openFile(name, expectSynced);
  if (!file) return file;
  return std::make_shared<File>(*this, std::move(file), std::string(name));
}

RecordingDirectory::Ranges RecordingDirectory::recorded() {
  std::lock_guard lock(mutex);
  return ranges_;
}

void RecordingDirectory::add(const std::string& name, ByteRange range) {
  if (range.length == 0) return;
  std::lock_guard lock(mutex);
  ranges_[name].push_back(range);
}


Hotcache Hotcache::record(Directory& dir, uint64_t segId) {
  RecordingDirectory recording(dir);
  {
    // Whatever the open touches is the hotcache; the reader goes away here.
    auto reader = PostingsReader::create(recording, segId);
    (void)reader;
  }
  std::string segStr = Postings::getSortableString(segId);
  Hotcache cache;
  for (auto& [name, ranges] : recording.recorded()) {
    auto parsed = parseDataFile(name);
    if (!parsed || parsed->segStr != segStr) continue;
    auto file = dir.openFile(name);
    uint64_t fileSize = file ? (uint64_t)file->size() : 0;
    for (auto& range : ranges) {
      uint64_t begin = range.offset / kAlign * kAlign;
      uint64_t finish = std::min(fileSize, (range.offset + range.length + kAlign - 1) / kAlign * kAlign);
      range = {begin, finish > begin ? finish - begin : 0};
    }
    std::ranges::sort(ranges, {}, &ByteRange::offset);
    std::vector<ByteRange> merged;
    for (const auto& range : ranges) {
      if (range.length == 0) continue;
      if (!merged.empty() && range.offset <= merged.back().offset + merged.back().length) {
        uint64_t finish = std::max(merged.back().offset + merged.back().length, range.offset + range.length);
        merged.back().length = finish - merged.back().offset;
      } else {
        merged.push_back(range);
      }
    }
    for (const auto& range : merged) cache.entries.push_back({parsed->fileNum, range});
  }
  std::ranges::sort(cache.entries, [](const Entry& a, const Entry& b) {
    return a.fileNum != b.fileNum ? a.fileNum < b.fileNum : a.range.offset < b.range.offset;
  });
  return cache;
}

FileDescriptor Hotcache::write(Directory& dir, uint64_t segId) {
  std::string bytes = record(dir, segId).encode();
  auto file = dir.createFile(fileName(segId));
  OutputStream out(file.get());
  out.write(bytes.data(), bytes.size());
  out.close();
  dir.finishFile(*file);
  return file->descriptor();
}

void writeHotcacheIfRemote(Directory& dir, uint64_t segId, std::vector<std::string>* filenames,
                           std::vector<FileDescriptor>* descriptors) {
  if (!(dir.capabilities() & Directory::REMOTE)) return;
  try {
    auto descriptor = Hotcache::write(dir, segId);
    if (filenames) filenames->push_back(descriptor.name);
    if (descriptors) descriptors->push_back(std::move(descriptor));
  } catch (const std::exception& e) {
    LOG_WARN("segment {} has no hotcache: {}", Postings::getSortableString(segId), e.what());
  }
}

}  // namespace luxir
