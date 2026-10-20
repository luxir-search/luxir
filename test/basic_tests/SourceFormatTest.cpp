// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include <gtest/gtest.h>

#include "luxir/source/SourceFormat.h"
#include "luxir/source/SourceJson.h"

namespace luxir::test {

using namespace luxir::source;

namespace {

struct StringSink : Sink {
  std::string bytes;
  void write(const char* data, size_t len) override { bytes.append(data, len); }
};

std::string docBody(int i) {
  return "{\"id\":\"d" + std::to_string(i) + "\",\"n\":\"" + std::string(40 + i % 7, (char)('a' + i % 26)) + "\"}";
}

// 300 records over several small blocks; record i has id "d<i>" and version i + 1.
std::string writeFile(WriterOptions options = {}, uint32_t count = 300) {
  options.blockBytes = 512;
  StringSink sink;
  FileWriter writer(sink, options, "test", 1234);
  for (uint32_t i = 0; i < count; i++) {
    std::string id = "d" + std::to_string(i);
    std::string body = docBody((int)i);
    writer.add({id, body, i + 1, kFlagMinifiedJson});
  }
  writer.finish();
  return sink.bytes;
}

void expectRecord(const DecodedBlock& block, uint32_t ordinal) {
  auto r = block.record(ordinal - block.firstOrdinal);
  EXPECT_EQ(r.id, "d" + std::to_string(ordinal));
  EXPECT_EQ(r.body, docBody((int)ordinal));
  EXPECT_EQ(r.version, ordinal + 1);
  EXPECT_EQ(r.flags, kFlagMinifiedJson);
}

}  // namespace

TEST(SourceFormat, RoundTripsEveryRecord) {
  auto bytes = writeFile();
  auto file = FileReader::open(bytes);
  ASSERT_TRUE(file);
  EXPECT_TRUE(file->footerVerified());
  EXPECT_EQ(file->docCount(), 300u);
  EXPECT_EQ(file->header().origin, "test");
  EXPECT_EQ(file->header().createdMs, 1234u);
  EXPECT_GT(file->blocks().size(), 3u);

  Codecs codecs;
  for (uint32_t ordinal = 0; ordinal < 300; ordinal++) {
    auto block = file->blockFor(ordinal);
    ASSERT_GE(block, 0);
    DecodedBlock decoded;
    ASSERT_TRUE(file->readBlock((size_t)block, codecs, decoded));
    expectRecord(decoded, ordinal);
  }
  EXPECT_EQ(file->blockFor(300), -1);
}

TEST(SourceFormat, UncompressedBlocksRead) {
  WriterOptions options;
  options.codec = Codec::NONE;
  auto bytes = writeFile(options, 40);
  auto file = FileReader::open(bytes);
  ASSERT_TRUE(file);
  Codecs codecs;
  DecodedBlock decoded;
  ASSERT_TRUE(file->readBlock(0, codecs, decoded));
  expectRecord(decoded, 0);
}

TEST(SourceFormat, CorruptBlockLosesOnlyItsRecords) {
  auto bytes = writeFile();
  auto clean = FileReader::open(bytes);
  ASSERT_TRUE(clean);
  auto victim = clean->blocks()[2];
  bytes[victim.offset + kDataHeaderBytes + 3] ^= 0x40;  // inside the payload

  auto file = FileReader::open(bytes);
  ASSERT_TRUE(file);
  Codecs codecs;
  uint32_t lost = 0;
  for (uint32_t ordinal = 0; ordinal < 300; ordinal++) {
    auto block = file->blockFor(ordinal);
    DecodedBlock decoded;
    if (block < 0 || !file->readBlock((size_t)block, codecs, decoded)) {
      lost++;
      continue;
    }
    expectRecord(decoded, ordinal);
  }
  EXPECT_EQ(lost, victim.nDocs);
}

TEST(SourceFormat, ScanRecoversWithoutTheFooter) {
  auto bytes = writeFile();
  auto clean = FileReader::open(bytes);
  ASSERT_TRUE(clean);
  size_t blocks = clean->blocks().size();
  bytes.resize(bytes.size() - 20);  // cut into the footer

  auto file = FileReader::open(bytes);
  ASSERT_TRUE(file);
  EXPECT_FALSE(file->footerVerified());
  EXPECT_EQ(file->blocks().size(), blocks);
  EXPECT_EQ(file->docCount(), 300u);
  Codecs codecs;
  DecodedBlock decoded;
  ASSERT_TRUE(file->readBlock(blocks - 1, codecs, decoded));
  expectRecord(decoded, 299);
}

TEST(SourceFormat, ScanResynchronizesPastDamagedHeader) {
  auto bytes = writeFile();
  auto clean = FileReader::open(bytes);
  ASSERT_TRUE(clean);
  auto victim = clean->blocks()[1];
  bytes[victim.offset + kSyncBytes + 6] ^= 0x01;  // first_ordinal in the block header
  bytes.resize(bytes.size() - 1);                 // and no footer

  auto file = FileReader::open(bytes);
  ASSERT_TRUE(file);
  EXPECT_EQ(file->blocks().size(), clean->blocks().size() - 1);
  EXPECT_EQ(file->blockFor(victim.firstOrdinal), -1);
}

TEST(SourceFormat, TruncationKeepsTheBlocksBeforeIt) {
  auto bytes = writeFile();
  auto clean = FileReader::open(bytes);
  ASSERT_TRUE(clean);
  auto cut = clean->blocks()[3];
  bytes.resize(cut.offset + 30);
  auto file = FileReader::open(bytes);
  ASSERT_TRUE(file);
  EXPECT_EQ(file->blocks().size(), 3u);
}

TEST(SourceFormat, RejectsForeignAndDamagedHeaders) {
  std::string error;
  EXPECT_FALSE(FileReader::open("not a source file at all, but long enough to try", &error));
  auto bytes = writeFile();
  bytes[12] ^= 1;  // file id
  EXPECT_FALSE(FileReader::open(bytes, &error));
  EXPECT_NE(error.find("checksum"), std::string::npos);
}

TEST(SourceFormat, BlockMovedToAnotherOffsetFailsVerification) {
  auto bytes = writeFile();
  auto clean = FileReader::open(bytes);
  ASSERT_TRUE(clean);
  auto a = clean->blocks()[0];
  auto b = clean->blocks()[1];
  // Overwrite block 1 with block 0's bytes when they are the same size: the
  // content is intact but its checksum was made for another offset.
  if (b.offset - a.offset == clean->blocks()[2].offset - b.offset) {
    bytes.replace(b.offset, b.offset - a.offset, bytes.substr(a.offset, b.offset - a.offset));
    auto file = FileReader::open(bytes);
    ASSERT_TRUE(file);
    Codecs codecs;
    DecodedBlock decoded;
    EXPECT_FALSE(file->readBlock(1, codecs, decoded));
  }
}

TEST(SourceFormat, DictionaryBlocksAreSelfContained) {
  std::string dict(2000, 'x');
  for (int i = 0; i < 2000; i++) dict[i] = (char)('a' + (i * 7) % 26);
  WriterOptions options;
  options.blockBytes = 512;
  StringSink sink;
  FileWriter writer(sink, options, "dict", 1);
  writer.addDictionary(9, dict);
  writer.useDictionary(9);
  for (uint32_t i = 0; i < 100; i++) {
    std::string id = "d" + std::to_string(i);
    std::string body = docBody((int)i);
    writer.add({id, body, i + 1, kFlagMinifiedJson});
  }
  writer.finish();

  auto file = FileReader::open(sink.bytes);
  ASSERT_TRUE(file);
  ASSERT_NE(file->dictionary(9), nullptr);
  EXPECT_EQ(*file->dictionary(9), dict);
  Codecs codecs;
  DecodedBlock decoded;
  ASSERT_TRUE(file->readBlock(2, codecs, decoded));
  expectRecord(decoded, decoded.firstOrdinal);
  FileReader::RawBlock raw;
  ASSERT_TRUE(file->rawBlock(2, raw));
  EXPECT_EQ(raw.dictId, 9u);
}

TEST(SourceFormat, RawBlocksCopyVerbatimWithRebasedOrdinals) {
  auto bytes = writeFile();
  auto source = FileReader::open(bytes);
  ASSERT_TRUE(source);

  StringSink sink;
  FileWriter writer(sink, {}, "copy", 5);
  writer.add({"first", "{}", 7, kFlagMinifiedJson});
  FileReader::RawBlock raw;
  ASSERT_TRUE(source->rawBlock(4, raw));
  writer.addRawBlock(raw.codec, raw.dictId, raw.nDocs, raw.rawLen, raw.payload);
  writer.finish();

  auto copy = FileReader::open(sink.bytes);
  ASSERT_TRUE(copy);
  EXPECT_EQ(copy->docCount(), 1 + raw.nDocs);
  Codecs codecs;
  DecodedBlock decoded;
  ASSERT_TRUE(copy->readBlock((size_t)copy->blockFor(1), codecs, decoded));
  EXPECT_EQ(decoded.firstOrdinal, 1u);
  EXPECT_EQ(decoded.record(0).id, "d" + std::to_string(source->blocks()[4].firstOrdinal));
}

TEST(SourceFormat, OversizedDocumentGetsItsOwnBlock) {
  StringSink sink;
  FileWriter writer(sink, {}, "big", 1);
  std::string big(200000, 'z');
  writer.add({"a", "{}", 1, 0});
  writer.add({"b", big, 2, 0});
  writer.add({"c", "{}", 3, 0});
  writer.finish();
  auto file = FileReader::open(sink.bytes);
  ASSERT_TRUE(file);
  Codecs codecs;
  DecodedBlock decoded;
  ASSERT_TRUE(file->readBlock((size_t)file->blockFor(1), codecs, decoded));
  EXPECT_EQ(decoded.record(1 - (decoded.firstOrdinal)).body, big);
}

TEST(SourceJson, MinifyKeepsStringsAndSpelling) {
  std::string out;
  minifyJson("{ \"a b\" : [ 1.50 , \"x \\\" y\" ],\n\t\"a b\": 0042 }", out);
  EXPECT_EQ(out, "{\"a b\":[1.50,\"x \\\" y\"],\"a b\":0042}");
}

TEST(SourceJson, FindsTheDocElementsOfAnUpdateBody) {
  std::string body = R"({"commit":{"x":1}, "docs" : [ {"id":"1","t":"a,]"}, {"id":"2","n":[1,2]} , 3 ], "allow_dups":true})";
  std::vector<std::string_view> spans;
  ASSERT_TRUE(findUpdateDocSpans(body, spans));
  ASSERT_EQ(spans.size(), 3u);
  EXPECT_EQ(spans[0], R"({"id":"1","t":"a,]"})");
  EXPECT_EQ(spans[1], R"({"id":"2","n":[1,2]})");
  EXPECT_EQ(spans[2], "3");

  EXPECT_TRUE(findUpdateDocSpans(R"({"collection":"c"})", spans));
  EXPECT_TRUE(spans.empty());
  EXPECT_FALSE(findUpdateDocSpans(R"({"docs":[{"id":1}],"docs":[]})", spans));
  EXPECT_TRUE(spans.empty());
  EXPECT_FALSE(findUpdateDocSpans(R"({"docs":[{"id":1})", spans));
  EXPECT_FALSE(findUpdateDocSpans("[1]", spans));
}

}  // namespace luxir::test
