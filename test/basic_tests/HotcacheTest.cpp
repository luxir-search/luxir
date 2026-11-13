// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include <gtest/gtest.h>

#include <algorithm>
#include <map>
#include <set>

#include "luxir/reader/Hotcache.h"
#include "luxir/reader/Postings.h"
#include "luxir/reader/PostingsReader.h"
#include "luxir/index/PostingsWriter.h"
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
  EXPECT_EQ("s01.1__hot", Hotcache::fileName(1, 1));
  EXPECT_EQ("s110.a__hot", Hotcache::fileName(36, 10));
  auto data = Hotcache::parseDataFile("s01.2_03");
  ASSERT_TRUE(data);
  EXPECT_EQ("01.2", data->segStr);
  EXPECT_EQ(3u, data->fileNum);
  EXPECT_EQ(36u, Hotcache::parseDataFile("s0a.1_110")->fileNum);
  for (std::string_view other : {"s01.1__hot", "s01.1__L01.1", "s01.1__vec.title_v_00_00", "commit_3.1", "STATE", "s01.1",
                                 "s01.1_", "s01.1_0", "x01.1_00", "s01.1_00_00", "s01.1_0A", "s01_00", "s01._00",
                                 "s01.0_00", "s01.1.1_00"}) {
    EXPECT_FALSE(Hotcache::parseDataFile(other)) << other;
  }
}

TEST(HotcacheTest, defaultRangeStreamSlicesTheWholeStream) {
  RAMInputFile ram(std::make_unique_for_overwrite<char[]>(10), 10);
  EXPECT_FALSE(ram.lazy());
  EXPECT_EQ(4, ram.rangeStream(3, 4).size());
  EXPECT_EQ(0, ram.rangeStream(10, 0).left());
  EXPECT_THROW(ram.rangeStream(8, 3), FileIOException);
  std::vector<ByteRange> ranges{{0, 5}};
  EXPECT_NO_THROW(ram.prefetchRanges(ranges));  // memory needs no readahead
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

namespace {

// A directory that says its bytes are fetched over a network, so a finished
// segment gets a hotcache.
class RemoteDirectory : public DelegatingDirectory {
public:
  using DelegatingDirectory::DelegatingDirectory;
  Capabilities capabilities() const override { return delegate->capabilities() | REMOTE; }
};

// A one-document segment with one stored column, as CompoundFileTest writes it.
void writeSegment(Directory& dir) {
  PostingsWriter writer(dir, 0, 1, 1);
  auto out = writer.getOutputStream();
  auto& field = writer.addField("f");
  field.type = FieldType::BIN;
  field.flags = FieldType::COLUMN_STORED;
  field.docsWithField = 1;
  field.numValues = 1;
  field.columnLoc = out->slocation();
  field.columnMetaOff = 1;
  out->write((char)0x7f);
  out.reset();
  writeUnversionedRuns(writer);
  writer.finish();
}
}  // namespace

TEST(HotcacheTest, aRemoteDirectoryGetsTheHotcacheWithTheSegment) {
  auto ram = std::make_shared<RAMDir>();
  RemoteDirectory remote(ram);
  writeSegment(remote);
  auto hot = ram->openFile(Hotcache::fileName(0, 1));
  ASSERT_NE(nullptr, hot);
  EXPECT_FALSE(Hotcache::decode(hot->read()).entries.empty());

  RAMDir local;
  writeSegment(local);
  EXPECT_EQ(nullptr, local.openFile(Hotcache::fileName(0, 1)));  // local opens read nothing remotely
}

// A real segment, indexed by a node and copied into a directory of its own.
class HotcacheSegmentTest : public LuxirTest {
protected:
  std::shared_ptr<RAMDir> dir = std::make_shared<RAMDir>();
  uint64_t segId = 0;
  uint64_t epoch = 0;
  std::string segStr;
  std::vector<std::string> dataFiles;

  void SetUp() override {
    LuxirTest::SetUp();
    LuxirNode node;
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
    auto source = node.getCollection("main")->getShard()->getDirectory();
    std::vector<Directory::FileInfo> files;
    source->listFiles(files);
    std::map<std::string, std::vector<std::string>> bySegment;
    for (const auto& f : files) {
      if (auto parsed = Hotcache::parseDataFile(f.name)) bySegment[std::string(parsed->segStr)].push_back(f.name);
    }
    ASSERT_FALSE(bySegment.empty());
    for (const auto& [seg, names] : bySegment) {
      if (names.size() > dataFiles.size()) {
        segStr = seg;
        dataFiles = names;
      }
    }
    for (const auto& name : dataFiles) dir->copyFile(*source, name);
    auto dot = segStr.find('.');
    segId = Postings::parseSortableString(std::string_view(segStr).substr(0, dot));
    epoch = Postings::parseEpoch(std::string_view(segStr).substr(dot + 1));
  }

  Hotcache writeHotcache() {
    RemoteDirectory remote(dir);
    std::vector<std::string> names;
    std::vector<FileDescriptor> descriptors;
    writeHotcacheIfRemote(remote, segId, epoch, &names, &descriptors);
    EXPECT_EQ(std::vector<std::string>{Hotcache::fileName(segStr)}, names);
    EXPECT_EQ(1u, descriptors.size());
    auto file = dir->openFile(Hotcache::fileName(segStr));
    if (!file) throw std::runtime_error("no hotcache written");
    return Hotcache::decode(file->read());
  }
};

TEST_F(HotcacheSegmentTest, theHotcacheIsSortedAlignedAndSmall) {
  ASSERT_GE(dataFiles.size(), 2u);  // the segment spans several files
  auto cache = writeHotcache();
  std::map<uint32_t, uint64_t> sizes;
  for (const auto& name : dataFiles) sizes[Hotcache::parseDataFile(name)->fileNum] = dir->openFile(name)->size();
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
  EXPECT_EQ(sizes.size(), covered.size());  // every file's header is in it
  uint64_t hotBytes = 0;
  for (const auto& e : cache.entries) hotBytes += e.range.length;
  uint64_t total = 0;
  for (const auto& [num, size] : sizes) total += size;
  EXPECT_LT(hotBytes * 2, total);  // each file costs at least a page of it
}

// Opening the segment over lazy files reads only ranges the hotcache holds, so a
// directory that prefetches them serves the whole open from that one batch.
TEST_F(HotcacheSegmentTest, openingASegmentReadsNothingOutsideTheHotcache) {
  auto cache = writeHotcache();
  RecordingDirectory recording(*dir);
  auto reader = PostingsReader::create(recording, segId, epoch);
  EXPECT_EQ(1500, reader->maxDoc());
  EXPECT_GT(reader->numFields(), 3);
  ASSERT_NE(nullptr, reader->fieldInfo("a_un"));  // the field catalog is part of the open
  auto recorded = recording.recorded();
  EXPECT_FALSE(recorded.empty());
  for (const auto& [name, ranges] : recorded) {
    auto data = Hotcache::parseDataFile(name);
    ASSERT_TRUE(data) << name;
    auto hot = cache.rangesOf(data->fileNum);
    for (const auto& range : ranges) {
      bool inside = std::any_of(hot.begin(), hot.end(), [&](const ByteRange& h) {
        return range.offset >= h.offset && range.offset + range.length <= h.offset + h.length;
      });
      EXPECT_TRUE(inside) << name << " [" << range.offset << ", +" << range.length << ")";
    }
  }
}
