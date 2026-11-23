// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include "luxir/reader/SegmentCore.h"
#include "luxir/index/SegmentMerger.h"
#include <oneapi/tbb/task_arena.h>
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
#include "luxir/server/HttpServer.h"
#include "luxir/util/Uuid.h"
#include "test/FieldRecordTest.h"
#include "test/HttpReq.h"
#include "test/StorageTestUtils.h"
#include "luxir/store/IndexFormat.h"
#include "luxir/store/CorruptionMarker.h"
#include "luxir/store/FileVerifier.h"
#include "luxir/store/Directory.h"
#include "luxir/store/Manifest.h"
#include "test/CollectionHelper.h"
#include "test/DurableIndexInfo.h"
#include "test/LuxirTest.h"
#include "test/TestUtils.h"
#include "test/RowSpaceFixture.h"
#include "test/SchemaBuilder.h"

namespace luxir::test {

class IndexFormatTest : public LuxirTest {};

namespace {

// The digest of the reference index written by IndexFormat::VERSION. A byte
// change requires reviewing the format version. Writer choices among layouts
// already supported by that version can change the digest within the version.
constexpr uint64_t kReferenceVersion = 18;
constexpr uint64_t kReferenceDigest = 0x8df212bff9a64ad9;

constexpr IdFilter::Options REFERENCE_ID_FILTER = {.kind = IdFilter::Kind::BLOOM};

// A small index over every default field template's kind, with three segments
// and a delete. Each segment is one request flushed by a single inverter.
// Merges are left out: fields merge as parallel tasks, so a merged segment's
// file layout varies.
void buildReferenceIndex(CollectionHelper& ch) {
  SchemaBuilder schema;
  auto& match = schema.field("numeric_match");
  match.type = api::FieldDef::FieldClass::INT;
  match.index = api::FieldDef::IndexMode::MATCH;
  auto& array = schema.field("numeric_array");
  array.type = api::FieldDef::FieldClass::INT;
  array.multi = api::FieldDef::MultiMode::ARRAY;
  auto& decimal = schema.field("decimal_value");
  decimal.type = api::FieldDef::FieldClass::DECIMAL;
  decimal.scale = 2;
  decimal.index = api::FieldDef::IndexMode::RANGE;
  auto& interval = schema.field("interval_value");
  interval.type = api::FieldDef::FieldClass::LONG_RANGE;
  interval.index = api::FieldDef::IndexMode::RANGE;
  schema.set(ch.collection());
  // Writers leave the id filter out by default; the reference covers it.
  ch.getIndexWriter()->idFilterOptions = REFERENCE_ID_FILTER;
  ASSERT_TRUE(ch.indexAll({
      flatdoc("id", "a", "numeric_match", (int64_t)-7, "numeric_array", vec_i(2, 1, 2), "title_t", "the quick brown fox", "tag_s", "red", "tags_ss", vecs("x", "y"),
              "n_i", (int64_t)7, "ns_is", std::vector<int64_t>{1, 2}, "on_b", true,
              "flags_bs", std::vector<bool>{true, false}, "address_ip", "10.0.0.1", "addresses_ips", vecs("10.0.0.2", "2001:db8::1"), "decimal_value", "1.005", "f_f", 1.5f, "d_d", 2.25,
              "dt_dt", "2026-01-02T03:04:05Z", "w_w", "Hello World", "vec_v", std::vector<float>{1, 0},
              "code_sc", "k1", "codes_ssc", vecs("k2", "k3"), "fs_fs", std::vector<double>{0.5, -1.25},
              "ds_ds", std::vector<double>{3.75}, "dts_dts", vecs("2026-01-02T03:04:05Z", "2024-02-29T00:00:00Z"),
              "at_p", std::vector<double>{-74.006, 40.7128},
              "stops_ps", std::vector<std::vector<double>>{{-0.1278, 51.5074}, {139.6503, 35.6762}}),
      flatdoc("id", "b", "title_t", "lazy dogs sleep", "tag_s", "blue", "n_i", (int64_t)-3,
              "vec_v", std::vector<float>{0, 1}, "at_p", std::vector<double>{2.3522, 48.8566}),
  }, UpdateMessage::COMMIT).success);
  CollectionHelper::UpdateBuilder range;
  for (std::string_view json : {
      R"({"id":"range","interval_value":{"gte":-9223372036854775808,"lte":9223372036854775807}})",
      R"({"id":"range_point","interval_value":{"gte":0,"lte":0}})"}) {
    range.addRaw([json](api::Map& doc, std::pmr::memory_resource& mr) {
      ASSERT_TRUE(api::read_json(doc, json, mr));
    });
  }
  range.commit();
  ASSERT_TRUE(ch.submit(range).success);
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
  for (auto& segment : segments) {
    EXPECT_TRUE(segment.overlays.empty());
    EXPECT_FALSE(segment.merge_rows.has_value());
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
  std::vector<uint64_t> fingerprints{referenceDigest(ch.getIndexWriter()->dir)};
  // Cover the child catalog and both optional reverse-map states as well as
  // the ordinary root index, so a child-only format change trips the oracle.
  RAMDir bundles;
  using Fixture = RowSpaceFixture;
  std::vector<Fixture::Row> roots{{17, {"root"}, "root", "root source"}, {}, {19, {"root"}, "root", {}}};
  std::vector<Fixture::Row> children{{11, {"common", "common"}, "a", "child source"}, {}, {11, {"common"}, "a", {}}};
  std::vector<int64_t> ends{2, 2, 3}, owners{0, 0, 2};
  // Keep the format oracle's original physical names independent of the
  // request schema's qualified-name convention.
  std::vector<FileDescriptor> files;
  for (uint64_t id = 1; id <= 3; id++) {
    PostingsWriter writer(bundles, id, 1, (int32_t)roots.size());
    Fixture::fields(writer, SpaceOrdinal::ROOT, "", roots);
    if (id < 3) {
      auto reverse = id == 1 ? std::optional<std::span<const int64_t>>(owners) : std::nullopt;
      auto child = writer.addSpace(Fixture::CHILD);
      writer.finishSpace(child, (int32_t)children.size(), Fixture::shape(writer, ends, reverse));
      Fixture::fields(writer, child, "", children);
    }
    writeUnversionedRuns(writer);
    EXPECT_TRUE(writer.finish(nullptr, &files));
  }
  for (const auto& file : files) {
    fingerprints.push_back(file.size);
    fingerprints.push_back(file.xxh3);
  }
  return XXH3_64bits(fingerprints.data(), fingerprints.size() * sizeof(uint64_t));
}

uint64_t buildMergedReferenceDigest() {
  LuxirNode node(LuxirConfig{});
  CollectionHelper ch(node, "main");
  buildReferenceIndex(ch);
  auto reader = ch.getIndexWriter()->snapshots.readers.getReader();
  std::vector<std::shared_ptr<SegmentCore>> inputs;
  std::vector<LiveDocs*> live;
  for (const auto& segment : reader->segments()) {
    inputs.push_back(segment.segmentCoreShared());
    live.push_back(segment.liveDocs());
  }
  RAMDir output;
  IndexRamBudget budget;
  budget.setTotalBytes(1);
  StoredDictionaryRegistry dictionaries;
  PostingsWriter writer(output, 3, 1);
  writer.idFilterOptions = REFERENCE_ID_FILTER;
  SegmentMerger merger(inputs, live, writer, budget, INT64_MAX, 1, 4, *reader->schema(), dictionaries);
  oneapi::tbb::task_arena serial(1);
  serial.execute([&] { merger.merge(); });
  std::vector<FileDescriptor> files;
  EXPECT_TRUE(writer.finish(nullptr, &files));
  std::vector<uint64_t> fingerprints;
  for (const auto& file : files) {
    fingerprints.push_back(file.size);
    fingerprints.push_back(file.xxh3);
  }
  return XXH3_64bits(fingerprints.data(), fingerprints.size() * sizeof(uint64_t));
}

void writeFile(Directory& dir, std::string_view name, std::string_view bytes) {
  auto file = dir.createFile(name);
  OutputStream out(file.get());
  out.write(bytes.data(), bytes.size());
  out.close();
  dir.finishFile(*file);
}

template <class Action>
void expectCorrupt(Action action, std::string_view context, std::string_view reason) {
  try {
    action();
    ADD_FAILURE() << "Expected corruption: " << reason;
  } catch (const SegmentCorruptError& e) {
    auto message = std::string_view(e.what());
    EXPECT_TRUE(message.contains(context) && message.contains(reason)) << message;
    // A request that meets corruption reports a server fault, whatever its phase.
    EXPECT_EQ(ErrorKind::INTERNAL, classifyException(e, ErrorKind::INVALID_REQUEST).kind);
    EXPECT_EQ("segment_corrupt", e.code);
  }
}

void storeInt(std::string& bytes, size_t at, int32_t value) {
  std::memcpy(bytes.data() + at, &value, sizeof(value));
}

// Rehashes a deliberately changed catalog, so its decoder rather than the hash check sees it.
void rehashCatalog(std::string_view file, api::CatalogRange& range) {
  if (range.offset <= file.size()) range.xxh3 = XXH3_64bits(file.data() + range.offset, file.size() - range.offset);
}

} // namespace

TEST_F(IndexFormatTest, referenceIndexBytesMatchFormatVersion) {
  auto digest = buildReferenceDigest();
  ASSERT_EQ(digest, buildReferenceDigest()) << "the reference index is not deterministic";
  EXPECT_EQ(IndexFormat::VERSION, kReferenceVersion)
      << "IndexFormat::VERSION changed: update kReferenceVersion and kReferenceDigest";
  EXPECT_EQ(digest, kReferenceDigest) << std::format(
      "The bytes Luxir writes changed (digest {:#x}). Review IndexFormat::VERSION "
      "and update the reference digest.", digest);
}

TEST_F(IndexFormatTest, serialRootMergeBytesMatchReference) {
  auto digest = buildMergedReferenceDigest();
  ASSERT_EQ(digest, buildMergedReferenceDigest()) << "the serial merge is not deterministic";
  EXPECT_EQ(0x2ca76afa0391dcf9ULL, digest) << std::format("merged digest {:#x}", digest);
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

TEST_F(IndexFormatTest, catalogSurvivesPublicationDeleteAndMerge) {
  auto path = std::filesystem::temp_directory_path() / ("luxir-catalog-" + newUuid());
  auto cleanup = scope_guard([&] { std::filesystem::remove_all(path); });
  LuxirConfig config;
  config.store.backend = "fs";
  config.store.data_dir = path.string();
  LuxirNode node(config);
  CollectionHelper helper(node, "main");
  auto writer = helper.getIndexWriter();
  writer->mergePolicy->setMergeFactor(100);
  ASSERT_TRUE(helper.indexAll({flatdoc("id", "a", "n_i", 11), flatdoc("id", "b", "n_i", 22)},
                              UpdateMessage::COMMIT).success);
  auto before = writer->snapshots.readers.getReader();
  ASSERT_EQ(1u, before->segments().size());
  auto durable = readDurableIndexInfo(writer->dir);
  auto range = durable->segments[0].catalog;
  EXPECT_EQ(catalogRangeFromManifest(range), before->segments()[0].segmentCore().catalogRange());
  expectValidInventory(writer->dir, *durable);

  FSDirectory disk(incarnationPath(path, "main"));
  IndexReader reopened(disk);
  ASSERT_EQ(1u, reopened.segments().size());
  auto& postings = reopened.segments()[0].postingsReader();
  EXPECT_EQ(catalogRangeFromManifest(range), reopened.segments()[0].segmentCore().catalogRange());
  for (int32_t ordinal = 0; ordinal < postings.numFields(); ++ordinal) {
    auto fieldHeader = postings.fieldHeader(ordinal);
    EXPECT_EQ(fieldHeader, recordHeader(*postings.fieldInfo(ordinal)));
  }

  ASSERT_TRUE(helper.deleteById("a", UpdateMessage::COMMIT).success);
  durable = readDurableIndexInfo(writer->dir);
  ASSERT_EQ(1u, durable->segments.size());
  EXPECT_EQ(range, durable->segments[0].catalog);
  EXPECT_NE(0u, durable->segments[0].live_gen);
  auto after = writer->snapshots.readers.getReader();
  EXPECT_EQ(&before->segments()[0].segmentCore(), &after->segments()[0].segmentCore());

  ASSERT_TRUE(helper.index(flatdoc("id", "c", "n_i", 33), UpdateMessage::COMMIT).success);
  durable = readDurableIndexInfo(writer->dir);
  ASSERT_EQ(2u, durable->segments.size());
  auto snapshot = writer->snapshots.snapshot();
  ASSERT_TRUE(snapshot);
  ASSERT_EQ(2u, snapshot->openCores.size());
  ASSERT_TRUE(snapshot->openCores.back().expired());
  // Merge admission must open this segment while holding the writer mutex.
  auto published = writer->snapshots.readers.indexReader.load();
  ASSERT_TRUE(published);
  ASSERT_EQ(nullptr, published->findSegment(durable->segments.back().seg_id));
  writer->mergeSegments();
  durable = readDurableIndexInfo(writer->dir);
  ASSERT_EQ(1u, durable->segments.size());
  expectValidInventory(writer->dir, *durable);
  IndexReader merged(disk);
  auto& output = merged.segments()[0];
  EXPECT_EQ(2, output.maxDoc());
  EXPECT_EQ(catalogRangeFromManifest(durable->segments[0].catalog), output.segmentCore().catalogRange());
  const auto* number = output.postingsReader().fieldInfo("n_i");
  ASSERT_NE(nullptr, number);
  EXPECT_EQ(22, number->scalarBounds()->min);
}

TEST_F(IndexFormatTest, malformedSegmentMetadataThrows) {
  RAMDir original;
  PostingsWriter writer(original, 1, 1, 1);
  for (std::string_view name : {"aa", "bbb"}) {
    auto& field = writer.addField(SpaceOrdinal::ROOT, name);
    field.header.type = FieldType::INT;
    field.setPresence({1, {}});
  }
  for (auto path : {"a", "a.b"}) {
    auto child = writer.addSpace(path);
    std::array<int64_t, 1> ends{1};
    writer.finishSpace(child, 1, RowSpaceFixture::shape(writer, ends));
  }
  writeUnversionedRuns(writer);
  std::vector<FileDescriptor> files;
  ASSERT_TRUE(writer.finish(nullptr, &files));
  ASSERT_EQ(1u, files.size());
  const auto& file = files.front().name;
  api::SegmentInfo segment;
  segment.seg_id = 1;
  segment.creator_epoch = 1;
  segment.max_doc = 1;
  segment.catalog = catalogRangeToManifest(writer.catalogRange());
  std::pmr::monotonic_buffer_resource arena;
  auto wireFiles = toWire(files, arena);
  segment.files = wireFiles;
  auto originalBytes = original.openFile(file);
  auto hold = originalBytes->acquire({0, originalBytes->size()});
  std::string_view bytes(hold.bytes());
  auto footer = bytes.size() - sizeof(uint64_t);
  auto length = loadUnaligned<uint64_t>(bytes.data() + footer);
  InputStream in(bytes.data() + footer - length, bytes.data() + footer);
  auto pos = [&] { return (size_t)(in.ptr() - bytes.data()); };
  ASSERT_EQ(1u, in.checkedVint());
  auto directoryStart = pos();
  in.checkedVlong();
  auto fileCount = pos();
  ASSERT_EQ(1u, in.checkedVint());
  ASSERT_EQ(0u, in.checkedVint());
  for (int i = 0; i < 4; ++i) in.checkedVlong();
  auto versionCount = pos();
  ASSERT_EQ(1u, in.checkedVint());
  ASSERT_EQ(3u, in.checkedVint());
  ASSERT_EQ(0u, in.checkedVint());
  ASSERT_EQ(1u, in.checkedVint());
  ASSERT_EQ(0u, in.checkedVlong());
  auto table = segment.catalog.offset + in.checkedVlong();
  ASSERT_EQ(2u, in.checkedVint());
  ASSERT_EQ(1u, in.checkedVint());
  auto childPath = pos();
  in.skip(1);
  for (int i = 0; i < 6; ++i) in.checkedVlong();
  auto ownershipBytes = pos();
  in.checkedVlong();
  ASSERT_EQ(0u, in.checkedVint());
  ASSERT_EQ(3u, in.checkedVint());
  auto grandchildPath = pos();

  auto firstName = segment.catalog.offset + 1;
  auto secondName = firstName + loadUnaligned<uint32_t>(bytes.data() + table + sizeof(uint32_t));
  auto recordEnd = segment.catalog.offset + loadUnaligned<uint32_t>(bytes.data() + table + 2 * sizeof(uint32_t));
  ASSERT_EQ(recordEnd, table);
  ASSERT_NE(0u, table % alignof(FieldOffset));
  std::array<api::FileDescriptor, 2> extraFiles{wireFiles.front(), wireFiles.front()};
  auto unknownFile = Postings::getIndexFileName(Postings::segmentKey(1, 1), 1);
  extraFiles[1].name = unknownFile;
  struct Case {
    std::string_view reason;
    std::function<void(std::string&, api::SegmentInfo&)> corrupt;
    bool namesFile = true;
  };
  const Case cases[] = {
      {"directory length", [&](auto& b, auto&) { std::memset(b.data() + footer, 0xff, sizeof(uint64_t)); }},
      {"directory start", [&](auto& b, auto&) { b[directoryStart] ^= 1; }},
      {"invalid vint", [&](auto& b, auto&) { b[fileCount] = (char)VARINT_CONTINUATION; }},
      {"inventory differs from segment files", [&](auto&, auto& s) { s.files = extraFiles; }},
      {"version runs layout", [&](auto& b, auto&) { b[versionCount] = 0; }},
      {"child row space", [&](auto& b, auto&) { b[childPath] = 'z'; }},
      {"missing row space parent", [&](auto& b, auto&) { b[grandchildPath] = 'b'; }},
      {"ownership index layout", [&](auto& b, auto&) { b[ownershipBytes] = 0; }},
      {"record area length", [&](auto& b, auto&) { storeInt(b, table + 2 * sizeof(uint32_t), 0); }},
      {"field area endpoints", [&](auto& b, auto&) { storeInt(b, table, 1); }},
      {"field offset order", [&](auto& b, auto&) { storeInt(b, table + sizeof(uint32_t), INT32_MAX); }},
      {"field name order", [&](auto& b, auto&) { b[firstName] = 'z'; }},
      {"field name order", [&](auto& b, auto&) { b.replace(secondName, 3, "000"); }},
      {"trailing catalog bytes", [&](auto& b, auto& s) {
        b.insert(footer, 1, 0);
        auto longer = length + 1;
        std::memcpy(b.data() + footer + 1, &longer, sizeof(longer));
        ++extraFiles[0].size;
        ++s.catalog.length;
        s.files = std::span(extraFiles).first(1);
      }},
      {"row count differs from manifest", [](auto&, auto& s) { ++s.max_doc; }, false},
      {"catalog range must reach the end of file 0", [](auto&, auto& s) { --s.catalog.length; }},
  };
  ASSERT_NO_THROW(SegmentCore::create(original, segment));
  auto segmentName = Postings::segmentKey(segment.seg_id, segment.creator_epoch);
  for (size_t i = 0; const auto& [reason, corrupt, namesFile] : cases) {
    SCOPED_TRACE(i++);
    RAMDir copy;
    std::string changed(bytes);
    auto manifest = segment;
    corrupt(changed, manifest);
    rehashCatalog(changed, manifest.catalog);
    writeFile(copy, file, changed);
    // An open reads every file the manifest lists, so the one the catalog
    // does not know exists, and the inventory check is what rejects it.
    writeFile(copy, unknownFile, bytes);
    expectCorrupt([&] { SegmentCore::create(copy, manifest); }, namesFile ? file : segmentName, reason);
  }
}

TEST_F(IndexFormatTest, invalidSpacePathAndRootCount) {
  RAMDir dir;
  {
    PostingsWriter paths(dir, 2, 1, 0);
    paths.addSpace("a");
    EXPECT_THROW(paths.addSpace("a."), std::invalid_argument);
  }
  PostingsWriter writer(dir, 1, 1, 1);
  EXPECT_THROW(writer.setMaxDoc(HARD_MAX_ROW_COUNT), std::invalid_argument);
  writeUnversionedRuns(writer);
  ASSERT_TRUE(writer.finish());

  auto file = dir.openFile(Postings::getIndexFileName(Postings::segmentKey(1, 1), Postings::PRIMARY_FILE));
  auto hold = file->acquire({0, file->size()});
  std::string_view original(hold.bytes());
  auto footer = original.size() - sizeof(uint64_t);
  auto length = loadUnaligned<uint64_t>(original.data() + footer);
  auto directoryStart = footer - length;
  ASSERT_EQ(1, original[directoryStart]);
  char count[MAX_VINT_SIZE];
  auto countBytes = writeVInt(count, HARD_MAX_ROW_COUNT) - count;
  std::string corrupt(original);
  corrupt.replace(directoryStart, 1, count, countBytes);
  length += countBytes - 1;
  std::memcpy(corrupt.data() + corrupt.size() - sizeof(length), &length, sizeof(length));
  auto range = writer.catalogRange();
  range.length = corrupt.size() - range.offset;
  range.xxh3 = XXH3_64bits(corrupt.data() + range.offset, range.length);
  expectCorrupt([&] { SegmentCatalog::readDirectory(corrupt); }, "segment catalog", "root doc count out of range");
  expectCorrupt([&] {
    SegmentCatalog::readDirectory(std::span(corrupt).subspan(range.offset), range, corrupt.size());
  }, "segment catalog", "root doc count out of range");
}

TEST_F(IndexFormatTest, opensCatalogFromCopiedRange) {
  LuxirNode node(LuxirConfig{});
  CollectionHelper ch(node, "main");
  ASSERT_TRUE(ch.index(flatdoc("id", "a", "tag_s", "red"), UpdateMessage::COMMIT).success);
  auto& dir = ch.getIndexWriter()->dir;
  auto durable = readDurableIndexInfo(dir);
  const auto& segment = durable.info.segments.front();
  auto range = catalogRangeFromManifest(segment.catalog);
  std::vector<FileDescriptor> files;
  for (const auto& file : segment.files) files.push_back({std::string(file.name), file.size});
  auto source = SegmentCore::source(segment.seg_id, segment.creator_epoch, range,
                                     segment.max_doc, files);
  auto name = Postings::getIndexFileName(Postings::segmentKey(segment.seg_id, segment.creator_epoch), 0);
  ASSERT_GT(range.offset, 0u);
  std::string bytes;
  {
    auto file = dir.openFile(name);
    auto hold = file->acquire({range.offset, range.length});
    bytes.assign(hold.getData(), hold.getLength());
  }

  auto head = SegmentCatalog::readDirectory(bytes, range, source.files[0]);
  auto catalog = SegmentCatalog::decode(head, source.files);
  auto ordinal = catalog.rowSpaces()[0].fields.find("tag_s");
  ASSERT_GE(ordinal, 0);
  auto header = catalog.fieldHeader(SpaceOrdinal::ROOT, ordinal);
  EXPECT_EQ("tag_s", std::string_view(header.name));
  EXPECT_EQ(1, header.docsWithField);
  auto record = catalog.field(SpaceOrdinal::ROOT, ordinal);
  ASSERT_TRUE(record->terms());
  EXPECT_EQ(1, record->indexedTotals().numTerms);
  EXPECT_THROW(SegmentCatalog::readDirectory(bytes, range, source.files[0] + 1), SegmentCorruptError);
}

// Any changed catalog byte fails the manifest hash before decoding, whether the
// catalog is read in place or as copied range bytes.
TEST_F(IndexFormatTest, catalogHashMismatchIsCorruption) {
  LuxirNode node(LuxirConfig{});
  CollectionHelper ch(node, "main");
  ASSERT_TRUE(ch.index(flatdoc("id", "a", "tag_s", "red"), UpdateMessage::COMMIT).success);
  auto& dir = ch.getIndexWriter()->dir;
  auto durable = readDurableIndexInfo(dir);
  const auto& segment = durable.info.segments.front();
  auto range = catalogRangeFromManifest(segment.catalog);
  auto name = Postings::getIndexFileName(Postings::segmentKey(segment.seg_id, segment.creator_epoch), 0);
  auto readWhole = [&](const std::string& file) {
    auto input = dir.openFile(file);
    auto hold = input->acquire({0, input->size()});
    return std::string(hold.getData(), hold.getLength());
  };
  std::string bytes = readWhole(name);
  bytes[range.offset + range.length / 2] ^= 1;
  RAMDir copy;
  writeFile(copy, name, bytes);
  for (const auto& file : segment.files) {
    if (file.name != name) writeFile(copy, file.name, readWhole(std::string(file.name)));
  }
  expectCorrupt([&] { SegmentCore::create(copy, segment); }, name, "catalog hash differs from manifest");
  auto catalog = std::string_view(bytes).substr(range.offset);
  expectCorrupt([&] { SegmentCatalog::readDirectory(catalog, range, bytes.size()); },
                "segment catalog", "catalog hash differs from manifest");
}

TEST_F(IndexFormatTest, manifestRequiresCatalogRange) {
  LuxirNode node(LuxirConfig{});
  CollectionHelper ch(node, "main");
  ASSERT_TRUE(ch.index(flatdoc("id", "a"), UpdateMessage::COMMIT).success);
  auto manifest = Manifest::load(ch.getIndexWriter()->dir);
  std::pmr::monotonic_buffer_resource arena;
  auto info = Manifest::decode(manifest.bytes, arena);
  std::vector<api::SegmentInfo> segments(info.segments.begin(), info.segments.end());
  segments[0].catalog = {};
  info.segments = segments;
  auto bytes = std::make_shared<std::vector<std::byte>>();
  ASSERT_TRUE(api::encode(info, *bytes));
  EXPECT_THROW(Manifest::decode(bytes, arena), std::runtime_error);
}

TEST_F(IndexFormatTest, corruptFieldPrefixRespectsVerificationPolicy) {
  LuxirNode node(LuxirConfig{});
  CollectionHelper ch(node, "main");
  buildReferenceIndex(ch);
  auto& dir = ch.getIndexWriter()->dir;
  auto durable = readDurableIndexInfo(dir);
  const auto& segment = durable.info.segments[0];
  auto file = Postings::getIndexFileName(Postings::segmentKey(segment.seg_id, segment.creator_epoch), 0);
  auto entry = segment.catalog.offset;
  std::string field;
  RAMDir copy;
  std::vector<Directory::FileInfo> files;
  dir.listFiles(files);
  for (const auto& descriptor : files) {
    auto input = dir.openFile(descriptor.name);
    auto hold = input->acquire({0, input->size()});
    std::string bytes(hold.getData(), hold.getLength());
    if (descriptor.name == file) {
      field = bytes.substr(entry + 1, (uint8_t)bytes[entry]);
      bytes[entry + 1 + field.size()] = 0x7f; // invalid field type, after the still-readable name
    }
    writeFile(copy, descriptor.name, bytes);
  }
  ASSERT_FALSE(field.empty());
  auto fieldContext = std::format("field '{}'", field);
  {
    CommitSnapshotRegistry snapshot(copy);
    EXPECT_THROW(snapshot.openLocalSnapshot(true), ChecksumMismatchError);
    // Without file verification the manifest's catalog hash still rejects the record.
    expectCorrupt([&] { snapshot.openLocalSnapshot(false); }, file, "catalog hash differs from manifest");
    CorruptionMarker::write(copy, std::span(&file, 1), "bad field prefix");
    EXPECT_THROW(snapshot.openLocalSnapshot(false), ChecksumMismatchError);
  }
  // A catalog found from the trailer has no hash; the record fails when it decodes.
  SegmentCore core(copy, segment.seg_id, segment.creator_epoch);
  EXPECT_FALSE(core.catalogRange().xxh3.has_value());
  EXPECT_THROW(catalogRangeToManifest(core.catalogRange()), std::logic_error);
  expectCorrupt([&] { core.rootReader().fieldInfo(0); }, file, fieldContext);
}

} // namespace luxir::test
