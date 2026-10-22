// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include <gtest/gtest.h>

#include <algorithm>
#include <climits>
#include <cstring>
#include <format>
#include <functional>
#include <memory_resource>
#include <string>
#include <vector>

#include "luxir/reader/PostingsReader.h"
#include "luxir/server/LuxirNode.h"
#include "luxir/store/IndexFormat.h"
#include "luxir/store/Directory.h"
#include "luxir/store/Manifest.h"
#include "test/CollectionHelper.h"
#include "test/DurableIndexInfo.h"
#include "test/LuxirTest.h"
#include "test/TestUtils.h"

namespace luxir::test {

class IndexFormatTest : public LuxirTest {};

namespace {

// The digest of the reference index written by IndexFormat::VERSION. When a
// change alters the bytes Luxir writes, this test fails: bump
// IndexFormat::VERSION (and HEADER) and record both new values here. Changing
// only the digest leaves old collections loading under the new reader.
constexpr uint64_t kReferenceVersion = 8;
constexpr uint64_t kReferenceDigest = 0x493015b943bbc69b;

// A small index over most field kinds, with two segments and a delete. Each
// segment is one request flushed by a single inverter. Merges are left out:
// fields merge as parallel tasks, so a merged segment's file layout varies.
void buildReferenceIndex(CollectionHelper& ch) {
  ASSERT_TRUE(ch.indexAll({
      flatdoc("id", "a", "title_t", "the quick brown fox", "tag_s", "red", "tags_ss", vecs("x", "y"),
              "n_i", (int64_t)7, "ns_is", std::vector<int64_t>{1, 2}, "f_f", 1.5f, "d_d", 2.25,
              "dt_dt", "2026-01-02T03:04:05Z", "w_w", "Hello World", "vec_v", std::vector<float>{1, 0}),
      flatdoc("id", "b", "title_t", "lazy dogs sleep", "tag_s", "blue", "n_i", (int64_t)-3,
              "vec_v", std::vector<float>{0, 1}),
  }, UpdateMessage::COMMIT).success);
  ASSERT_TRUE(ch.indexAll({
      flatdoc("id", "c", "title_t", "quick quick fox", "tag_s", "red", "n_i", (int64_t)42,
              "dt_dt", "2025-12-31T00:00:00Z", "vec_v", std::vector<float>{0.5f, 0.5f}),
  }, UpdateMessage::COMMIT).success);
  ASSERT_TRUE(ch.deleteById("b", UpdateMessage::COMMIT).success);
}

// The manifest re-encoded without the fields that differ from run to run, so
// file names, sizes and content hashes stand in for the data files.
uint64_t referenceDigest(Directory& dir) {
  auto durable = readDurableIndexInfo(dir);
  auto info = durable.info;
  std::vector<api::SegmentInfo> segments(info.segments.begin(), info.segments.end());
  for (auto& segment : segments) segment.commit_time = 0;
  info.segments = segments;
  info.commit_time = 0;
  info.incarnation = "";
  info.schema.reset(); // the default schema can change without the format changing
  std::vector<std::byte> bytes;
  EXPECT_TRUE(api::encode(info, bytes));
  return XXH3_64bits(bytes.data(), bytes.size());
}

uint64_t buildReferenceDigest() {
  LuxirNode node(LuxirConfig{});
  CollectionHelper ch(node, "main");
  buildReferenceIndex(ch);
  return referenceDigest(ch.getIndexWriter()->dir);
}

// Copies every file of the reference index's first segment into a RAM
// directory, letting `mutate` change file 0.
std::unique_ptr<RAMDir> copySegment(Directory& source, const std::function<void(std::string&)>& mutate) {
  auto durable = readDurableIndexInfo(source);
  auto copy = std::make_unique<RAMDir>();
  for (const auto& file : durable.info.segments[0].files) {
    std::string name(file.name);
    std::string bytes(source.openFile(name)->read());
    if (name == Postings::getIndexFileName(Postings::getSortableString(durable.info.segments[0].seg_id), 0)) {
      mutate(bytes);
    }
    auto out = copy->createFile(name);
    OutputStream stream(out.get());
    stream.write(bytes.data(), bytes.size());
    stream.close();
    copy->finishFile(*out);
  }
  return copy;
}

int32_t loadInt(const std::string& bytes, size_t at) {
  int32_t value;
  std::memcpy(&value, bytes.data() + at, sizeof(value));
  return value;
}

void storeInt(std::string& bytes, size_t at, int32_t value) {
  std::memcpy(bytes.data() + at, &value, sizeof(value));
}

} // namespace

TEST_F(IndexFormatTest, referenceIndexBytesMatchFormatVersion) {
  auto digest = buildReferenceDigest();
  ASSERT_EQ(digest, buildReferenceDigest()) << "the reference index is not deterministic";
  EXPECT_EQ(IndexFormat::VERSION, kReferenceVersion)
      << "IndexFormat::VERSION changed: record the new reference digest";
  EXPECT_EQ(digest, kReferenceDigest) << std::format(
      "The bytes Luxir writes changed (digest {:#x}). Bump IndexFormat::VERSION and HEADER, "
      "then update kReferenceVersion and kReferenceDigest.", digest);
}

TEST_F(IndexFormatTest, otherFormatVersionFailsToLoad) {
  LuxirNode node(LuxirConfig{});
  CollectionHelper ch(node, "main");
  ASSERT_TRUE(ch.index(flatdoc("id", "a"), UpdateMessage::COMMIT).success);
  auto& dir = ch.getIndexWriter()->dir;
  auto manifest = Manifest::load(dir);
  std::pmr::monotonic_buffer_resource arena;
  api::IndexInfo info;
  ASSERT_TRUE(api::decode(info, api::copyToPaddedInput(*manifest.bytes, arena), arena));
  info.version = IndexFormat::VERSION - 1;

  RAMDir old;
  std::vector<std::byte> bytes;
  ASSERT_TRUE(api::encode(info, bytes));
  Manifest::write(old, info.index_gen, bytes);
  EXPECT_THROW(Manifest::load(old), IndexFormatError);
}

TEST_F(IndexFormatTest, malformedSegmentMetadataThrows) {
  LuxirNode node(LuxirConfig{});
  CollectionHelper ch(node, "main");
  buildReferenceIndex(ch);
  auto& dir = ch.getIndexWriter()->dir;
  auto segId = readDurableIndexInfo(dir).info.segments[0].seg_id;

  // File 0 ends with: field metadata, field offsets, field count, segment info, its size.
  auto segInfoOffset = [](const std::string& b) { return b.size() - 4 - (size_t)loadInt(b, b.size() - 4); };
  auto fieldCount = [&](const std::string& b) { return segInfoOffset(b) - 4; };
  auto offsetsTable = [&](const std::string& b) { return fieldCount(b) - 4 * (size_t)loadInt(b, fieldCount(b)); };

  std::vector<std::function<void(std::string&)>> corruptions = {
      [](std::string& b) { storeInt(b, b.size() - 4, INT32_MAX); },
      [](std::string& b) { storeInt(b, b.size() - 4, -1); },
      [&](std::string& b) { std::memset(b.data() + segInfoOffset(b), 0xff, b.size() - 4 - segInfoOffset(b)); },
      [&](std::string& b) { storeInt(b, fieldCount(b), INT32_MAX); },
      [&](std::string& b) { storeInt(b, offsetsTable(b), INT32_MAX); },
      [&](std::string& b) { storeInt(b, offsetsTable(b), 0); },
  };
  for (size_t i = 0; i < corruptions.size(); i++) {
    auto copy = copySegment(dir, corruptions[i]);
    EXPECT_THROW({ PostingsReader reader(*copy, segId); }, std::runtime_error) << "corruption " << i;
  }

  // A field entry whose fields run on: the first value after its name never terminates.
  auto copy = copySegment(dir, [&](std::string& b) {
    size_t entry = offsetsTable(b) - (uint32_t)loadInt(b, offsetsTable(b));
    size_t afterName = entry + 1 + (uint8_t)b[entry];
    std::memset(b.data() + afterName, 0xff, std::min<size_t>(16, offsetsTable(b) - afterName));
  });
  PostingsReader reader(*copy, segId);
  ASSERT_GT(reader.numFields(), 0);
  EXPECT_THROW(reader.fieldInfo(0), std::runtime_error);
}

} // namespace luxir::test
