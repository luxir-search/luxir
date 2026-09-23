// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <memory>
#include <set>
#include <map>
#include <gtest/gtest.h>
#include <memory_resource>
#include <span>
#include <stdexcept>

#include "luxir/api/padded_input.h"
#include "luxir/api/luxir_index.hpp"
#include "luxir/reader/Postings.h"
#include "luxir/store/Manifest.h"

namespace luxir::test {

// Owns the arena backing the non-owning IndexInfo view returned by the decoder.
struct DurableIndexInfo {
  std::unique_ptr<std::pmr::monotonic_buffer_resource> arena =
    std::make_unique<std::pmr::monotonic_buffer_resource>();
  luxir::api::IndexInfo info;

  const luxir::api::IndexInfo* operator->() const { return &info; }
  const luxir::api::IndexInfo& operator*() const { return info; }
};

inline DurableIndexInfo readDurableIndexInfo(Directory& dir) {
  DurableIndexInfo result;
  result.info = Manifest::decode(Manifest::load(dir).bytes, *result.arena);
  return result;
}

inline std::vector<api::FileDescriptor> manifestFiles(const api::IndexInfo& info) {
  std::vector<api::FileDescriptor> files;
  auto append = [&](const auto& source) { files.insert(files.end(), source.begin(), source.end()); };
  for (const auto& segment : info.segments) {
    append(segment.files);
    for (const auto& overlay : segment.overlays) append(overlay.files);
  }
  for (const auto& aux : info.aux_indexes) append(aux.files);
  return files;
}

inline std::map<std::string, std::pair<uint64_t, uint64_t>> fileInventory(const api::IndexInfo& info) {
  std::map<std::string, std::pair<uint64_t, uint64_t>> result;
  for (const auto& file : manifestFiles(info)) result.emplace(file.name, std::pair(file.size, file.xxh3));
  return result;
}

inline std::vector<std::string> fileNames(std::span<const api::FileDescriptor> files) {
  std::vector<std::string> result;
  for (const auto& file : files) result.emplace_back(file.name);
  return result;
}

// Segment inventory must match its standalone footer plus current liveDocs.
inline void expectValidInventory(Directory& dir, const api::IndexInfo& info) {
  for (const auto& seg : info.segments) {
    std::set<std::string> expected;
    auto id = Postings::getSortableString(seg.seg_id);
    auto first = dir.openFile(Postings::getIndexFileName(id, 0));
    ASSERT_NE(nullptr, first);
    auto input = first->getInputStream();
    input.seek(input.size() - sizeof(int32_t));
    auto footerSize = input.readInt();
    input.seek(input.size() - sizeof(int32_t) - footerSize);
    input.readVint(); // maxDoc
    auto count = input.readVint();
    for (uint32_t i = 0; i < count; i++) expected.insert(Postings::getIndexFileName(id, input.readVint()));
    if (seg.live_gen) expected.insert(Postings::getLiveDocsFileName(id, seg.live_gen));
    auto names = fileNames(seg.files);
    EXPECT_EQ(expected, std::set<std::string>(names.begin(), names.end()));
  }
  std::set<std::string> actual;
  for (const auto& descriptor : manifestFiles(info)) {
    EXPECT_TRUE(actual.emplace(descriptor.name).second);
    auto file = dir.openFile(descriptor.name);
    ASSERT_NE(nullptr, file);
    auto bytes = file->read();
    EXPECT_EQ(descriptor.size, bytes.size());
    EXPECT_EQ(descriptor.xxh3, XXH3_64bits(bytes.data(), bytes.size()));
  }
}

} // namespace luxir::test
