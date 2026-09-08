// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include <bit>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "gtest/gtest.h"
#include "luxir/query/TermQuery.h"
#include "test/LuxirTest.h"
#include "test/TestIndex.h"
#include "test/TestUtils.h"

using namespace luxir;
using namespace luxir::test;

namespace {

struct NormDoc {
  int32_t doc;
  int32_t length;
};

struct ScoreHit {
  int32_t doc;
  int32_t tf;
  int32_t length;
};

SegFieldInfo readFieldInfo(TestIndex& testIndex, std::string_view field) {
  auto& seg = testIndex.reader->segments()[0];
  FieldReader fieldReader(seg.postingsReader());
  EXPECT_TRUE(fieldReader.seek(field));
  SegFieldInfo fieldInfo;
  fieldReader.readFieldInfo(fieldInfo);
  return fieldInfo;
}

void assertNormDocs(PostingsReader& postingsReader, const SegFieldInfo& fieldInfo,
                    std::span<const NormDoc> expected) {
  NormsReader reader(postingsReader, fieldInfo);
  NormsReader::Iterator iter(reader);
  for (const auto& e : expected) {
    ASSERT_EQ(e.doc, iter.next());
    uint8_t encoded = SmallFloat::intToByte4(e.length);
    EXPECT_EQ(encoded, iter.value()) << "doc=" << e.doc;
    EXPECT_EQ(encoded, reader.value(e.doc)) << "doc=" << e.doc;
  }
  EXPECT_EQ(NormsReader::ENDDOC, iter.next());
}

void assertTermScores(TestIndex& testIndex, std::string_view field, std::string_view term,
                      std::span<const ScoreHit> expected) {
  auto& seg = testIndex.reader->segments()[0];

  MemPool expectedPool;
  FieldReader fieldReader(seg.postingsReader());
  ASSERT_TRUE(fieldReader.seek(field));
  SegFieldInfo fieldInfo;
  fieldReader.readFieldInfo(fieldInfo);
  TermsEnum termsEnum(expectedPool, seg.postingsReader(), fieldInfo);
  ASSERT_TRUE(termsEnum.seek(term));
  DocsFreqEnum docsEnum(termsEnum);

  Similarity::FieldStats fieldStats;
  fieldStats.sumTotalTermFreq = termsEnum.sumTotalTermFreq();
  fieldStats.sumDocFreq = termsEnum.sumDocFreq();
  fieldStats.docsWithField = termsEnum.docsWithField();
  fieldStats.maxDoc = seg.maxDoc();

  Similarity::TermStats termStats;
  termStats.docFreq = docsEnum.numDocs();
  termStats.totalTermFreq = docsEnum.totalTermFreq();

  Similarity sim;
  Similarity::BM25Scorer simScorer = sim.getScorer(1.0f, fieldStats, termStats);

  MemPool queryPool;
  Query::Context qContext(queryPool, *testIndex.reader);
  TermQuery query(field, term);
  auto* weight = query.createWeight(qContext, Query::NEED_SCORES);
  auto* scorer = weight->createScorer(queryPool, qContext.topReader.segments()[0]);
  ASSERT_NE(nullptr, scorer);

  for (const auto& e : expected) {
    ASSERT_EQ(e.doc, scorer->next());
    uint8_t encodedNorm = SmallFloat::intToByte4(e.length);
    float expectedScore = simScorer.score((float)e.tf, (int64_t)encodedNorm);
    float actualScore = scorer->score();
    EXPECT_EQ(std::bit_cast<uint32_t>(expectedScore),
              std::bit_cast<uint32_t>(actualScore)) << "doc=" << e.doc;
  }
  EXPECT_EQ(PostingsReader::END, scorer->next());
}

} // namespace

class NormsFormatTest : public LuxirTest {
};

TEST_F(NormsFormatTest, denseFieldUsesFlatNoBitset) {
  TestIndex testIndex;
  TestField f(testIndex, "body_w");
  f.startIndexing();
  f.add(0, "needle a");
  f.add(1, "needle needle a");
  f.add(2, "a needle");
  f.add(3, "needle");
  testIndex.flush();
  testIndex.initReader();

  SegFieldInfo info = readFieldInfo(testIndex, "body_w");
  auto& postingsReader = testIndex.reader->segments()[0].postingsReader();
  ASSERT_EQ(4, postingsReader.maxDoc());
  EXPECT_EQ(4, info.docsWithField);
  EXPECT_EQ(SegFieldInfo::NORMS_FLAT, info.normsFormat);
  EXPECT_EQ(4, info.normsLen);
  EXPECT_EQ(0u, info.docsWithFieldEndLoc.offset());
  EXPECT_EQ(0u, info.columnLoc.offset());
  EXPECT_EQ(0, info.columnMetaOff);

  NormsReader reader(postingsReader, info);
  EXPECT_TRUE(reader.isFlat());
  EXPECT_FALSE(reader.docsReader().hasBitset());
  const std::vector<NormDoc> norms = {{0, 2}, {1, 3}, {2, 2}, {3, 1}};
  assertNormDocs(postingsReader, info, norms);

  const std::vector<ScoreHit> hits = {{0, 1, 2}, {1, 2, 3}, {2, 1, 2}, {3, 1, 1}};
  assertTermScores(testIndex, "body_w", "needle", hits);
}

TEST_F(NormsFormatTest, partialFlatStoresPresenceButReadsByDoc) {
  TestIndex testIndex;
  TestField f(testIndex, "body_w");
  f.startIndexing();
  f.add(0, "needle");
  f.add(2, "needle needle filler");
  f.add(4, "filler filler");
  f.add(6, "needle filler filler filler");
  f.add(9, "needle filler");
  testIndex.flush();
  testIndex.initReader();

  SegFieldInfo info = readFieldInfo(testIndex, "body_w");
  auto& postingsReader = testIndex.reader->segments()[0].postingsReader();
  ASSERT_EQ(10, postingsReader.maxDoc());
  EXPECT_EQ(5, info.docsWithField);
  EXPECT_EQ(SegFieldInfo::NORMS_FLAT, info.normsFormat);
  EXPECT_EQ(10, info.normsLen);
  EXPECT_NE(0u, info.docsWithFieldEndLoc.offset());

  NormsReader reader(postingsReader, info);
  EXPECT_TRUE(reader.isFlat());
  EXPECT_TRUE(reader.docsReader().hasBitset());
  EXPECT_EQ(0, (int)reader.value(1));
  const std::vector<NormDoc> norms = {{0, 1}, {2, 3}, {4, 2}, {6, 4}, {9, 2}};
  assertNormDocs(postingsReader, info, norms);

  const std::vector<ScoreHit> hits = {{0, 1, 1}, {2, 2, 3}, {6, 1, 4}, {9, 1, 2}};
  assertTermScores(testIndex, "body_w", "needle", hits);
}

TEST_F(NormsFormatTest, sparseFieldUsesBitsetRankOrdinal) {
  TestIndex testIndex;
  TestField f(testIndex, "body_w");
  f.startIndexing();
  f.add(1, "needle filler");
  f.add(4, "needle needle filler filler");
  f.add(7, "filler");
  f.add(9, "needle filler filler");
  testIndex.flush();
  testIndex.initReader();

  SegFieldInfo info = readFieldInfo(testIndex, "body_w");
  auto& postingsReader = testIndex.reader->segments()[0].postingsReader();
  ASSERT_EQ(10, postingsReader.maxDoc());
  EXPECT_EQ(4, info.docsWithField);
  EXPECT_EQ(SegFieldInfo::NORMS_SPARSE, info.normsFormat);
  EXPECT_EQ(4, info.normsLen);
  EXPECT_NE(0u, info.docsWithFieldEndLoc.offset());

  NormsReader reader(postingsReader, info);
  EXPECT_TRUE(reader.isSparse());
  EXPECT_TRUE(reader.docsReader().hasBitset());
  const std::vector<NormDoc> norms = {{1, 2}, {4, 4}, {7, 1}, {9, 3}};
  assertNormDocs(postingsReader, info, norms);

  const std::vector<ScoreHit> hits = {{1, 1, 2}, {4, 2, 4}, {9, 1, 3}};
  assertTermScores(testIndex, "body_w", "needle", hits);
}

TEST_F(NormsFormatTest, sparseHighDocidsStayOrdinalSized) {
  TestIndex testIndex;
  TestField f(testIndex, "body_w");
  f.startIndexing();
  f.add(90000, "needle filler");
  f.add(95000, "needle needle filler filler");
  f.add(99000, "filler");
  f.add(99999, "needle filler filler");
  testIndex.flush();
  testIndex.initReader();

  SegFieldInfo info = readFieldInfo(testIndex, "body_w");
  auto& postingsReader = testIndex.reader->segments()[0].postingsReader();
  ASSERT_EQ(100000, postingsReader.maxDoc());
  EXPECT_EQ(4, info.docsWithField);
  EXPECT_EQ(SegFieldInfo::NORMS_SPARSE, info.normsFormat);
  EXPECT_EQ(4, info.normsLen);
  EXPECT_LT(info.normsLen * 1000, postingsReader.maxDoc());
  EXPECT_NE(0u, info.docsWithFieldEndLoc.offset());

  NormsReader reader(postingsReader, info);
  EXPECT_TRUE(reader.isSparse());
  EXPECT_TRUE(reader.docsReader().hasBitset());
  const std::vector<NormDoc> norms = {
      {90000, 2}, {95000, 4}, {99000, 1}, {99999, 3}};
  assertNormDocs(postingsReader, info, norms);

  const std::vector<ScoreHit> hits = {
      {90000, 1, 2}, {95000, 2, 4}, {99999, 1, 3}};
  assertTermScores(testIndex, "body_w", "needle", hits);
}

TEST_F(NormsFormatTest, multiFieldNormsStayIndependent) {
  TestIndex testIndex;
  TestField title(testIndex, "title_w");
  TestField body(testIndex, "body_w");
  title.startIndexing();
  body.startIndexing();
  for (int32_t doc = 0; doc < 6; doc++) {
    title.add(doc, doc == 3 ? "needle title title" : "needle title");
    if (doc == 0) {
      body.add(doc, "needle body body");
    } else if (doc == 5) {
      body.add(doc, "needle body");
    }
  }
  testIndex.flush();
  testIndex.initReader();

  auto& postingsReader = testIndex.reader->segments()[0].postingsReader();
  ASSERT_EQ(6, postingsReader.maxDoc());

  SegFieldInfo titleInfo = readFieldInfo(testIndex, "title_w");
  EXPECT_EQ(6, titleInfo.docsWithField);
  EXPECT_EQ(SegFieldInfo::NORMS_FLAT, titleInfo.normsFormat);
  EXPECT_EQ(6, titleInfo.normsLen);
  EXPECT_EQ(0u, titleInfo.docsWithFieldEndLoc.offset());
  const std::vector<NormDoc> titleNorms = {
      {0, 2}, {1, 2}, {2, 2}, {3, 3}, {4, 2}, {5, 2}};
  assertNormDocs(postingsReader, titleInfo, titleNorms);

  SegFieldInfo bodyInfo = readFieldInfo(testIndex, "body_w");
  EXPECT_EQ(2, bodyInfo.docsWithField);
  EXPECT_EQ(SegFieldInfo::NORMS_SPARSE, bodyInfo.normsFormat);
  EXPECT_EQ(2, bodyInfo.normsLen);
  EXPECT_NE(0u, bodyInfo.docsWithFieldEndLoc.offset());
  const std::vector<NormDoc> bodyNorms = {{0, 3}, {5, 2}};
  assertNormDocs(postingsReader, bodyInfo, bodyNorms);

  const std::vector<ScoreHit> titleHits = {
      {0, 1, 2}, {1, 1, 2}, {2, 1, 2}, {3, 1, 3}, {4, 1, 2}, {5, 1, 2}};
  assertTermScores(testIndex, "title_w", "needle", titleHits);
  const std::vector<ScoreHit> bodyHits = {{0, 1, 3}, {5, 1, 2}};
  assertTermScores(testIndex, "body_w", "needle", bodyHits);
}
