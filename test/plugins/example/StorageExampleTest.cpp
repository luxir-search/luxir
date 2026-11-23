// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include <gtest/gtest.h>
#include <filesystem>
#include <unistd.h>

#include "CountingStorage.h"
#include "luxir/plugin/Registries.h"
#include "luxir/reader/OwnershipShape.h"
#include "luxir/reader/TermsEnum.h"
#include "luxir/store/AcquireJoin.h"
#include "luxir/store/Manifest.h"
#include "luxir/store/ReadOnlyDirectory.h"
#include "luxir/store/CorruptionMarker.h"
#include "luxir/store/FileIOException.h"
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
      SegmentCatalog::readDirectory(catalogBytes.bytes(), range, primary->size()), source.files, {});

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
  // The open acquires the headers and the catalog, nothing it can build later.
  expectPart({{0, range.offset}, range.length}, ReadClass::METADATA);
  check();
  EXPECT_FALSE(postings.core().readPastCatalog());

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
  EXPECT_TRUE(postings.core().readPastCatalog());

  // The first query that returns versions acquires the version runs, once.
  expectPart(catalog.versionRuns().slice, ReadClass::METADATA);
  for (int i = 0; i < 2; ++i) {
    auto req = localReq(h.getSearchEngine());
    req->topDocs().matchQuery("freqs", "needle").fields({"_version_"});
    req->execute(reader);
    ASSERT_TRUE(req->ok()) << req->errorMsg();
    check();
  }
}

// A manifest open issues one batch per segment's file 0, holding its header and
// catalog, issues all of them before it reads anything else, and reads nothing
// more: version runs and ownership shapes wait for their first use.
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
    EXPECT_EQ(1u, stats.rounds) << name;
    EXPECT_TRUE(stats.metadataParts.contains({0, Postings::LUXIR_HEADER.size()})) << name;
    EXPECT_TRUE(stats.metadataParts.contains({range.offset, range.length})) << name;
    firstRounds.push_back(stats.firstRound);
  }
  std::ranges::sort(firstRounds);
  EXPECT_EQ((std::vector<uint64_t>{1, 2, 3}), firstRounds);
  EXPECT_EQ(3u, counters->totalRounds());
  auto opened = segmentOpenCounters.segmentsOpened.load();
  for (auto& segment : reader->segments()) EXPECT_FALSE(segment.segmentCore().readPastCatalog());

  // The first use of the version runs acquires them as one more round.
  auto& first = reader->segments().front().segmentCore();
  first.versionRuns();
  EXPECT_EQ(4u, counters->totalRounds());
  EXPECT_TRUE(first.readPastCatalog());
  EXPECT_EQ(opened + 1, segmentOpenCounters.segmentsOpened.load());

  // A later batch acquires only the parts the segment does not hold yet.
  auto& core = reader->segments().front().postingsReader().core();
  auto name = Postings::getIndexFileName(core.segmentName(), 0);
  auto before = counters->fileStats(name);
  field_parts::Slice parts[] = {{{0, 0}, 2}, {{0, 2}, 3}, {{0, 0}, 2}, {{0, 0}, Postings::LUXIR_HEADER.size()}};
  auto bytes = core.slices(parts);
  ASSERT_EQ(4u, bytes.size());
  EXPECT_EQ(Postings::LUXIR_HEADER.substr(2, 3), std::string_view(bytes[1].data(), bytes[1].size()));
  EXPECT_EQ(bytes[0].data(), bytes[2].data());
  auto after = counters->fileStats(name);
  EXPECT_EQ(before.batches + 1, after.batches);
  EXPECT_EQ(before.acquires + 2, after.acquires);
}

// A term lookup in a large dictionary holds its routing data, the block it
// lands in and the found term's docs, not the dictionary or the run's docs.
// An enum past POINT_READS point reads holds them whole.
TEST_F(StorageExampleTest, termLookupHoldsItsBlockAndPostings) {
  LuxirConfig config;
  LuxirNode node(config);
  CollectionHelper h(node, "main");
  auto writer = h.getIndexWriter();
  std::vector<Doc> docs;
  // Each term in two docs, so its postings are not inline.
  auto tag = [](int i) { return std::format("tag{:05}x{:04x}", i, (i * 7919) % 65536); };
  for (int i = 0; i < 40000; ++i) docs.push_back(flatdoc("id", std::to_string(i), "tag_s", tag(i % 20000)));
  ASSERT_TRUE(h.indexAll(docs, UpdateMessage::COMMIT).success);
  auto snapshot = *writer->snapshots.snapshot();
  snapshot.openCores.clear();
  auto counters = std::make_shared<StrictStorageCounters>();
  StrictDirectory dir(std::shared_ptr<Directory>(writer, &writer->dir), counters, StrictWholeFiles::DENY);
  auto reader = std::make_shared<IndexReader>(dir, snapshot);
  auto& postings = reader->segments().front().postingsReader();
  const auto* record = postings.fieldInfo("tag_s");
  ASSERT_TRUE(record && record->terms());
  const auto& terms = *record->terms();
  ASSERT_GT(terms.dictionaryBytes, field_parts::WHOLE_READ_BYTES);
  ASSERT_GT(terms.runs.front().docs.bytes, field_parts::WHOLE_READ_BYTES);
  auto& core = postings.core();
  MemPool pool;
  TermsEnum lookup(pool, postings, *record);
  ASSERT_TRUE(lookup.seek(tag(1234)));
  auto state = lookup.postingsState();
  EXPECT_EQ(2, state.docFreq);
  EXPECT_TRUE(core.held(record->termNavigation()));
  EXPECT_FALSE(core.held(terms.dictionaryBlocks()));
  EXPECT_FALSE(core.held(terms.runs.front().docs));
  auto name = Postings::getIndexFileName(core.segmentName(), terms.location.filenum());
  auto stats = counters->fileStats(name);
  uint64_t blockBytes = 0;
  for (auto [offset, length] : stats.parts) {
    if (offset >= terms.location.offset() && offset + length <= terms.dictionaryBlocks().end().offset()) {
      blockBytes += length;
    }
  }
  EXPECT_GT(blockBytes, 0u);
  EXPECT_LT(blockBytes, terms.dictionaryBytes / 10);

  for (int i = 0; i < TermsEnum::POINT_READS; ++i) {
    ASSERT_TRUE(lookup.seek(tag(1000 * i)));
    lookup.postingsState();
  }
  EXPECT_TRUE(core.held(terms.dictionaryBlocks()));
  EXPECT_TRUE(core.held(terms.runs.front().docs));
}

// A cold scored term query declares what it reads in every segment and waits
// on a few rounds of batches, issued together across segments, however many
// segments it reads: routing data (with the dictionary when it is small, or
// alone, then the block the lookup lands in), the postings of the term found,
// then the norms its scorer reads. With declares capped away, every part is a
// round of its own.
TEST_F(StorageExampleTest, coldTermQueryWaitsOnRoundsNotSegments) {
  struct Cost {
    uint64_t rounds = 0;   // the request's sequential acquire waits
    uint64_t batches = 0;  // batches the storage saw
    uint64_t singles = 0;  // single acquires the storage saw
    uint64_t maxBatchesPerFile = 0;
  };
  auto measure = [](int segments, bool largeDictionary, uint64_t readAheadBytes) {
    LuxirConfig config;
    config.search.read_ahead_max_bytes = readAheadBytes;
    LuxirNode node(config);
    CollectionHelper h(node, "main");
    auto writer = h.getIndexWriter();
    for (int i = 0; i < segments; ++i) {
      std::string body = "needle";
      // About 100 KB of dictionary: past WHOLE_READ_BYTES, so it is held block by block.
      for (int t = 0; largeDictionary && t < 8000; ++t) body += std::format(" term{:05}x{}", t, i);
      // In two docs, so the term's postings are a part of their own, not inline.
      EXPECT_TRUE(h.indexAll({flatdoc("id", std::to_string(i), "body_w", body),
                              flatdoc("id", "x" + std::to_string(i), "body_w", "hay needle")},
                             UpdateMessage::COMMIT).success);
    }
    auto snapshot = *writer->snapshots.snapshot();
    snapshot.openCores.clear();
    auto counters = std::make_shared<StrictStorageCounters>();
    StrictDirectory dir(std::shared_ptr<Directory>(writer, &writer->dir), counters, StrictWholeFiles::DENY);
    auto reader = std::make_shared<IndexReader>(dir, snapshot);
    EXPECT_EQ((size_t)segments, reader->segments().size());
    auto totals = [&] {
      Cost cost;
      for (const auto& file : snapshot.files) {
        auto stats = counters->fileStats(file.name);
        cost.batches += stats.batches;
        cost.singles += stats.rounds - stats.batches;
      }
      return cost;
    };
    auto before = totals();
    std::map<std::string, uint64_t> batchesBefore;
    for (const auto& file : snapshot.files) batchesBefore[file.name] = counters->fileStats(file.name).batches;
    auto req = localReq(h.getSearchEngine());
    req->profile().topDocs().matchQuery("body_w", "needle").fields({"_score_"}).withStats();
    req->execute(reader);
    EXPECT_TRUE(req->ok()) << req->errorMsg();
    EXPECT_EQ(2 * segments, req->getMatchCount());
    auto after = totals();
    Cost cost;
    cost.rounds = req->responses.back()->proto.profile.value_or(api::ExecutionProfile{}).io_acquire_rounds;
    cost.batches = after.batches - before.batches;
    cost.singles = after.singles - before.singles;
    for (const auto& file : snapshot.files) {
      auto stats = counters->fileStats(file.name);
      EXPECT_EQ(0u, stats.wholeFileAccesses) << file.name;
      cost.maxBatchesPerFile = std::max(cost.maxBatchesPerFile, stats.batches - batchesBefore[file.name]);
    }
    return cost;
  };

  for (bool large : {false, true}) {
    auto few = measure(2, large, 256ULL << 20);
    auto many = measure(5, large, 256ULL << 20);
    // Small dictionary: routing data and dictionary, postings, norms.
    // Large one: routing data, block, postings, norms.
    uint64_t rounds = large ? 4 : 3;
    EXPECT_EQ(rounds, few.rounds) << large;
    EXPECT_EQ(rounds, many.rounds) << large;
    EXPECT_EQ(0u, few.singles) << large;
    EXPECT_EQ(0u, many.singles) << large;
    EXPECT_LE(many.maxBatchesPerFile, rounds) << large;
    EXPECT_GT(many.batches, few.batches) << large;

    // Capped to nothing, every part is acquired on demand, one round each.
    auto capped = measure(5, large, 1);
    EXPECT_EQ(0u, capped.batches) << large;
    EXPECT_EQ(capped.singles, capped.rounds) << large;
    EXPECT_GE(capped.rounds, 5 * rounds) << large;
  }
}

// An upsert batch over cold segments acquires its id lookups in rounds across
// segments and ids: the filters' header and blocks, the routing data, then
// the blocks the ids land in with the version runs. A segment holds one doc
// per id, whose postings are pulsed into its block, so no postings are read. A segment its bounds exclude reads nothing,
// one its filter excludes reads its filter header and one block, and no
// segment holds its dictionary or filter whole.
TEST_F(StorageExampleTest, coldUpsertWaitsOnRoundsNotIdsOrSegments) {
  constexpr int perSegment = 6000;
  // Segment s holds ids "s" + 15 hex digits, so bounds tell segments apart.
  auto id = [](int segment, int i) { return std::format("{}{:015x}", segment, (uint64_t)i * 0x9e3779b97f4a7c15ULL >> 4); };
  struct Cost {
    uint64_t rounds = 0, batches = 0, singles = 0, maxBatchesPerFile = 0;
  };
  auto measure = [&](int segments, int upserts) {
    LuxirConfig config;
    config.store.backend = "ram";
    config.store.layers = {"strict"};
    config.store.options = {{"strict", {{"whole_segment_files", "deny"}}}};
    config.index.id_filter_min_kb = 128;  // past WHOLE_READ_BYTES: probed block by block
    LuxirNode node(config);
    CollectionHelper h(node, "main");
    auto writer = h.getIndexWriter();
    for (int s = 0; s < segments; ++s) {
      std::vector<Doc> docs;
      for (int i = 0; i < perSegment; ++i) docs.push_back(flatdoc("id", id(s, i)));
      EXPECT_TRUE(h.indexAll(docs, UpdateMessage::COMMIT).success);
    }
    auto reader = writer->snapshots.readers.getReader();
    EXPECT_EQ((size_t)segments, reader->segments().size());
    auto* layer = node.collections().storage().find<StrictLayer>();
    EXPECT_NE(nullptr, layer);
    auto& counters = *layer->counters;
    auto old = writer->snapshots.snapshot()->files;
    std::map<std::string, StrictStorageCounters::FileStats> before;
    for (const auto& file : old) before[file.name] = counters.fileStats(file.name);
    for (const auto& segment : reader->segments()) {
      const auto* record = segment.postingsReader().fieldInfo("id");
      EXPECT_FALSE(segment.segmentCore().held(record->termNavigation()));
      EXPECT_GT(record->terms()->dictionaryBytes, field_parts::WHOLE_READ_BYTES);
      EXPECT_GT(record->idFilter().bytes, field_parts::WHOLE_READ_BYTES);
    }
    auto roundsBefore = writer->stats(false, false).idLookupRounds;

    // Upserts into the first half of the segments, round robin, and one id
    // absent from the last segment but within its bounds.
    std::vector<Doc> docs;
    int targets = std::max(1, segments / 2);
    for (int u = 0; u < upserts; ++u) docs.push_back(flatdoc("id", id(u % targets, u * 7 % (perSegment / 2))));
    auto absent = std::format("{}{:015x}", segments - 1, 1);
    docs.push_back(flatdoc("id", absent));
    EXPECT_TRUE(h.indexAll(docs, UpdateMessage::COMMIT, true).success);
    EXPECT_EQ((int64_t)segments * perSegment - upserts + (int64_t)docs.size(),
              (int64_t)writer->snapshots.readers.getReader()->liveDocs());

    Cost cost;
    cost.rounds = writer->stats(false, false).idLookupRounds - roundsBefore;
    for (const auto& file : old) {
      auto stats = counters.fileStats(file.name);
      auto& was = before[file.name];
      auto batches = stats.batches - was.batches;
      cost.batches += batches;
      cost.singles += stats.rounds - was.rounds - batches;
      cost.maxBatchesPerFile = std::max(cost.maxBatchesPerFile, batches);
      EXPECT_EQ(0u, stats.wholeFileAccesses) << file.name;
    }
    for (const auto& segment : reader->segments()) {
      auto& core = segment.segmentCore();
      const auto* record = segment.postingsReader().fieldInfo("id");
      auto name = Postings::getIndexFileName(core.segmentName(), record->terms()->location.filenum());
      auto acquired = counters.fileStats(name).acquires - before[name].acquires;
      EXPECT_FALSE(core.held(record->terms()->dictionaryBlocks())) << name;
      EXPECT_FALSE(core.held(record->idFilter())) << name;
      int s = std::string_view(record->dictionaryBounds()->lower)[0] - '0';
      if (s < targets) continue;
      // Bounds exclude the upserts; the filter excludes the absent id, read
      // at its header and one block.
      EXPECT_EQ(s == segments - 1 ? 2u : 0u, acquired) << name;
      EXPECT_FALSE(core.held(record->termNavigation())) << name;
    }

    // A second batch holds the routing data already, and reads the live docs
    // the first wrote with its blocks: filter blocks, then term blocks.
    docs.clear();
    for (int u = 0; u < upserts; ++u) docs.push_back(flatdoc("id", id(u % targets, perSegment / 2 + u)));
    roundsBefore = writer->stats(false, false).idLookupRounds;
    auto totalBefore = counters.totalRounds();
    EXPECT_TRUE(h.indexAll(docs, UpdateMessage::COMMIT, true).success);
    EXPECT_EQ(2u, writer->stats(false, false).idLookupRounds - roundsBefore);
    EXPECT_EQ((int64_t)segments * perSegment + 1, (int64_t)writer->snapshots.readers.getReader()->liveDocs());
    uint64_t oldSingles = 0;
    for (const auto& file : old) {
      auto stats = counters.fileStats(file.name);
      if (stats.lastRound > totalBefore) oldSingles += stats.rounds - stats.batches - (before[file.name].rounds - before[file.name].batches);
    }
    EXPECT_EQ(cost.singles, oldSingles);
    return cost;
  };

  auto few = measure(2, 1);
  auto many = measure(6, 60);
  for (const auto& cost : {few, many}) {
    EXPECT_EQ(3u, cost.rounds);  // filters, routing data, blocks
    EXPECT_EQ(0u, cost.singles);
    EXPECT_LE(cost.maxBatchesPerFile, cost.rounds);
  }
  EXPECT_GT(many.batches, few.batches);
}

// listParts() lists each segment's catalog and its fields' navigation parts,
// and the data of the fields asked for. A segment that holds what it lists
// serves a query on those fields without another acquire.
TEST_F(StorageExampleTest, listedPartsServeAQueryOnTheirFields) {
  LuxirConfig config;
  LuxirNode node(config);
  CollectionHelper h(node, "main");
  auto writer = h.getIndexWriter();
  for (int i = 0; i < 2; ++i) {
    ASSERT_TRUE(h.indexAll({flatdoc("id", std::to_string(i), "body_w", "needle hay", "c_s", "x", "n_i", (int64_t)i),
                            flatdoc("id", "x" + std::to_string(i), "body_w", "needle", "c_s", "y")},
                           UpdateMessage::COMMIT).success);
  }
  auto snapshot = *writer->snapshots.snapshot();
  snapshot.openCores.clear();
  auto counters = std::make_shared<StrictStorageCounters>();
  StrictDirectory dir(std::shared_ptr<Directory>(writer, &writer->dir), counters, StrictWholeFiles::DENY);
  auto reader = std::make_shared<IndexReader>(dir, snapshot);
  ASSERT_EQ(2u, reader->segments().size());
  using Kind = SegmentCore::ListedPart::Kind;

  std::string_view fields[] = {"body_w"};
  auto parts = reader->listParts(fields);
  auto count = [&](Kind kind, std::string_view field, ReadClass readClass) {
    return std::ranges::count_if(parts, [&](const auto& part) {
      return part.kind == kind && part.field == field && part.readClass == readClass;
    });
  };
  EXPECT_GE(count(Kind::CATALOG, "", ReadClass::METADATA), 4);         // a header and a catalog per segment
  EXPECT_EQ(2, count(Kind::NAVIGATION, "body_w", ReadClass::METADATA));  // term routing data per segment
  EXPECT_GE(count(Kind::NAVIGATION, "c_s", ReadClass::METADATA), 2);
  EXPECT_GE(count(Kind::NAVIGATION, "n_i", ReadClass::METADATA), 2);     // the column index
  EXPECT_GT(count(Kind::DATA, "body_w", ReadClass::DATA), 2);
  EXPECT_EQ(0, count(Kind::DATA, "c_s", ReadClass::DATA));
  EXPECT_GT(std::ranges::count_if(reader->listParts({}, true), [](const auto& part) {
    return part.kind == Kind::DATA && part.field == "c_s";
  }), 0);
  for (const auto& part : parts) {
    EXPECT_TRUE(std::ranges::any_of(snapshot.files, [&](const auto& file) { return file.name == part.file; }));
    EXPECT_GT(part.length, 0u) << part.file;
  }

  // Hold what the segments list, as a warmed backend would serve it.
  auto data = [](std::string_view, std::string_view field) { return field == "body_w"; };
  for (auto& segment : reader->segments()) {
    std::vector<SegmentCore::Part> held;
    segment.segmentCore().listParts(data, [&](const SegmentCore::ListedPart& part) {
      held.push_back({part.slice, part.readClass});
    });
    AcquireJoin join;
    segment.segmentCore().acquireParts(held, join);
    join.wait();
  }
  auto acquires = [&] {
    uint64_t total = 0;
    for (const auto& file : snapshot.files) total += counters->fileStats(file.name).acquires;
    return total;
  };
  auto before = acquires();
  auto req = localReq(h.getSearchEngine());
  req->profile().topDocs().matchQuery("body_w", "needle").fields({"_score_"}).withStats();
  req->execute(reader);
  ASSERT_TRUE(req->ok()) << req->errorMsg();
  EXPECT_EQ(4, req->getMatchCount());
  EXPECT_EQ(before, acquires());
  EXPECT_EQ(0u, req->responses.back()->proto.profile.value_or(api::ExecutionProfile{}).io_acquire_rounds);
}

// The canonical query shapes read parts, never whole segment files, and each
// segment acquires its catalog as metadata.
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
                             "c_s", "c" + std::to_string(n % 5), "n_i", (int64_t)(n % 31)));
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
  run([](auto& r) { r.facet("f", "c_s"); r.avg("a", "n_i"); r.topDocs().allQuery().countLimit(); }, 200);

  auto* layer = node.collections().storage().find<StrictLayer>();
  ASSERT_NE(nullptr, layer);
  for (const auto& file : h.getIndexWriter()->snapshots.snapshot()->files) {
    if (Manifest::segmentDataFile(file.name)) {
      EXPECT_EQ(0u, layer->counters->fileStats(file.name).wholeFileAccesses) << file.name;
    }
  }
  auto info = readDurableIndexInfo(h.getIndexWriter()->dir);
  ASSERT_EQ(2u, info->segments.size());
  for (const auto& segment : info->segments) {
    auto primary = Postings::getIndexFileName(Postings::segmentKey(segment.seg_id, segment.creator_epoch), 0);
    EXPECT_GT(layer->counters->fileStats(primary).metadataAcquires, 0u) << primary;
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

// A first use whose read fails fails only that use: nothing is kept from it,
// so the next use reads again and succeeds.
TEST_F(StorageExampleTest, failedFirstUseIsRetried) {
  LuxirConfig config;
  LuxirNode node(config);
  CollectionHelper h(node, "main");
  CollectionHelper::UpdateBuilder input;
  input.addRaw([](auto& doc, auto& arena) {
    ASSERT_TRUE(api::read_json(doc, R"({"id":"a","items_nested":[{},{}]})", arena));
  });
  ASSERT_TRUE(h.submit(input.commit()).success);
  auto writer = h.getIndexWriter();
  auto snapshot = *writer->snapshots.snapshot();
  snapshot.openCores.clear();
  auto counters = std::make_shared<StrictStorageCounters>();
  StrictDirectory dir(std::shared_ptr<Directory>(writer, &writer->dir), counters, StrictWholeFiles::DENY);
  auto reader = std::make_shared<IndexReader>(dir, snapshot);
  auto& core = reader->segments().front().segmentCore();

  counters->failNextRounds(1);
  EXPECT_THROW(core.versionRuns(), FileIOException);
  EXPECT_EQ(1u, core.versionRuns().runs().size());
  counters->failNextRounds(1);
  EXPECT_THROW(core.ownership("items_nested"), FileIOException);
  ASSERT_TRUE(core.ownership("items_nested"));
  EXPECT_EQ(2, core.ownership("items_nested")->childCount());
}

// A snapshot open opens the live docs file without reading it and counts
// live rows from the manifest; the first use decodes the bitmap.
TEST_F(StorageExampleTest, liveDocsDecodeOnFirstUse) {
  LuxirConfig config;
  LuxirNode node(config);
  CollectionHelper h(node, "main");
  ASSERT_TRUE(h.indexAll({flatdoc("id", "a", "c_s", "x"), flatdoc("id", "b", "c_s", "x"),
                          flatdoc("id", "c", "c_s", "x")}, UpdateMessage::COMMIT).success);
  ASSERT_TRUE(h.deleteById("b", UpdateMessage::COMMIT).success);
  auto writer = h.getIndexWriter();
  auto info = readDurableIndexInfo(writer->dir);
  ASSERT_EQ(1u, info->segments.size());
  const auto& segment = info->segments.front();
  auto liveName = Postings::getLiveDocsFileName(Postings::segmentKey(segment.seg_id, segment.creator_epoch),
                                                segment.live_gen, segment.live_epoch);
  auto snapshot = *writer->snapshots.snapshot();
  snapshot.openCores.clear();
  auto counters = std::make_shared<StrictStorageCounters>();
  StrictDirectory dir(std::shared_ptr<Directory>(writer, &writer->dir), counters, StrictWholeFiles::DENY);
  auto reader = std::make_shared<IndexReader>(dir, snapshot);
  const auto& leaf = reader->segments().front();
  EXPECT_EQ(2, reader->liveDocs());
  EXPECT_EQ(2, leaf.numLive());
  EXPECT_TRUE(leaf.hasDeletes());
  EXPECT_EQ(0u, counters->fileStats(liveName).acquires);
  EXPECT_EQ(0u, counters->fileStats(liveName).sizeRequests);

  // A failed read fails the request; the next one decodes the bitmap. The
  // request acquires the live docs ahead of parse, and a failure there only
  // leaves them to the first use, so every read has to fail.
  auto search = [&] {
    auto req = localReq(h.getSearchEngine());
    req->topDocs().matchQuery("c_s", "x").countLimit(-1);
    req->execute(reader);
    return req;
  };
  counters->failNextRounds(1000);
  EXPECT_FALSE(search()->ok());
  counters->failNextRounds(0);
  auto req = search();
  ASSERT_TRUE(req->ok()) << req->errorMsg();
  EXPECT_EQ(2, req->getMatchCount());
  EXPECT_EQ(1u, counters->fileStats(liveName).acquires);
  ASSERT_TRUE(leaf.liveDocs());
  EXPECT_EQ(2, leaf.liveDocs()->numLive());
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
