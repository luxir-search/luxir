#include <gtest/gtest.h>

#include <algorithm>
#include <cstdio>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "solux/index/PostingsWriter.h"
#include "solux/index/IndexWriter.h"
#include "solux/query/Query.h"
#include "solux/reader/DocsEnum.h"
#include "solux/reader/FieldReader.h"
#include "solux/search/IndexReader.h"
#include "solux/reader/PostingsReader.h"
#include "solux/schema/Schema.h"
#include "solux/schema/FieldType.h"
#include "test/SoluxTest.h"
#include "test/TestIndex.h"

using namespace solux;
using namespace solux::test;

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

struct IteratedStats {
  int32_t df = 0;
  int64_t ttf = 0;
};

struct MetadataRunCodes {
  uint32_t docsEnd = 0;
  uint32_t df = 0;
  uint32_t ttfCode = 0;
  uint32_t posOff = 0;
  uint32_t pulsed = 0;
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

IteratedStats iterateStats(DocsEnum& docsEnum, bool readPositions) {
  IteratedStats stats;
  for (;;) {
    int32_t doc = docsEnum.nextDoc();
    if (doc == DocsEnum::END) {
      break;
    }
    unused(doc);
    stats.df++;
    int32_t tf = docsEnum.termFreq();
    stats.ttf += tf;
    if (readPositions) {
      docsEnum.startPositions();
      for (int32_t i = 0; i < tf; i++) {
        EXPECT_NE(docsEnum.nextPosition(), DocsEnum::END);
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
  FieldReader fieldReader(pool, *reader);
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
  FieldReader fieldReader(pool, *reader);
  ASSERT_TRUE(fieldReader.readNextField());
  fieldReader.readFieldInfo(fieldInfo);
}

MetadataRunCodes readFirstBlockMetadataRunCodes(PostingsReader& reader, const SegFieldInfo& fieldInfo) {
  InputStream termsIS = reader.getInputStreamSeek(fieldInfo.termsLoc);
  int32_t nTerms = (int32_t)std::min<int64_t>(Postings::TERMS_BLOCK_SIZE,
                                               fieldInfo.nTerms);
  termsIS.readPackedTerm();
  termsIS.readVlong();
  termsIS.readVlong();
  termsIS.readByte();
  termsIS.skip((int64_t) sizeof(uint32_t));
  uint32_t suffixBytesTotal = termsIS.readVint();
  termsIS.skip(nTerms);
  termsIS.skip(nTerms - 1);
  termsIS.skip(nTerms - 1);
  termsIS.skip(suffixBytesTotal);

  MetadataRunCodes codes;
  codes.docsEnd = termsIS.readVint();
  codes.df = termsIS.readVint();
  if (FieldType::hasFreqs(fieldInfo.flags)) {
    codes.ttfCode = termsIS.readVint();
  }
  if (FieldType::hasPositions(fieldInfo.flags)) {
    codes.posOff = termsIS.readVint();
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
  IteratedStats stats;
  for (int i = 0; i < (int) reader.segments().size(); i++) {
    TermsEnum* termsEnum = cachedFieldInfo.termsEnums[i];
    if (termsEnum == nullptr || !termsEnum->seek(term)) {
      continue;
    }
    DocsEnum docsEnum(pool, reader.segments()[(size_t) i].postingsReader(), *termsEnum);
    IteratedStats segmentStats = iterateStats(docsEnum, readPositions);
    stats.df += segmentStats.df;
    stats.ttf += segmentStats.ttf;
  }
  return stats;
}

} // namespace

class TermsDictTest : public SoluxTest {
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
    DocsEnum docsEnum(guard.pool(), seg->postingsReader(), tenum);
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
  EXPECT_EQ(codes.df, 0u);
  EXPECT_EQ(codes.ttfCode, 0u);
  EXPECT_EQ(codes.posOff, 0u);
  EXPECT_NE(codes.pulsed, 0u);

  TermsEnum tenum(pool, *reader, fieldInfo);
  for (const RawTermSpec& term : terms) {
    ASSERT_TRUE(tenum.nextTerm()) << term.name;
    ASSERT_EQ((std::string_view) tenum.term(), term.name);
    int32_t dictDf = tenum.docFreq();
    int64_t dictTtf = tenum.totalTermFreq();
    DocsEnum docsEnum(pool, *reader, tenum);
    IteratedStats stats = iterateStats(docsEnum, true);
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

  MetadataRunCodes codes = readFirstBlockMetadataRunCodes(*reader, fieldInfo);
  EXPECT_NE(codes.docsEnd, 0u);
  EXPECT_EQ(codes.df, 0u);
  EXPECT_NE(codes.ttfCode, 0u);
  EXPECT_NE(codes.posOff, 0u);
  EXPECT_NE(codes.pulsed, 0u);

  TermsEnum tenum(pool, *reader, fieldInfo);
  for (const RawTermSpec& term : terms) {
    ASSERT_TRUE(tenum.nextTerm()) << term.name;
    int32_t dictDf = tenum.docFreq();
    int64_t dictTtf = tenum.totalTermFreq();
    DocsEnum docsEnum(pool, *reader, tenum);
    IteratedStats stats = iterateStats(docsEnum, true);
    EXPECT_EQ(dictDf, stats.df) << term.name;
    EXPECT_EQ(dictTtf, stats.ttf) << term.name;
    EXPECT_EQ(stats.ttf, expectedTtf(fieldInfo.flags, term)) << term.name;
  }
  EXPECT_FALSE(tenum.nextTerm());
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

    TermsEnum tenum(pool, *reader, fieldInfo);
    for (const RawTermSpec& term : terms) {
      ASSERT_TRUE(tenum.nextTerm()) << term.name;
      ASSERT_EQ((std::string_view) tenum.term(), term.name);
      int32_t dictDf = tenum.docFreq();
      int64_t dictTtf = tenum.totalTermFreq();
      DocsEnum docsEnum(pool, *reader, tenum);

      int32_t seenDocs = 0;
      int64_t seenTtf = 0;
      for (const RawDocSpec& doc : term.docs) {
        ASSERT_EQ(docsEnum.nextDoc(), doc.docid) << term.name;
        int32_t expectedTf = FieldType::hasFreqs(flags) ? doc.tf : 1;
        ASSERT_EQ(docsEnum.termFreq(), expectedTf) << term.name;
        seenDocs++;
        seenTtf += expectedTf;
        if (FieldType::hasPositions(flags)) {
          docsEnum.startPositions();
          for (int32_t i = 0; i < expectedTf; i++) {
            ASSERT_EQ(docsEnum.nextPosition(), i * 2) << term.name;
          }
        }
      }
      ASSERT_EQ(docsEnum.nextDoc(), DocsEnum::END) << term.name;
      EXPECT_EQ(dictDf, seenDocs) << term.name;
      EXPECT_EQ(dictTtf, seenTtf) << term.name;
      EXPECT_EQ(dictDf, (int32_t) term.docs.size()) << term.name;
      EXPECT_EQ(dictTtf, expectedTtf(flags, term)) << term.name;
    }
    ASSERT_FALSE(tenum.nextTerm());
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
    IndexWriter writer(dir, [schema]() { return schema; });
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
    DocsEnum docsEnum(pool, *reader, tenum);
    ASSERT_EQ(docsEnum.nextDoc(), term.docs[0].docid) << term.name;
    EXPECT_EQ(docsEnum.termFreq(), 1) << term.name;
    docsEnum.startPositions();
    EXPECT_EQ(docsEnum.nextPosition(), 0) << term.name;
    EXPECT_EQ(docsEnum.nextDoc(), DocsEnum::END) << term.name;
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

    TermsEnum tenum(pool, *reader, fieldInfo);
    for (std::string_view name : {"a", "c"}) {
      ASSERT_TRUE(tenum.seek(name)) << name;
      DocsEnum docsEnum(pool, *reader, tenum);
      int32_t expectedDoc = name == "a" ? 7 : 19;
      ASSERT_EQ(docsEnum.nextDoc(), expectedDoc) << name;
      EXPECT_EQ(docsEnum.termFreq(), 1) << name;
      if (FieldType::hasPositions(flags)) {
        docsEnum.startPositions();
        EXPECT_EQ(docsEnum.nextPosition(), 0) << name;
      }
      EXPECT_EQ(docsEnum.nextDoc(), DocsEnum::END) << name;
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
    IndexWriter writer(dir, [schema]() { return schema; });
    addMergeSegment(writer, level.type == FieldType::STRING, 3, 2);
    addMergeSegment(writer, level.type == FieldType::STRING, 4, 3);
    writer.mergeSegments();

    MemPool pool;
    IndexReader indexReader(dir);
    ASSERT_EQ(indexReader.segments().size(), 1u);
    auto& segment = indexReader.segments()[0];
    FieldReader fieldReader(pool, segment.postingsReader());
    ASSERT_TRUE(fieldReader.seek("f"));
    SegFieldInfo fieldInfo;
    fieldReader.readFieldInfo(fieldInfo);
    ASSERT_EQ(FieldType::hasFreqs(fieldInfo.flags), FieldType::hasFreqs(level.flags));
    ASSERT_EQ(FieldType::hasPositions(fieldInfo.flags), FieldType::hasPositions(level.flags));

    TermsEnum tenum(pool, segment.postingsReader(), fieldInfo);
    ASSERT_TRUE(tenum.seek("hot"));
    int64_t expectedTtf = FieldType::hasFreqs(level.flags) ? 18 : 7;
    EXPECT_EQ(tenum.docFreq(), 7);
    EXPECT_EQ(tenum.totalTermFreq(), expectedTtf);

    DocsEnum docsEnum(pool, segment.postingsReader(), tenum);
    IteratedStats stats = iterateStats(docsEnum, FieldType::hasPositions(level.flags));
    EXPECT_EQ(stats.df, 7);
    EXPECT_EQ(stats.ttf, expectedTtf);
  }
}
