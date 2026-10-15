// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include <gtest/gtest.h>

#include <cstring>
#include <numeric>
#include <set>
#include <string>
#include <vector>

#include "luxir/index/Sidecar.h"
#include "luxir/reader/SidecarReader.h"
#include "luxir/search/OrdMap.h"
#include "luxir/search/OrdMapStore.h"
#include "luxir/server/LuxirNode.h"
#include "test/CollectionHelper.h"
#include "test/DurableIndexInfo.h"
#include "test/LuxirTest.h"
#include "test/TestUtils.h"

using namespace luxir;
using namespace luxir::test;

class SidecarTest : public LuxirTest {
protected:
  std::unique_ptr<CollectionHelper> h;
  void SetUp() override { h = std::make_unique<CollectionHelper>(*luxirNode, "sidecar"); h->clear(); }
  void TearDown() override {
    h->getIndexWriter()->persistOrdMaps = IndexWriter::OrdMapPersistence::USED;
    h.reset();
  }

  Directory& dir() { return h->getIndexWriter()->dir; }

  void indexSegment(int base, int n) {
    for (int i = base; i < base + n; i++) {
      h->index(flatdoc("id", std::to_string(i), "color_s", "c" + std::to_string(i % 7),
                       "title_t", "red fox " + std::to_string(i), "num_i", (int64_t)i));
    }
    h->commit();
  }

  std::vector<std::pair<std::string, int64_t>> colorFacet() {
    auto request = localReq(h->getSearchEngine());
    request->collection("sidecar");
    auto& topDocs = request->topDocs();
    topDocs.allQuery();
    topDocs.facet("f", "color_s").limit(-1);
    request->execute(true);
    EXPECT_TRUE(request->ok()) << request->errorMsg();
    const auto& result = *request->docList("q")->ops.at("f")->facetResult();
    std::vector<std::pair<std::string, int64_t>> rows;
    const auto& ids = std::get<api::ColStr>(result.bucket_ids->kind).v;
    for (size_t i = 0; i < ids.size(); i++) rows.emplace_back(ids[i], result.counts[i]);
    return rows;
  }

  const api::AuxIndexInfo* ordMapsAux(const api::IndexInfo& info) {
    for (const auto& aux : info.aux_indexes) {
      if (aux.kind == OrdMapStore::KIND) return &aux;
    }
    return nullptr;
  }
};

TEST_F(SidecarTest, segmentSidecarCoversEveryDataFile) {
  indexSegment(0, 50);
  auto info = readDurableIndexInfo(dir());
  expectValidInventory(dir(), *info);
  ASSERT_EQ(1u, info->segments.size());
  const auto& seg = info->segments[0];
  auto name = sidecar::segmentFileName(seg.seg_id);
  const api::FileDescriptor* listed = nullptr;
  for (const auto& file : seg.files) if (file.name == name) listed = &file;
  ASSERT_NE(nullptr, listed);
  auto reader = SidecarReader::open(dir(), {name, listed->size, listed->xxh3});
  ASSERT_TRUE(reader);
  EXPECT_EQ(sidecar::CHUNK_SIZE, reader->chunkSize);
  std::set<sidecar::Kind> kinds;
  for (const auto& file : seg.files) {
    if (file.name == name) continue;
    auto* entry = reader->find(file.name);
    ASSERT_NE(nullptr, entry) << file.name;
    EXPECT_EQ(file.size, entry->size);
    EXPECT_EQ(file.xxh3, entry->xxh3);
    uint64_t end = 0;
    for (const auto& range : entry->ranges) {
      EXPECT_EQ(end, range.offset);
      EXPECT_EQ(sidecar::tierOf(range.kind), range.tier);
      end += range.length;
      kinds.insert(range.kind);
    }
    EXPECT_EQ(file.size, end);
  }
  for (auto kind : {sidecar::Kind::FILE_HEADER, sidecar::Kind::SEGMENT_INFO, sidecar::Kind::FIELD_DIRECTORY,
                    sidecar::Kind::TERM_INDEX, sidecar::Kind::TERM_BLOCKS, sidecar::Kind::DOC_POSTINGS,
                    sidecar::Kind::POSITIONS, sidecar::Kind::COLUMN, sidecar::Kind::STORED_FIELDS}) {
    EXPECT_TRUE(kinds.contains(kind)) << (int)kind;
  }

  auto ranges = SidecarReader::snapshotRanges(dir(), *info);
  ASSERT_FALSE(ranges.empty());
  EXPECT_TRUE(std::ranges::is_sorted(ranges, {}, &SidecarReader::Range::tier));
  EXPECT_EQ(0u, ranges.front().tier);
  EXPECT_TRUE(std::ranges::any_of(ranges, [](const auto& r) { return r.field == "color_s"; }));
}

TEST_F(SidecarTest, chunkHashesVerifyAndDetectAFlippedByte) {
  std::string bytes(sidecar::CHUNK_SIZE * 2 + 100, 'x');
  for (size_t i = 0; i < bytes.size(); i++) bytes[i] = (char)(i * 31);
  sidecar::FileMap map{"f", 0, 0, {}, {}};
  sidecar::hash(bytes, map);
  ASSERT_EQ(3u, map.chunks.size());
  SidecarReader reader;
  reader.chunkSize = sidecar::CHUNK_SIZE;
  SidecarReader::File file{"f", map.size, map.xxh3, map.chunks, {}};
  std::span<const char> all(bytes.data(), bytes.size());
  EXPECT_TRUE(reader.verifyChunk(file, 0, all.subspan(0, sidecar::CHUNK_SIZE)));
  EXPECT_TRUE(reader.verifyChunk(file, 2, all.subspan(2 * sidecar::CHUNK_SIZE)));
  EXPECT_FALSE(reader.verifyChunk(file, 2, all.subspan(2 * sidecar::CHUNK_SIZE, 50)));
  bytes[sidecar::CHUNK_SIZE + 7] ^= 1;
  EXPECT_FALSE(reader.verifyChunk(file, 1, all.subspan(sidecar::CHUNK_SIZE, sidecar::CHUNK_SIZE)));

  // Real segment files verify chunk by chunk.
  indexSegment(0, 20);
  auto info = readDurableIndexInfo(dir());
  auto seg = info->segments[0];
  auto name = sidecar::segmentFileName(seg.seg_id);
  for (const auto& listed : seg.files) {
    if (listed.name != name) continue;
    auto sidecarReader = SidecarReader::open(dir(), {name, listed.size, listed.xxh3});
    ASSERT_TRUE(sidecarReader);
    for (const auto& entry : sidecarReader->files) {
      auto data = dir().openFile(entry.name)->read();
      for (uint64_t c = 0; c < entry.chunks.size(); c++) {
        auto chunk = std::span(data).subspan(c * sidecar::CHUNK_SIZE);
        if (chunk.size() > sidecar::CHUNK_SIZE) chunk = chunk.first(sidecar::CHUNK_SIZE);
        EXPECT_TRUE(sidecarReader->verifyChunk(entry, c, chunk));
        std::string flipped(chunk.begin(), chunk.end());
        flipped[flipped.size() / 2] ^= 0x10;
        EXPECT_FALSE(sidecarReader->verifyChunk(entry, c, flipped));
      }
    }
  }
}

TEST_F(SidecarTest, corruptSidecarIsIgnored) {
  indexSegment(0, 10);
  auto info = readDurableIndexInfo(dir());
  auto name = sidecar::segmentFileName(info->segments[0].seg_id);
  std::string bytes(dir().openFile(name)->read());
  EXPECT_TRUE(SidecarReader::parse(bytes, name));
  bytes[3] ^= 1;
  EXPECT_FALSE(SidecarReader::parse(bytes, name));
  EXPECT_FALSE(SidecarReader::parse(bytes.substr(0, 10), name));
  // A sidecar that differs from its manifest descriptor is not used.
  EXPECT_FALSE(SidecarReader::open(dir(), {name, bytes.size(), 1}));
}

TEST_F(SidecarTest, snapshotSidecarCoversLiveDocsAndOrdersTransfers) {
  indexSegment(0, 30);
  indexSegment(30, 30);
  {
    auto info = readDurableIndexInfo(dir());
    EXPECT_FALSE(info->sidecar);  // nothing outside segment data yet
  }
  h->deleteById("3", UpdateMessage::COMMIT);
  auto info = readDurableIndexInfo(dir());
  expectValidInventory(dir(), *info);
  ASSERT_TRUE(info->sidecar);
  auto reader = SidecarReader::open(dir(), {std::string(info->sidecar->name), info->sidecar->size,
                                            info->sidecar->xxh3});
  ASSERT_TRUE(reader);
  bool liveDocs = false;
  for (const auto& file : reader->files) {
    if (file.name.find("__L") == std::string::npos) continue;
    liveDocs = true;
    ASSERT_EQ(1u, file.ranges.size());
    EXPECT_EQ(sidecar::Kind::LIVE_DOCS, file.ranges[0].kind);
    EXPECT_EQ(0u, file.ranges[0].tier);
  }
  EXPECT_TRUE(liveDocs);

  auto files = filesOf(*info);
  auto order = SidecarReader::transferOrder(dir(), *info, files);
  ASSERT_EQ(files.size(), order.size());
  auto sidecars = SidecarReader::sidecarsOf(*info);
  EXPECT_EQ(3u, sidecars.size());
  for (size_t i = 0; i < sidecars.size(); i++) {
    EXPECT_TRUE(std::ranges::any_of(sidecars, [&](const auto& s) { return s.name == order[i].name; }));
  }
}

TEST_F(SidecarTest, mergedSegmentGetsASidecar) {
  for (int i = 0; i < 4; i++) indexSegment(i * 20, 20);
  h->index(flatdoc("id", "last"), UpdateMessage::COMMIT, false, 1);
  auto info = readDurableIndexInfo(dir());
  ASSERT_EQ(1u, info->segments.size());
  expectValidInventory(dir(), *info);
  auto ranges = SidecarReader::snapshotRanges(dir(), *info);
  uint64_t covered = 0, data = 0;
  for (const auto& range : ranges) covered += range.length;
  auto name = sidecar::segmentFileName(info->segments[0].seg_id);
  for (const auto& file : info->segments[0].files) if (file.name != name) data += file.size;
  EXPECT_EQ(data, covered);
}

TEST_F(SidecarTest, persistedOrdMapsMatchAFreshBuild) {
  indexSegment(0, 40);
  indexSegment(40, 40);
  auto before = colorFacet();  // builds the map, so the next segment change persists it
  indexSegment(80, 40);
  auto info = readDurableIndexInfo(dir());
  auto* aux = ordMapsAux(*info);
  ASSERT_NE(nullptr, aux);
  EXPECT_EQ(info->core_gen, aux->built_core_gen);

  auto reader = h->getIndexWriter()->snapshots.readers.getReader();
  ASSERT_NE(nullptr, reader->ordMapStore());
  ASSERT_TRUE(reader->ordMapStore()->contains("color_s"));
  auto loaded = reader->getOrdMap("color_s");
  auto built = OrdMap::build("color_s", *reader);
  ASSERT_NE(nullptr, loaded);
  ASSERT_NE(nullptr, built);
  EXPECT_EQ(built->numOrds(), loaded->numOrds());
  EXPECT_EQ(built->firstFullSeg(), loaded->firstFullSeg());
  EXPECT_TRUE(std::ranges::equal(built->bytes(), loaded->bytes()));
  ASSERT_EQ(built->topTerms().entries.size(), loaded->topTerms().entries.size());
  for (size_t i = 0; i < built->topTerms().entries.size(); i++) {
    EXPECT_EQ(built->topTerms().entries[i].ord, loaded->topTerms().entries[i].ord);
    EXPECT_EQ(built->topTerms().entries[i].df, loaded->topTerms().entries[i].df);
  }
  EXPECT_EQ(built->topTerms().unlistedBound, loaded->topTerms().unlistedBound);

  auto persisted = colorFacet();
  h->getIndexWriter()->persistOrdMaps = IndexWriter::OrdMapPersistence::OFF;
  h->deleteById("nonexistent", UpdateMessage::COMMIT);
  auto unpersisted = readDurableIndexInfo(dir());
  EXPECT_EQ(nullptr, ordMapsAux(*unpersisted));
  EXPECT_EQ(persisted, colorFacet());
  EXPECT_EQ(120, std::accumulate(persisted.begin(), persisted.end(), (int64_t)0,
                                 [](int64_t sum, const auto& row) { return sum + row.second; }));
  EXPECT_EQ(7u, before.size());
}

TEST_F(SidecarTest, deletesOnlyCommitKeepsOrdMaps) {
  h->getIndexWriter()->persistOrdMaps = IndexWriter::OrdMapPersistence::ALL;
  indexSegment(0, 30);
  indexSegment(30, 30);
  auto first = readDurableIndexInfo(dir());
  auto* aux = ordMapsAux(*first);
  ASSERT_NE(nullptr, aux);
  std::string file(aux->files[0].name);
  h->deleteById("5", UpdateMessage::COMMIT);
  auto second = readDurableIndexInfo(dir());
  EXPECT_EQ(first->core_gen, second->core_gen);
  auto* kept = ordMapsAux(*second);
  ASSERT_NE(nullptr, kept);
  EXPECT_EQ(file, kept->files[0].name);
  // The snapshot sidecar describes the persisted maps.
  auto ranges = SidecarReader::snapshotRanges(dir(), *second);
  EXPECT_TRUE(std::ranges::any_of(ranges, [](const auto& r) { return r.kind == sidecar::Kind::ORDMAP_DIRECTORY; }));
  // A new segment rebuilds them for the new composition.
  indexSegment(60, 10);
  auto third = readDurableIndexInfo(dir());
  ASSERT_NE(nullptr, ordMapsAux(*third));
  EXPECT_NE(file, ordMapsAux(*third)->files[0].name);
}

TEST_F(SidecarTest, corruptOrdMapStoreFallsBack) {
  h->getIndexWriter()->persistOrdMaps = IndexWriter::OrdMapPersistence::ALL;
  indexSegment(0, 30);
  indexSegment(30, 30);
  auto info = readDurableIndexInfo(dir());
  auto* aux = ordMapsAux(*info);
  ASSERT_NE(nullptr, aux);
  std::string bytes(dir().openFile(aux->files[0].name)->read());
  auto reader = h->getIndexWriter()->snapshots.readers.getReader();
  auto open = [&](const std::string& content) {
    auto copy = std::make_unique<char[]>(content.size());
    std::memcpy(copy.get(), content.data(), content.size());
    return OrdMapStore("ordmaps", aux->gen, aux->built_core_gen,
                       std::make_shared<RAMInputFile>(std::move(copy), content.size()));
  };
  auto intact = open(bytes);
  ASSERT_TRUE(intact.contains("color_s"));
  ASSERT_NE(nullptr, intact.load("color_s"));
  // A flipped payload byte fails that field's digest; the reader would build.
  auto payload = bytes;
  payload[0] ^= 1;
  EXPECT_EQ(nullptr, open(payload).load("color_s"));
  // A flipped directory byte invalidates the whole store.
  auto directory = bytes;
  directory[directory.size() - 20] ^= 1;
  EXPECT_FALSE(open(directory).contains("color_s"));
  std::vector<OrdMapStore::Segment> none;
  EXPECT_FALSE(open(directory).matches(info->core_gen, none));
  EXPECT_FALSE(intact.matches(info->core_gen, none));
  // Fallback still answers the same.
  EXPECT_EQ(7u, colorFacet().size());
}
