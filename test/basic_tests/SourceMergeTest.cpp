// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

// The source layout of a merge over opaque records: which files the output
// references, which it relocates, and the runs that follow the merge's own
// doc mapping. The relocation is a fake that only counts.

#include <gtest/gtest.h>

#include <map>

#include "luxir/index/SourceMerge.h"

namespace luxir::test {

namespace {

class CountingRelocation final : public SourceRelocation {
public:
  std::map<std::string, uint32_t> copied;  // file -> records copied
  std::vector<std::string> unreadable;     // files whose verification fails
  uint32_t out = 0;

  bool verify(const SourceFileRef& file, uint32_t, uint32_t) override {
    return std::ranges::find(unreadable, file.name) == unreadable.end();
  }
  void copy(const SourceFileRef& file, uint32_t, uint32_t count, uint32_t& firstOut) override {
    firstOut = out;
    out += count;
    copied[file.name] += count;
  }
  const std::string& outputName() const override { return name; }
  std::optional<Output> finish() override {
    if (out == 0) return std::nullopt;
    return Output{{name, out, (uint64_t)out * 100}, {name, (uint64_t)out * 100, 0}};
  }

private:
  std::string name = "vout";
};

SourceMap mapOf(const SourceFileRef& file, int32_t docs) {
  SourceMap map;
  map.addRun(0, docs, map.fileIndex(file), 0);
  return map;
}

SourceSettings settings(double minLiveRatio = 0.5, uint64_t minFileBytes = 0, uint32_t maxFiles = 32) {
  SourceSettings s;
  s.enabled = true;
  s.minLiveRatio = minLiveRatio;
  s.minFileBytes = minFileBytes;
  s.maxFiles = maxFiles;
  return s;
}

// Plain merge order: input docs land one after the other, deleted ones dropped.
SourceDocMap sequential(std::vector<std::vector<int32_t>> mapped) {
  return [mapped = std::move(mapped)](size_t input, int32_t doc) { return mapped[input][(size_t)doc]; };
}

}  // namespace

TEST(SourceMergeTest, referencesLiveFilesAndRelocatesMostlyDeletedOnes) {
  SourceFileRef big{"v1", 4, 4000}, small{"v2", 4, 4000};
  auto a = mapOf(big, 4), b = mapOf(small, 4);
  const SourceMap* inputs[] = {&a, &b};
  CountingRelocation relocation;
  // All of v1 live; only one doc of v2 live.
  auto result = mergeSources(settings(), inputs, sequential({{0, 1, 2, 3}, {-1, 4, -1, -1}}), relocation);
  ASSERT_EQ(2u, result.map.files.size());
  EXPECT_EQ("v1", result.map.files[0].name);
  EXPECT_EQ("vout", result.map.files[1].name);
  EXPECT_EQ(1u, result.map.files[1].docs);
  ASSERT_EQ(2u, result.map.runs.size());
  EXPECT_EQ(0, result.map.runs[0].docStart);
  EXPECT_EQ(4, result.map.runs[0].count);
  EXPECT_EQ(4, result.map.runs[1].docStart);
  EXPECT_EQ(1, result.map.runs[1].count);
  EXPECT_EQ(0u, result.map.runs[1].ordinal);  // first record of the output file
  EXPECT_EQ(1u, relocation.copied["v2"]);
  EXPECT_EQ(0u, relocation.copied.count("v1"));
  ASSERT_TRUE(result.written.has_value());
  EXPECT_EQ("vout", result.written->name);
}

TEST(SourceMergeTest, followsAReorderingDocMap) {
  SourceFileRef file{"v1", 4, 4000};
  auto a = mapOf(file, 4);
  const SourceMap* inputs[] = {&a};
  CountingRelocation relocation;
  // A sorted merge: docs 0..3 land at 2, 0, 3, 1.
  auto result = mergeSources(settings(), inputs, sequential({{2, 0, 3, 1}}), relocation);
  ASSERT_EQ(4u, result.map.runs.size());
  uint32_t file0 = 0, ordinal = 0;
  for (int32_t doc : {0, 1, 2, 3}) {
    ASSERT_TRUE(result.map.locate(doc, file0, ordinal));
  }
  ASSERT_TRUE(result.map.locate(2, file0, ordinal));
  EXPECT_EQ(0u, ordinal);  // merged doc 2 is input doc 0
  ASSERT_TRUE(result.map.locate(1, file0, ordinal));
  EXPECT_EQ(3u, ordinal);
  EXPECT_TRUE(relocation.copied.empty());
}

TEST(SourceMergeTest, unverifiableFileStaysReferenced) {
  SourceFileRef file{"v1", 4, 4000};
  auto a = mapOf(file, 4);
  const SourceMap* inputs[] = {&a};
  CountingRelocation relocation;
  relocation.unreadable = {"v1"};
  // One live doc of four would be relocated, but the file cannot be read.
  auto result = mergeSources(settings(), inputs, sequential({{-1, -1, 0, -1}}), relocation);
  ASSERT_EQ(1u, result.map.files.size());
  EXPECT_EQ("v1", result.map.files[0].name);
  EXPECT_EQ((std::vector<std::string>{"v1"}), result.unreadable);
  EXPECT_FALSE(result.written.has_value());
  EXPECT_TRUE(relocation.copied.empty());
}

TEST(SourceMergeTest, maxFilesRelocatesTheSmallShare) {
  // Three fully live files; the third holds under a third of the bytes.
  SourceFileRef f1{"v1", 2, 5000}, f2{"v2", 2, 5000}, f3{"v3", 2, 100};
  auto a = mapOf(f1, 2), b = mapOf(f2, 2), c = mapOf(f3, 2);
  const SourceMap* inputs[] = {&a, &b, &c};
  CountingRelocation relocation;
  auto result = mergeSources(settings(0.5, 0, 3), inputs, sequential({{0, 1}, {2, 3}, {4, 5}}), relocation);
  std::vector<std::string> names;
  for (const auto& ref : result.map.files) names.push_back(ref.name);
  EXPECT_EQ((std::vector<std::string>{"v1", "v2", "vout"}), names);
  EXPECT_EQ(2u, relocation.copied["v3"]);
}

TEST(SourceMergeTest, referencedFilesStayWithinMaxFiles) {
  // Four files, each half live (at the ratio floor) and each exactly one share of the live bytes.
  auto a = mapOf({"v1", 2, 100}, 2), b = mapOf({"v2", 2, 100}, 2);
  auto c = mapOf({"v3", 2, 100}, 2), d = mapOf({"v4", 2, 100}, 2);
  const SourceMap* inputs[] = {&a, &b, &c, &d};
  CountingRelocation relocation;
  auto result = mergeSources(settings(0.5, 0, 2), inputs, sequential({{0, -1}, {1, -1}, {2, -1}, {3, -1}}), relocation);
  EXPECT_EQ(3u, result.map.files.size());  // two referenced files and the one written
  EXPECT_EQ(2u, relocation.copied.size());
}

TEST(SourceMergeTest, minFileBytesFoldsSmallFilesEvenWhenLive) {
  SourceFileRef f1{"v1", 2, 100}, f2{"v2", 2, 100};
  auto a = mapOf(f1, 2), b = mapOf(f2, 2);
  const SourceMap* inputs[] = {&a, &b};
  CountingRelocation relocation;
  auto result = mergeSources(settings(0.5, 4096), inputs, sequential({{0, 1}, {2, 3}}), relocation);
  ASSERT_EQ(1u, result.map.files.size());
  EXPECT_EQ("vout", result.map.files[0].name);
  ASSERT_EQ(1u, result.map.runs.size());
  EXPECT_EQ(4, result.map.runs[0].count);
}

TEST(SourceMergeTest, nothingToCarryYieldsAnEmptyMap) {
  const SourceMap* inputs[] = {nullptr, nullptr};
  CountingRelocation relocation;
  auto result = mergeSources(settings(), inputs, sequential({{0}, {1}}), relocation);
  EXPECT_TRUE(result.map.runs.empty());
  EXPECT_FALSE(result.written.has_value());
}

}  // namespace luxir::test
