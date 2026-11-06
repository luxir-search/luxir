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

#include "luxir/index/SegmentSummary.h"
#include "luxir/reader/PostingsReader.h"
#include "luxir/server/LuxirNode.h"
#include "luxir/server/HttpServer.h"
#include "luxir/util/Uuid.h"
#include "test/HttpReq.h"
#include "test/StorageTestUtils.h"
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
// IndexFormat::VERSION, then update kReferenceVersion and kReferenceDigest. Changing
// only the digest leaves old collections loading under the new reader.
constexpr uint64_t kReferenceVersion = 10;
constexpr uint64_t kReferenceDigest = 0x62b02a328ada5f26;

// A small index over every default field template's kind, with two segments
// and a delete. Each segment is one request flushed by a single inverter.
// Merges are left out: fields merge as parallel tasks, so a merged segment's
// file layout varies.
void buildReferenceIndex(CollectionHelper& ch) {
  ASSERT_TRUE(ch.indexAll({
      flatdoc("id", "a", "title_t", "the quick brown fox", "tag_s", "red", "tags_ss", vecs("x", "y"),
              "n_i", (int64_t)7, "ns_is", std::vector<int64_t>{1, 2}, "f_f", 1.5f, "d_d", 2.25,
              "dt_dt", "2026-01-02T03:04:05Z", "w_w", "Hello World", "vec_v", std::vector<float>{1, 0},
              "code_sc", "k1", "codes_ssc", vecs("k2", "k3"), "fs_fs", std::vector<double>{0.5, -1.25},
              "ds_ds", std::vector<double>{3.75}, "dts_dts", vecs("2026-01-02T03:04:05Z", "2024-02-29T00:00:00Z"),
              "at_p", std::vector<double>{-74.006, 40.7128},
              "stops_ps", std::vector<std::vector<double>>{{-0.1278, 51.5074}, {139.6503, 35.6762}}),
      flatdoc("id", "b", "title_t", "lazy dogs sleep", "tag_s", "blue", "n_i", (int64_t)-3,
              "vec_v", std::vector<float>{0, 1}, "at_p", std::vector<double>{2.3522, 48.8566}),
  }, UpdateMessage::COMMIT).success);
  ASSERT_TRUE(ch.indexAll({
      flatdoc("id", "c", "title_t", "quick quick fox", "tag_s", "red", "n_i", (int64_t)42,
              "dt_dt", "2025-12-31T00:00:00Z", "vec_v", std::vector<float>{0.5f, 0.5f},
              "at_p", std::vector<double>{151.2093, -33.8688},
              "stops_ps", std::vector<std::vector<double>>{{-58.3816, -34.6037}}),
  }, UpdateMessage::COMMIT).success);
  ASSERT_TRUE(ch.deleteById("b", UpdateMessage::COMMIT).success);
  // The reference vector column stores raw floats, with no normalization or ANN.
  auto* vector = dynamic_cast<const VectorFieldType*>(ch.collection().getSchema()->getFieldTypePtr("vec_v"));
  ASSERT_NE(nullptr, vector);
  ASSERT_EQ(VectorFieldType::METRIC_NONE, vector->metric_);
}

// The manifest re-encoded without the fields that differ from run to run, so
// file names, sizes and content hashes stand in for the data files.
uint64_t referenceDigest(Directory& dir) {
  auto durable = readDurableIndexInfo(dir);
  auto info = durable.info;
  EXPECT_TRUE(info.aux_indexes.empty());
  std::vector<api::SegmentInfo> segments(info.segments.begin(), info.segments.end());
  std::vector<std::vector<api::FileDescriptor>> files;
  std::vector<std::vector<api::AuxIndexInfo>> overlays;
  for (auto& segment : segments) {
    // Only the segment summary, whose entry carries a commit time.
    auto& summaries = overlays.emplace_back(segment.overlays.begin(), segment.overlays.end());
    for (auto& overlay : summaries) {
      EXPECT_EQ(SegmentSummary::OVERLAY_KIND, overlay.kind);
      overlay.commit_time = 0;
    }
    segment.overlays = summaries;
    segment.commit_time = 0;
    segment.schema = {};
    auto& kept = files.emplace_back();
    for (const auto& file : segment.files) if (!SchemaBinding::isFile(file.name)) kept.push_back(file);
    segment.files = kept;
  }
  info.segments = segments;
  info.commit_time = 0;
  info.incarnation = "";
  info.schema.reset(); // the default schema can change without the format changing
  info.schema_file = {};
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
    if (name == Postings::getIndexFileName(Postings::segmentKey(durable.info.segments[0].seg_id, durable.info.segments[0].creator_epoch), 0)) {
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
      << "IndexFormat::VERSION changed: update kReferenceVersion and kReferenceDigest";
  EXPECT_EQ(digest, kReferenceDigest) << std::format(
      "The bytes Luxir writes changed (digest {:#x}). Bump IndexFormat::VERSION, "
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

  for (bool missingSchema : {false, true}) {
    auto changed = info;
    if (missingSchema) changed.schema_file = {};
    RAMDir old;
    reserveIncarnation(old);
    std::vector<std::byte> bytes;
    ASSERT_TRUE(api::encode(changed, bytes));
    Manifest::write(old, info.index_gen, bytes, info.writer_epoch);
    IncarnationState::install(old, Manifest::tip(info.index_gen, info.writer_epoch));
    EXPECT_THROW(Manifest::load(old), IndexFormatError);
    EXPECT_THROW(Manifest::files(std::make_shared<const std::vector<std::byte>>(bytes)), IndexFormatError);
  }
}

TEST_F(IndexFormatTest, incompatibleCollectionFailsAloneAfterRestart) {
  auto path = std::filesystem::temp_directory_path() / ("luxir-format-" + newUuid());
  auto cleanup = scope_guard([&] { std::filesystem::remove_all(path); });
  LuxirConfig config;
  config.store.backend = "fs";
  config.store.data_dir = path.string();
  {
    LuxirNode node(config);
    for (auto name : {"bad", "good"}) {
      CollectionHelper ch(node, name);
      ASSERT_TRUE(ch.index(flatdoc("id", name), UpdateMessage::COMMIT).success);
    }
  }
  FSDirectory dir(incarnationPath(path, "bad"));
  auto durable = readDurableIndexInfo(dir);
  auto info = durable.info;
  info.version = IndexFormat::VERSION + 1;
  std::vector<std::byte> bytes;
  ASSERT_TRUE(api::encode(info, bytes));
  ASSERT_TRUE(dir.publishFile(Manifest::name(info.index_gen, info.writer_epoch), Manifest::withFooter(bytes), Directory::PublishMode::REPLACE));
  LuxirNode node(config);
  auto entries = node.collectionEntries();
  auto failed = std::ranges::find_if(entries, [](const auto& entry) { return entry.id.name == "bad"; });
  ASSERT_NE(entries.end(), failed);
  EXPECT_EQ("failed to load: " + std::string(IndexFormatError(info.version).what()), failed->error);
  EXPECT_THROW(node.getCollection("bad"), CollectionUnavailableError);
  for (const auto& file : filesOf(info)) EXPECT_TRUE(dir.openFile(file.name));
  HttpServer server(node, 1, 0);
  server.start();
  auto result = httpRequest(server.getPort(), http::verb::post, "/collections/good/_search", R"({"query":"id:good","fields":["id"]})");
  EXPECT_EQ(200, result.result_int()) << result.body();
  EXPECT_NE(std::string::npos, result.body().find(R"("good")"));
}

TEST_F(IndexFormatTest, malformedSegmentMetadataThrows) {
  LuxirNode node(LuxirConfig{});
  CollectionHelper ch(node, "main");
  buildReferenceIndex(ch);
  auto& dir = ch.getIndexWriter()->dir;
  auto durable = readDurableIndexInfo(dir);
  auto segId = durable.info.segments[0].seg_id;
  auto creatorEpoch = durable.info.segments[0].creator_epoch;

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
    EXPECT_THROW({ PostingsReader reader(*copy, segId, creatorEpoch); }, std::runtime_error) << "corruption " << i;
  }

  // A field entry whose fields run on: the first value after its name never terminates.
  auto copy = copySegment(dir, [&](std::string& b) {
    size_t entry = offsetsTable(b) - (uint32_t)loadInt(b, offsetsTable(b));
    size_t afterName = entry + 1 + (uint8_t)b[entry];
    std::memset(b.data() + afterName, 0xff, std::min<size_t>(16, offsetsTable(b) - afterName));
  });
  PostingsReader reader(*copy, segId, creatorEpoch);
  ASSERT_GT(reader.numFields(), 0);
  EXPECT_THROW(reader.fieldInfo(0), std::runtime_error);
}

} // namespace luxir::test
