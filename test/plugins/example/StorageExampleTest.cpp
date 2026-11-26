// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include <gtest/gtest.h>
#include <filesystem>
#include <unistd.h>

#include "CountingStorage.h"
#include "luxir/index/PostingsWriter.h"
#include "luxir/plugin/Registries.h"
#include "luxir/reader/OwnershipShape.h"
#include "luxir/search/ProtobufSearchParser.h"
#include "luxir/search/ops/RootOp.h"
#include "luxir/store/Manifest.h"
#include "luxir/store/ReadOnlyDirectory.h"
#include "luxir/store/CorruptionMarker.h"
#include "luxir/store/FileIOException.h"
#include "luxir/util/luxir_util.h"
#include "luxir/store/CheckedDirFactory.h"
#include "test/CollectionHelper.h"
#include "test/DurableIndexInfo.h"
#include "test/GatedStorage.h"
#include "test/LocalReq.h"
#include "test/LuxirTest.h"
#include "test/StrictStorage.h"
#include "test/TestUtils.h"

using namespace luxir;
using namespace luxir::test;
using luxir::example::CountingLayer;
using luxir::example::CountingRamBackend;

class StorageExampleTest : public LuxirTest {
protected:
  void expectDeferredBinding(CollectionHelper& collection, std::string_view json) {
    auto writer = collection.getIndexWriter();
    auto snapshot = *writer->snapshots.snapshot();
    snapshot.openCores.clear();
    auto counters = std::make_shared<StrictStorageCounters>();
    StrictDirectory dir(std::shared_ptr<Directory>(writer, &writer->dir), counters, StrictWholeFiles::DENY);
    auto req = localReq(collection.getSearchEngine());
    req->reader = std::make_shared<IndexReader>(dir, snapshot);
    req->schema = req->reader->schema();
    ASSERT_TRUE(api::read_json(req->rawRequest(), json, req->mr));

    auto before = counters->totalSubmissions();
    auto* root = ProtobufSearchParser(*req).parse();
    EXPECT_EQ(before, counters->totalSubmissions());
    req->materialize();
    EXPECT_GT(counters->totalSubmissions(), before);
    root->init();
  }
};

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

TEST_F(StorageExampleTest, ordinalMapsBindAfterLogicalPlanning) {
  CollectionHelper collection;
  ASSERT_TRUE(collection.indexAll({flatdoc("id", "a", "cat_s", "red"),
      flatdoc("id", "b", "cat_s", "blue")}, UpdateMessage::COMMIT).success);
  expectDeferredBinding(collection,
      R"json({"ops":{"q":{"top_docs":{"collapse":"cat_s","limit":1,"count_limit":-1}}}})json");
  expectDeferredBinding(collection, R"json({"ops":{"q":{"top_docs":{"limit":0,"ops":{
    "cat":{"field_facet":{"field":"cat_s","limit":1,"selected":["red"],
          "term_filter":{"include":["blue"]},"count_distinct":"id"}}}}}}})json");
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

namespace {

LuxirConfig deleteCostConfig(bool bloom) {
  LuxirConfig config;
  config.store.backend = "ram";
  config.store.layers = {"strict"};
  config.store.options = {{"strict", {{"whole_segment_files", "deny"}}}};
  config.index.id_filter = bloom ? "bloom" : "none";
  config.index.merge_factor = 1000;
  return config;
}

struct DeleteCostIndex {
  LuxirNode node;
  CollectionHelper h;
  std::shared_ptr<IndexWriter> writer;
  StrictStorageCounters& counters;

  explicit DeleteCostIndex(bool bloom = true)
      : node(deleteCostConfig(bloom)), h(node), writer(h.getIndexWriter()),
        counters(*node.collections().storage().find<StrictLayer>()->counters) {
    node.getIndexRamBudget().setTotalBytes(0);  // Only the measured commit applies deletes.
  }

  // One base file per segment, distinct bounds with a gap for a filter miss.
  void seed(int count) {
    for (int i = 0; i < count; ++i) {
      auto prefix = std::to_string(i);
      ASSERT_TRUE(h.indexAll({flatdoc("id", prefix + "a"), flatdoc("id", prefix + "z")},
                             UpdateMessage::COMMIT).success);
    }
    ASSERT_EQ((size_t)count, writer->snapshots.readers.getReader()->segments().size());
  }

  static std::string baseFile(const Segment& seg) {
    return Postings::getIndexFileName(seg.segmentCore().segmentName(), 0);
  }
};

} // namespace

TEST_F(StorageExampleTest, idLookupPrefetchColdAndWarm) {
  for (int count : {2, 6}) {
    DeleteCostIndex index;
    ASSERT_NO_FATAL_FAILURE(index.seed(count));
    auto reader = index.writer->snapshots.readers.getReader();
    int targets = count / 2;
    std::vector<Doc> updates;
    for (int i = 0; i < targets; ++i) updates.push_back(flatdoc("id", std::to_string(i) + "a"));
    updates.push_back(flatdoc("id", std::to_string(count - 1) + "m"));

    std::map<std::string, StrictStorageCounters::FileStats> before;
    std::map<std::string, field_parts::Slice> versions;
    for (const auto& seg : reader->segments()) {
      auto name = index.baseFile(seg);
      before[name] = index.counters.fileStats(name);
      versions[name] = seg.segmentCore().versionRunsPart();
    }
    ASSERT_TRUE(index.h.indexAll(updates, UpdateMessage::COMMIT, true).success);
    EXPECT_EQ(2 * count + 1, index.writer->snapshots.readers.getReader()->liveDocs());
    for (const auto& seg : reader->segments()) {
      const auto* field = seg.postingsReader().fieldInfo("id");
      auto name = index.baseFile(seg);
      SCOPED_TRACE(name);
      auto after = index.counters.fileStats(name);
      auto& was = before.at(name);
      uint64_t batches = after.batches - was.batches;
      EXPECT_EQ(batches, after.submissions - was.submissions);
      EXPECT_LE(batches, 3u);
      int i = std::string_view(field->dictionaryBounds()->lower).front() - '0';
      uint64_t expectedRanges = 0;
      auto expectPart = [&](field_parts::Slice part, ReadClass cls) {
        ASSERT_TRUE(part);
        auto key = std::pair(part.location.offset(), part.bytes);
        EXPECT_FALSE(was.parts.contains(key));
        EXPECT_TRUE(after.parts.contains(key));
        EXPECT_EQ(cls == ReadClass::METADATA, after.metadataParts.contains(key));
        ++expectedRanges;
      };
      if (i < targets || i == count - 1) expectPart(field->idFilter(), ReadClass::METADATA);
      if (i < targets) {
        expectPart(field->termNavigation(), ReadClass::METADATA);
        expectPart(field->terms()->dictionaryBlocks(), ReadClass::DATA);
        expectPart(versions.at(name), ReadClass::METADATA);
      }
      EXPECT_EQ(expectedRanges, after.acquires - was.acquires);
      was = std::move(after);
    }
    updates.clear();
    for (int i = 0; i < targets; ++i) updates.push_back(flatdoc("id", std::to_string(i) + "z"));
    ASSERT_TRUE(index.h.indexAll(updates, UpdateMessage::COMMIT, true).success);
    for (const auto& [name, was] : before) EXPECT_EQ(was.submissions, index.counters.fileStats(name).submissions);
    EXPECT_EQ(2 * count + 1, index.writer->snapshots.readers.getReader()->liveDocs());
  }
}

TEST_F(StorageExampleTest, idLookupPrefetchIssuesEachPhaseAcrossSegments) {
  for (bool filters : {true, false}) {
    DeleteCostIndex index(filters);
    ASSERT_NO_FATAL_FAILURE(index.seed(2));
    // With filters, the two filter batches must be in flight together. Without
    // filters the first submissions are metadata and data for each survivor.
    auto gate = std::make_shared<BatchGate>(filters ? 2 : 4, std::chrono::seconds(5));
    for (const auto& seg : index.writer->snapshots.readers.getReader()->segments()) {
      index.counters.gateBatches(index.baseFile(seg), gate);
    }
    ASSERT_TRUE(index.h.deleteByIds({"0a", "1a"}, UpdateMessage::COMMIT).success);
    EXPECT_FALSE(gate->timedOut());
    EXPECT_EQ(filters ? 6u : 4u, gate->submissions());
    EXPECT_EQ(gate->submissions(), gate->deliveries());
    EXPECT_EQ((std::vector<std::string>{"0z", "1z"}), allIds(index.h));
  }
}

TEST_F(StorageExampleTest, idLookupPrefetchFailedBatchRetriesOnUse) {
  for (bool filters : {true, false}) {
    DeleteCostIndex index(filters);
    ASSERT_NO_FATAL_FAILURE(index.seed(2));
    auto reader = index.writer->snapshots.readers.getReader();
    std::map<std::string, StrictStorageCounters::FileStats> before;
    for (const auto& seg : reader->segments()) before[index.baseFile(seg)] = index.counters.fileStats(index.baseFile(seg));
    // Both cores are open, so the next submission is a prefetch batch on one of their base files.
    index.counters.failNextRounds(1);
    ASSERT_TRUE(index.h.deleteByIds({"0a", "1a"}, UpdateMessage::COMMIT).success);
    int retried = 0;
    for (const auto& [name, was] : before) {
      auto after = index.counters.fileStats(name);
      auto singles = (after.submissions - was.submissions) - (after.batches - was.batches);
      if (singles) ++retried;
      EXPECT_GT(after.batches, was.batches);
    }
    EXPECT_EQ(1, retried);  // Only the failed segment needed single acquires.
    EXPECT_EQ((std::vector<std::string>{"0z", "1z"}), allIds(index.h));
  }
}

TEST_F(StorageExampleTest, idLookupPrefetchMergedDuplicatePostings) {
  DeleteCostIndex index;
  ASSERT_TRUE(index.h.index(flatdoc("id", "dup"), UpdateMessage::COMMIT).success);
  ASSERT_TRUE(index.h.indexAll({flatdoc("id", "dup"), flatdoc("id", "keep")}, UpdateMessage::COMMIT).success);
  index.writer->mergeSegments();
  auto reader = index.writer->snapshots.readers.getReader();
  ASSERT_EQ(1u, reader->segments().size());
  const auto& seg = reader->segments().front();
  auto docs = seg.postingsReader().fieldInfo("id")->terms()->runs.front().docs;
  ASSERT_TRUE(docs);
  auto name = Postings::getIndexFileName(seg.segmentCore().segmentName(), docs.location.filenum());
  auto before = index.counters.fileStats(name);
  auto range = std::pair(docs.location.offset(), docs.bytes);
  ASSERT_FALSE(before.parts.contains(range));
  ASSERT_TRUE(index.h.index(flatdoc("id", "dup"), UpdateMessage::COMMIT, true).success);
  auto after = index.counters.fileStats(name);
  EXPECT_TRUE(after.parts.contains(range));
  EXPECT_FALSE(after.metadataParts.contains(range));
  EXPECT_EQ(after.batches - before.batches, after.submissions - before.submissions);
  EXPECT_EQ((std::vector<std::string>{"dup", "keep"}), allIds(index.h));
}

TEST_F(StorageExampleTest, idLookupPrefetchUnpublishedLiveDocsOnDemand) {
  DeleteCostIndex index;
  ASSERT_TRUE(index.h.indexAll({flatdoc("id", "a"), flatdoc("id", "b"), flatdoc("id", "keep")},
                               UpdateMessage::COMMIT).success);
  auto reader = index.writer->snapshots.readers.getReader();
  const auto& published = reader->segments().front();
  {
    // Leave a writer generation newer than the reader by failing before publication.
    Signal::listen("commitDataSync", [](void*, void*, void*) -> void* { throw std::runtime_error("test sync failure"); });
    auto cleanup = scope_guard([] { Signal::unlisten("commitDataSync"); });
    ExpectLog quiet("test sync failure");
    ASSERT_FALSE(index.h.deleteById("a", UpdateMessage::COMMIT).success);
  }
  const auto& seg = *index.writer->segInfos.at(published.segInfo.seg_id);
  ASSERT_GT(seg.liveGen, published.segInfo.live_gen);
  auto live = Postings::getLiveDocsFileName(published.segmentCore().segmentName(), seg.liveGen, seg.liveEpoch);
  auto before = index.counters.fileStats(live);
  auto base = index.baseFile(published);
  auto baseBefore = index.counters.fileStats(base);
  ASSERT_TRUE(index.h.deleteById("b", UpdateMessage::COMMIT).success);
  auto after = index.counters.fileStats(live);
  EXPECT_EQ(before.submissions + 1, after.submissions);
  EXPECT_EQ(before.batches, after.batches);
  EXPECT_EQ(baseBefore.submissions, index.counters.fileStats(base).submissions);
  EXPECT_EQ((std::vector<std::string>{"keep"}), allIds(index.h));
}

// A manifest open issues one batch per segment file: file 0's holds its header
// and catalog, every other file's its header. A snapshot open issues all of
// them before it waits on any, so a gate holding back every completion until
// the last batch arrives opens without timing out. It reads nothing more:
// version runs and ownership shapes wait for their first use.
TEST_F(StorageExampleTest, snapshotOpenIssuesEveryHeaderAndCatalogTogether) {
  LuxirConfig config;
  LuxirNode node(config);
  CollectionHelper h(node, "main");
  auto writer = h.getIndexWriter();
  for (int i = 0; i < 2; ++i) {
    ASSERT_TRUE(h.indexAll({flatdoc("id", std::to_string(i), "body_w", "needle")}, UpdateMessage::COMMIT).success);
  }
  // A vector larger than a stream's RAM buffer spills to a second file.
  std::vector<float> large(PostingsWriter::RAM_SPILL_BYTES / sizeof(float) + 1, 1.0f);
  ASSERT_TRUE(h.indexAll({flatdoc("id", "large", "vec_v", large)}, UpdateMessage::COMMIT).success);
  auto info = readDurableIndexInfo(writer->dir);
  ASSERT_EQ(3u, info->segments.size());
  std::vector<std::string> dataFiles;
  for (const auto& segment : info->segments) {
    for (const auto& file : segment.files) {
      if (Manifest::segmentDataFile(file.name)) dataFiles.emplace_back(file.name);
    }
  }
  ASSERT_GT(dataFiles.size(), info->segments.size());
  auto snapshot = *writer->snapshots.snapshot();
  snapshot.openCores.clear();

  auto counters = std::make_shared<StrictStorageCounters>();
  auto strict = std::make_shared<StrictDirectory>(std::shared_ptr<Directory>(writer, &writer->dir), counters,
                                                  StrictWholeFiles::DENY);
  GatedDirectory dir(strict, dataFiles.size(), std::chrono::seconds(5));
  auto reader = std::make_shared<IndexReader>(dir, snapshot);
  ASSERT_EQ(3u, reader->segments().size());
  EXPECT_FALSE(dir.gate.timedOut());
  EXPECT_EQ(dataFiles.size(), dir.gate.submissions());
  for (const auto& name : dataFiles) {
    auto stats = counters->fileStats(name);
    EXPECT_EQ(1u, stats.batches) << name;
    EXPECT_TRUE(stats.metadataParts.contains({0, Postings::LUXIR_HEADER.size()})) << name;
  }
  for (const auto& segment : info->segments) {
    auto name = Postings::getIndexFileName(Postings::segmentKey(segment.seg_id, segment.creator_epoch), 0);
    auto range = catalogRangeFromManifest(segment.catalog);
    EXPECT_TRUE(counters->fileStats(name).metadataParts.contains({range.offset, range.length})) << name;
  }
  // The open submitted those batches and nothing else; the first use of the
  // version runs acquires them as one more round.
  EXPECT_EQ(dataFiles.size(), counters->totalSubmissions());
  reader->segments().front().segmentCore().versionRuns();
  EXPECT_EQ(dataFiles.size() + 1, counters->totalSubmissions());

  // A later batch acquires only the parts the segment does not hold yet,
  // and slice() of them then makes no request. The catalog stays held; the
  // header's hold went once the open checked it.
  auto& core = reader->segments().front().postingsReader().core();
  auto name = Postings::getIndexFileName(core.segmentName(), 0);
  auto before = counters->fileStats(name);
  auto catalog = core.catalogRange();
  field_parts::Slice parts[] = {{{0, 0}, 2}, {{0, 2}, 3}, {{0, 0}, 2}, {{0, catalog.offset}, catalog.length},
                                {{0, 0}, Postings::LUXIR_HEADER.size()}};
  core.acquireSlices(parts, ReadClass::METADATA);
  auto after = counters->fileStats(name);
  EXPECT_EQ(before.batches + 1, after.batches);
  EXPECT_EQ(before.acquires + 3, after.acquires);
  auto bytes = core.slice(parts[1], ReadClass::METADATA);
  EXPECT_EQ(Postings::LUXIR_HEADER.substr(2, 3), std::string_view(bytes.data(), bytes.size()));
  EXPECT_EQ(core.slice(parts[0], ReadClass::METADATA).data(), core.slice(parts[2], ReadClass::METADATA).data());
  EXPECT_EQ(after.acquires, counters->fileStats(name).acquires);
}

// Batch failures surface in segment order, naming the segment, whichever
// completes first: the gate completes the second segment's batch first.
TEST_F(StorageExampleTest, openReportsTheLowestSegmentsBatchFailure) {
  LuxirConfig config;
  LuxirNode node(config);
  CollectionHelper h(node, "main");
  auto writer = h.getIndexWriter();
  for (int i = 0; i < 2; ++i) {
    ASSERT_TRUE(h.indexAll({flatdoc("id", std::to_string(i))}, UpdateMessage::COMMIT).success);
  }
  auto info = readDurableIndexInfo(writer->dir);
  ASSERT_EQ(2u, info->segments.size());
  auto strict = std::make_shared<StrictDirectory>(std::shared_ptr<Directory>(writer, &writer->dir),
                                                  std::make_shared<StrictStorageCounters>());
  GatedDirectory dir(strict, 2, std::chrono::seconds(5));
  std::vector<std::string> names;
  for (const auto& segment : info->segments) {
    names.push_back(Postings::segmentKey(segment.seg_id, segment.creator_epoch));
    dir.failing.insert(Postings::getIndexFileName(names.back(), 0));
  }
  std::vector<std::shared_ptr<SegmentCore>> cores(2);
  try {
    SegmentCore::open(dir, info->segments, cores);
    FAIL() << "the open should fail";
  } catch (const ApiError& e) {
    std::string_view message(e.what());
    EXPECT_TRUE(message.contains(std::format("Segment '{}'", names[0])) && !message.contains(names[1])) << message;
  }
  EXPECT_FALSE(dir.gate.timedOut());
}

// When a later segment's open throws, the open waits for the batches already
// in flight to complete into their cores before it releases them.
TEST_F(StorageExampleTest, failedOpenWaitsForBatchesInFlight) {
  LuxirConfig config;
  LuxirNode node(config);
  CollectionHelper h(node, "main");
  auto writer = h.getIndexWriter();
  ASSERT_TRUE(h.indexAll({flatdoc("id", "a")}, UpdateMessage::COMMIT).success);
  auto info = readDurableIndexInfo(writer->dir);
  ASSERT_EQ(1u, info->segments.size());
  std::vector<api::SegmentInfo> segments{info->segments[0], info->segments[0]};
  ++segments[1].creator_epoch;  // Names none of the listed files, so its open throws.

  auto counters = std::make_shared<StrictStorageCounters>();
  auto strict = std::make_shared<StrictDirectory>(std::shared_ptr<Directory>(writer, &writer->dir), counters);
  // Holds the first segment's batch for one from the second that never comes.
  GatedDirectory dir(strict, 2, std::chrono::milliseconds(50));
  std::vector<std::shared_ptr<SegmentCore>> cores(segments.size());
  EXPECT_THROW(SegmentCore::open(dir, segments, cores), std::runtime_error);
  EXPECT_EQ(1u, dir.gate.deliveries());
  EXPECT_TRUE(dir.gate.timedOut());
  EXPECT_EQ(0u, counters->liveStats().holds);
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

TEST_F(StorageExampleTest, ownershipBindsAfterLogicalPlanning) {
  CollectionHelper collection;
  CollectionHelper::UpdateBuilder input;
  input.addRaw([](auto& doc, auto& arena) {
    ASSERT_TRUE(api::read_json(doc, R"({"id":"a","items_nested":[{},{}]})", arena));
  });
  ASSERT_TRUE(collection.submit(input.commit()).success);
  expectDeferredBinding(collection, R"json({"ops":{"q":{"top_docs":{"limit":0,"count_limit":-1,
    "query":{"to_parent":{"path":"items_nested","query":{"all":true}}}}}}})json");
  expectDeferredBinding(collection, R"json({"ops":{"q":{"domain":{"to":"items_nested"},
    "top_docs":{"limit":0,"count_limit":-1}}}})json");
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

  // A failed read fails the request; the next one decodes the bitmap.
  auto search = [&] {
    auto req = localReq(h.getSearchEngine());
    req->topDocs().matchQuery("c_s", "x").countLimit(-1);
    req->execute(reader);
    return req;
  };
  counters->failNextRounds(1);
  EXPECT_FALSE(search()->ok());
  auto req = search();
  ASSERT_TRUE(req->ok()) << req->errorMsg();
  EXPECT_EQ(2, req->getMatchCount());
  EXPECT_EQ(1u, counters->fileStats(liveName).acquires);
  ASSERT_TRUE(leaf.liveDocs());
  EXPECT_EQ(2, leaf.liveDocs()->numLive());
}

// The writer opens a source's live docs at the size it recorded for them, so
// applying deletes to them and merging them make no size request on their files.
TEST_F(StorageExampleTest, mergeOpensLiveDocsAtTheirRecordedSize) {
  LuxirConfig config;
  config.store.backend = "ram";
  config.store.layers = {"strict"};
  config.store.options = {{"strict", {{"whole_segment_files", "deny"}}}};
  LuxirNode node(config);
  CollectionHelper h(node, "main");
  auto writer = h.getIndexWriter();
  ASSERT_TRUE(h.indexAll({flatdoc("id", "a"), flatdoc("id", "b"), flatdoc("id", "c")}, UpdateMessage::COMMIT).success);
  ASSERT_TRUE(h.indexAll({flatdoc("id", "d"), flatdoc("id", "e"), flatdoc("id", "f")}, UpdateMessage::COMMIT).success);
  auto liveFiles = [&](uint64_t gen) {
    std::vector<std::string> names;
    for (const auto& segment : readDurableIndexInfo(writer->dir)->segments) {
      EXPECT_EQ(gen, segment.live_gen);
      names.push_back(Postings::getLiveDocsFileName(Postings::segmentKey(segment.seg_id, segment.creator_epoch),
                                                    segment.live_gen, segment.live_epoch));
    }
    return names;
  };
  auto* layer = node.collections().storage().find<StrictLayer>();
  ASSERT_NE(nullptr, layer);
  auto expectOneReadNoSize = [&](const std::vector<std::string>& names) {
    ASSERT_EQ(2u, names.size());
    for (const auto& name : names) {
      auto stats = layer->counters->fileStats(name);
      EXPECT_EQ(1u, stats.acquires) << name;
      EXPECT_EQ(0u, stats.sizeRequests) << name;
    }
  };
  ASSERT_TRUE(h.deleteByIds({"a", "d"}, UpdateMessage::COMMIT).success);
  auto first = liveFiles(1);
  // No reader opens on these generations, so the delete path and the merge
  // read their files.
  ASSERT_TRUE(h.deleteByIds({"b", "e"}, UpdateMessage::COMMIT).success);
  expectOneReadNoSize(first);
  auto second = liveFiles(2);

  CollectionHelper::UpdateBuilder merge;
  ASSERT_TRUE(h.submit(merge.commit(true, 1)).success);
  auto reader = writer->snapshots.readers.getReader();
  ASSERT_EQ(1u, reader->segments().size());
  EXPECT_EQ(2, reader->liveDocs());
  expectOneReadNoSize(second);
}

TEST_F(StorageExampleTest, aggregateOrdinalsBindAfterLogicalPlanning) {
  CollectionHelper collection;
  ASSERT_TRUE(collection.index(flatdoc("id", "a", "cat_s", "red"), UpdateMessage::COMMIT).success);
  expectDeferredBinding(collection,
      R"json({"ops":{"z":"cardinality(cat_s)+1","a":"cardinality(cat_s)"}})json");
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
