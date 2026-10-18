// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include <gtest/gtest.h>

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
#include "luxir/reader/FieldReader.h"
#include "luxir/search/IndexReader.h"
#include "luxir/reader/PostingsReader.h"
#include "luxir/schema/Schema.h"
#include "luxir/schema/FieldType.h"
#include "test/CollectionHelper.h"
#include "test/LocalReq.h"
#include "test/LuxirTest.h"
#include "test/TestIndex.h"
#include "test/TestUtils.h"

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
  TermsEnum te(guard.pool(), seg->postingsReader(), field.fieldInfo);
  std::vector<std::string> got;
  while (te.nextTerm()) {
    got.push_back(std::string((std::string_view)te.term()));
  }
  return got;
}

std::vector<std::string> collectSeekCeil(TestField& field) {
  auto guard = field.testIndex.pool.rewindScopeGuard();
  auto* seg = field.currentSegment();
  TermsEnum te(guard.pool(), seg->postingsReader(), field.fieldInfo);
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
  TermsEnum te(guard.pool(), seg->postingsReader(), field.fieldInfo);
  for (int i = (int)expected.size() - 1; i >= 0; i--) {
    ASSERT_TRUE(te.seek(expected[(size_t)i])) << i;
    EXPECT_EQ((std::string_view)te.term(), expected[(size_t)i]) << i;
  }
}

void expectCeil(TestField& field, std::string_view target, std::string_view expected) {
  auto guard = field.testIndex.pool.rewindScopeGuard();
  auto* seg = field.currentSegment();
  TermsEnum te(guard.pool(), seg->postingsReader(), field.fieldInfo);
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
  uint32_t pulsedGaps = 0;
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
                   std::unique_ptr<PostingsReader>& reader,
                   SegFieldInfo& fieldInfo) {
  PostingsWriter postingsWriter(dir, 0, 1000000);
  {
    TextWriter writer(postingsWriter);
    auto& finfo = postingsWriter.addField("f");
    finfo.type = FieldType::TEXT;
    finfo.flags = flags;
    writer.startField(&finfo);

    for (const RawTermSpec& term : terms) {
      TermRef termRef(pool, term.name.data(), (uint32_t) term.name.size());
      writer.startTerm(termRef);
      for (const RawDocSpec& doc : term.docs) {
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
    writer.endField();
  }
  postingsWriter.finish();

  reader = std::make_unique<PostingsReader>(dir, 0);
  FieldReader fieldReader(*reader);
  ASSERT_TRUE(fieldReader.readNextField());
  fieldReader.readFieldInfo(fieldInfo);
}

constexpr uint64_t HIGH_RANGE_ORD = (uint64_t{1} << 32) + 17;

class RangeInspectTermsEnum : public TermsEnum {
public:
  using TermsEnum::TermsEnum;
  using TermsEnum::rowForTerm;
  using TermsEnum::selectRow;
};

void buildRawHighOrdRangeField(RAMDir& dir, MemPool& pool,
                               std::unique_ptr<PostingsReader>& reader,
                               SegFieldInfo& fieldInfo) {
  PostingsWriter postingsWriter(dir, 0, 1);
  auto& finfo = postingsWriter.addField("f");
  finfo.type = FieldType::TEXT;
  finfo.flags = FieldType::INDEX_DOCS;
  {
    TextWriter writer(postingsWriter);
    writer.startField(&finfo);
    for (int32_t i = 0; i < 3 * Postings::TERMS_BLOCK_SIZE; i++) {
      char term[16];
      snprintf(term, sizeof(term), "t%03d", i);
      TermRef termRef(pool, term, 4);
      writer.startTerm(termRef);
      writer.addDoc(0, 1);
      writer.endTerm(termRef);
    }
    writer.endField();
  }

  {
    auto tableOut = postingsWriter.getOutputStream();
    tableOut->align(8);
    finfo.flags |= FieldType::TERM_RANGES;
    finfo.rangeTableLoc = tableOut->slocation();
    finfo.nTerms = (int64_t) HIGH_RANGE_ORD + 2 * Postings::TERMS_BLOCK_SIZE;
    finfo.sumDocFreq = finfo.nTerms;
    finfo.sumTotalTermFreq = finfo.nTerms;

    const TermRangeTableHeader header{3, 3};
    const TermRangeRow rows[] = {
      {0, 0, 0, finfo.docsLoc, finfo.posLoc, finfo.termsLoc, 0, 0},
      {HIGH_RANGE_ORD, 1, 0, finfo.docsLoc, finfo.posLoc, finfo.termsLoc, 0, 0},
      {HIGH_RANGE_ORD + Postings::TERMS_BLOCK_SIZE, 2, 0,
       finfo.docsLoc, finfo.posLoc, finfo.termsLoc, 0, 0}
    };
    tableOut->write(&header, sizeof(header));
    tableOut->write(rows, sizeof(rows));
  }
  postingsWriter.finish();

  reader = std::make_unique<PostingsReader>(dir, 0);
  FieldReader fieldReader(*reader);
  ASSERT_TRUE(fieldReader.readNextField());
  fieldReader.readFieldInfo(fieldInfo);
}

MetadataRunCodes readFirstBlockMetadataRunCodes(PostingsReader& reader, const SegFieldInfo& fieldInfo) {
  InputStream termsIS = reader.getInputStreamSeek(fieldInfo.termsLoc);
  int32_t nTerms = (int32_t)std::min<int64_t>(Postings::TERMS_BLOCK_SIZE,
                                               fieldInfo.nTerms);
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
  if (!FieldType::hasFreqs(fieldInfo.flags)) {
    codes.pulsedGaps = termsIS.readVint();
  }
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

TEST_F(TermsDictTest, RangeTableSeeksAcross64BitTermOrdinals) {
  RAMDir dir;
  MemPool pool;
  std::unique_ptr<PostingsReader> reader;
  SegFieldInfo fieldInfo;
  buildRawHighOrdRangeField(dir, pool, reader, fieldInfo);

  ASSERT_GT(fieldInfo.nTerms, (int64_t) UINT32_MAX);
  RangeInspectTermsEnum terms(pool, *reader, fieldInfo);
  const TermRangeRow* first = terms.rowForTerm((int64_t) HIGH_RANGE_ORD - 1);
  const TermRangeRow* second = terms.rowForTerm((int64_t) HIGH_RANGE_ORD);
  const TermRangeRow* third = terms.rowForTerm(
      (int64_t) HIGH_RANGE_ORD + Postings::TERMS_BLOCK_SIZE);
  EXPECT_EQ(first->firstTermOrd, 0u);
  EXPECT_EQ(second->firstTermOrd, HIGH_RANGE_ORD);
  EXPECT_EQ(third->firstTermOrd, HIGH_RANGE_ORD + Postings::TERMS_BLOCK_SIZE);

  terms.selectRow(second);
  terms.seekOrd((int64_t) HIGH_RANGE_ORD + 5);
  EXPECT_EQ(terms.ord(), (int64_t) HIGH_RANGE_ORD + 5);
  EXPECT_EQ((std::string_view) terms.term(), "t037");

  terms.seekOrd((int64_t) HIGH_RANGE_ORD + 2 * Postings::TERMS_BLOCK_SIZE - 1);
  EXPECT_EQ(terms.ord(), (int64_t) HIGH_RANGE_ORD + 2 * Postings::TERMS_BLOCK_SIZE - 1);
  EXPECT_EQ((std::string_view) terms.term(), "t095");

  terms.seekOrd((int64_t) HIGH_RANGE_ORD + Postings::TERMS_BLOCK_SIZE - 1);
  EXPECT_EQ(terms.ord(), (int64_t) HIGH_RANGE_ORD + Postings::TERMS_BLOCK_SIZE - 1);
  EXPECT_EQ((std::string_view) terms.term(), "t063");

  // Block docFreqs step down across the 64-bit row boundary.
  TermsEnum blocks(pool, *reader, fieldInfo);
  auto last = blocks.docFreqBlock(fieldInfo.nTerms - 1);
  EXPECT_EQ(last.firstOrd, (int64_t) HIGH_RANGE_ORD + Postings::TERMS_BLOCK_SIZE);
  EXPECT_EQ(last.docFreqs.size(), (size_t) Postings::TERMS_BLOCK_SIZE);
  auto previous = blocks.docFreqBlock(last.firstOrd - 1);
  EXPECT_EQ(previous.firstOrd, (int64_t) HIGH_RANGE_ORD);
  EXPECT_EQ(previous.docFreqs[5], 1u);
  blocks.seekOrd((int64_t) HIGH_RANGE_ORD + 5);
  EXPECT_EQ((std::string_view) blocks.term(), "t037");
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
    TermsEnum te(guard.pool(), seg->postingsReader(), field.fieldInfo);
    ASSERT_TRUE(te.seek(expected[Postings::TERMS_BLOCK_SIZE]));
    ASSERT_EQ(te.ord(), Postings::TERMS_BLOCK_SIZE);
    EXPECT_EQ(te.docFreq(), 1);
    EXPECT_EQ(te.totalTermFreq(), 1);
  }

  {
    auto guard = field.testIndex.pool.rewindScopeGuard();
    auto* seg = field.currentSegment();
    TermsEnum te(guard.pool(), seg->postingsReader(), field.fieldInfo);
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
  TermsEnum tenum(guard.pool(), seg->postingsReader(), field.fieldInfo);
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
  std::unique_ptr<PostingsReader> reader;
  SegFieldInfo fieldInfo;
  buildRawField(dir, pool, FieldType::INDEX_DOCS_FREQS_POSITIONS, terms, reader, fieldInfo);

  MetadataRunCodes codes = readFirstBlockMetadataRunCodes(*reader, fieldInfo);
  EXPECT_EQ(codes.docsEnd, 0u);
  EXPECT_EQ(codes.packedBlocks, 0u);
  EXPECT_EQ(codes.df, 0u);
  EXPECT_EQ(codes.ttfCode, 0u);
  EXPECT_EQ(codes.posOff, 0u);
  EXPECT_NE(codes.termImpact, 0u);
  EXPECT_NE(codes.pulsed, 0u);

  TermsEnum tenum(pool, *reader, fieldInfo);
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
  std::unique_ptr<PostingsReader> reader;
  SegFieldInfo fieldInfo;
  buildRawField(dir, pool, FieldType::INDEX_DOCS_FREQS_POSITIONS, terms, reader, fieldInfo);

  // Every term has one doc, so every term pulses whatever its tf; the tf 2
  // terms keep their positions in the positions stream.
  MetadataRunCodes codes = readFirstBlockMetadataRunCodes(*reader, fieldInfo);
  EXPECT_EQ(codes.pulsedMaskCode, Postings::PULSED_MASK_ALL);
  EXPECT_EQ(codes.docsEnd, 0u);
  EXPECT_EQ(codes.packedBlocks, 0u);
  EXPECT_EQ(codes.df, 0u);
  EXPECT_NE(codes.ttfCode, 0u);
  EXPECT_NE(codes.posOff, 0u);
  EXPECT_NE(codes.termImpact, 0u);
  EXPECT_NE(codes.pulsed, 0u);

  TermsEnum tenum(pool, *reader, fieldInfo);
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
    std::unique_ptr<PostingsReader> reader;
    SegFieldInfo fieldInfo;
    buildRawField(dir, pool, flags, terms, reader, fieldInfo);

    TermsEnum tenum(pool, *reader, fieldInfo);
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
    std::unique_ptr<PostingsReader> reader;
    SegFieldInfo fieldInfo;
    buildRawField(dir, pool, flags, terms, reader, fieldInfo);

    auto check = [&]<DocsEnumTier Tier>() {
      TermsEnum tenum(pool, *reader, fieldInfo);
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
    CommitSnapshotRegistry writerSnapshots(dir);
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
  std::unique_ptr<PostingsReader> reader;
  SegFieldInfo fieldInfo;
  buildRawField(dir, pool, FieldType::INDEX_DOCS_FREQS_POSITIONS, terms, reader, fieldInfo);

  TermsEnum tenum(pool, *reader, fieldInfo);
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
    std::unique_ptr<PostingsReader> reader;
    SegFieldInfo fieldInfo;
    buildRawField(dir, pool, flags, terms, reader, fieldInfo);

    auto check = [&]<DocsEnumTier Tier>() {
      TermsEnum tenum(pool, *reader, fieldInfo);
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
    CommitSnapshotRegistry writerSnapshots(dir);
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

// Docs-only terms pulse up to PULSED_MAX_DOCS docs, with the docs after the
// first stored as gaps in the dictionary; longer terms keep the docs stream.
TEST_F(TermsDictTest, DocsOnlyTermsPulseUpToMaxDocs) {
  std::vector<RawTermSpec> terms;
  for (int32_t df = 1; df <= Postings::PULSED_MAX_DOCS + 2; df++) {
    RawTermSpec term{"k" + std::to_string(df), {}};
    int32_t doc = df;
    for (int32_t i = 0; i < df; i++) {
      term.docs.push_back({doc, 1});
      doc += i % 3 == 0 ? 3 : i % 3 == 1 ? 300 : 70000;  // 1, 2 and 3 byte gaps
    }
    terms.push_back(std::move(term));
  }
  std::sort(terms.begin(), terms.end(),
            [](const RawTermSpec& a, const RawTermSpec& b) { return a.name < b.name; });

  RAMDir dir;
  MemPool pool;
  std::unique_ptr<PostingsReader> reader;
  SegFieldInfo fieldInfo;
  buildRawField(dir, pool, FieldType::INDEX_DOCS, terms, reader, fieldInfo);

  MetadataRunCodes codes = readFirstBlockMetadataRunCodes(*reader, fieldInfo);
  EXPECT_EQ(codes.pulsedMaskCode, Postings::PULSED_MASK_EXPLICIT);
  EXPECT_NE(codes.docsEnd, 0u);
  EXPECT_NE(codes.pulsedGaps, 0u);

  TermsEnum tenum(pool, *reader, fieldInfo);
  for (const RawTermSpec& term : terms) {
    SCOPED_TRACE(term.name);
    ASSERT_TRUE(tenum.nextTerm());
    ASSERT_EQ((std::string_view) tenum.term(), term.name);
    std::vector<int32_t> expected;
    for (const RawDocSpec& doc : term.docs) expected.push_back(doc.docid);
    auto state = tenum.postingsState();
    EXPECT_EQ(state.docsStart == state.docsEnd,
              (int32_t) expected.size() <= Postings::PULSED_MAX_DOCS);
    EXPECT_EQ(DocsOnlyEnum::firstDocLowerBound(state), expected[0]);

    DocsOnlyEnum docsEnum(state);
    std::vector<int32_t> seen;
    for (int32_t doc = docsEnum.nextDoc(); doc != DocsEnumMeta::END; doc = docsEnum.nextDoc()) {
      seen.push_back(doc);
    }
    EXPECT_EQ(seen, expected);

    // advance into the middle, then a bitset fill of the rest
    docsEnum.reset();
    int32_t middle = expected[expected.size() / 2];
    EXPECT_EQ(docsEnum.advance(middle), middle);
    std::vector<uint64_t> words(FixedBitSet::sizeInWords(1000000), 0);
    docsEnum.intoBitSet(words, 0, 1000000);
    FixedBitSet bits(words.data(), 1000000);
    for (size_t i = 0; i < expected.size(); i++) {
      EXPECT_EQ(bits.get(expected[i]), i >= expected.size() / 2) << expected[i];
    }

    DocsFreqEnum freqEnum(state);
    seen.clear();
    for (int32_t doc = freqEnum.nextDoc(); doc != DocsEnumMeta::END; doc = freqEnum.nextDoc()) {
      EXPECT_EQ(freqEnum.termFreq(), 1);
      seen.push_back(doc);
    }
    EXPECT_EQ(seen, expected);
  }
}

// Single-doc terms pulse whatever their tf: the tf comes from ttf, and the
// positions (here up to two positions blocks) stay in the positions stream.
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
    std::unique_ptr<PostingsReader> reader;
    SegFieldInfo fieldInfo;
    buildRawField(dir, pool, flags, terms, reader, fieldInfo);
    bool hasPositions = FieldType::hasPositions(flags);

    TermsEnum tenum(pool, *reader, fieldInfo);
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
          EXPECT_EQ(posEnum->nextPosition(), PosEnum::END);
        }
      }
      EXPECT_EQ(docsEnum.nextDoc(), DocsEnumMeta::END);
    }
  }
}

// A StreamVByte tail whose tfs are all 1 drops its freq run; the reader
// restores tf 1 from ttfCode == 0, also behind full blocks that keep theirs.
TEST_F(TermsDictTest, AllOnesTailOmitsFreqs) {
  std::vector<RawTermSpec> terms = {
    {"blocks_ones", {}}, {"tail_mixed", {}}, {"tail_ones", {}}
  };
  for (int32_t i = 0; i < 2 * Postings::DOCS_BLOCK_SIZE + 40; i++) {
    terms[0].docs.push_back({i * 3, 1});
  }
  for (int32_t i = 0; i < 50; i++) {
    terms[1].docs.push_back({i * 7, i == 49 ? 2 : 1});
    terms[2].docs.push_back({i * 7, 1});
  }

  for (FieldType::flag_type flags : {
           FieldType::INDEX_DOCS_FREQS,
           FieldType::INDEX_DOCS_FREQS_POSITIONS}) {
    SCOPED_TRACE(::testing::Message() << "flags=" << flags);
    RAMDir dir;
    MemPool pool;
    std::unique_ptr<PostingsReader> reader;
    SegFieldInfo fieldInfo;
    buildRawField(dir, pool, flags, terms, reader, fieldInfo);

    TermsEnum tenum(pool, *reader, fieldInfo);
    int64_t tailBytes[3];
    for (size_t t = 0; t < terms.size(); t++) {
      const RawTermSpec& term = terms[t];
      SCOPED_TRACE(term.name);
      ASSERT_TRUE(tenum.nextTerm());
      ASSERT_EQ((std::string_view) tenum.term(), term.name);
      auto state = tenum.postingsState();
      tailBytes[t] = state.docsEnd - state.docsStart;

      DocsFreqEnum freqEnum(state);
      for (const RawDocSpec& doc : term.docs) {
        ASSERT_EQ(freqEnum.nextDoc(), doc.docid);
        ASSERT_EQ(freqEnum.termFreq(), doc.tf);
      }
      EXPECT_EQ(freqEnum.nextDoc(), DocsEnumMeta::END);

      DocsOnlyEnum docsEnum(state);
      for (const RawDocSpec& doc : term.docs) {
        ASSERT_EQ(docsEnum.nextDoc(), doc.docid);
      }
      EXPECT_EQ(docsEnum.nextDoc(), DocsEnumMeta::END);
    }
    // Same docs: the mixed tail pays a freq run (13 key bytes + 50 values).
    EXPECT_GE(tailBytes[1], tailBytes[2] + 13 + 50);
  }
}

// Low-df keyword terms through the whole engine: search before and after a
// merge, with deletes applied to committed segments in between.  Each tag kN
// holds N docs per segment, so per-segment and merged docFreqs cross
// PULSED_MAX_DOCS in both directions.
TEST_F(TermsDictTest, LowDfKeywordsSearchAcrossDeletesAndMerge) {
  CollectionHelper helper;
  helper.getIndexWriter()->mergePolicy->setMergeFactor(100);
  constexpr int32_t MAX_TAG = Postings::PULSED_MAX_DOCS + 3;
  std::map<std::string, std::vector<std::string>> tagIds;
  std::vector<std::string> deletes;
  for (int32_t seg = 0; seg < 2; seg++) {
    std::vector<Doc> docs;
    for (int32_t tag = 1; tag <= MAX_TAG; tag++) {
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

  auto search = [&](auto&& configure) {
    auto request = localReq(helper.getSearchEngine());
    configure(request->collection("main").topDocs("q").fields({"id"}).limit(-1));
    request->execute();
    std::vector<std::string> ids;
    for (const auto& doc : request->getDocs()) {
      ids.push_back(std::get<std::string>(*find(doc, "id")));
    }
    std::sort(ids.begin(), ids.end());
    return ids;
  };
  auto verify = [&](std::string_view label) {
    SCOPED_TRACE(label);
    size_t live = 0;
    for (auto& [tagName, ids] : tagIds) {
      SCOPED_TRACE(tagName);
      std::sort(ids.begin(), ids.end());
      live += ids.size();
      EXPECT_EQ(search([&](OpCursor& op) { op.matchQuery("tag_s", tagName); }), ids);
      // the tf 2 single-doc text term, through a phrase over its positions
      std::vector<std::string> phraseIds;
      for (const auto& id : ids) {
        if (id.substr(id.rfind('_')) == "_0") phraseIds.push_back(id);
      }
      EXPECT_EQ(search([&](OpCursor& op) {
                  op.phraseText("body_w", tagName + " " + tagName + " end");
                }), phraseIds);
    }
    EXPECT_EQ(search([&](OpCursor& op) { op.prefixQuery("tag_s", "k"); }).size(), live);
  };

  ASSERT_EQ(2u, helper.durableSegmentCount());
  verify("segments");
  CollectionHelper::UpdateBuilder merge;
  merge.commit(true, 1);
  ASSERT_TRUE(helper.submit(merge).success);
  ASSERT_EQ(1u, helper.durableSegmentCount());
  verify("merged");
}
