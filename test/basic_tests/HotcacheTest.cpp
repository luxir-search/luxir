// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include <gtest/gtest.h>

#include <filesystem>
#include <map>
#include <set>

#include "luxir/reader/Hotcache.h"
#include "luxir/reader/Postings.h"
#include "luxir/reader/PostingsReader.h"
#include "luxir/store/ObjectStoreDirectory.h"
#include "luxir/util/Uuid.h"
#include "test/CollectionHelper.h"
#include "test/LuxirTest.h"
#include "test/TestUtils.h"

using namespace luxir;
using namespace luxir::test;

TEST(HotcacheTest, roundTripsAndRejectsDamage) {
  Hotcache cache;
  cache.entries = {{0, {0, 4096}}, {0, {8192, 12288}}, {3, {0, 4096}}, {7, {1ULL << 40, 4096}}};
  std::string bytes = cache.encode();
  auto decoded = Hotcache::decode(bytes);
  ASSERT_EQ(cache.entries.size(), decoded.entries.size());
  for (size_t i = 0; i < cache.entries.size(); i++) {
    EXPECT_EQ(cache.entries[i].fileNum, decoded.entries[i].fileNum);
    EXPECT_EQ(cache.entries[i].range.offset, decoded.entries[i].range.offset);
    EXPECT_EQ(cache.entries[i].range.length, decoded.entries[i].range.length);
  }
  auto ranges = decoded.rangesOf(0);
  ASSERT_EQ(2u, ranges.size());
  EXPECT_EQ(8192u, ranges[1].offset);
  EXPECT_TRUE(decoded.rangesOf(5).empty());
  EXPECT_TRUE(Hotcache::decode(Hotcache{}.encode()).entries.empty());

  for (size_t flip : {size_t(0), size_t(9), bytes.size() / 2, bytes.size() - 1}) {
    std::string damaged = bytes;
    damaged[flip] = (char)~damaged[flip];
    EXPECT_THROW(Hotcache::decode(damaged), std::runtime_error) << flip;
  }
  EXPECT_THROW(Hotcache::decode(bytes.substr(0, bytes.size() - 3)), std::runtime_error);
  EXPECT_THROW(Hotcache::decode(""), std::runtime_error);
}

TEST(HotcacheTest, namesAreRecognized) {
  EXPECT_EQ("s01__hot", Hotcache::fileName(1));
  EXPECT_EQ("s110__hot", Hotcache::fileName(36));
  auto data = Hotcache::parseDataFile("s01_03");
  ASSERT_TRUE(data);
  EXPECT_EQ("01", data->segStr);
  EXPECT_EQ(3u, data->fileNum);
  EXPECT_EQ(36u, Hotcache::parseDataFile("s0a_110")->fileNum);
  for (std::string_view other : {"s01__hot", "s01__L01", "s01__vec.title_v_00_00", "commit_3", "CURRENT", "s01", "s01_",
                                 "s01_0", "x01_00", "s01_00_00", "s01_0A"}) {
    EXPECT_FALSE(Hotcache::parseDataFile(other)) << other;
  }
}

TEST(HotcacheTest, recordingDirectoryNotesEveryRangeRead) {
  RAMDir ram;
  {
    auto file = ram.createFile("f");
    OutputStream out(file.get());
    out.writeBytes(std::string(100, 'x'));
    out.close();
    ram.finishFile(*file);
  }
  RecordingDirectory recording(ram);
  auto input = recording.openFile("f");
  EXPECT_TRUE(input->lazy());
  EXPECT_EQ(nullptr, recording.openFile("absent"));
  input->rangeStream(10, 5);
  std::vector<ByteRange> ranges{{50, 10}, {70, 0}};
  input->prefetchRanges(ranges);
  EXPECT_EQ(2u, recording.recorded()["f"].size());  // the empty range is not recorded
  input->read();
  auto recorded = recording.recorded()["f"];
  ASSERT_EQ(3u, recorded.size());
  EXPECT_EQ(10u, recorded[0].offset);
  EXPECT_EQ(5u, recorded[0].length);
  EXPECT_EQ(0u, recorded[2].offset);
  EXPECT_EQ(100u, recorded[2].length);
}

// A real segment, written through the ram_object backend and read back through
// a fresh directory, so that nothing is staged locally.
class HotcacheSegmentTest : public LuxirTest {
protected:
  std::string storeName;
  std::shared_ptr<RamObjectStore> store;
  std::filesystem::path staging;
  LuxirConfig config;
  std::string key;
  ObjectStoreDirectory::Options options;

  void SetUp() override {
    LuxirTest::SetUp();
    storeName = "hot_" + newUuid();
    staging = std::filesystem::temp_directory_path() / ("luxir-hot-" + newUuid());
    store = RamObjectStore::named(storeName);
    config.store.backend = "ram_object";
    config.store.options = {{"store_name", storeName}, {"chunk_bytes", "4096"}, {"staging_dir", staging.string()},
                            {"spill_bytes", "8K"}};
    options.chunkBytes = 4096;
    {
      LuxirNode node(config);
      CollectionHelper h(node, "main");
      std::vector<Doc> docs;
      for (int i = 0; i < 1500; i++) {
        std::string a, b;
        for (int w = 0; w < 12; w++) {
          a += "alpha" + std::to_string((i * 7 + w * 13) % 997) + " ";
          b += "beta" + std::to_string((i * 11 + w * 17) % 1009) + " ";
        }
        docs.push_back(flatdoc("id", "d" + std::to_string(i), "a_un", a, "b_un", b, "n_i", (int64_t)(i % 31),
                               "c_s", "c" + std::to_string(i % 7)));
      }
      ASSERT_TRUE(h.indexAll(docs, UpdateMessage::COMMIT).success);
      key = "t/t0/main/" + node.getCollection("main")->getShard()->getSnapshots().snapshot()->id.incarnation + "/";
    }
  }
  void TearDown() override {
    store.reset();
    RamObjectStore::drop(storeName);
    std::filesystem::remove_all(staging);
    LuxirTest::TearDown();
  }

  struct Segment {
    uint64_t id = 0;
    std::string segStr;
    std::vector<std::string> dataFiles;
  };
  Segment segment(ObjectStoreDirectory& dir) {
    Segment seg;
    std::vector<Directory::FileInfo> files;
    dir.listFiles(files);
    for (const auto& f : files) {
      if (auto parsed = Hotcache::parseDataFile(f.name)) {
        seg.segStr = std::string(parsed->segStr);
        seg.id = Postings::parseSortableString(parsed->segStr);
        seg.dataFiles.push_back(f.name);
      }
    }
    return seg;
  }
};

// Directory that hands out files opened up front, so their fetch counters stay visible.
class PreopenedDirectory : public DelegatingDirectory {
public:
  std::map<std::string, std::shared_ptr<InputFile>, std::less<>> files;
  explicit PreopenedDirectory(std::shared_ptr<Directory> inner) : DelegatingDirectory(std::move(inner)) {}
  std::shared_ptr<InputFile> openFile(std::string_view name, bool expectSynced = false) override {
    auto it = files.find(name);
    return it != files.end() ? it->second : delegate->openFile(name, expectSynced);
  }
};

TEST_F(HotcacheSegmentTest, openingASegmentFetchesNothingOutsideTheHotcache) {
  auto dir = std::make_shared<ObjectStoreDirectory>(store, key, options);
  auto seg = segment(*dir);
  ASSERT_GE(seg.dataFiles.size(), 2u);  // the segment spans several files

  // The hotcache: sorted, aligned, within the files, and every file's header is in it.
  auto hotFile = dir->openFile(Hotcache::fileName(seg.segStr));
  ASSERT_NE(nullptr, hotFile);
  auto cache = Hotcache::decode(hotFile->read());
  std::map<uint32_t, uint64_t> sizes;
  for (const auto& name : seg.dataFiles) sizes[Hotcache::parseDataFile(name)->fileNum] = dir->openFile(name)->size();
  std::set<uint32_t> covered;
  const Hotcache::Entry* previous = nullptr;
  for (const auto& e : cache.entries) {
    EXPECT_EQ(0u, e.range.offset % Hotcache::kAlign);
    EXPECT_GT(e.range.length, 0u);
    ASSERT_TRUE(sizes.contains(e.fileNum));
    EXPECT_LE(e.range.offset + e.range.length, sizes[e.fileNum]);
    if (previous && previous->fileNum == e.fileNum) {
      EXPECT_GT(e.range.offset, previous->range.offset + previous->range.length);
    }
    previous = &e;
    covered.insert(e.fileNum);
  }
  EXPECT_EQ(sizes.size(), covered.size());
  uint64_t hotBytes = 0;
  for (const auto& e : cache.entries) hotBytes += e.range.length;
  uint64_t total = 0;
  for (const auto& [num, size] : sizes) total += size;
  EXPECT_LT(hotBytes * 4, total);

  // Open every file the way the directory does (prefetching the hotcache ranges), then
  // open the segment over those very files: no file fetches anything more.
  auto fresh = std::make_shared<ObjectStoreDirectory>(store, key, options);
  PreopenedDirectory pre(fresh);
  std::map<std::string, std::shared_ptr<LazyInputFile>> lazy;
  for (const auto& name : seg.dataFiles) {
    pre.files[name] = fresh->openFile(name);
    lazy[name] = std::dynamic_pointer_cast<LazyInputFile>(pre.files[name]);
    ASSERT_NE(nullptr, lazy[name]) << name;
  }
  std::map<std::string, uint64_t> before;
  for (const auto& [name, file] : lazy) before[name] = file->fetchRequests();
  store->resetStats();
  auto reader = PostingsReader::create(pre, seg.id);
  EXPECT_EQ(1500, reader->maxDoc());
  EXPECT_GT(reader->numFields(), 3);
  ASSERT_NE(nullptr, reader->fieldInfo("a_un"));  // the field catalog is part of the open
  EXPECT_EQ(0u, store->requests("read"));
  for (const auto& [name, file] : lazy) EXPECT_EQ(before[name], file->fetchRequests()) << name;
  uint64_t fetched = 0;
  for (const auto& [name, file] : lazy) fetched += file->fetchedBytes();
  EXPECT_LT(fetched * 4, total);
}

TEST_F(HotcacheSegmentTest, aMissingOrDamagedHotcacheOnlyCostsRequests) {
  auto dir = std::make_shared<ObjectStoreDirectory>(store, key, options);
  auto seg = segment(*dir);
  auto openCost = [&] {
    auto fresh = std::make_shared<ObjectStoreDirectory>(store, key, options);
    store->resetStats();
    auto reader = PostingsReader::create(*fresh, seg.id);
    EXPECT_EQ(1500, reader->maxDoc());
    EXPECT_NE(nullptr, reader->fieldInfo("b_un"));
    return store->requests("read", key + "s");
  };
  uint64_t withHot = openCost();

  std::string hotName = key + Hotcache::fileName(seg.segStr);
  store->put(hotName, "not a hotcache");
  uint64_t damaged = openCost();

  store->remove(hotName);
  uint64_t without = openCost();

  std::cout << "open reads: with hotcache " << withHot << ", damaged " << damaged << ", without " << without << "\n";
  // The hotcache turns the open's dependent reads into one batch known up front; a segment
  // without one still opens with a handful of small reads per file.
  EXPECT_LE(withHot, 2 * seg.dataFiles.size() + 1);
  EXPECT_LE(damaged, 2 * seg.dataFiles.size() + 2);  // the unusable hotcache itself, then as without
  EXPECT_LE(without, 6 * seg.dataFiles.size());
}
