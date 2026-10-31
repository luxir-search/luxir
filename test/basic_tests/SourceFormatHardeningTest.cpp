// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

// Untrusted-bytes checks of the source format, the lz4 codec and block caps,
// and the file-name rules the sweeps rely on.

#include <gtest/gtest.h>

#include "luxir/source/SourceFormat.h"
#include "luxir/store/Manifest.h"
#include "luxir/store/SourceFiles.h"

namespace luxir::test {

using namespace luxir::source;

namespace {

struct StringSink : Sink {
  std::string bytes;
  void write(const char* data, size_t len) override { bytes.append(data, len); }
};

std::string varint(uint64_t v) {
  std::string out;
  while (v >= 0x80) {
    out.push_back((char)(v | 0x80));
    v >>= 7;
  }
  out.push_back((char)v);
  return out;
}

}  // namespace

TEST(SourceFormatHardening, claimedCountBeyondThePayloadIsRefused) {
  DecodedBlock block;
  // One record's worth of bytes claiming a billion records.
  std::string payload = varint(1'000'000'000) + varint(1) + varint(1) + varint(0) + std::string("\0ab", 3);
  EXPECT_FALSE(block.parse(payload, 0, 1'000'000'000));
  // A consistent single record parses.
  std::string one = varint(1) + varint(1) + varint(1) + varint(7) + std::string(1, (char)kFlagMinifiedJson) + "ab";
  ASSERT_TRUE(block.parse(one, 3, 1));
  auto record = block.record(0);
  EXPECT_EQ("a", record.id);
  EXPECT_EQ("b", record.body);
  EXPECT_EQ(7u, record.version);
}

TEST(SourceFormatHardening, tenByteVarintWithHighBitsIsRefused) {
  DecodedBlock block;
  std::string overflow(9, (char)0xff);
  overflow.push_back((char)0x7f);  // bits 63 and up set in the tenth byte
  std::string payload = varint(1) + varint(0) + varint(0) + overflow + std::string(1, 0);
  EXPECT_FALSE(block.parse(payload, 0, 1));
  std::string max(9, (char)0xff);
  max.push_back((char)0x01);  // exactly UINT64_MAX
  std::string fine = varint(1) + varint(0) + varint(0) + max + std::string(1, 0);
  ASSERT_TRUE(block.parse(fine, 0, 1));
  EXPECT_EQ(UINT64_MAX, block.record(0).version);
}

TEST(SourceFormatHardening, lz4BlocksRoundTripAndDocCapSealsBlocks) {
  WriterOptions options;
  options.codec = Codec::LZ4;
  options.blockBytes = 1 << 20;
  options.maxDocsPerBlock = 4;
  StringSink sink;
  FileWriter writer(sink, options, "lz4", 1);
  for (uint32_t i = 0; i < 10; i++) {
    writer.add({"id" + std::to_string(i), std::string(200, (char)('a' + i)), i, kFlagMinifiedJson});
  }
  writer.finish();
  auto file = FileReader::open(sink.bytes);
  ASSERT_TRUE(file.has_value());
  EXPECT_EQ(3u, file->blocks().size());  // 4 + 4 + 2 records
  EXPECT_EQ(10u, file->docCount());
  Codecs codecs;
  DecodedBlock block;
  FileReader::RawBlock raw;
  ASSERT_TRUE(file->rawBlock(0, raw));
  EXPECT_EQ(Codec::LZ4, raw.codec);
  for (uint32_t ord = 0; ord < 10; ord++) {
    auto index = file->blockFor(ord);
    ASSERT_GE(index, 0);
    ASSERT_TRUE(file->readBlock((size_t)index, codecs, block));
    EXPECT_EQ("id" + std::to_string(ord), block.record(ord - block.firstOrdinal).id);
  }
  // The hinted lookup agrees with the search.
  EXPECT_EQ(2, file->blockFor(9, 1));
  EXPECT_EQ(0, file->blockFor(1, 2));
}

TEST(SourceFormatHardening, pendingBytesFollowTheBuffers) {
  StringSink sink;
  FileWriter writer(sink, {}, "ram", 1);
  size_t empty = writer.pendingBytes();
  writer.add({"id", std::string(4000, 'x'), 1, kFlagMinifiedJson});
  EXPECT_GE(writer.pendingBytes(), empty + 4000);
}

TEST(SourceFiles, namesAreStrict) {
  EXPECT_TRUE(isSourceFile("v00"));
  EXPECT_TRUE(isSourceFile("v0a"));
  EXPECT_TRUE(isSourceFile("v110"));
  EXPECT_TRUE(isSourceFile("t00"));
  EXPECT_TRUE(isSourceMapFile("s00__S01"));
  EXPECT_FALSE(isSourceFile("tmp123"));   // length prefix 'm' does not match the digits
  EXPECT_TRUE(isSourceFile("v01"));       // seq 1: length prefix 0, one digit
  EXPECT_FALSE(isSourceFile("v100"));     // a leading zero digit
  EXPECT_FALSE(isSourceFile("v0"));
  EXPECT_FALSE(isSourceFile("s00_01"));
  EXPECT_FALSE(isSourceFile("version"));
  // A write that never finished is swept with its kind.
  EXPECT_TRUE(Manifest::indexFile("v00.tmp"));
  EXPECT_TRUE(Manifest::indexFile("s00__S01.tmp"));
  EXPECT_FALSE(Manifest::indexFile("notes.tmp"));
}

}  // namespace luxir::test
