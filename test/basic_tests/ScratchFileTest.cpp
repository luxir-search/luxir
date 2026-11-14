// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include "test/LuxirTest.h"
#include "luxir/util/ScratchFile.h"

#include <cstring>
#include <filesystem>
#include <numeric>
#include <vector>

using namespace luxir;

class ScratchFileTest : public luxir::LuxirTest {};

namespace {
size_t scratchEntries() {
  size_t n = 0;
  for (const auto& entry : std::filesystem::directory_iterator(ScratchFile::directory())) {
    n += entry.path().filename().string().starts_with("luxir-scratch-");
  }
  return n;
}
}  // namespace

TEST_F(ScratchFileTest, appendsAcrossBuffersAndMaps) {
  size_t before = scratchEntries();
  ScratchFile file(10);  // tiny buffer: appends straddle flushes
  EXPECT_EQ(before, scratchEntries());  // unlinked at creation
  std::vector<uint32_t> values(1000);
  std::iota(values.begin(), values.end(), 7u);
  file.append(values.data(), 3);
  file.append((const char*)values.data() + 3, values.size() * sizeof(uint32_t) - 3);
  EXPECT_EQ(values.size() * sizeof(uint32_t), file.size());
  auto bytes = file.map();
  ASSERT_EQ(values.size() * sizeof(uint32_t), bytes.size());
  EXPECT_EQ(0, std::memcmp(values.data(), bytes.data(), bytes.size()));
  EXPECT_EQ(bytes.data(), file.map().data());
  EXPECT_THROW(file.append(values.data(), 1), std::logic_error);
}

TEST_F(ScratchFileTest, emptyMapsToNothing) {
  ScratchFile file;
  EXPECT_TRUE(file.map().empty());
}
