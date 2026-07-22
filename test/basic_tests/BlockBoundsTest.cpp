#include <bit>
#include <cmath>
#include <limits>
#include <string>
#include <vector>

#include "solux/index/BlockBoundsBuilder.h"
#include "solux/query/ImpactsIndex.h"
#include "solux/query/TermQuery.h"
#include "solux/reader/BlockBounds.h"
#include "solux/reader/DocsEnum.h"
#include "solux/reader/FieldReader.h"
#include "solux/reader/TermsEnum.h"
#include "solux/search/IndexReader.h"
#include "test/SoluxTest.h"
#include "test/TestIndex.h"

namespace solux::test {

namespace {

struct FixtureIndex {
  TestIndex index;
  std::string field = "body_w";

  FixtureIndex() {
    TestField f(index, field);
    f.startIndexing();
    for (int32_t doc = 0; doc < 700; doc++) {
      switch (doc % 4) {
        case 0: f.add(doc, "common common common alpha pad"); break;
        case 1: f.add(doc, "common common beta pad pad"); break;
        case 2: f.add(doc, "common gamma pad"); break;
        default: f.add(doc, "common delta pad pad pad pad"); break;
      }
    }
    index.flush();
    index.initReader();
  }

  SegFieldInfo fieldInfo(IndexReader& reader) {
    MemPool pool;
    FieldReader fields(pool, reader.segments()[0].postingsReader());
    EXPECT_TRUE(fields.seek(field));
    SegFieldInfo info{};
    fields.readFieldInfo(info);
    return info;
  }

  BlockBoundsBuilder::Result build() {
    auto& segment = index.reader->segments()[0];
    SegFieldInfo info = fieldInfo(*index.reader);
    return BlockBoundsBuilder::build(index.dir, segment.segInfo.seg_id,
                                     segment.postingsReader(), info);
  }
};

std::vector<std::pair<int32_t, uint32_t>> queryScores(IndexReader& reader,
                                                       std::string_view field) {
  MemPool pool;
  Query::Context context(pool, reader);
  TermQuery query(field, "common");
  Query::Weight* weight = query.createWeight(context, Query::NEED_SCORES);
  Query::Scorer* scorer = weight->createScorer(pool, reader.segments()[0]);
  std::vector<std::pair<int32_t, uint32_t>> out;
  for (int32_t doc = scorer->next(); doc != DocsEnumMeta::END; doc = scorer->next()) {
    out.emplace_back(doc, std::bit_cast<uint32_t>(scorer->score()));
  }
  return out;
}

std::vector<char> fileBytes(Directory& dir, std::string_view name) {
  auto file = dir.openFile(name);
  InputStream in = file->getInputStream();
  return {in.ptr(), in.ptr() + in.size()};
}

void replaceFile(Directory& dir, std::string_view name, std::span<const char> bytes) {
  auto file = dir.createFile(name);
  OutputStream out(file.get());
  out.write(bytes.data(), bytes.size());
  out.close();
  dir.finishFile(*file);
}

void putU32(std::vector<char>& bytes, size_t off, uint32_t value) {
  memcpy(bytes.data() + off, &value, sizeof(value));
}

uint64_t getU64(const std::vector<char>& bytes, size_t off) {
  uint64_t value;
  memcpy(&value, bytes.data() + off, sizeof(value));
  return value;
}

void refreshChecksum(std::vector<char>& bytes) {
  uint64_t crc = BlockBounds::checksum({bytes.data(), bytes.size() - 8});
  memcpy(bytes.data() + bytes.size() - 8, &crc, sizeof(crc));
}

} // namespace

class BlockBoundsTest : public SoluxTest {};

TEST_F(BlockBoundsTest, sourceWalkGeometryAndQueryIdentity) {
  FixtureIndex fixture;
  auto baseline = queryScores(*fixture.index.reader, fixture.field);
  auto result = fixture.build();
  EXPECT_EQ(12u, result.blocks);
  EXPECT_EQ(2u, result.groups);

  IndexReader attached(fixture.index.dir);
  ASSERT_EQ(1u, attached.segments().size());
  const BlockBounds* bounds = attached.segments()[0].blockBounds(fixture.field);
  ASSERT_NE(nullptr, bounds);
  SegFieldInfo info = fixture.fieldInfo(attached);
  MemPool pool;
  TermsEnum terms(pool, attached.segments()[0].postingsReader(), info);
  ASSERT_TRUE(terms.seek("common"));
  DocsOnlyEnum docs(terms);
  EXPECT_EQ(terms.ord(), docs.termOrd());
  auto view = bounds->find(docs.termOrd());
  ASSERT_TRUE(view);
  EXPECT_EQ(docs.numImpactBlocks(), view.blockCount());
  EXPECT_EQ(docs.numImpactGroups(), view.groupCount());
  ASSERT_TRUE(terms.seek("alpha"));
  EXPECT_FALSE(bounds->find(terms.ord()));

  Similarity similarity;
  std::array<float, 256> envelopeInv;
  for (int32_t norm = 0; norm < 256; norm++) {
    envelopeInv[(size_t) norm] = Similarity::bm25InvNorm(
        similarity.k1, similarity.b, SmallFloat::decodeLengthByte((uint8_t) norm),
        bounds->envelopeAvgdl());
  }
  DocsEnumMeta::GroupImpacts groups;
  docs.readGroupImpacts(groups);
  ASSERT_EQ(groups.lastDocs.size(), (size_t) view.groupCount());
  for (int32_t g = 0; g < view.groupCount(); g++) {
    EXPECT_EQ(groups.lastDocs[(size_t) g], view.groupLastDoc(g));
    for (int32_t i = groups.frontiers.offsets[(size_t) g];
         i < groups.frontiers.offsets[(size_t) g + 1]; i++) {
      float exact = Similarity::bm25Denominator(
          (float) groups.frontiers.tfs[(size_t) i],
          envelopeInv[(size_t) groups.frontiers.norms[(size_t) i]]);
      EXPECT_GE(view.groupDenominator(g), exact);
    }
  }
  int32_t block = 0;
  DocsEnumMeta::GroupBlockImpactScratch scratch;
  for (int32_t g = 0; g < view.groupCount(); g++) {
    docs.visitGroupBlockImpacts(
        g, groups.bodyOffsets[(size_t) g], groups.baseLastDocs[(size_t) g], scratch,
        [&](int32_t, int32_t lastDoc, int32_t, int32_t,
            std::span<const int32_t> tfs, std::span<const int32_t> norms, bool spilled) {
          ASSERT_FALSE(spilled);
          EXPECT_EQ(lastDoc, view.blockLastDoc(block));
          for (size_t i = 0; i < tfs.size(); i++) {
            float exact = Similarity::bm25Denominator(
                (float) tfs[i], envelopeInv[(size_t) norms[i]]);
            EXPECT_GE(view.blockDenominator(block), exact);
          }
          EXPECT_EQ(block, view.blockContaining(lastDoc));
          block++;
        });
  }
  EXPECT_EQ(view.blockCount(), block);
  EXPECT_EQ(baseline, queryScores(attached, fixture.field));
}

TEST_F(BlockBoundsTest, denominatorDominatesScalarAndVectorKernels) {
  Similarity similarity;
  const float generatingAvgdl = 37.25f;
  const float envelope = 2.0f * generatingAvgdl;
  const std::array<uint32_t, 7> tfs{1, 2, 127, 65535, 65536, 1000000,
                                    (uint32_t) INT32_MAX};
  const std::array<float, 3> avgdls{generatingAvgdl / 8.0f, generatingAvgdl, envelope};
  const std::array<float, 3> idfs{0.01f, 1.0f, 17.0f};
  const std::array<float, 4> boosts{0.0f, 1.0f, 3.5f, 1000000.0f};
  for (int32_t norm = 0; norm < 256; norm++) {
    float length = SmallFloat::decodeLengthByte((uint8_t) norm);
    float envelopeInv = Similarity::bm25InvNorm(
        similarity.k1, similarity.b, length, envelope);
    for (uint32_t tf : tfs) {
      float upper = Similarity::bm25DenominatorUpper(tf, envelopeInv);
      for (float avgdl : avgdls) {
        for (float idf : idfs) {
          Similarity::BM25Scorer scorer(1.0f, similarity.k1, similarity.b, idf, avgdl);
          for (float boost : boosts) {
            float bound = scorer.scoreFromUpperDenominator(upper, boost);
            float scalar = boost * scorer.score((float) tf, norm);
            int32_t tfBlock = (int32_t) tf;
            uint8_t normBlock = (uint8_t) norm;
            float vector = -1.0f;
            scorer.scoreBlock(&tfBlock, &normBlock, boost, &vector, 1);
            ASSERT_GE(bound, scalar) << norm << " " << tf << " " << avgdl;
            ASSERT_GE(bound, vector) << norm << " " << tf << " " << avgdl;
          }
        }
      }
    }
  }
}

TEST_F(BlockBoundsTest, corruptionAndEnvelopeFailuresFallBack) {
  FixtureIndex fixture;
  auto result = fixture.build();
  auto original = fileBytes(fixture.index.dir, result.fileName);
  IndexReader reader(fixture.index.dir);
  SegFieldInfo info = fixture.fieldInfo(reader);
  uint64_t segId = reader.segments()[0].segInfo.seg_id;
  Similarity similarity;
  Similarity::FieldStats stats;
  stats.docsWithField = info.docsWithField;
  stats.sumTotalTermFreq = info.sumTotalTermFreq;
  float avgdl = similarity.avgFieldLength(stats);

  const std::array<size_t, 5> truncations{
      BlockBounds::FIXED_HEADER_SIZE, BlockBounds::FIXED_HEADER_SIZE + fixture.field.size(),
      (size_t) getU64(original, 64), original.size() - 8, original.size() - 1};
  for (size_t size : truncations) {
    replaceFile(fixture.index.dir, result.fileName, {original.data(), size});
    EXPECT_EQ(nullptr, BlockBounds::open(fixture.index.dir, segId, info, 700, avgdl).get());
  }
  const std::array<size_t, 4> flips{0, BlockBounds::FIXED_HEADER_SIZE,
                                    BlockBounds::FIXED_HEADER_SIZE + fixture.field.size(),
                                    original.size() - 1};
  for (size_t off : flips) {
    auto corrupt = original;
    corrupt[off] ^= 0x40;
    replaceFile(fixture.index.dir, result.fileName, corrupt);
    EXPECT_EQ(nullptr, BlockBounds::open(fixture.index.dir, segId, info, 700, avgdl).get());
  }
  auto stale = original;
  uint64_t wrongSeg = segId + 1;
  memcpy(stale.data() + 28, &wrongSeg, sizeof(wrongSeg));
  refreshChecksum(stale);
  replaceFile(fixture.index.dir, result.fileName, stale);
  EXPECT_EQ(nullptr, BlockBounds::open(fixture.index.dir, segId, info, 700, avgdl).get());

  auto wrongTerms = original;
  putU32(wrongTerms, 40, (uint32_t) info.nTerms + 1);
  refreshChecksum(wrongTerms);
  replaceFile(fixture.index.dir, result.fileName, wrongTerms);
  EXPECT_EQ(nullptr, BlockBounds::open(fixture.index.dir, segId, info, 700, avgdl).get());
  IndexReader corruptFallback(fixture.index.dir);
  EXPECT_EQ(nullptr, corruptFallback.segments()[0].blockBounds(fixture.field));
  EXPECT_EQ(700u, queryScores(corruptFallback, fixture.field).size());

  fixture.index.dir.deleteFile(result.fileName);
  replaceFile(fixture.index.dir, result.fileName + ".build.crashed", original);
  IndexReader crashTempFallback(fixture.index.dir);
  EXPECT_EQ(nullptr, crashTempFallback.segments()[0].blockBounds(fixture.field));
  EXPECT_EQ(700u, queryScores(crashTempFallback, fixture.field).size());
  fixture.index.dir.deleteFile(result.fileName + ".build.crashed");

  replaceFile(fixture.index.dir, result.fileName, original);
  EXPECT_EQ(nullptr, BlockBounds::open(
      fixture.index.dir, segId, info, 700, result.envelope + 1.0f).get());
  IndexReader fallback(fixture.index.dir);
  EXPECT_NE(nullptr, fallback.segments()[0].blockBounds(fixture.field));
  EXPECT_EQ(700u, queryScores(fallback, fixture.field).size());
}

TEST_F(BlockBoundsTest, boostGuardsAndGeometryStateAreIndependent) {
  FixtureIndex fixture;
  fixture.build();
  IndexReader reader(fixture.index.dir);
  SegFieldInfo info = fixture.fieldInfo(reader);
  MemPool pool;
  TermsEnum terms(pool, reader.segments()[0].postingsReader(), info);
  ASSERT_TRUE(terms.seek("common"));
  DocsOnlyEnum docs(terms);
  auto* field = reader.segments()[0].blockBounds(fixture.field);
  ASSERT_NE(nullptr, field);
  auto view = field->find(docs.termOrd());
  Similarity::FieldStats stats;
  stats.docsWithField = info.docsWithField;
  stats.sumTotalTermFreq = info.sumTotalTermFreq;
  Similarity::TermStats termStats{docs.numDocs(), docs.totalTermFreq()};
  auto scorer = Similarity().getScorer(1.0f, stats, termStats);

  ImpactsIndex zero;
  zero.build(pool, docs, scorer, 0.0f, true, view);
  ASSERT_TRUE(zero.hasSidecarGeometry());
  EXPECT_EQ(0.0f, zero.sidecarBlockBoundUpper(0));
  ImpactsIndex negative;
  negative.build(pool, docs, scorer, -1.0f, true, view);
  EXPECT_FALSE(negative.hasSidecarGeometry());
  ImpactsIndex nan;
  nan.build(pool, docs, scorer, std::numeric_limits<float>::quiet_NaN(), true, view);
  EXPECT_FALSE(nan.hasSidecarGeometry());
  ImpactsIndex infinity;
  infinity.build(pool, docs, scorer, std::numeric_limits<float>::infinity(), true, view);
  EXPECT_FALSE(infinity.hasSidecarGeometry());

  ImpactsIndex positive;
  positive.build(pool, docs, scorer, 2.0f, true, view);
  ASSERT_TRUE(positive.hasSidecarGeometry());
  EXPECT_EQ(view.blockContaining(511), positive.sidecarBlockContaining(511));
  int32_t before = positive.blockContaining(511);
  EXPECT_EQ(view.blockContaining(511), before);
  EXPECT_EQ(view.blockContaining(511), positive.sidecarBlockContaining(511));
}

} // namespace solux::test
