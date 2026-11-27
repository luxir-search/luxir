// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include <set>

#include "luxir/reader/SegmentCore.h"
#include "test/FieldRecordTest.h"
#include "test/StorageTestUtils.h"
#include <gtest/gtest.h>
#include "test/TestUtils.h"

#include <algorithm>
#include <bit>
#include <cstdio>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "luxir/index/PostingsWriter.h"
#include "luxir/index/IndexWriter.h"
#include "luxir/query/Query.h"
#include "luxir/reader/DocsEnum.h"
#include "luxir/reader/PosEnum.h"
#include "luxir/reader/FieldCursor.h"
#include "luxir/search/IndexReader.h"
#include "luxir/reader/PostingsReader.h"
#include "luxir/schema/Schema.h"
#include "luxir/schema/FieldType.h"
#include "test/CollectionHelper.h"
#include "test/LocalReq.h"
#include "test/LuxirTest.h"
#include "test/TestIndex.h"

using namespace luxir;
using namespace luxir::test;

namespace {

void indexTerms(TestIndex& ti, TestField& field, const std::vector<std::string>& terms) {
  field.startIndexing();
  for (int i = 0; i < (int)terms.size(); i++) {
    field.add(i, terms[(size_t)i]);
  }
  ti.flush();
  field.startReading();
  ASSERT_NE(field.currentSegment(), nullptr);
}

std::vector<std::string> collectNext(TestField& field) {
  auto guard = field.testIndex.pool.rewindScopeGuard();
  auto* seg = field.currentSegment();
  TermsEnum te(guard.pool(), seg->postingsReader(), field.fieldInfo());
  std::vector<std::string> got;
  while (te.nextTerm()) {
    got.push_back(std::string((std::string_view)te.term()));
  }
  return got;
}

std::vector<std::string> collectSeekCeil(TestField& field) {
  auto guard = field.testIndex.pool.rewindScopeGuard();
  auto* seg = field.currentSegment();
  TermsEnum te(guard.pool(), seg->postingsReader(), field.fieldInfo());
  std::vector<std::string> got;
  std::string target;
  while (te.seekCeil(target)) {
    std::string term((std::string_view)te.term());
    got.push_back(term);
    target = term;
    target.push_back('\0');
  }
  return got;
}

void expectExactSeeks(TestField& field, const std::vector<std::string>& expected) {
  auto guard = field.testIndex.pool.rewindScopeGuard();
  auto* seg = field.currentSegment();
  TermsEnum te(guard.pool(), seg->postingsReader(), field.fieldInfo());
  for (int i = (int)expected.size() - 1; i >= 0; i--) {
    ASSERT_TRUE(te.seek(expected[(size_t)i])) << i;
    EXPECT_EQ((std::string_view)te.term(), expected[(size_t)i]) << i;
  }
}

void expectCeil(TestField& field, std::string_view target, std::string_view expected) {
  auto guard = field.testIndex.pool.rewindScopeGuard();
  auto* seg = field.currentSegment();
  TermsEnum te(guard.pool(), seg->postingsReader(), field.fieldInfo());
  ASSERT_TRUE(te.seekCeil(target));
  EXPECT_EQ((std::string_view)te.term(), expected);
}

struct RawDocSpec {
  int32_t docid;
  int32_t tf;
};

struct RawTermSpec {
  std::string name;
  std::vector<RawDocSpec> docs;
};

struct BlockCensus {
  int32_t contiguous = 0;
  int32_t packed = 0;
  int32_t bitset = 0;
  std::vector<int32_t> packedPerGroup;
};

BlockCensus recomputeBlockCensus(const RawTermSpec& term) {
  BlockCensus census;
  int32_t blockCount =
      ((int32_t) term.docs.size() + Postings::DOCS_BLOCK_SIZE - 1)
      / Postings::DOCS_BLOCK_SIZE;
  census.packedPerGroup.resize(
      (size_t) ((blockCount + DocsEnumMeta::L1_PERIOD - 1)
                / DocsEnumMeta::L1_PERIOD),
      0);

  uint32_t base = 0;
  int32_t fullBlocks = (int32_t) term.docs.size() / Postings::DOCS_BLOCK_SIZE;
  for (int32_t block = 0; block < fullBlocks; block++) {
    int32_t start = block * Postings::DOCS_BLOCK_SIZE;
    int32_t end = start + Postings::DOCS_BLOCK_SIZE;
    uint32_t lastDoc = (uint32_t) term.docs[(size_t) end - 1].docid;
    uint32_t docBase = base + (block == 0 ? 0 : 1);
    uint32_t spanBits = lastDoc - docBase + 1;
    uint32_t deltaOr = (uint32_t) term.docs[(size_t) start].docid - base;
    for (int32_t i = start + 1; i < end; i++) {
      deltaOr |= (uint32_t) (term.docs[(size_t) i].docid
                             - term.docs[(size_t) i - 1].docid);
    }
    uint32_t bitsPerValue =
        32 - (uint32_t) std::countl_zero(deltaOr | 1);
    uint32_t numWords = (spanBits + 63) / 64;
    if (spanBits == (uint32_t) Postings::DOCS_BLOCK_SIZE) {
      census.contiguous++;
    } else if (std::min(32u, bitsPerValue + 1)
                   * (uint32_t) Postings::DOCS_BLOCK_SIZE
               <= numWords * 64) {
      census.packed++;
      census.packedPerGroup[(size_t) block / DocsEnumMeta::L1_PERIOD]++;
    } else {
      census.bitset++;
    }
    base = lastDoc;
  }
  return census;
}

struct IteratedStats {
  int32_t df = 0;
  int64_t ttf = 0;
};

struct MetadataRunCodes {
  uint32_t docsEnd = 0;
  uint32_t packedBlocks = 0;
  uint32_t df = 0;
  uint32_t ttfCode = 0;
  uint32_t posOff = 0;
  uint32_t termImpact = 0;
  uint32_t pulsed = 0;
  uint8_t pulsedMaskCode = 0;
};

int64_t expectedTtf(FieldType::flag_type flags, const RawTermSpec& term) {
  if (!FieldType::hasFreqs(flags)) {
    return (int64_t) term.docs.size();
  }
  int64_t ttf = 0;
  for (const RawDocSpec& doc : term.docs) {
    ttf += doc.tf;
  }
  return ttf;
}

std::string repeatedTerm(std::string_view term, int32_t tf) {
  std::string text;
  for (int32_t i = 0; i < tf; i++) {
    if (!text.empty()) {
      text.push_back(' ');
    }
    text += term;
  }
  return text;
}

template<DocsEnumTier Tier>
IteratedStats iterateStats(BasicDocsEnum<Tier>& docsEnum, PosEnum* posEnum = nullptr) {
  IteratedStats stats;
  for (;;) {
    int32_t doc = docsEnum.nextDoc();
    if (doc == DocsEnumMeta::END) {
      break;
    }
    unused(doc);
    stats.df++;
    int32_t tf = docsEnum.termFreq();
    stats.ttf += tf;
    if (posEnum != nullptr) {
      posEnum->startPositions();
      for (int32_t i = 0; i < tf; i++) {
        EXPECT_NE(posEnum->nextPosition(), PosEnum::END);
      }
    }
  }
  return stats;
}

void buildRawField(RAMDir& dir, MemPool& pool, FieldType::flag_type flags,
                   const std::vector<RawTermSpec>& terms,
                   std::unique_ptr<SegmentCore>& reader) {
  PostingsWriter postingsWriter(dir, 0, 1, 1000000);
  {
    TextWriter writer(postingsWriter);
    auto& finfo = postingsWriter.addField(SpaceOrdinal::ROOT, "f");
    finfo.header.type = FieldType::TEXT;
    finfo.header.flags = flags;
    writer.startField(finfo.space, finfo);

    std::set<int32_t> docs;
    for (const RawTermSpec& term : terms) {
      TermRef termRef(pool, term.name.data(), (uint32_t) term.name.size());
      writer.startTerm(termRef);
      for (const RawDocSpec& doc : term.docs) {
        docs.insert(doc.docid);
        if (FieldType::hasPositions(flags)) {
          writer.startDoc(doc.docid);
          for (int32_t i = 0; i < doc.tf; i++) {
            writer.addPositionDelta(i == 0 ? 1 : 2);
          }
          writer.endDoc(doc.docid);
        } else {
          writer.addDoc(doc.docid, doc.tf);
        }
      }
      writer.endTerm(termRef);
    }
    finfo.setPostings(writer.endField());
    luxir::test::writeFieldPresence(postingsWriter, finfo, docs);
  }
  luxir::test::writeUnversionedRuns(postingsWriter);
  postingsWriter.finish();

  reader = std::make_unique<SegmentCore>(dir, 0, 1);
}

MetadataRunCodes readFirstBlockMetadataRunCodes(PostingsReader& reader, const DecodedFieldRecord& fieldInfo) {
  auto bytes = reader.core().slice(fieldInfo.terms()->dictionaryBlocks());
  InputStream termsIS(bytes.data(), bytes.data() + bytes.size());
  int32_t nTerms = (int32_t)std::min<int64_t>(Postings::TERMS_BLOCK_SIZE,
                                               fieldInfo.indexedTotals().numTerms);
  MetadataRunCodes codes;
  termsIS.readPackedTerm();
  termsIS.readVlong();
  if (FieldType::hasPositions(fieldInfo.flags)) {
    termsIS.readVlong();
  }
  termsIS.readByte();
  codes.pulsedMaskCode = (uint8_t) termsIS.readByte();
  if (codes.pulsedMaskCode == Postings::PULSED_MASK_EXPLICIT) {
    termsIS.skip((int64_t) sizeof(uint32_t));
  }
  uint32_t suffixBytesTotal = termsIS.readVint();
  termsIS.skip(nTerms);
  termsIS.skip(nTerms - 1);
  termsIS.skip(nTerms - 1);
  termsIS.skip(suffixBytesTotal);

  codes.docsEnd = termsIS.readVint();
  codes.packedBlocks = termsIS.readVint();
  codes.df = termsIS.readVint();
  if (FieldType::hasFreqs(fieldInfo.flags)) {
    codes.ttfCode = termsIS.readVint();
  }
  if (FieldType::hasPositions(fieldInfo.flags)) {
    codes.posOff = termsIS.readVint();
  }
  if (FieldType::hasFreqs(fieldInfo.flags)
      && FieldType::hasPositions(fieldInfo.flags)) {
    codes.termImpact = termsIS.readVint();
  }
  codes.pulsed = termsIS.readVint();
  return codes;
}

std::shared_ptr<Schema> schemaForLevel(FieldType::Type type, FieldType::flag_type flags) {
  auto schema = std::make_shared<Schema>();
  if (type == FieldType::STRING) {
    schema->fieldTypeMap["f"] = std::make_shared<StrFieldType>("f", flags);
  } else {
    schema->fieldTypeMap["f"] = std::make_shared<TextFieldType>("f", flags, "whitespace");
  }
  return schema;
}

void addMergeSegment(IndexWriter& writer, bool isString, int32_t numDocs, int32_t tf) {
  Inverter& inverter = writer.obtainInverter();
  auto& handler = inverter.getIndexHandler("f");
  std::string text = isString ? std::string("hot") : repeatedTerm("hot", tf);
  for (int32_t doc = 0; doc < numDocs; doc++) {
    inverter.setDoc(doc);
    handler.index(inverter, text);
  }
  writer.releaseInverter(inverter);
  writer.commit();
}

void indexQueryStatsTerms(IndexWriter& writer, FieldType::Type type,
                          const std::vector<RawTermSpec>& terms) {
  Inverter& inverter = writer.obtainInverter();
  auto& handler = inverter.getIndexHandler("f");
  for (const RawTermSpec& term : terms) {
    for (const RawDocSpec& doc : term.docs) {
      inverter.setDoc(doc.docid);
      if (type == FieldType::STRING) {
        handler.index(inverter, std::string_view(term.name));
      } else {
        handler.index(inverter, repeatedTerm(term.name, doc.tf));
      }
    }
  }
  writer.releaseInverter(inverter);
  writer.commit();
}

IteratedStats readQueryPostingsStats(MemPool& pool, IndexReader& reader,
                                     CachedFieldInfo& cachedFieldInfo,
                                     std::string_view term,
                                     bool readPositions) {
  auto read = [&]<DocsEnumTier Tier>() {
    IteratedStats stats;
    for (auto& segment : reader.segments()) {
      auto* field = cachedFieldInfo.segInfos.at(segment);
      if (!field) continue;
      TermsEnum terms(pool, segment.postingsReader(), *field);
      if (!terms.seek(term)) continue;
      BasicDocsEnum<Tier> docsEnum(terms);
      IteratedStats segmentStats;
      if constexpr (Tier == DocsEnumTier::POSITIONS) {
        PosEnum posEnum(docsEnum);
        segmentStats = iterateStats(docsEnum, &posEnum);
      } else {
        segmentStats = iterateStats(docsEnum);
      }
      stats.df += segmentStats.df;
      stats.ttf += segmentStats.ttf;
    }
    return stats;
  };
  return readPositions
      ? read.template operator()<DocsEnumTier::POSITIONS>()
      : read.template operator()<DocsEnumTier::FREQS>();
}

} // namespace

class TermsDictTest : public LuxirTest {
};

TEST_F(TermsDictTest, ContiguousRoutingAtTrieLimit) {
  using namespace field_parts;
  constexpr uint32_t blocks = Postings::MAX_TERM_BLOCKS;
  constexpr int64_t firstTerm = (int64_t)(blocks - 1) * Postings::TERMS_BLOCK_SIZE - 1;
  constexpr int64_t termCount = firstTerm + Postings::TERMS_BLOCK_SIZE;
  constexpr uint64_t firstBytes = UINT32_MAX;
  constexpr uint64_t endOfDictionary = 8 + firstBytes + 512;
  constexpr uint64_t endOfOffsets = alignUp(endOfDictionary, Terms::OFFSETS_ALIGN)
      + (uint64_t)blocks * sizeof(Terms::BlockOffset);
  constexpr int64_t sumDocFreq = int64_t{1} << 34;
  constexpr uint64_t firstDocsBytes = uint64_t{1} << 35;
  constexpr uint64_t trieBytes = (uint64_t)1 << 33;

  const std::array runsOnWrite{
      TermRun{0, 0, 0, {{1, 8}, firstDocsBytes}, {}},
      TermRun{(int32_t)firstTerm, (int32_t)blocks - 1, firstBytes, {{1, 8 + firstDocsBytes}, 64}, {}}};
  Terms terms{{0, 8}, endOfDictionary - 8, trieBytes, trieBytes - 16, runsOnWrite};
  MemPool pool;
  FieldRecordHeader header{.name = PackedTerm(pool, "f"), .type = FieldType::TEXT,
                           .flags = FieldType::INDEX_DOCS, .parts = {}, .docsWithField = 64,
                           .indexedTotals = {termCount, sumDocFreq, sumDocFreq}, .bounds = {}};
  RAMFile file("");
  OutputStream out(&file);
  IndexFieldInfo fieldOnWrite;
  fieldOnWrite.header = header;
  fieldOnWrite.setPostings({header.indexedTotals, terms, 0, {}});
  FieldRecordCodec::encode(out, fieldOnWrite, header.docsWithField);
  out.close();
  std::vector<char> bytes(file.size());
  file.copyTo(bytes.data());
  std::array<uint64_t, 2> sizes{endOfOffsets + trieBytes, 8 + firstDocsBytes + 64};
  auto field = FieldRecordCodec::decode(bytes, header.docsWithField, sizes, -1);
  EXPECT_EQ(header.docsWithField, field->docsWithField);
  EXPECT_EQ(0, field->numValues);
  EXPECT_EQ(header.indexedTotals, field->indexedTotals());
  auto runs = field->terms()->runs;
  ASSERT_EQ(2u, runs.size());
  EXPECT_EQ(blocks - 2, runs[0].blockForTerm(firstTerm - 1));
  EXPECT_EQ(firstTerm, runs[1].firstTermOrd);
  EXPECT_EQ(blocks - 1, runs[1].blockForTerm(firstTerm));
  EXPECT_EQ(blocks - 1, runs[1].blockForTerm(termCount - 1));
  EXPECT_EQ(firstTerm, runs[1].firstTermForBlock(blocks - 1));
  EXPECT_EQ(firstBytes, runs[1].dictionaryStart);
  EXPECT_EQ(endOfDictionary, field->terms()->dictionaryBlocks().end().offset());
  EXPECT_EQ(trieBytes - 16, field->terms()->trieRootOffset);
  ASSERT_TRUE(runs[1].docs);
  EXPECT_EQ(8 + firstDocsBytes, runs[1].docs.location.offset());
}

TEST_F(TermsDictTest, DecodesEscapedPrefixAndSuffixLengths) {
  TestIndex ti;
  TestField field(ti, "foo_s");

  std::string longPrefix(200, 'p');
  std::string sharedA = longPrefix + "a";
  std::string sharedB = longPrefix + "b";

  std::string longSuffixA = std::string(30, 'a') + std::string(180, 'x');
  std::string longSuffixB = std::string(30, 'a') + std::string(180, 'y');
  std::string suffixOnlyA = std::string("q") + std::string(200, 'a');
  std::string suffixOnlyB = std::string("r") + std::string(200, 'b');

  std::vector<std::string> terms = {
    longSuffixA, longSuffixB, sharedA, sharedB, suffixOnlyA, suffixOnlyB
  };
  std::vector<std::string> expected = terms;
  std::sort(expected.begin(), expected.end());

  indexTerms(ti, field, terms);

  EXPECT_EQ(collectNext(field), expected);
  EXPECT_EQ(collectSeekCeil(field), expected);
  expectExactSeeks(field, expected);

  expectCeil(field, longSuffixA + "!", longSuffixB);
  expectCeil(field, sharedA + "!", sharedB);
  expectCeil(field, suffixOnlyA + "!", suffixOnlyB);
}

TEST_F(TermsDictTest, DecodesBlockPrefixFactoredRunsAcrossBlocks) {
  TestIndex ti;
  TestField field(ti, "prefix_s");

  std::string shared(150, 'a');
  shared += "/tenant/collection/";
  std::vector<std::string> terms;
  for (int32_t i = 0; i < 70; i++) {
    char buf[32];
    snprintf(buf, sizeof(buf), "%03d", i);
    std::string term = shared + buf;
    if ((i % 5) == 0) {
      term += std::string(80, (char) ('k' + (i % 7)));
    }
    terms.push_back(term);
  }
  std::vector<std::string> expected = terms;
  std::sort(expected.begin(), expected.end());

  indexTerms(ti, field, terms);

  EXPECT_EQ(collectNext(field), expected);
  EXPECT_EQ(collectSeekCeil(field), expected);
  expectExactSeeks(field, expected);

  {
    auto guard = field.testIndex.pool.rewindScopeGuard();
    auto* seg = field.currentSegment();
    TermsEnum te(guard.pool(), seg->postingsReader(), field.fieldInfo());
    ASSERT_TRUE(te.seek(expected[Postings::TERMS_BLOCK_SIZE]));
    ASSERT_EQ(te.ord(), Postings::TERMS_BLOCK_SIZE);
    EXPECT_EQ(te.docFreq(), 1);
    EXPECT_EQ(te.totalTermFreq(), 1);
  }

  {
    auto guard = field.testIndex.pool.rewindScopeGuard();
    auto* seg = field.currentSegment();
    TermsEnum te(guard.pool(), seg->postingsReader(), field.fieldInfo());
    ASSERT_TRUE(te.seek(expected[Postings::TERMS_BLOCK_SIZE]));
    ASSERT_TRUE(te.nextTerm());
    ASSERT_EQ((std::string_view) te.term(), expected[Postings::TERMS_BLOCK_SIZE + 1]);
    EXPECT_EQ(te.docFreq(), 1);
    EXPECT_EQ(te.totalTermFreq(), 1);
  }
}

TEST_F(TermsDictTest, PulsedMaskEdgesPreserveDocsOnlyPostingsAndStats) {
  TestIndex ti;
  TestField field(ti, "mask_s");
  field.startIndexing();

  std::vector<std::string> terms;
  std::vector<int32_t> dfs;
  int32_t docid = 0;
  for (int32_t i = 0; i < 32; i++) {
    char buf[16];
    snprintf(buf, sizeof(buf), "a%03d", i);
    terms.emplace_back(buf);
    dfs.push_back(1);
    field.add(docid++, terms.back());
  }
  for (int32_t i = 0; i < 32; i++) {
    char buf[16];
    snprintf(buf, sizeof(buf), "b%03d", i);
    terms.emplace_back(buf);
    dfs.push_back(2);
    field.add(docid++, terms.back());
    field.add(docid++, terms.back());
  }
  for (int32_t i = 0; i < 5; i++) {
    char buf[16];
    snprintf(buf, sizeof(buf), "c%03d", i);
    terms.emplace_back(buf);
    dfs.push_back(1);
    field.add(docid++, terms.back());
  }
  ti.flush();
  field.startReading();
  ASSERT_NE(field.currentSegment(), nullptr);

  auto guard = field.testIndex.pool.rewindScopeGuard();
  auto* seg = field.currentSegment();
  TermsEnum tenum(guard.pool(), seg->postingsReader(), field.fieldInfo());
  for (size_t i = 0; i < terms.size(); i++) {
    ASSERT_TRUE(tenum.seek(terms[i])) << terms[i];
    EXPECT_EQ(tenum.docFreq(), dfs[i]) << terms[i];
    EXPECT_EQ(tenum.totalTermFreq(), dfs[i]) << terms[i];
    DocsOnlyEnum docsEnum(tenum);
    EXPECT_EQ(docsEnum.numDocs(), dfs[i]) << terms[i];
    EXPECT_EQ(docsEnum.totalTermFreq(), dfs[i]) << terms[i];
  }
}

TEST_F(TermsDictTest, AllDefaultMetadataRunsPreservePulsedMultiBlockPostings) {
  std::vector<RawTermSpec> terms;
  for (int32_t i = 0; i < Postings::TERMS_BLOCK_SIZE * 2 + 3; i++) {
    char buf[16];
    snprintf(buf, sizeof(buf), "d%03d", i);
    terms.push_back({buf, {{i * 3 + 1, 1}}});
  }

  RAMDir dir;
  MemPool pool;
  std::unique_ptr<SegmentCore> reader;
  buildRawField(dir, pool, FieldType::INDEX_DOCS_FREQS_POSITIONS, terms, reader);
  const auto& fieldInfo = *reader->rootReader().fieldInfo("f");

  MetadataRunCodes codes = readFirstBlockMetadataRunCodes(reader->rootReader(), fieldInfo);
  EXPECT_EQ(codes.docsEnd, 0u);
  EXPECT_EQ(codes.packedBlocks, 0u);
  EXPECT_EQ(codes.df, 0u);
  EXPECT_EQ(codes.ttfCode, 0u);
  EXPECT_EQ(codes.posOff, 0u);
  EXPECT_NE(codes.termImpact, 0u);
  EXPECT_NE(codes.pulsed, 0u);

  TermsEnum tenum(pool, reader->rootReader(), fieldInfo);
  for (const RawTermSpec& term : terms) {
    ASSERT_TRUE(tenum.nextTerm()) << term.name;
    ASSERT_EQ((std::string_view) tenum.term(), term.name);
    int32_t dictDf = tenum.docFreq();
    int64_t dictTtf = tenum.totalTermFreq();
    DocsPosEnum docsEnum(tenum);
    PosEnum posEnum(docsEnum);
    IteratedStats stats = iterateStats(docsEnum, &posEnum);
    EXPECT_EQ(stats.df, 1) << term.name;
    EXPECT_EQ(stats.ttf, 1) << term.name;
    EXPECT_EQ(dictDf, stats.df) << term.name;
    EXPECT_EQ(dictTtf, stats.ttf) << term.name;
  }
  EXPECT_FALSE(tenum.nextTerm());
}

TEST_F(TermsDictTest, MixedMetadataRunsKeepNonUniformRowsExplicit) {
  std::vector<RawTermSpec> terms;
  for (int32_t i = 0; i < Postings::TERMS_BLOCK_SIZE; i++) {
    char buf[16];
    snprintf(buf, sizeof(buf), "m%03d", i);
    int32_t tf = (i % 3) == 0 ? 2 : 1;
    terms.push_back({buf, {{i * 5 + 1, tf}}});
  }

  RAMDir dir;
  MemPool pool;
  std::unique_ptr<SegmentCore> reader;
  buildRawField(dir, pool, FieldType::INDEX_DOCS_FREQS_POSITIONS, terms, reader);
  const auto& fieldInfo = *reader->rootReader().fieldInfo("f");

  // Every term has one doc, so every term pulses whatever its tf; the tf 2
  // terms keep their positions in the positions stream.
  MetadataRunCodes codes = readFirstBlockMetadataRunCodes(reader->rootReader(), fieldInfo);
  EXPECT_EQ(codes.pulsedMaskCode, Postings::PULSED_MASK_ALL);
  EXPECT_EQ(codes.docsEnd, 0u);
  EXPECT_EQ(codes.packedBlocks, 0u);
  EXPECT_EQ(codes.df, 0u);
  EXPECT_NE(codes.ttfCode, 0u);
  EXPECT_NE(codes.posOff, 0u);
  EXPECT_NE(codes.termImpact, 0u);
  EXPECT_NE(codes.pulsed, 0u);

  TermsEnum tenum(pool, reader->rootReader(), fieldInfo);
  for (const RawTermSpec& term : terms) {
    ASSERT_TRUE(tenum.nextTerm()) << term.name;
    int32_t dictDf = tenum.docFreq();
    int64_t dictTtf = tenum.totalTermFreq();
    DocsPosEnum docsEnum(tenum);
    PosEnum posEnum(docsEnum);
    IteratedStats stats = iterateStats(docsEnum, &posEnum);
    EXPECT_EQ(dictDf, stats.df) << term.name;
    EXPECT_EQ(dictTtf, stats.ttf) << term.name;
    EXPECT_EQ(stats.ttf, expectedTtf(fieldInfo.flags, term)) << term.name;
  }
  EXPECT_FALSE(tenum.nextTerm());
}

TEST_F(TermsDictTest, PackedBlockCountsRoundTripAcrossIndexLevels) {
  std::vector<RawTermSpec> terms;

  RawTermSpec mixed{"mixed", {}};
  for (int32_t doc = 0; doc < Postings::DOCS_BLOCK_SIZE; doc++) {
    mixed.docs.push_back({doc, 1});
  }
  int32_t doc = 256;
  for (int32_t i = 0; i < Postings::DOCS_BLOCK_SIZE; i++) {
    mixed.docs.push_back({doc, 1});
    doc += 16;
  }
  doc = mixed.docs.back().docid;
  for (int32_t i = 0; i < Postings::DOCS_BLOCK_SIZE; i++) {
    doc += i == 0 ? 1 : 2;
    mixed.docs.push_back({doc, 1});
  }
  for (int32_t i = 0; i < 17; i++) {
    doc += 11;
    mixed.docs.push_back({doc, 1});
  }
  terms.push_back(std::move(mixed));

  RawTermSpec multi{"multi", {}};
  doc = 0;
  int32_t multiDocs =
      DocsEnumMeta::L1_DOCS + 2 * Postings::DOCS_BLOCK_SIZE + 17;
  for (int32_t i = 0; i < multiDocs; i++) {
    doc += 10;
    multi.docs.push_back({doc, 1});
  }
  terms.push_back(std::move(multi));
  terms.push_back({"pulse", {{7, 1}}});

  BlockCensus mixedCensus = recomputeBlockCensus(terms[0]);
  EXPECT_EQ(mixedCensus.contiguous, 1);
  EXPECT_EQ(mixedCensus.packed, 1);
  EXPECT_EQ(mixedCensus.bitset, 1);
  BlockCensus multiCensus = recomputeBlockCensus(terms[1]);
  EXPECT_GT(multiCensus.packed, DocsEnumMeta::L1_PERIOD);
  EXPECT_GT((int32_t) terms[1].docs.size(), DocsEnumMeta::L1_DOCS);

  for (FieldType::flag_type flags : {
           FieldType::INDEX_DOCS,
           FieldType::INDEX_DOCS_FREQS,
           FieldType::INDEX_DOCS_FREQS_POSITIONS}) {
    SCOPED_TRACE(::testing::Message() << "flags=" << flags);
    RAMDir dir;
    MemPool pool;
    std::unique_ptr<SegmentCore> reader;
    buildRawField(dir, pool, flags, terms, reader);
    const auto& fieldInfo = *reader->rootReader().fieldInfo("f");

    TermsEnum tenum(pool, reader->rootReader(), fieldInfo);
    for (const RawTermSpec& term : terms) {
      ASSERT_TRUE(tenum.nextTerm()) << term.name;
      ASSERT_EQ((std::string_view) tenum.term(), term.name);
      BlockCensus census = recomputeBlockCensus(term);
      int32_t expectedPacked = term.name == "pulse" ? 0 : census.packed;
      EXPECT_EQ(tenum.packedBlockCount(), expectedPacked) << term.name;

      DocsOnlyEnum groupReader(tenum);
      EXPECT_EQ(groupReader.packedBlockCount(), expectedPacked) << term.name;
      DocsEnumMeta::GroupImpacts groups;
      groupReader.readGroupImpacts(groups);
      std::vector<int32_t> expectedGroups =
          term.name == "pulse" ? std::vector<int32_t>{} : census.packedPerGroup;
      EXPECT_EQ(groups.packedBlockCounts, expectedGroups) << term.name;

      // The group bytes are asserted via readGroupImpacts above; the
      // sequential cursor skips L1 headers without parsing them. Iterate to
      // prove the header byte does not disturb postings iteration, and that
      // the per-group counts sum to the term total.
      int64_t groupSum = 0;
      for (int32_t count : groups.packedBlockCounts) {
        groupSum += count;
      }
      EXPECT_EQ(groupSum, expectedPacked) << term.name;
      DocsOnlyEnum docs(tenum);
      int32_t seen = 0;
      for (int32_t actual = docs.nextDoc(); actual != DocsEnumMeta::END;
           actual = docs.nextDoc()) {
        ASSERT_EQ(actual, term.docs[(size_t) seen].docid) << term.name;
        seen++;
      }
      EXPECT_EQ(seen, (int32_t) term.docs.size()) << term.name;
    }
    EXPECT_FALSE(tenum.nextTerm());
  }
}

TEST_F(TermsDictTest, StatsAccessorsMatchDocsEnumAcrossIndexLevels) {
  std::vector<RawTermSpec> terms;
  for (int32_t i = 0; i < 77; i++) {
    char buf[16];
    snprintf(buf, sizeof(buf), "t%03d", i);
    RawTermSpec term;
    term.name = buf;
    int32_t nDocs = 1 + (int32_t) (rng.rint(0, 5));
    int32_t docid = i * 100;
    for (int32_t j = 0; j < nDocs; j++) {
      docid += 1 + (int32_t) rng.rint(0, 3);
      int32_t tf = 1 + (int32_t) rng.rint(0, 4);
      term.docs.push_back({docid, tf});
    }
    terms.push_back(std::move(term));
  }

  for (FieldType::flag_type flags : {
           FieldType::INDEX_DOCS,
           FieldType::INDEX_DOCS_FREQS,
           FieldType::INDEX_DOCS_FREQS_POSITIONS}) {
    SCOPED_TRACE(::testing::Message() << "flags=" << flags);
    RAMDir dir;
    MemPool pool;
    std::unique_ptr<SegmentCore> reader;
    buildRawField(dir, pool, flags, terms, reader);
    const auto& fieldInfo = *reader->rootReader().fieldInfo("f");

    auto check = [&]<DocsEnumTier Tier>() {
      TermsEnum tenum(pool, reader->rootReader(), fieldInfo);
      for (const RawTermSpec& term : terms) {
        ASSERT_TRUE(tenum.nextTerm()) << term.name;
        ASSERT_EQ((std::string_view) tenum.term(), term.name);
        int32_t dictDf = tenum.docFreq();
        int64_t dictTtf = tenum.totalTermFreq();
        BasicDocsEnum<Tier> docsEnum(tenum);
        std::unique_ptr<PosEnum> posEnum;
        if constexpr (Tier == DocsEnumTier::POSITIONS) {
          posEnum = std::make_unique<PosEnum>(docsEnum);
        }

        int32_t seenDocs = 0;
        int64_t seenTtf = 0;
        for (const RawDocSpec& doc : term.docs) {
          ASSERT_EQ(docsEnum.nextDoc(), doc.docid) << term.name;
          int32_t expectedTf = FieldType::hasFreqs(flags) ? doc.tf : 1;
          ASSERT_EQ(docsEnum.termFreq(), expectedTf) << term.name;
          seenDocs++;
          seenTtf += expectedTf;
          if constexpr (Tier == DocsEnumTier::POSITIONS) {
            posEnum->startPositions();
            for (int32_t i = 0; i < expectedTf; i++) {
              ASSERT_EQ(posEnum->nextPosition(), i * 2) << term.name;
            }
          }
        }
        ASSERT_EQ(docsEnum.nextDoc(), DocsEnumMeta::END) << term.name;
        EXPECT_EQ(dictDf, seenDocs) << term.name;
        EXPECT_EQ(dictTtf, seenTtf) << term.name;
        EXPECT_EQ(dictDf, (int32_t) term.docs.size()) << term.name;
        EXPECT_EQ(dictTtf, expectedTtf(flags, term)) << term.name;
      }
      ASSERT_FALSE(tenum.nextTerm());
    };
    if (FieldType::hasPositions(flags)) {
      check.template operator()<DocsEnumTier::POSITIONS>();
    } else {
      check.template operator()<DocsEnumTier::FREQS>();
    }
  }
}

TEST_F(TermsDictTest, QueryStatsOnlyPathMatchesDocsEnumAcrossIndexLevels) {
  struct Level {
    FieldType::Type type;
    FieldType::flag_type flags;
    const char* label;
  };

  std::vector<RawTermSpec> terms;
  for (int32_t i = 0; i < 73; i++) {
    char buf[16];
    snprintf(buf, sizeof(buf), "q%03d", i);
    RawTermSpec term;
    term.name = buf;
    int32_t nDocs = 1 + (int32_t) rng.rint(0, 4);
    int32_t docid = i * 100;
    for (int32_t j = 0; j < nDocs; j++) {
      docid += 1 + (int32_t) rng.rint(0, 5);
      int32_t tf = 1 + (int32_t) rng.rint(0, 3);
      term.docs.push_back({docid, tf});
    }
    terms.push_back(std::move(term));
  }

  for (Level level : {
           Level{FieldType::STRING, FieldType::INDEX_DOCS, "docs"},
           Level{FieldType::TEXT, FieldType::INDEX_DOCS_FREQS, "freqs"},
           Level{FieldType::TEXT, FieldType::INDEX_DOCS_FREQS_POSITIONS, "positions"}}) {
    SCOPED_TRACE(level.label);
    RAMDir dir;
    auto schema = schemaForLevel(level.type, level.flags);
    CommitSnapshotRegistry writerSnapshots(test::reserveIncarnation(dir));
    IndexWriter writer(writerSnapshots, schema);
    indexQueryStatsTerms(writer, level.type, terms);

    MemPool pool;
    IndexReader reader(dir);
    Query::Context context(pool, reader);
    CachedFieldInfo* cachedFieldInfo = context.getCachedFieldInfo("f");
    ASSERT_NE(cachedFieldInfo, nullptr);
    ASSERT_TRUE(cachedFieldInfo->termInfos.empty());

    for (const RawTermSpec& term : terms) {
      Similarity::TermStats dictStats;
      ASSERT_TRUE(context.lookupTermStats(*cachedFieldInfo, term.name, dictStats)) << term.name;
      EXPECT_TRUE(cachedFieldInfo->termInfos.empty()) << term.name;

      IteratedStats postingsStats = readQueryPostingsStats(
        pool, reader, *cachedFieldInfo, term.name, FieldType::hasPositions(level.flags));
      EXPECT_EQ(dictStats.docFreq, postingsStats.df) << term.name;
      EXPECT_EQ(dictStats.totalTermFreq, postingsStats.ttf) << term.name;
    }

    Similarity::TermStats missingStats;
    missingStats.docFreq = 7;
    missingStats.totalTermFreq = 9;
    EXPECT_FALSE(context.lookupTermStats(*cachedFieldInfo, "missing", missingStats));
    EXPECT_EQ(missingStats.docFreq, 0);
    EXPECT_EQ(missingStats.totalTermFreq, 0);
    EXPECT_TRUE(cachedFieldInfo->termInfos.empty());
  }
}

TEST_F(TermsDictTest, LastTermBlockMetadataRunsDecodeUnderAsan) {
  std::vector<RawTermSpec> terms;
  for (int32_t i = 0; i < Postings::TERMS_BLOCK_SIZE; i++) {
    char buf[16];
    snprintf(buf, sizeof(buf), "p%03d", i);
    terms.push_back({buf, {{i * 3 + 1, 1}}});
  }

  RAMDir dir;
  MemPool pool;
  std::unique_ptr<SegmentCore> reader;
  buildRawField(dir, pool, FieldType::INDEX_DOCS_FREQS_POSITIONS, terms, reader);
  const auto& fieldInfo = *reader->rootReader().fieldInfo("f");

  TermsEnum tenum(pool, reader->rootReader(), fieldInfo);
  for (const RawTermSpec& term : terms) {
    ASSERT_TRUE(tenum.seek(term.name)) << term.name;
    EXPECT_EQ(tenum.docFreq(), 1) << term.name;
    EXPECT_EQ(tenum.totalTermFreq(), 1) << term.name;
    DocsPosEnum docsEnum(tenum);
    PosEnum posEnum(docsEnum);
    ASSERT_EQ(docsEnum.nextDoc(), term.docs[0].docid) << term.name;
    EXPECT_EQ(docsEnum.termFreq(), 1) << term.name;
    posEnum.startPositions();
    EXPECT_EQ(posEnum.nextPosition(), 0) << term.name;
    EXPECT_EQ(docsEnum.nextDoc(), DocsEnumMeta::END) << term.name;
  }
}

TEST_F(TermsDictTest, PulsedDocsEnumReadsDocOnlyAndPositions) {
  std::vector<RawTermSpec> terms = {
    {"a", {{7, 1}}},
    {"b", {{11, 2}}},
    {"c", {{19, 1}}}
  };

  for (FieldType::flag_type flags : {
           FieldType::INDEX_DOCS,
           FieldType::INDEX_DOCS_FREQS_POSITIONS}) {
    SCOPED_TRACE(::testing::Message() << "flags=" << flags);
    RAMDir dir;
    MemPool pool;
    std::unique_ptr<SegmentCore> reader;
    buildRawField(dir, pool, flags, terms, reader);
    const auto& fieldInfo = *reader->rootReader().fieldInfo("f");

    auto check = [&]<DocsEnumTier Tier>() {
      TermsEnum tenum(pool, reader->rootReader(), fieldInfo);
      for (std::string_view name : {"a", "c"}) {
        ASSERT_TRUE(tenum.seek(name)) << name;
        BasicDocsEnum<Tier> docsEnum(tenum);
        std::unique_ptr<PosEnum> posEnum;
        if constexpr (Tier == DocsEnumTier::POSITIONS) {
          posEnum = std::make_unique<PosEnum>(docsEnum);
        }
        int32_t expectedDoc = name == "a" ? 7 : 19;
        ASSERT_EQ(docsEnum.nextDoc(), expectedDoc) << name;
        EXPECT_EQ(docsEnum.termFreq(), 1) << name;
        if constexpr (Tier == DocsEnumTier::POSITIONS) {
          posEnum->startPositions();
          EXPECT_EQ(posEnum->nextPosition(), 0) << name;
        }
        EXPECT_EQ(docsEnum.nextDoc(), DocsEnumMeta::END) << name;
      }
    };
    if (FieldType::hasPositions(flags)) {
      check.template operator()<DocsEnumTier::POSITIONS>();
    } else {
      check.template operator()<DocsEnumTier::FREQS>();
    }
  }
}

TEST_F(TermsDictTest, PulsedIdHasNoPostingsStreams) {
  bool unalignedToEight = false;
  for (std::string_view name : {"a", "abcde"}) {
    RAMDir dir;
    MemPool pool;
    PostingsWriter postings(dir, 0, 1, 1);
    auto& info = postings.addField(SpaceOrdinal::ROOT, "id");
    info.header.type = FieldType::ID;
    info.header.flags = FieldType::INDEX_DOCS;
    TextWriter writer(postings);
    writer.startField(info.space, info);
    TermRef term(pool, name.data(), (uint32_t)name.size());
    EXPECT_EQ(1, writer.startTerm(term));
    writer.addDoc(0, 1);
    writer.endTerm(term);
    info.setPostings(writer.endField());
    info.setPresence({1, {}});
    ASSERT_TRUE(postings.idRange);
    EXPECT_EQ(name, postings.idRange->min);
    EXPECT_EQ(name, postings.idRange->max);
    EXPECT_EQ(1, postings.idDictionaryBlocks);
    writeUnversionedRuns(postings);
    postings.finish();

    SegmentCore core(dir, 0, 1);
    const auto& fieldInfo = *core.rootReader().fieldInfo("id");
    const auto& terms = *fieldInfo.terms();
    ASSERT_EQ(1u, terms.runs.size());
    EXPECT_FALSE(terms.runs[0].docs);
    EXPECT_FALSE(terms.runs[0].positions);
    auto offsets = fieldInfo.termBlockOffsets();
    EXPECT_EQ(4u, offsets.bytes);
    EXPECT_EQ(0u, offsets.location.offset() % 4);
    unalignedToEight |= offsets.location.offset() % 8 == 4;
    EXPECT_EQ(offsets.end(), fieldInfo.termTrie().location);
    EXPECT_GE(offsets.bytes + terms.trieBytes, SVB_OVERREAD_PAD);
    TermsEnum tenum(pool, core.rootReader(), fieldInfo);
    ASSERT_TRUE(tenum.seek(name));
    BasicDocsEnum<DocsEnumTier::DOCS> docs(tenum);
    EXPECT_EQ(0, docs.nextDoc());
    EXPECT_EQ(DocsEnumMeta::END, docs.nextDoc());
  }
  EXPECT_TRUE(unalignedToEight);
}

TEST_F(TermsDictTest, MergedSegmentsRoundTripAllIndexLevels) {
  struct Level {
    FieldType::Type type;
    FieldType::flag_type flags;
    const char* label;
  };

  for (Level level : {
           Level{FieldType::STRING, FieldType::INDEX_DOCS, "docs"},
           Level{FieldType::TEXT, FieldType::INDEX_DOCS_FREQS, "freqs"},
           Level{FieldType::TEXT, FieldType::INDEX_DOCS_FREQS_POSITIONS, "positions"}}) {
    SCOPED_TRACE(level.label);
    RAMDir dir;
    auto schema = schemaForLevel(level.type, level.flags);
    CommitSnapshotRegistry writerSnapshots(test::reserveIncarnation(dir));
    IndexWriter writer(writerSnapshots, schema);
    addMergeSegment(writer, level.type == FieldType::STRING, 3, 2);
    addMergeSegment(writer, level.type == FieldType::STRING, 4, 3);
    writer.mergeSegments();

    MemPool pool;
    IndexReader indexReader(dir);
    ASSERT_EQ(indexReader.segments().size(), 1u);
    auto& segment = indexReader.segments()[0];
    auto* fieldReader = segment.postingsReader().fieldInfo("f");
    ASSERT_TRUE((fieldReader != nullptr));
    const auto& fieldInfo = *fieldReader;
    ASSERT_EQ(FieldType::hasFreqs(fieldInfo.flags), FieldType::hasFreqs(level.flags));
    ASSERT_EQ(FieldType::hasPositions(fieldInfo.flags), FieldType::hasPositions(level.flags));

    TermsEnum tenum(pool, segment.postingsReader(), fieldInfo);
    ASSERT_TRUE(tenum.seek("hot"));
    int64_t expectedTtf = FieldType::hasFreqs(level.flags) ? 18 : 7;
    EXPECT_EQ(tenum.docFreq(), 7);
    EXPECT_EQ(tenum.totalTermFreq(), expectedTtf);

    IteratedStats stats;
    if (FieldType::hasPositions(level.flags)) {
      DocsPosEnum docsEnum(tenum);
      PosEnum posEnum(docsEnum);
      stats = iterateStats(docsEnum, &posEnum);
    } else {
      DocsFreqEnum docsEnum(tenum);
      stats = iterateStats(docsEnum);
    }
    EXPECT_EQ(stats.df, 7);
    EXPECT_EQ(stats.ttf, expectedTtf);
  }
}

// A single-doc term pulses whatever its tf: the tf comes from ttf, and its
// positions (here up to two positions blocks) stay in the positions stream
// unless it has only one.
TEST_F(TermsDictTest, SingleDocTermsPulseWithAnyTf) {
  std::vector<RawTermSpec> terms = {
    {"t001", {{5, 1}}},
    {"t002", {{9, 2}}},
    {"t005", {{12, 5}}},
    {"t200", {{40, 200}}},
    {"two", {{3, 2}, {8, 1}}}
  };
  for (FieldType::flag_type flags : {
           FieldType::INDEX_DOCS_FREQS,
           FieldType::INDEX_DOCS_FREQS_POSITIONS}) {
    SCOPED_TRACE(::testing::Message() << "flags=" << flags);
    RAMDir dir;
    MemPool pool;
    std::unique_ptr<SegmentCore> reader;
    buildRawField(dir, pool, flags, terms, reader);
    const auto& fieldInfo = *reader->rootReader().fieldInfo("f");
    bool hasPositions = FieldType::hasPositions(flags);

    MetadataRunCodes codes = readFirstBlockMetadataRunCodes(reader->rootReader(), fieldInfo);
    EXPECT_EQ(codes.pulsedMaskCode, Postings::PULSED_MASK_EXPLICIT);

    TermsEnum tenum(pool, reader->rootReader(), fieldInfo);
    for (const RawTermSpec& term : terms) {
      SCOPED_TRACE(term.name);
      ASSERT_TRUE(tenum.nextTerm());
      auto state = tenum.postingsState();
      EXPECT_EQ(state.docsStart == state.docsEnd, term.docs.size() == 1);
      if (hasPositions) {
        std::vector<int32_t> norms;
        std::vector<int32_t> tfs;
        ASSERT_GT(tenum.readTermImpactFrontier(norms, tfs), 0);
        int32_t maxTf = 0;
        for (const RawDocSpec& doc : term.docs) maxTf = std::max(maxTf, doc.tf);
        EXPECT_EQ(tfs.back(), maxTf);
      }

      DocsPosEnum docsEnum(state);
      std::unique_ptr<PosEnum> posEnum;
      if (hasPositions) posEnum = std::make_unique<PosEnum>(docsEnum);
      for (const RawDocSpec& doc : term.docs) {
        ASSERT_EQ(docsEnum.nextDoc(), doc.docid);
        ASSERT_EQ(docsEnum.termFreq(), doc.tf);
        if (hasPositions) {
          // buildRawField writes positions 0, 2, 4, ...
          posEnum->startPositions();
          for (int32_t i = 0; i < doc.tf; i++) {
            ASSERT_EQ(posEnum->nextPosition(), 2 * i);
          }
        }
      }
      EXPECT_EQ(docsEnum.nextDoc(), DocsEnumMeta::END);
    }
  }
}

// A docs-only block where every term pulses writes the one-byte "all" mask
// code and, without positions, no positions offset.
TEST_F(TermsDictTest, AllPulsedDocsOnlyBlockUsesMaskCode) {
  std::vector<RawTermSpec> terms;
  for (int32_t i = 0; i < Postings::TERMS_BLOCK_SIZE; i++) {
    char name[8];
    std::snprintf(name, sizeof(name), "id%03d", i);
    terms.push_back({name, {{i, 1}}});
  }
  RAMDir dir;
  MemPool pool;
  std::unique_ptr<SegmentCore> reader;
  buildRawField(dir, pool, FieldType::INDEX_DOCS, terms, reader);
  const auto& fieldInfo = *reader->rootReader().fieldInfo("f");
  MetadataRunCodes codes = readFirstBlockMetadataRunCodes(reader->rootReader(), fieldInfo);
  EXPECT_EQ(codes.pulsedMaskCode, Postings::PULSED_MASK_ALL);
  EXPECT_EQ(codes.docsEnd, 0u);

  TermsEnum tenum(pool, reader->rootReader(), fieldInfo);
  for (const RawTermSpec& term : terms) {
    ASSERT_TRUE(tenum.nextTerm());
    DocsOnlyEnum docsEnum(tenum);
    EXPECT_EQ(docsEnum.nextDoc(), term.docs[0].docid);
    EXPECT_EQ(docsEnum.nextDoc(), DocsEnumMeta::END);
  }
}

// Terms on both sides of every postings shape boundary: pulsed (df 1),
// headerless tail-only (df 2, 127), one full block (128), a full block plus a
// tail (129, 255), two full blocks and no tail (256), with all-1 and mixed
// tfs, on every index level.  Checks
// iteration, positions, advance, bitset fills, counts, reset and the impact
// readers, which present a headerless term as one block ending at its
// last doc.
TEST_F(TermsDictTest, PostingsShapesAcrossBlockBoundaries) {
  std::vector<RawTermSpec> terms;
  for (int32_t df : {1, 2, 127, 128, 129, 255, 256}) {
    for (bool ones : {true, false}) {
      char name[32];
      std::snprintf(name, sizeof(name), "df%03d_%s", df, ones ? "ones" : "mixed");
      RawTermSpec term{name, {}};
      int32_t doc = df % 3;
      for (int32_t i = 0; i < df; i++) {
        // Runs of consecutive docs, then gaps from 2 to 7000.
        term.docs.push_back({doc, ones ? 1 : 1 + (i * 7) % 5 + (i == df / 2 ? 300 : 0)});
        doc += i % 16 < 8 ? 1 : (i % 3 == 0 ? 7000 : 2 + i % 300);
      }
      terms.push_back(std::move(term));
    }
  }

  for (FieldType::flag_type flags : {
           FieldType::INDEX_DOCS,
           FieldType::INDEX_DOCS_FREQS,
           FieldType::INDEX_DOCS_FREQS_POSITIONS}) {
    SCOPED_TRACE(::testing::Message() << "flags=" << flags);
    bool hasFreqs = FieldType::hasFreqs(flags);
    bool hasPositions = FieldType::hasPositions(flags);
    RAMDir dir;
    MemPool pool;
    std::unique_ptr<SegmentCore> reader;
    buildRawField(dir, pool, flags, terms, reader);
    const auto& fieldInfo = *reader->rootReader().fieldInfo("f");
    int32_t maxDoc = 0;
    for (const RawTermSpec& term : terms) maxDoc = std::max(maxDoc, term.docs.back().docid + 1);

    TermsEnum tenum(pool, reader->rootReader(), fieldInfo);
    for (const RawTermSpec& term : terms) {
      SCOPED_TRACE(term.name);
      ASSERT_TRUE(tenum.nextTerm());
      ASSERT_EQ((std::string_view) tenum.term(), term.name);
      auto state = tenum.postingsState();
      int32_t df = (int32_t) term.docs.size();
      std::vector<int32_t> docs;
      int32_t maxTf = 0;
      for (const RawDocSpec& doc : term.docs) {
        docs.push_back(doc.docid);
        maxTf = std::max(maxTf, hasFreqs ? doc.tf : 1);
      }
      EXPECT_EQ(state.docsStart == state.docsEnd, df == 1);
      if (df < Postings::DOCS_BLOCK_SIZE) {
        EXPECT_EQ(DocsOnlyEnum::firstDocLowerBound(state), docs[0]);
      }

      // Full iteration with tfs and positions.
      {
        DocsPosEnum docsEnum(state);
        std::unique_ptr<PosEnum> posEnum;
        if (hasPositions) posEnum = std::make_unique<PosEnum>(docsEnum);
        for (const RawDocSpec& doc : term.docs) {
          ASSERT_EQ(docsEnum.nextDoc(), doc.docid);
          ASSERT_EQ(docsEnum.termFreq(), hasFreqs ? doc.tf : 1);
          if (hasPositions && doc.docid % 2 == 0) {
            posEnum->startPositions();
            for (int32_t i = 0; i < doc.tf; i++) {
              ASSERT_EQ(posEnum->nextPosition(), 2 * i) << doc.docid;
            }
          }
        }
        EXPECT_EQ(docsEnum.nextDoc(), DocsEnumMeta::END);
      }

      // Scored advance into every third doc, also from a reset enum.
      {
        DocsFreqEnum docsEnum(state);
        for (int32_t pass = 0; pass < 2; pass++) {
          for (int32_t i = 1; i < df; i += 3) {
            ASSERT_EQ(docsEnum.advance(docs[(size_t) i - 1] + 1), docs[(size_t) i]);
            ASSERT_EQ(docsEnum.termFreq(), hasFreqs ? term.docs[(size_t) i].tf : 1);
          }
          EXPECT_EQ(docsEnum.advance(docs.back() + 1), DocsEnumMeta::END);
          docsEnum.reset();
        }
      }

      // Docs-only advance to the middle, then a bitset fill of the rest and,
      // after a reset, a count against a bitset holding every other doc.
      {
        DocsOnlyEnum docsEnum(state);
        int32_t middle = docs[docs.size() / 2];
        EXPECT_EQ(docsEnum.advance(middle), middle);
        std::vector<uint64_t> words(FixedBitSet::sizeInWords(maxDoc), 0);
        docsEnum.intoBitSet(words, 0, maxDoc);
        FixedBitSet bits(words.data(), maxDoc);
        int32_t set = 0;
        for (size_t i = 0; i < docs.size(); i++) {
          EXPECT_EQ(bits.get(docs[i]), i >= docs.size() / 2) << docs[i];
        }
        for (int32_t doc = 0; doc < maxDoc; doc++) set += bits.get(doc) ? 1 : 0;
        EXPECT_EQ(set, df - (int32_t) docs.size() / 2);

        docsEnum.reset();
        std::fill(words.begin(), words.end(), 0);
        int32_t expected = 0;
        for (size_t i = 0; i < docs.size(); i += 2) {
          bits.set(docs[i]);
          expected++;
        }
        EXPECT_EQ(docsEnum.countInBitSet(words), expected);
      }

      // Impact readers: block bounds cover the block's docs and tfs; a
      // headerless term reads as a single block ending at its last doc.
      {
        DocsFreqEnum docsEnum(state);
        std::vector<int32_t> blockMaxTf;
        std::vector<int32_t> blockLastDocs;
        docsEnum.readBlockMaxTf(blockMaxTf, nullptr, &blockLastDocs);
        DocsEnumMeta::GroupImpacts groups;
        docsEnum.readGroupImpacts(groups);
        if (df == 1) {
          EXPECT_EQ(docsEnum.numImpactBlocks(), 0);
          EXPECT_TRUE(blockMaxTf.empty());
        } else {
          int32_t blocks = (df + Postings::DOCS_BLOCK_SIZE - 1) / Postings::DOCS_BLOCK_SIZE;
          ASSERT_EQ(docsEnum.numImpactBlocks(), blocks);
          ASSERT_EQ((int32_t) blockMaxTf.size(), blocks);
          ASSERT_EQ(docsEnum.numImpactGroups(), 1);
          ASSERT_EQ(groups.lastDocs.size(), 1u);
          EXPECT_GE(groups.spanMaxTfs[0], maxTf);
          for (int32_t b = 0; b < blocks; b++) {
            int32_t last = std::min(df, (b + 1) * Postings::DOCS_BLOCK_SIZE) - 1;
            int32_t blockTf = 0;
            for (int32_t i = b * Postings::DOCS_BLOCK_SIZE; i <= last; i++) {
              blockTf = std::max(blockTf, hasFreqs ? term.docs[(size_t) i].tf : 1);
            }
            EXPECT_GE(blockMaxTf[(size_t) b], blockTf) << b;
            if (df < Postings::DOCS_BLOCK_SIZE) {
              EXPECT_EQ(blockLastDocs[(size_t) b], docs.back());
              EXPECT_EQ(groups.lastDocs[0], docs.back());
              // With a term frontier the bound is exact.
              if (hasPositions) {
                EXPECT_EQ(blockMaxTf[(size_t) b], blockTf);
              }
            } else {
              EXPECT_EQ(blockLastDocs[(size_t) b], docs[(size_t) last]);
            }
          }
        }
      }
    }
    EXPECT_FALSE(tenum.nextTerm());
  }
}

// A tail-only term costs its PackedTail block alone: consecutive docs with
// all-1 tfs need only the widths byte, and mixed tfs add tf bits.
TEST_F(TermsDictTest, PackedTailSizes) {
  std::vector<RawTermSpec> terms = {{"mixed", {}}, {"ones", {}}};
  for (int32_t i = 0; i < 100; i++) {
    terms[0].docs.push_back({i, 1 + i % 2});
    terms[1].docs.push_back({i, 1});
  }
  for (FieldType::flag_type flags : {
           FieldType::INDEX_DOCS,
           FieldType::INDEX_DOCS_FREQS,
           FieldType::INDEX_DOCS_FREQS_POSITIONS}) {
    SCOPED_TRACE(::testing::Message() << "flags=" << flags);
    RAMDir dir;
    MemPool pool;
    std::unique_ptr<SegmentCore> reader;
    buildRawField(dir, pool, flags, terms, reader);
    const auto& fieldInfo = *reader->rootReader().fieldInfo("f");
    TermsEnum tenum(pool, reader->rootReader(), fieldInfo);
    ASSERT_TRUE(tenum.nextTerm());
    auto mixed = tenum.postingsState();
    ASSERT_TRUE(tenum.nextTerm());
    auto ones = tenum.postingsState();
    EXPECT_EQ(ones.docsEnd - ones.docsStart, 1);
    EXPECT_EQ(mixed.docsEnd - mixed.docsStart,
              FieldType::hasFreqs(flags) ? 1 + 100 / 8 + 1 : 1);
  }
}

// Low-df keyword and text terms through the whole engine: search before and
// after a merge, with deletes applied to committed segments in between, and
// a fresh reader over the committed index.  Each tag kN holds N docs per
// segment, so per-segment and merged docFreqs cross the pulsed (1) and
// headerless (DOCS_BLOCK_SIZE - 1) boundaries in both directions.
TEST_F(TermsDictTest, LowDfKeywordsSearchAcrossDeletesAndMerge) {
  CollectionHelper helper;
  helper.getIndexWriter()->mergePolicy->setMergeFactor(100);
  const std::vector<int32_t> tags = {1, 2, 3, 64, 127, 128, 129};
  std::map<std::string, std::vector<std::string>> tagIds;
  std::vector<std::string> deletes;
  for (int32_t seg = 0; seg < 2; seg++) {
    std::vector<Doc> docs;
    for (int32_t tag : tags) {
      for (int32_t i = 0; i < tag; i++) {
        std::string id = "s" + std::to_string(seg) + "_k" + std::to_string(tag)
            + "_" + std::to_string(i);
        std::string tagName = "k" + std::to_string(tag);
        docs.push_back(flatdoc("id", id, "tag_s", tagName,
                               "body_w", i == 0 ? tagName + " " + tagName + " end" : "end"));
        // Segment 0 loses two docs of every odd tag.
        if (seg == 0 && tag % 2 == 1 && i < 2) {
          deletes.push_back(id);
        } else {
          tagIds[tagName].push_back(id);
        }
      }
    }
    ASSERT_TRUE(helper.indexAll(docs, UpdateMessage::COMMIT).success);
  }
  ASSERT_TRUE(helper.deleteByIds(deletes, UpdateMessage::COMMIT).success);

  // Match counts, plus the ids of the first 100 hits (one response page),
  // which is every hit of the smaller tags.
  struct Hits {
    int64_t count = 0;
    std::vector<std::string> ids;
  };
  auto search = [&](auto&& configure) {
    auto request = localReq(helper.getSearchEngine());
    configure(request->collection("main").topDocs("q").fields({"id"}).limit(1000).countLimit());
    request->execute();
    Hits hits;
    hits.count = request->getMatchCount();
    for (const auto& doc : request->getDocs()) {
      hits.ids.push_back(std::get<std::string>(*find(doc, "id")));
    }
    std::sort(hits.ids.begin(), hits.ids.end());
    return hits;
  };
  auto verify = [&](std::string_view label) {
    SCOPED_TRACE(label);
    size_t live = 0;
    for (auto& [tagName, ids] : tagIds) {
      SCOPED_TRACE(tagName);
      std::sort(ids.begin(), ids.end());
      live += ids.size();
      Hits hits = search([&](OpCursor& op) { op.matchQuery("tag_s", tagName); });
      EXPECT_EQ(hits.count, (int64_t) ids.size());
      if (ids.size() <= 100) {
        EXPECT_EQ(hits.ids, ids);
      }
      // the tf 2 single-doc text term, through a phrase over its positions
      std::vector<std::string> phraseIds;
      for (const auto& id : ids) {
        if (id.substr(id.rfind('_')) == "_0") phraseIds.push_back(id);
      }
      EXPECT_EQ(search([&](OpCursor& op) {
                  op.phraseText("body_w", tagName + " " + tagName + " end");
                }).ids, phraseIds);
    }
    EXPECT_EQ(search([&](OpCursor& op) { op.prefixQuery("tag_s", "k"); }).count,
              (int64_t) live);
  };

  ASSERT_EQ(2u, helper.durableSegmentCount());
  verify("segments");
  CollectionHelper::UpdateBuilder merge;
  merge.commit(true, 1);
  ASSERT_TRUE(helper.submit(merge).success);
  ASSERT_EQ(1u, helper.durableSegmentCount());
  verify("merged");

  // A fresh reader over the committed files sees the same postings.
  auto reader = std::make_shared<IndexReader>(helper.getIndexWriter()->dir);
  ASSERT_EQ(1u, reader->segments().size());
  Segment& segment = reader->segments()[0];
  auto* tagField = segment.postingsReader().fieldInfo("tag_s");
  ASSERT_NE(nullptr, tagField);
  MemPool pool;
  TermsEnum terms(pool, segment.postingsReader(), *tagField);
  for (const auto& [tagName, ids] : tagIds) {
    ASSERT_TRUE(terms.seek(tagName)) << tagName;
    EXPECT_EQ(terms.docFreq(), (int32_t) ids.size()) << tagName;
    DocsOnlyEnum docsEnum(terms);
    int32_t count = 0;
    while (docsEnum.nextDoc() != DocsEnumMeta::END) count++;
    EXPECT_EQ(count, (int32_t) ids.size()) << tagName;
  }
}
