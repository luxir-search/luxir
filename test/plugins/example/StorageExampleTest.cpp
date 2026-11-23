// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include <gtest/gtest.h>
#include <filesystem>
#include <unistd.h>

#include "CountingStorage.h"
#include "luxir/plugin/Registries.h"
#include "luxir/reader/OwnershipShape.h"
#include "luxir/store/Manifest.h"
#include "luxir/store/ReadOnlyDirectory.h"
#include "luxir/store/CorruptionMarker.h"
#include "luxir/util/luxir_util.h"
#include "luxir/store/CheckedDirFactory.h"
#include "test/CollectionHelper.h"
#include "test/DurableIndexInfo.h"
#include "test/LocalReq.h"
#include "test/LuxirTest.h"
#include "test/StrictStorage.h"
#include "test/TestUtils.h"

using namespace luxir;
using namespace luxir::test;
using luxir::example::CountingLayer;
using luxir::example::CountingRamBackend;

class StorageExampleTest : public LuxirTest {};

TEST_F(StorageExampleTest, nodeRunsOnPluginBackendAndLayer) {
  LuxirConfig config;
  config.store.backend = "counting_ram";
  config.store.layers = {"counting"};
  config.store.checked_dir.sync = "throw";
  LuxirNode node(config);
  CollectionHelper h(node, "main");
  ASSERT_TRUE(h.indexAll({flatdoc("id", "a"), flatdoc("id", "b")}, UpdateMessage::COMMIT).success);
  auto incarnation = node.collections().catalog().current("main");
  ASSERT_TRUE(incarnation);
  auto label = "t/t0/i/" + *incarnation;
  auto* backend = node.collections().storage().find<CountingRamBackend>();
  auto* layer = node.collections().storage().find<CountingLayer>();
  ASSERT_NE(nullptr, backend);
  ASSERT_NE(nullptr, layer);
  EXPECT_GT(backend->counters->count("open", label), 0u);
  EXPECT_GT(layer->counters->count("create", label), 0u);
  EXPECT_GT(layer->counters->count("open"), 0u);
  EXPECT_NE(node.collections().storage().describe().find("counting(counting_ram)"), std::string::npos);
}

TEST_F(StorageExampleTest, nodeIndexesMergesAndSearchesOnStrictRam) {
  LuxirConfig config;
  config.store.backend = "ram";
  config.store.layers = {"strict"};
  LuxirNode node(config);
  CollectionHelper h(node, "main");
  auto writer = h.getIndexWriter();
  ASSERT_TRUE(h.index(flatdoc("id", "a", "cat_s", "red"), UpdateMessage::COMMIT).success);
  ASSERT_TRUE(h.index(flatdoc("id", "b", "cat_s", "red"), UpdateMessage::COMMIT).success);
  ASSERT_TRUE(h.index(flatdoc("id", "c", "cat_s", "blue"), UpdateMessage::COMMIT).success);
  ASSERT_GT(writer->snapshots.readers.getReader()->segments().size(), 1u);
  CollectionHelper::UpdateBuilder merge;
  ASSERT_TRUE(h.submit(merge.commit(true, 1)).success);
  EXPECT_EQ(1u, writer->snapshots.readers.getReader()->segments().size());

  auto req = localReq(h.getSearchEngine());
  req->collection("main").topDocs().matchQuery("cat_s", "red").countLimit(-1);
  req->execute();
  ASSERT_TRUE(req->ok()) << req->errorMsg();
  EXPECT_EQ(2, req->getMatchCount());

  auto* layer = node.collections().storage().find<StrictLayer>();
  ASSERT_NE(nullptr, layer);
  auto snapshot = writer->snapshots.snapshot();
  EXPECT_FALSE(std::ranges::any_of(snapshot->files, [&](const auto& file) {
    return Manifest::segmentDataFile(file.name) && layer->counters->fileStats(file.name).wholeFileAccesses > 0;
  }));
}

TEST_F(StorageExampleTest, mergedReaderAcquiresOnlyOpenedParts) {
  LuxirConfig config;
  LuxirNode node(config);
  CollectionHelper h(node, "main");
  auto schema = Schema::createDefaultSchema();
  schema->fieldTypeMap["freqs"] = std::make_shared<TextFieldType>("freqs", FieldType::INDEX_DOCS_FREQS, "whitespace");
  schema->fieldTypeMap["blob"] = std::make_shared<StrFieldType>("blob", FieldType::COLUMN_STORED);
  h.collection().setSchema(schema);
  auto writer = h.getIndexWriter();
  std::string blob(PostingsWriter::RAM_SPILL_BYTES + 1, 'x');
  for (int i = 0; i < 2; ++i) {
    ASSERT_TRUE(h.indexAll({flatdoc("id", std::to_string(2 * i), "freqs", "needle needle", "body_w", "needle needle",
                                    "blob", blob),
                            flatdoc("id", std::to_string(2 * i + 1), "freqs", "needle", "body_w", "needle")},
                           UpdateMessage::COMMIT).success);
  }
  ASSERT_EQ(2u, h.durableSegmentCount());
  CollectionHelper::UpdateBuilder merge;
  ASSERT_TRUE(h.submit(merge.commit(true, 1)).success);
  auto info = readDurableIndexInfo(writer->dir);
  ASSERT_EQ(1u, info->segments.size());
  const auto& segment = info->segments.front();
  ASSERT_GT(segment.files.size(), 1u);

  auto snapshot = *writer->snapshots.snapshot();
  snapshot.openCores.clear();
  auto segmentName = Postings::segmentKey(segment.seg_id, segment.creator_epoch);
  auto range = catalogRangeFromManifest(segment.catalog);
  auto source = SegmentCore::source(segment.seg_id, segment.creator_epoch, range, segment.max_doc, snapshot.files);
  auto primary = writer->dir.openFile(Postings::getIndexFileName(segmentName, 0));
  auto catalogBytes = primary->acquire({range.offset, range.length});
  auto catalog = SegmentCatalog::decode(
      SegmentCatalog::readDirectory(catalogBytes.bytes(), range.offset, primary->size()), source.files, {});

  auto counters = std::make_shared<StrictStorageCounters>();
  StrictDirectory dir(std::shared_ptr<Directory>(writer, &writer->dir), counters, StrictWholeFiles::DENY);
  auto reader = std::make_shared<IndexReader>(dir, snapshot);
  auto& postings = reader->segments().front().postingsReader();
  std::map<uint32_t, std::set<std::pair<uint64_t, uint64_t>>> expected, metadata;
  auto expectPart = [&](field_parts::Slice slice, ReadClass readClass = ReadClass::DATA) {
    ASSERT_TRUE(slice);
    expected[slice.location.filenum()].emplace(slice.location.offset(), slice.bytes);
    if (readClass == ReadClass::METADATA) metadata[slice.location.filenum()].emplace(slice.location.offset(), slice.bytes);
  };
  auto check = [&] {
    postings.core().forEachBaseFile([&](uint32_t number, uint64_t) {
      auto name = Postings::getIndexFileName(segmentName, number);
      auto stats = counters->fileStats(name);
      EXPECT_EQ(0u, stats.wholeFileAccesses) << name;
      EXPECT_EQ(0u, stats.sizeRequests) << name;  // the manifest records every size
      EXPECT_EQ(expected[number].size(), stats.acquires) << name;
      EXPECT_EQ(expected[number], stats.parts) << name;
      EXPECT_EQ(metadata[number], stats.metadataParts) << name;
    });
  };
  postings.core().forEachBaseFile([&](uint32_t number, uint64_t) {
    expected[number].emplace(0, Postings::LUXIR_HEADER.size());
    metadata[number].emplace(0, Postings::LUXIR_HEADER.size());
  });
  expectPart({{0, range.offset}, range.length}, ReadClass::METADATA);
  expectPart(catalog.versionRuns().slice, ReadClass::METADATA);
  check();

  for (auto field : {"freqs", "body_w"}) {
    const auto* record = postings.fieldInfo(field);
    ASSERT_TRUE(record && record->terms());
    const auto& terms = *record->terms();
    ASSERT_EQ(1u, terms.runs.size());
    auto offsets = record->termBlockOffsets();
    expectPart(terms.dictionaryBlocks());
    expectPart({offsets.location, record->termTrie().end().offset() - offsets.location.offset()}, ReadClass::METADATA);
    expectPart(terms.runs.front().docs);
    expectPart(record->normsValues(postings.maxDoc()));
    if (FieldType::hasPositions(record->flags)) expectPart(terms.runs.front().positions);
    for (int i = 0; i < 2; ++i) {
      auto req = localReq(h.getSearchEngine());
      req->topDocs().matchQuery(field, "needle").fields({"_score_"}).withStats();
      req->execute(reader);
      ASSERT_TRUE(req->ok()) << req->errorMsg();
      EXPECT_EQ(4, req->getMatchCount());
      check();
    }
  }
}

// A manifest open issues one batch per segment's file 0, holding its header and
// catalog, and issues all of them before it reads anything else.
TEST_F(StorageExampleTest, manifestOpenAcquiresCatalogsInOneRound) {
  LuxirConfig config;
  LuxirNode node(config);
  CollectionHelper h(node, "main");
  auto writer = h.getIndexWriter();
  for (int i = 0; i < 3; ++i) {
    ASSERT_TRUE(h.indexAll({flatdoc("id", std::to_string(i), "body_w", "needle")}, UpdateMessage::COMMIT).success);
  }
  auto info = readDurableIndexInfo(writer->dir);
  ASSERT_EQ(3u, info->segments.size());
  auto snapshot = *writer->snapshots.snapshot();
  snapshot.openCores.clear();

  auto counters = std::make_shared<StrictStorageCounters>();
  StrictDirectory dir(std::shared_ptr<Directory>(writer, &writer->dir), counters, StrictWholeFiles::DENY);
  auto reader = std::make_shared<IndexReader>(dir, snapshot);
  ASSERT_EQ(3u, reader->segments().size());
  std::vector<uint64_t> firstRounds;
  for (const auto& segment : info->segments) {
    auto name = Postings::getIndexFileName(Postings::segmentKey(segment.seg_id, segment.creator_epoch), 0);
    auto range = catalogRangeFromManifest(segment.catalog);
    auto stats = counters->fileStats(name);
    EXPECT_EQ(1u, stats.batches) << name;
    EXPECT_EQ(2u, stats.rounds) << name;  // the catalog batch, then the version runs
    EXPECT_TRUE(stats.metadataParts.contains({0, Postings::LUXIR_HEADER.size()})) << name;
    EXPECT_TRUE(stats.metadataParts.contains({range.offset, range.length})) << name;
    firstRounds.push_back(stats.firstRound);
  }
  std::ranges::sort(firstRounds);
  EXPECT_EQ((std::vector<uint64_t>{1, 2, 3}), firstRounds);
  EXPECT_EQ(6u, counters->totalRounds());

  // A later batch acquires only the parts the segment does not hold yet,
  // and slice() of them then makes no request.
  auto& core = reader->segments().front().postingsReader().core();
  auto name = Postings::getIndexFileName(core.segmentName(), 0);
  auto before = counters->fileStats(name);
  auto catalog = core.catalogRange();
  field_parts::Slice parts[] = {{{0, 0}, 2}, {{0, 2}, 3}, {{0, 0}, 2}, {{0, catalog.offset}, catalog.length}};
  core.acquireSlices(parts, ReadClass::METADATA);
  auto after = counters->fileStats(name);
  EXPECT_EQ(before.batches + 1, after.batches);
  EXPECT_EQ(before.acquires + 2, after.acquires);
  auto bytes = core.slice(parts[1], ReadClass::METADATA);
  EXPECT_EQ(Postings::LUXIR_HEADER.substr(2, 3), std::string_view(bytes.data(), bytes.size()));
  EXPECT_EQ(core.slice(parts[0], ReadClass::METADATA).data(), core.slice(parts[2], ReadClass::METADATA).data());
  EXPECT_EQ(after.acquires, counters->fileStats(name).acquires);
}

// The canonical query shapes read parts, never whole segment files, need no
// size requests, and acquire the catalog, points directories and numeric
// column indexes as metadata.
TEST_F(StorageExampleTest, canonicalQueriesRunWithWholeSegmentFilesDenied) {
  LuxirConfig config;
  config.store.layers = {"strict"};
  config.store.options = {{"strict", {{"whole_segment_files", "deny"}}}};
  LuxirNode node(config);
  CollectionHelper h(node, "main");
  for (int batch = 0; batch < 2; batch++) {
    std::vector<Doc> docs;
    for (int i = 0; i < 100; i++) {
      int n = batch * 100 + i;
      docs.push_back(flatdoc("id", "d" + std::to_string(n), "body_t", "alpha beta w" + std::to_string(n % 13),
                             "c_s", "c" + std::to_string(n % 5), "n_i", (int64_t)(n % 31), "t_dt", (int64_t)n * 1000));
    }
    ASSERT_TRUE(h.indexAll(docs, UpdateMessage::COMMIT).success);
  }
  auto run = [&](auto&& build, int64_t expected) {
    auto req = localReq(h.getSearchEngine());
    build(req->collection("main"));
    req->execute();
    ASSERT_TRUE(req->ok()) << req->errorMsg();
    if (expected >= 0) {
      EXPECT_EQ(expected, req->getMatchCount());
    }
  };
  run([](auto& r) { r.topDocs().matchQuery("c_s", "c1").countLimit(); }, 40);
  run([](auto& r) { r.topDocs().matchQuery("body_t", "alpha").withStats().fields({"id", "n_i", "c_s"}); }, 200);
  run([](auto& r) { r.topDocs().phraseText("body_t", "alpha beta").countLimit(); }, 200);
  run([](auto& r) { r.topDocs().prefixQuery("body_t", "w1").countLimit(); }, -1);
  run([](auto& r) { r.topDocs().exprQuery("n_i:[3 TO 5]").countLimit(); }, -1);
  run([](auto& r) { r.topDocs().existsQuery("n_i").countLimit(); }, 200);
  for (auto when : {"1970-01-01T00:00:05Z", "1970-01-01T00:02:30Z"}) {  // one per segment
    run([&](auto& r) { r.topDocs().matchQuery("t_dt", when).countLimit(); }, 1);
  }
  run([](auto& r) { r.facet("f", "c_s"); r.avg("a", "n_i"); r.topDocs().allQuery().countLimit(); }, 200);

  auto* layer = node.collections().storage().find<StrictLayer>();
  ASSERT_NE(nullptr, layer);
  for (const auto& file : h.getIndexWriter()->snapshots.snapshot()->files) {
    if (Manifest::segmentDataFile(file.name)) {
      EXPECT_EQ(0u, layer->counters->fileStats(file.name).wholeFileAccesses) << file.name;
      EXPECT_EQ(0u, layer->counters->fileStats(file.name).sizeRequests) << file.name;
    }
  }
  auto info = readDurableIndexInfo(h.getIndexWriter()->dir);
  ASSERT_EQ(2u, info->segments.size());
  auto reader = h.getIndexWriter()->snapshots.readers.getReader();
  for (const auto& segment : info->segments) {
    auto key = Postings::segmentKey(segment.seg_id, segment.creator_epoch);
    auto expectMetadata = [&](field_parts::Slice part) {
      ASSERT_TRUE(part);
      auto name = Postings::getIndexFileName(key, part.location.filenum());
      EXPECT_TRUE(layer->counters->fileStats(name).metadataParts.contains({part.location.offset(), part.bytes}))
          << name << " @" << part.location.offset();
    };
    expectMetadata({{Postings::PRIMARY_FILE, segment.catalog.offset}, segment.catalog.length});
    auto found = std::ranges::find_if(reader->segments(), [&](const auto& open) {
      return open.postingsReader().core().id() == segment.seg_id;
    });
    ASSERT_NE(reader->segments().end(), found);
    const auto* number = found->postingsReader().fieldInfo("n_i");
    const auto* date = found->postingsReader().fieldInfo("t_dt");
    ASSERT_TRUE(number && number->numberColumn() && date);
    expectMetadata(number->numberColumn()->index());
    expectMetadata(date->pointsDirectory());
  }
}

TEST_F(StorageExampleTest, nestedReaderOpensWithWholeSegmentFilesDenied) {
  LuxirConfig config;
  config.store.layers = {"strict"};
  config.store.options = {{"strict", {{"whole_segment_files", "deny"}}}};
  LuxirNode node(config);
  CollectionHelper h(node, "main");
  CollectionHelper::UpdateBuilder input;
  input.addRaw([](auto& doc, auto& arena) {
    ASSERT_TRUE(api::read_json(doc, R"({"id":"a","items_nested":[{},{}]})", arena));
  });
  ASSERT_TRUE(h.submit(input.commit()).success);
  auto reader = std::make_shared<IndexReader>(h.getIndexWriter()->dir);
  ASSERT_EQ(1u, reader->segments().size());
  const auto* shape = reader->segments().front().postingsReader().core().ownership("items_nested");
  ASSERT_TRUE(shape);
  EXPECT_EQ(2, shape->childCount());
}

TEST_F(StorageExampleTest, strictRejectsInvalidWholeFileMode) {
  LuxirConfig config;
  config.store.backend = "ram";
  config.store.layers = {"strict"};
  config.store.options = {{"strict", {{"whole_segment_files", "allow"}}}};
  EXPECT_THROW(LuxirNode node(config), ParamError);
}

TEST_F(StorageExampleTest, unknownBackendAndLayerNamesListRegistered) {
  LuxirConfig config;
  config.store.backend = "nope";
  try {
    LuxirNode node(config);
    FAIL() << "expected an unknown backend error";
  } catch (const std::invalid_argument& e) {
    EXPECT_NE(std::string(e.what()).find("counting_ram"), std::string::npos) << e.what();
  }
  config.store.backend = "ram";
  config.store.layers = {"nope"};
  try {
    LuxirNode node(config);
    FAIL() << "expected an unknown layer error";
  } catch (const std::invalid_argument& e) {
    EXPECT_NE(std::string(e.what()).find("valid layers: counting"), std::string::npos) << e.what();
  }
}

TEST_F(StorageExampleTest, coreReadOnlyWrapperStaysOutsideLayers) {
  LuxirConfig config;
  ParamReader params("counting", ParamReader::Entries{});
  StorageContext ctx{config, params, {}};
  auto& storage = Registries::global().storage;
  auto layered = storage.makeLayer("counting", storage.makeBackend("counting_ram", ctx), ctx);
  std::string path = "t/t0/i/00000000001";
  ReadOnlyDirFactory readOnly(std::make_unique<CheckedDirFactory>(std::move(layered), CheckedDirMode::THROW));
  EXPECT_EQ("read_only(checked(counting(counting_ram)))", readOnly.describe());
  auto* layer = readOnly.find<CountingLayer>();
  ASSERT_NE(nullptr, layer);
  EXPECT_NE(nullptr, readOnly.find<CountingRamBackend>());
  EXPECT_EQ(&readOnly, readOnly.find<ReadOnlyDirFactory>());
  auto dir = readOnly.directory(StorageRoot::AUTHORITATIVE, path);
  EXPECT_THROW(dir->createFile("f"), ReadOnlyError);
  EXPECT_EQ(0u, layer->counters->count("create"));  // refused before the layer saw it
  EXPECT_THROW(readOnly.removeTree(StorageRoot::AUTHORITATIVE, path), ReadOnlyError);
}

TEST_F(StorageExampleTest, nodesCountIndependently) {
  LuxirConfig config;
  config.store.backend = "counting_ram";
  config.store.layers = {"counting"};
  LuxirNode first(config), second(config);
  auto* firstBackend = first.collections().storage().find<CountingRamBackend>();
  auto* secondBackend = second.collections().storage().find<CountingRamBackend>();
  auto* firstLayer = first.collections().storage().find<CountingLayer>();
  auto* secondLayer = second.collections().storage().find<CountingLayer>();
  ASSERT_TRUE(firstBackend && secondBackend && firstLayer && secondLayer);
  auto firstBefore = firstBackend->counters->count("open");
  auto before = secondBackend->counters->count("open");
  CollectionHelper h(first, "additional");
  ASSERT_TRUE(h.indexAll({flatdoc("id", "a")}, UpdateMessage::COMMIT).success);
  EXPECT_GT(firstBackend->counters->count("open"), firstBefore);
  EXPECT_EQ(before, secondBackend->counters->count("open"));
  EXPECT_GT(firstLayer->counters->count("create"), 0u);
  EXPECT_EQ(0u, secondLayer->counters->count("create"));
}

TEST_F(StorageExampleTest, unknownOptionsFailStartup) {
  LuxirConfig config;
  config.store.backend = "ram";
  config.store.layers = {"counting"};
  config.store.options = {{"cache", {{"bytes", "10G"}}}};
  try { LuxirNode node(config); FAIL() << "accepted an unconfigured component"; }
  catch (const ParamError& e) {
    EXPECT_STREQ("store options for unconfigured component 'cache'; configured components: ram, counting", e.what());
  }
  config.store.options = {{"counting", {{"anything", "1"}}}};
  try { LuxirNode node(config); FAIL() << "accepted an unknown layer key"; }
  catch (const ParamError& e) {
    EXPECT_STREQ("store layer 'counting': unknown parameter 'anything'; it takes no parameters", e.what());
  }
  config.store.options = {{"ram", {{"anything", "1"}}}};
  try { LuxirNode node(config); FAIL() << "accepted an unknown backend key"; }
  catch (const ParamError& e) {
    EXPECT_STREQ("store backend 'ram': unknown parameter 'anything'; it takes no parameters", e.what());
  }
}

TEST_F(StorageExampleTest, pluginRamBackendAlsoRejectsReadOnly) {
  LuxirConfig config;
  config.store.backend = "counting_ram";
  config.read_only = true;
  EXPECT_THROW(LuxirNode node(config), std::runtime_error);
}

// Reopening a writer at startup reads its commit root once and lists its
// incarnation once: the recovery check, the marker check, the writer and its
// orphan sweep share one listing. On an object store a listing is a paginated
// request priced like a write, so each extra one shows on every restart.
TEST_F(StorageExampleTest, aWriterOpenListsItsIncarnationOnce) {
  auto path = std::filesystem::temp_directory_path() / ("luxir-open-listing-" + std::to_string(::getpid()));
  std::filesystem::remove_all(path);
  auto cleanup = scope_guard([&] { std::filesystem::remove_all(path); });
  LuxirConfig config;
  config.store.backend = "fs";
  config.store.data_dir = path.string();
  config.store.layers = {"counting"};
  std::string label;
  {
    LuxirNode node(config);
    CollectionHelper h(node, "main");
    ASSERT_TRUE(h.indexAll({flatdoc("id", "a"), flatdoc("id", "b")}, UpdateMessage::COMMIT).success);
    auto incarnation = node.collections().catalog().current("main");
    ASSERT_TRUE(incarnation);
    label = "t/t0/i/" + *incarnation;
    // A marker naming a committed file: the reopen verifies it and clears it.
    auto dir = node.collections().catalog().directory(*incarnation);
    auto files = h.getIndexWriter()->snapshots.snapshot()->files;
    ASSERT_FALSE(files.empty());
    std::vector<std::string> suspects{files.front().name};
    CorruptionMarker::write(*dir, suspects, "test suspect");
  }
  LuxirNode reopened(config);
  auto* layer = reopened.collections().storage().find<CountingLayer>();
  ASSERT_NE(nullptr, layer);
  auto collection = reopened.getCollection("main");
  ASSERT_NE(nullptr, collection);
  EXPECT_EQ(2, collection->getReaderManager().getReader()->liveDocs());
  EXPECT_EQ(1u, layer->counters->count("list", label)) << "a writer open listed its incarnation more than once";
  auto dir = reopened.collections().catalog().directory(*reopened.collections().catalog().current("main"));
  EXPECT_FALSE(CorruptionMarker::present(*dir)) << "the verified marker was not cleared";
}

TEST_F(StorageExampleTest, aFollowerOpenListsItsIncarnationOnce) {
  LuxirConfig config;
  config.store.backend = "ram";
  config.store.layers = {"counting"};
  config.replication.source = "http://127.0.0.1:1";
  LuxirNode node(config, LuxirNode::Mode::PULL);
  auto& collections = node.collections();
  auto& catalog = collections.catalog();
  auto id = catalog.reserve("main");
  auto dir = catalog.directory(id);
  {
    CommitSnapshotRegistry snapshots(*dir);
    IndexWriter writer(snapshots, {}, nullptr, 10, id);
    auto snapshot = snapshots.snapshot();
    ASSERT_FALSE(snapshot->files.empty());
    CorruptionMarker::write(*dir, std::array{snapshot->files.front().name}, "test suspect");
  }
  catalog.activate("main", id);

  auto* layer = collections.storage().find<CountingLayer>();
  ASSERT_NE(nullptr, layer);
  auto lists = [&] { return layer->counters->count("list", "t/t0/i/" + id); };
  auto before = lists();
  collections.open(Collections::Role::FOLLOWER);
  EXPECT_EQ(id, node.getCollection("main")->getShard()->getSnapshots().snapshot()->id.incarnation);
  EXPECT_EQ(before + 1, lists());
  EXPECT_FALSE(CorruptionMarker::present(*dir));
}
