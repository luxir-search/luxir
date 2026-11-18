// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0
#include <fstream>
#include "test/StorageTestUtils.h"
#include <map>
#include "test/CollectionHelper.h"
#include "test/TestUtils.h"
#include "luxir/query/TermQuery.h"
#include "luxir/reader/TestOverlayAuxReader.h"
#include "luxir/store/ReadOnlyDirectory.h"
#include "luxir/store/CorruptionMarker.h"
#include "luxir/server/TenantCatalog.h"
#include "luxir/util/Uuid.h"

using namespace luxir;
using namespace luxir::test;

namespace {
using Tree = std::map<std::string, std::string>;
Tree authoritativeBytes(const std::filesystem::path& path) {
  Tree bytes;
  for (const auto& entry : std::filesystem::recursive_directory_iterator(path)) {
    if (!entry.is_regular_file()) continue;
    if (entry.path().filename() == FSDirectory::kPublicationLock) continue;
    auto name = entry.path().lexically_relative(path).generic_string();
    if (name != "LAYOUT" && !name.starts_with("t/")) continue;
    std::ifstream input(entry.path(), std::ios::binary);
    bytes.emplace(name, std::string(std::istreambuf_iterator<char>(input), {}));
  }
  return bytes;
}
void copyBytes(const Tree& tree, DirectoryFactory& target) {
  for (const auto& [key, bytes] : tree) {
    auto path = std::filesystem::path(key);
    auto dir = target.directory(StorageRoot::AUTHORITATIVE, path.parent_path().generic_string());
    ASSERT_TRUE(dir->publishFile(path.filename().string(), std::as_bytes(std::span(bytes)), Directory::PublishMode::CREATE_ONLY));
  }
}
std::vector<int64_t> matches(IndexReader& reader, std::string_view term) {
  MemPool pool;
  Query::Context context(pool, reader);
  TermQuery query("id", term);
  auto* weight = query.createWeight(context, 0);
  std::vector<int64_t> hits;
  for (auto& segment : reader.segments()) {
    auto* scorer = weight->createScorer(pool, segment);
    if (!scorer) continue;
    for (auto doc = scorer->next(); doc != PostingsReader::END; doc = scorer->next()) {
      if (!segment.liveDocs() || segment.liveDocs()->bitset().get(doc)) hits.push_back(segment.base + doc);
    }
  }
  return hits;
}
}

TEST(StoragePortabilityTest, authoritativeBytesOpenOnAnotherBackend) {
  auto path = std::filesystem::temp_directory_path() / ("luxir-portable-" + newUuid());
  auto cleanup = scope_guard([&] { std::filesystem::remove_all(path); });
  SearchOverridesGuard guard(TestOverlayAuxReader::enabledForTests);
  TestOverlayAuxReader::enabledForTests = true;
  LuxirConfig config;
  config.store.backend = "fs";
  config.store.data_dir = path;
  {
    LuxirNode source(config);
    CollectionHelper main(source);
    ASSERT_TRUE(main.indexAll({flatdoc("id", "a"), flatdoc("id", "b")}, UpdateMessage::COMMIT).success);
    ASSERT_TRUE(main.index(flatdoc("id", "c"), UpdateMessage::COMMIT).success);
    ASSERT_TRUE(main.deleteById("b", UpdateMessage::COMMIT).success);
    main.commit({std::string(TestOverlayAuxReader::NAME)});
    CollectionHelper other(source, CollectionId{"t1", "other"});
    ASSERT_TRUE(other.index(flatdoc("id", "second"), UpdateMessage::COMMIT).success);
  }
  FSDirFactory source(path, true);
  TenantCatalog catalog(source);
  auto incarnation = catalog.current("main").value();
  auto files = path / "t/t0/i" / incarnation;
  for (auto name : {"STATE.tmp", "commit_999.1.tmp", "s01.1_00.tmp", "corrupted_ignored.tmp", "README"}) {
    std::ofstream(files / name) << "ignored";
  }
  std::filesystem::create_directories(files / "empty");
  std::filesystem::create_directories(path / "t/.notes");
  std::ofstream(path / "t/.notes/README") << "unrelated";
  auto original = authoritativeBytes(path);
  auto ram = std::make_unique<RAMDirFactory>();
  auto* writable = ram.get();
  copyBytes(original, *ram); // Deliberately lexical order: no readers until the stopped copy completes.
  ReadOnlyDirFactory readonly(std::move(ram));
  TenantCatalog::initialize(readonly, false);
  auto names = TenantCatalog::collections(readonly);
  ASSERT_EQ(2u, names.size());
  for (const auto& name : names) {
    TenantCatalog from(source, name.tenant), to(readonly, name.tenant);
    auto id = from.current(name.name).value();
    EXPECT_EQ(id, to.current(name.name));
    auto fsDir = from.directory(id), ramDir = to.directory(id);
    CommitSnapshotRegistry fsSnapshots(*fsDir), ramSnapshots(*ramDir);
    fsSnapshots.openLocalSnapshot(true); ramSnapshots.openLocalSnapshot(true);
    EXPECT_EQ(fsSnapshots.snapshot()->id, ramSnapshots.snapshot()->id);
    for (auto term : {"a", "b", "c", "second"}) {
      EXPECT_EQ(matches(*fsSnapshots.readers.getReader(), term), matches(*ramSnapshots.readers.getReader(), term));
    }
    EXPECT_FALSE(CorruptionMarker::present(*ramDir));
    if (name.tenant == "t0") {
      auto reader = ramSnapshots.readers.getReader();
      EXPECT_TRUE(reader->segments()[0].getAuxReader(TestOverlayAuxReader::NAME));
      EXPECT_EQ(1u, matches(*reader, "a").size());
      EXPECT_TRUE(matches(*reader, "b").empty());
      EXPECT_EQ(1u, matches(*reader, "c").size());
    }
  }
  TenantCatalog imported(*writable);
  auto dir = imported.directory(incarnation);
  auto epoch = IncarnationState::read(*dir)->value.epoch;
  CommitSnapshotRegistry snapshots(*dir);
  IndexWriter writer(snapshots, {}, nullptr, 10, incarnation);
  EXPECT_EQ(epoch + 1, IncarnationState::read(*dir)->value.epoch);
  auto& inverter = writer.obtainInverter();
  inverter.startDoc();
  std::string added = "clone";
  inverter.getIndexHandler("id").index(inverter, added);
  inverter.finishDoc();
  writer.releaseInverter(inverter);
  writer.commit();
  EXPECT_EQ(1u, matches(*snapshots.readers.getReader(), "clone").size());
  EXPECT_EQ(original, authoritativeBytes(path));
}

TEST(StoragePortabilityTest, copiedScratchAndUnknownFilesAreNotAnIndex) {
  RAMDir dir;
  for (auto name : {"STATE.tmp", "commit_999.1.tmp", "commit_3.tmp", "s01.1_00.tmp", "corrupted_ignored.tmp", "commit_x", "s01.1_unknown"}) {
    ASSERT_TRUE(dir.publishFile(name, {}, Directory::PublishMode::CREATE_ONLY));
    EXPECT_FALSE(Manifest::indexFile(name));
  }
  EXPECT_THROW(Manifest::load(dir), WriterSupersededError);
  reserveIncarnation(dir);
  EXPECT_FALSE(Manifest::load(dir).bytes);
  EXPECT_FALSE(CorruptionMarker::present(dir));
}
