// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include <algorithm>
#include <cstdio>
#include <iterator>
#include <numeric>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "luxir/codec/LinearPack.h"
#include "luxir/index/MergeCostModel.h"
#include "luxir/index/OrdColWriter.h"
#include "luxir/reader/OrdColReader.h"
#include "luxir/search/ops/DomainIter.h"
#include "luxir/search/SortField.h"
#include "test/CollectionHelper.h"
#include "test/LocalReq.h"
#include "test/QueryBuild.h"
#include "test/LuxirTest.h"
#include "test/TestIndex.h"
#include "test/SchemaBuilder.h"
#include "test/TestUtils.h"

using namespace luxir;
using namespace luxir::test;

namespace {

class OrdFormatGuard {
  OrdColWriter::FormatOverride saved;

public:
  explicit OrdFormatGuard(OrdColWriter::FormatOverride value)
      : saved(OrdColWriter::setFormatOverrideForTests(value)) {}
  ~OrdFormatGuard() { OrdColWriter::setFormatOverrideForTests(saved); }
};

class OrdSortModeGuard {
  StringSortMode saved;
public:
  explicit OrdSortModeGuard(StringSortMode mode) : saved(SortField::setStringSortModeForTests(mode)) {}
  ~OrdSortModeGuard() { SortField::setStringSortModeForTests(saved); }
};

class OrdBulkMinHitsGuard {
  int32_t saved;

public:
  explicit OrdBulkMinHitsGuard(int32_t value)
      : saved(OrdColReader::setBulkMinHitsForTests(value)) {}
  ~OrdBulkMinHitsGuard() {
    OrdColReader::setBulkMinHitsForTests(saved);
  }
};

struct FacetOutput {
  std::vector<std::pair<std::string, int64_t>> buckets;
  int64_t missing = 0;
  bool operator==(const FacetOutput&) const = default;
};

std::string ordinalTerm(int32_t ord) {
  char buf[16];
  snprintf(buf, sizeof(buf), "%08d", ord);
  return buf;
}

void verifyPredictedShape(bool descending, bool jitter) {
  OrdFormatGuard guard(OrdColWriter::FormatOverride::PREDICTED);
  TestIndex index;
  TestField field(index, "shape_s");
  field.startIndexing();
  constexpr int32_t count = 4101;
  std::vector<int32_t> expected(count);
  for (int32_t doc = 0; doc < count; doc++) {
    int32_t value = descending ? count - 1 - doc : doc;
    if (jitter && (doc & 1) == 0 && doc + 1 < count) value++;
    else if (jitter && (doc & 1) != 0) value--;
    expected[doc] = value + 1;
    field.add(doc, ordinalTerm(value));
  }
  index.flush();
  field.startReading();
  for (int32_t doc = 0; doc < count; doc++) {
    ASSERT_EQ(doc, field.nextDoc());
    ASSERT_EQ(expected[doc], field.ord());
  }
  ASSERT_EQ(-1, field.nextDoc());
  EXPECT_EQ(SegFieldInfo::ORD_PREDICTED, field.fieldInfo.ordFormat);
  EXPECT_EQ(SegFieldInfo::ORD_DOCID, field.fieldInfo.ordIndexing);
  EXPECT_EQ(13, field.fieldInfo.ordBits);
}

std::vector<char> encodeDocOrds(int32_t count, uint8_t bits) {
  std::vector<char> encoded(LinearPack::byteSize((uint64_t)count, bits));
  LinearPack::Writer writer(encoded.data(), bits);
  for (int32_t doc = 0; doc < count; doc++) {
    writer.append((uint32_t)doc + 1);
  }
  writer.finish();
  return encoded;
}

} // namespace

class OrdColFormatTest : public LuxirTest {};

TEST_F(OrdColFormatTest, directDocIdAndRankShapes) {
  OrdFormatGuard guard(OrdColWriter::FormatOverride::DIRECT);
  {
    TestIndex index;
    TestField field(index, "dense_s");
    field.startIndexing();
    for (int32_t doc = 0; doc < 10; doc++) {
      if (doc != 2 && doc != 7) field.add(doc, ordinalTerm(doc));
    }
    index.flush();
    field.startReading();
    ASSERT_EQ(0, field.nextDoc());
    EXPECT_EQ(SegFieldInfo::ORD_DIRECT, field.fieldInfo.ordFormat);
    EXPECT_EQ(SegFieldInfo::ORD_DOCID, field.fieldInfo.ordIndexing);
  }
  {
    TestIndex index;
    TestField field(index, "sparse_s");
    field.startIndexing();
    field.add(1, "a");
    field.add(20, "b");
    index.flush();
    field.startReading();
    ASSERT_EQ(1, field.nextDoc());
    EXPECT_EQ(SegFieldInfo::ORD_DIRECT, field.fieldInfo.ordFormat);
    EXPECT_EQ(SegFieldInfo::ORD_RANK, field.fieldInfo.ordIndexing);
  }
}

TEST_F(OrdColFormatTest, declaredMultiObservedSingleUsesDocIdWithoutMono) {
  OrdFormatGuard guard(OrdColWriter::FormatOverride::DIRECT);
  TestIndex index;
  TestField field(index, "tags_ss");
  field.startIndexing();
  field.addStrings(0, {"a"});
  field.addStrings(1, {"b"});
  index.flush();
  field.startReading();
  ASSERT_EQ(0, field.nextDoc());
  EXPECT_EQ(SegFieldInfo::ORD_DOCID, field.fieldInfo.ordIndexing);
  EXPECT_EQ(0u, field.fieldInfo.monoLoc.offset());
  ASSERT_FALSE(field.ordReader->multiValued());

  std::vector<int32_t> sortedDocs = {0, 1};
  int32_t calls = 0;
  OrdColReader::getValues(index.pool, field.currentSegment()->postingsReader(),
                          field.fieldInfo, sortedDocs,
      [&](size_t inputIndex, int32_t doc, int32_t ord,
          int64_t valIdx, int64_t numVals) {
        EXPECT_EQ((size_t)calls, inputIndex);
        EXPECT_EQ(calls, doc);
        EXPECT_EQ(calls + 1, ord);
        EXPECT_EQ(0, valIdx);
        EXPECT_EQ(1, numVals);
        calls++;
      });
  EXPECT_EQ(2, calls);
}

TEST_F(OrdColFormatTest, observedMultiUsesRankAndMono) {
  OrdFormatGuard guard(OrdColWriter::FormatOverride::DIRECT);
  TestIndex index;
  TestField field(index, "tags_ss");
  field.startIndexing();
  field.addStrings(0, {"a", "b"});
  field.addStrings(1, {"c"});
  index.flush();
  field.startReading();
  ASSERT_EQ(0, field.nextDoc());
  EXPECT_EQ(SegFieldInfo::ORD_RANK, field.fieldInfo.ordIndexing);
  EXPECT_NE(0u, field.fieldInfo.monoLoc.offset());
  EXPECT_TRUE(field.ordReader->multiValued());
}

TEST_F(OrdColFormatTest, bitWidthOneAndThirtyTwoDecode) {
  {
    OrdFormatGuard guard(OrdColWriter::FormatOverride::DIRECT);
    TestIndex index;
    TestField field(index, "one_s");
    field.startIndexing();
    field.add(0, "same");
    field.add(1, "same");
    index.flush();
    field.startReading();
    ASSERT_EQ(0, field.nextDoc());
    EXPECT_EQ(1, field.fieldInfo.ordBits);
    EXPECT_EQ(1, field.ord());
  }

  std::vector<uint32_t> values = {0, 1, INT32_MAX, 42};
  std::vector<char> encoded(LinearPack::byteSize(values.size(), 32));
  LinearPack::Writer writer(encoded.data(), 32);
  for (uint32_t value : values) writer.append(value);
  writer.finish();
  OrdColReader reader(encoded.data(), (int32_t)values.size(), 32);
  for (int32_t doc = 0; doc < (int32_t)values.size(); doc++) {
    EXPECT_EQ((int32_t)values[doc], reader.ordAt(doc));
  }
}

TEST_F(OrdColFormatTest, arrayDomainAlwaysUsesPointOrds) {
  constexpr int32_t count = 1024;
  std::vector<char> encoded = encodeDocOrds(count, 11);
  OrdColReader reader(encoded.data(), count, 11);
  ArrDocSet domain(std::vector<int32_t>{0, 127, 128, 511, 512, 1023});
  OrdBulkMinHitsGuard threshold(0);
  OrdColReader::ForEachOrdStats stats;
  std::vector<std::pair<int32_t, int64_t>> seen;
  int64_t missing = 0;

  forEachOrdValue(&domain, reader, OrdMap::SegToGlobal::identity(), count,
      missing,
      [&](int32_t doc, int64_t ord) {
        seen.emplace_back(doc, ord);
      },
      &stats);

  ASSERT_EQ(domain.docs().size(), seen.size());
  for (auto [doc, ord] : seen) EXPECT_EQ(doc, ord);
  EXPECT_EQ(0, missing);
  EXPECT_EQ((int64_t)seen.size(), stats.pointLoads);
  EXPECT_EQ(0, stats.bulkLoads);
  EXPECT_EQ(0, stats.bulkBlocks);
}

TEST_F(OrdColFormatTest, docIdIndexedBitDomainChoosesPerBlockAtThreshold) {
  constexpr int32_t count = 256;
  std::vector<char> encoded = encodeDocOrds(count, 9);
  OrdColReader reader(encoded.data(), count, 9);
  ASSERT_TRUE(reader.docIdIndexed());
  RAMBitDocSet domain(count);
  for (int32_t doc : {0, 64, 127, 128, 129, 191, 255}) {
    domain.mutableBits().set(doc);
  }
  OrdBulkMinHitsGuard threshold(4);
  OrdColReader::ForEachOrdStats stats;
  std::vector<int32_t> seen;
  int64_t missing = 0;

  forEachOrdValue(&domain, reader, OrdMap::SegToGlobal::identity(), count,
      missing,
      [&](int32_t doc, int64_t ord) {
        EXPECT_EQ(doc, ord);
        seen.push_back(doc);
      },
      &stats);

  EXPECT_EQ((std::vector<int32_t>{0, 64, 127, 128, 129, 191, 255}), seen);
  EXPECT_EQ(0, missing);
  EXPECT_EQ(3, stats.pointLoads);
  EXPECT_EQ(4, stats.bulkLoads);
  EXPECT_EQ(1, stats.pointBlocks);
  EXPECT_EQ(1, stats.bulkBlocks);
}

TEST_F(OrdColFormatTest, bitDomainScalesThresholdForTailBlock) {
  constexpr int32_t count = 192;
  std::vector<char> encoded = encodeDocOrds(count, 8);
  OrdColReader reader(encoded.data(), count, 8);
  RAMBitDocSet domain(count);
  for (int32_t doc = 65; doc < 128; doc++) {
    domain.mutableBits().set(doc);
  }
  for (int32_t doc = 128; doc < 160; doc++) {
    domain.mutableBits().set(doc);
  }
  OrdBulkMinHitsGuard threshold(64);
  OrdColReader::ForEachOrdStats stats;
  std::vector<int32_t> seen;
  int64_t missing = 0;

  forEachOrdValue(&domain, reader, OrdMap::SegToGlobal::identity(), count,
      missing,
      [&](int32_t doc, int64_t ord) {
        EXPECT_EQ(doc, ord);
        seen.push_back(doc);
      },
      &stats);

  ASSERT_EQ(95, (int32_t)seen.size());
  EXPECT_EQ(65, seen.front());
  EXPECT_EQ(127, seen[62]);
  EXPECT_EQ(128, seen[63]);
  EXPECT_EQ(159, seen.back());
  EXPECT_EQ(63, stats.pointLoads);
  EXPECT_EQ(32, stats.bulkLoads);
  EXPECT_EQ(1, stats.pointBlocks);
  EXPECT_EQ(1, stats.bulkBlocks);
  EXPECT_EQ(0, missing);
}

TEST_F(OrdColFormatTest, rankIndexedArrayDomainAlwaysUsesPointOrds) {
  TestIndex index;
  TestField field(index, "tags_ss");
  field.startIndexing();
  field.addStrings(0, {"a", "b"});
  field.addStrings(2, {"c"});
  field.addStrings(5, {"d", "e", "f"});
  index.flush();
  field.startReading();
  ASSERT_EQ(0, field.nextDoc());
  ASSERT_EQ(SegFieldInfo::ORD_RANK, field.fieldInfo.ordIndexing);
  ASSERT_TRUE(field.ordReader->multiValued());

  ArrDocSet domain(std::vector<int32_t>{0, 1, 2, 4, 5});
  OrdBulkMinHitsGuard threshold(0);
  OrdColReader::ForEachOrdStats stats;
  std::vector<int32_t> seenDocs;
  int64_t missing = 0;

  forEachOrdValue(&domain, *field.ordReader, OrdMap::SegToGlobal::identity(),
      6, missing,
      [&](int32_t doc, int64_t ord) {
        EXPECT_GE(ord, 0);
        seenDocs.push_back(doc);
      },
      &stats);

  EXPECT_EQ((std::vector<int32_t>{0, 0, 2, 5, 5, 5}), seenDocs);
  EXPECT_EQ(2, missing);
  EXPECT_EQ(6, stats.pointLoads);
  EXPECT_EQ(0, stats.bulkLoads);
  EXPECT_EQ(0, stats.bulkBlocks);
}

TEST_F(OrdColFormatTest, rankIndexedBitAndAllDomainsKeepFixedBulkOrds) {
  TestIndex index;
  TestField field(index, "tags_ss");
  field.startIndexing();
  field.addStrings(0, {"a", "b"});
  field.addStrings(2, {"c"});
  field.addStrings(5, {"d", "e", "f"});
  index.flush();
  field.startReading();
  ASSERT_EQ(0, field.nextDoc());
  ASSERT_EQ(SegFieldInfo::ORD_RANK, field.fieldInfo.ordIndexing);
  ASSERT_TRUE(field.ordReader->multiValued());

  RAMBitDocSet domain(6);
  for (int32_t doc : {0, 1, 2, 4, 5}) {
    domain.mutableBits().set(doc);
  }
  OrdBulkMinHitsGuard threshold(0);
  OrdColReader::ForEachOrdStats stats;
  std::vector<int32_t> seenDocs;
  int64_t missing = 0;

  forEachOrdValue(&domain, *field.ordReader, OrdMap::SegToGlobal::identity(),
      6, missing,
      [&](int32_t doc, int64_t ord) {
        EXPECT_GE(ord, 0);
        seenDocs.push_back(doc);
      },
      &stats);

  EXPECT_EQ((std::vector<int32_t>{0, 0, 2, 5, 5, 5}), seenDocs);
  EXPECT_EQ(2, missing);
  EXPECT_EQ(0, stats.pointLoads);
  EXPECT_EQ(6, stats.bulkLoads);
  EXPECT_EQ(0, stats.pointBlocks);
  EXPECT_EQ(0, stats.bulkBlocks);

  stats = {};
  seenDocs.clear();
  missing = 0;
  forEachOrdValue(nullptr, *field.ordReader, OrdMap::SegToGlobal::identity(),
      6, missing,
      [&](int32_t doc, int64_t ord) {
        EXPECT_GE(ord, 0);
        seenDocs.push_back(doc);
      },
      &stats);

  EXPECT_EQ((std::vector<int32_t>{0, 0, 2, 5, 5, 5}), seenDocs);
  EXPECT_EQ(3, missing);
  EXPECT_EQ(0, stats.pointLoads);
  EXPECT_EQ(6, stats.bulkLoads);
  EXPECT_EQ(0, stats.pointBlocks);
  EXPECT_EQ(0, stats.bulkBlocks);
}

// The batched walk produces, for every column and domain shape, exactly the
// values and missing count a per-doc read finds: docid and rank indexing,
// direct and predicted formats, a multi-valued doc whose values straddle
// batches, array / bitset / all-docs domains and the complements of each,
// with every 128-doc block decoded by point, by the default policy, and in
// bulk.
TEST_F(OrdColFormatTest, batchedOrdWalkMatchesPerDocValues) {
  constexpr int32_t numDocs = 1100;
  constexpr int32_t straddleDoc = 301;
  enum class Shape { DOCID, RANK, MULTI };
  for (auto format : {OrdColWriter::FormatOverride::DIRECT,
                      OrdColWriter::FormatOverride::PREDICTED}) {
    for (Shape shape : {Shape::DOCID, Shape::RANK, Shape::MULTI}) {
      SCOPED_TRACE(::testing::Message()
                   << "format=" << (int)format << " shape=" << (int)shape);
      OrdFormatGuard guard(format);
      TestIndex index;
      TestField field(index, shape == Shape::MULTI ? "f_ss" : "f_s");
      field.startIndexing();
      for (int32_t doc = 0; doc < numDocs; doc++) {
        if (shape == Shape::DOCID) {
          if (doc % 9 != 4) field.add(doc, ordinalTerm((doc * 37) % 500));
        } else if (shape == Shape::RANK) {
          if (doc % 11 == 0) field.add(doc, ordinalTerm((doc * 37) % 500));
        } else if (doc == straddleDoc || doc % 5 != 0) {
          int32_t count = doc == straddleDoc ? 400 : 1 + doc % 4;
          std::vector<std::string> terms;
          for (int32_t j = 0; j < count; j++) {
            terms.push_back(ordinalTerm((doc * 7 + j * 13) % 997));
          }
          field.addStrings(doc, std::vector<std::string_view>(
                                    terms.begin(), terms.end()));
        }
      }
      index.flush();
      field.startReading();
      ASSERT_GE(field.nextDoc(), 0);
      ASSERT_EQ(shape == Shape::DOCID ? SegFieldInfo::ORD_DOCID
                                      : SegFieldInfo::ORD_RANK,
                field.fieldInfo.ordIndexing);
      ASSERT_EQ(format == OrdColWriter::FormatOverride::PREDICTED
                    ? SegFieldInfo::ORD_PREDICTED : SegFieldInfo::ORD_DIRECT,
                field.fieldInfo.ordFormat);
      ASSERT_EQ(shape == Shape::MULTI, field.ordReader->multiValued());
      PostingsReader& postings = field.currentSegment()->postingsReader();
      int32_t maxDoc = postings.maxDoc();
      OrdColReader& reader = *field.ordReader;

      using Values = std::vector<std::pair<int32_t, int64_t>>;
      auto expected = [&](const std::vector<int32_t>& docs, int64_t& missing) {
        Values values;
        int32_t lastDoc = -1;
        int64_t withValue = 0;
        OrdColReader::getValues(index.pool, postings, field.fieldInfo, docs,
            [&](size_t, int32_t doc, int32_t ord, int64_t, int64_t) {
              if (ord == 0) return;
              values.emplace_back(doc, (int64_t)ord - 1);
              withValue += doc != lastDoc;
              lastDoc = doc;
            });
        missing = (int64_t)docs.size() - withValue;
        return values;
      };

      std::vector<std::vector<int32_t>> domains = {{}, {}, {}, {}};
      for (int32_t doc = 0; doc < maxDoc; doc++) {
        if (doc % 7 == 1 || doc == straddleDoc) domains[0].push_back(doc);
        if (doc % 3 != 2) domains[1].push_back(doc);
        if (doc % 13 == 0 || doc == straddleDoc) domains[2].push_back(doc);
        if (doc % 10 != 3) domains[3].push_back(doc);
      }
      for (int32_t minHits : {0, OrdColReader::DEFAULT_BULK_MIN_HITS,
                              (int32_t)OrdColReader::BULK_SIZE}) {
        OrdBulkMinHitsGuard threshold(minHits);
        auto walk = [&](DocSet* domain, const std::vector<int32_t>& walked,
                        bool complement) {
          SCOPED_TRACE(::testing::Message()
                       << "minHits=" << minHits << " walked=" << walked.size()
                       << " complement=" << complement);
          Values got;
          int64_t missing = 0;
          auto collect = [&](int32_t doc, int64_t ord) {
            got.emplace_back(doc, ord);
          };
          if (complement) {
            DomainView view(domain, maxDoc);
            RequestMemTracker tracker(SIZE_MAX);
            forEachComplementOrdValue(view, index.pool, reader, missing,
                                      tracker, "ord walk", collect);
          } else {
            forEachOrdValue(domain, reader, OrdMap::SegToGlobal::identity(),
                            maxDoc, missing, collect);
          }
          int64_t expectedMissing = 0;
          EXPECT_EQ(expected(walked, expectedMissing), got);
          EXPECT_EQ(expectedMissing, missing);
        };

        std::vector<int32_t> all(maxDoc);
        std::iota(all.begin(), all.end(), 0);
        walk(nullptr, all, false);
        for (size_t i = 0; i < domains.size(); i++) {
          const std::vector<int32_t>& docs = domains[i];
          std::vector<int32_t> complementDocs;
          std::set_difference(all.begin(), all.end(), docs.begin(), docs.end(),
                              std::back_inserter(complementDocs));
          RAMBitDocSet bits(maxDoc);
          for (int32_t doc : docs) bits.mutableBits().set(doc);
          ArrDocSet array{std::vector<int32_t>(docs)};
          // Array and bitset domains of the same docs walk alike.
          walk(&array, docs, false);
          walk(&bits, docs, false);
          walk(&array, complementDocs, true);
          walk(&bits, complementDocs, true);
        }
      }
    }
  }
}

TEST_F(OrdColFormatTest, predictedAscendingDescendingAndJitterWithTailBlock) {
  verifyPredictedShape(false, false);
  verifyPredictedShape(true, false);
  verifyPredictedShape(false, true);
}

TEST_F(OrdColFormatTest, directAndPredictedFacetsMatch) {
  CollectionHelper helper;
  auto run = [&](const std::vector<Doc>& docs, std::string_view field,
                 OrdColWriter::FormatOverride format) {
    OrdFormatGuard guard(format);
    helper.clear();
    helper.indexAll(docs, UpdateMessage::COMMIT);
    auto req = localReq(luxirNode->getSearchEngine());
    req->collection("main");
    auto& top = req->topDocs("q").countLimit().allQuery();
    auto& facet = top.facet("f", field).limit(-1);
    std::get<luxir::api::FieldFacet>(facet.rawOp().kind).missing = true;
    req->execute(false);
    EXPECT_TRUE(req->ok()) << req->toString();
    const auto* result = req->docList("q")->ops.at("f")->facetResult();
    const auto& ids = std::get<luxir::api::ColStr>(result->bucket_ids->kind).v;
    FacetOutput out;
    for (size_t i = 0; i < ids.size(); i++) {
      out.buckets.emplace_back(ids[i], result->counts[i]);
    }
    out.missing = result->missing.value_or(0);
    return out;
  };

  std::vector<std::pair<std::vector<Doc>, std::string>> shapes;
  std::vector<Doc> dense;
  std::vector<Doc> mid;
  std::vector<Doc> sparse;
  std::vector<Doc> multi;
  for (int32_t i = 0; i < 10; i++) {
    std::string id = std::to_string(i);
    std::string value(1, (char)('a' + i % 5));
    dense.push_back(flatdoc("id", id, "cat_s", value));
    mid.push_back(i < 6 ? flatdoc("id", id, "cat_s", value)
                         : flatdoc("id", id));
    sparse.push_back(i < 3 ? flatdoc("id", id, "cat_s", value)
                            : flatdoc("id", id));
    multi.push_back(i < 7
        ? flatdoc("id", id, "cat_ss",
                  std::vector<std::string>{value, std::string(1, (char)('f' + i % 3))})
        : flatdoc("id", id));
  }
  shapes.emplace_back(std::move(dense), "cat_s");
  shapes.emplace_back(std::move(mid), "cat_s");
  shapes.emplace_back(std::move(sparse), "cat_s");
  shapes.emplace_back(std::move(multi), "cat_ss");
  for (const auto& [docs, field] : shapes) {
    EXPECT_EQ(run(docs, field, OrdColWriter::FormatOverride::DIRECT),
              run(docs, field, OrdColWriter::FormatOverride::PREDICTED));
  }
}

TEST_F(OrdColFormatTest, directAndPredictedFacetsMatchAcrossBlocksAndSparseDomain) {
  CollectionHelper helper;
  std::vector<Doc> docs;
  constexpr int32_t count = 9'013;
  docs.reserve(count);
  for (int32_t doc = 0; doc < count; doc++) {
    std::string id = ordinalTerm(doc);
    std::string value = ordinalTerm((doc * 37) % 1'003);
    docs.push_back(doc % 97 == 0
        ? flatdoc("id_s", id, "cat_s", value, "gate_s", "yes")
        : flatdoc("id_s", id, "cat_s", value));
  }

  auto run = [&](OrdColWriter::FormatOverride format) {
    OrdFormatGuard guard(format);
    helper.clear();
    EXPECT_TRUE(helper.indexAll(docs, UpdateMessage::COMMIT).success);
    auto req = localReq(luxirNode->getSearchEngine());
    req->collection("main");
    auto& top = req->topDocs("q").countLimit().limit(10);
    top.rawQuery() = qb::match(top.mr(), "gate_s", "yes");
    auto& facet = top.facet("f", "cat_s").limit(-1);
    std::get<luxir::api::FieldFacet>(facet.rawOp().kind).missing = true;
    req->execute(false);
    EXPECT_OK(req);
    EXPECT_EQ(93, req->getMatchCount());
    const auto* result = req->docList("q")->ops.at("f")->facetResult();
    const auto& ids = std::get<luxir::api::ColStr>(result->bucket_ids->kind).v;
    FacetOutput out;
    for (size_t i = 0; i < ids.size(); i++) {
      out.buckets.emplace_back(ids[i], result->counts[i]);
    }
    out.missing = result->missing.value_or(0);
    return out;
  };

  EXPECT_EQ(run(OrdColWriter::FormatOverride::DIRECT),
            run(OrdColWriter::FormatOverride::PREDICTED));
}

TEST_F(OrdColFormatTest, automaticSelectionRequiresBothSavingsGates) {
  {
    TestIndex index;
    TestField field(index, "small_s");
    field.startIndexing();
    for (int32_t doc = 0; doc < 100; doc++) field.add(doc, ordinalTerm(doc));
    index.flush();
    field.startReading();
    ASSERT_EQ(0, field.nextDoc());
    EXPECT_EQ(SegFieldInfo::ORD_DIRECT, field.fieldInfo.ordFormat);
  }
  {
    TestIndex index;
    TestField field(index, "large_s");
    field.startIndexing();
    for (int32_t doc = 0; doc < 4101; doc++) field.add(doc, ordinalTerm(doc));
    index.flush();
    field.startReading();
    ASSERT_EQ(0, field.nextDoc());
    EXPECT_EQ(SegFieldInfo::ORD_PREDICTED, field.fieldInfo.ordFormat);
  }
}

TEST_F(OrdColFormatTest, mergeOrdTermSafetyMargin) {
  EXPECT_TRUE(MergeCostModel::ordTermsFit(0, MergeCostModel::MAX_MERGED_ORD_TERMS));
  EXPECT_FALSE(MergeCostModel::ordTermsFit(MergeCostModel::MAX_MERGED_ORD_TERMS, 1));
  EXPECT_FALSE(MergeCostModel::ordTermsFit(0, -1));
}

TEST_F(OrdColFormatTest, mergeDocCountSafetyMargin) {
  EXPECT_TRUE(MergeCostModel::docsFit(0, PostingsReader::MAX_SEGMENT_DOCS));
  EXPECT_FALSE(MergeCostModel::docsFit(PostingsReader::MAX_SEGMENT_DOCS, 1));
  EXPECT_FALSE(MergeCostModel::docsFit(0, -1));
  EXPECT_TRUE(MergeCostModel::docsFit(
      PostingsReader::MAX_SEGMENT_DOCS - 5, 5));
}

TEST_F(OrdColFormatTest, stringSortMissingLastAcrossSegmentsBothDirections) {
  OrdFormatGuard guard(OrdColWriter::FormatOverride::PREDICTED);
  CollectionHelper helper;
  helper.index(flatdoc("id_s", "doc1", "name_s", "c"), UpdateMessage::NO_COMMIT);
  helper.index(flatdoc("id_s", "doc2"), UpdateMessage::NO_COMMIT);
  helper.index(flatdoc("id_s", "doc3", "name_s", "a"), UpdateMessage::COMMIT);
  helper.index(flatdoc("id_s", "doc4", "name_s", "b"), UpdateMessage::NO_COMMIT);
  helper.index(flatdoc("id_s", "doc5"), UpdateMessage::NO_COMMIT);
  helper.index(flatdoc("id_s", "doc6", "name_s", "d"), UpdateMessage::COMMIT);

  auto sortedIds = [&](qb::SortDir direction) {
    auto req = localReq(luxirNode->getSearchEngine());
    req->collection("main");
    auto& top = req->topDocs("q").countLimit().allQuery().limit(10)
        .fields({"id_s", "name_s"});
    qb::sort(top, "name_s", direction);
    req->execute(false);
    EXPECT_OK(req);
    const auto& ids = std::get<luxir::api::ColStr>(
        req->docList("q")->columns.at("id_s").kind).v;
    return std::vector<std::string>(ids.begin(), ids.end());
  };

  EXPECT_EQ((std::vector<std::string>{"doc3", "doc4", "doc1", "doc6", "doc2", "doc5"}),
            sortedIds(qb::ASC));
  EXPECT_EQ((std::vector<std::string>{"doc6", "doc1", "doc4", "doc3", "doc2", "doc5"}),
            sortedIds(qb::DESC));
}

TEST_F(OrdColFormatTest, predictedInputSegmentsMergeWithFacetAndSortParity) {
  OrdFormatGuard guard(OrdColWriter::FormatOverride::PREDICTED);
  CollectionHelper helper;
  for (int32_t segment = 0; segment < 2; segment++) {
    std::vector<Doc> docs;
    for (int32_t i = 0; i < 300; i++) {
      int32_t doc = segment * 300 + i;
      docs.push_back(flatdoc("id_s", ordinalTerm(doc),
                             "cat_s", ordinalTerm((doc * 13) % 41),
                             "sort_s", ordinalTerm((doc * 173) % 607)));
    }
    ASSERT_TRUE(helper.indexAll(docs, UpdateMessage::COMMIT).success);
  }

  struct Snapshot {
    FacetOutput facet;
    std::vector<std::string> sortedIds;
    bool operator==(const Snapshot&) const = default;
  };
  auto snapshot = [&]() {
    auto req = localReq(luxirNode->getSearchEngine());
    req->collection("main");
    auto& top = req->topDocs("q").countLimit().allQuery().limit(100)
        .fields({"id_s", "sort_s"});
    qb::sort(top, "sort_s", qb::ASC);
    auto& facet = top.facet("f", "cat_s").limit(-1);
    std::get<luxir::api::FieldFacet>(facet.rawOp().kind).missing = true;
    req->execute(false);
    EXPECT_OK(req);

    Snapshot out;
    const auto* facetResult = req->docList("q")->ops.at("f")->facetResult();
    const auto& bucketIds =
        std::get<luxir::api::ColStr>(facetResult->bucket_ids->kind).v;
    for (size_t i = 0; i < bucketIds.size(); i++) {
      out.facet.buckets.emplace_back(bucketIds[i], facetResult->counts[i]);
    }
    out.facet.missing = facetResult->missing.value_or(0);
    const auto& ids = std::get<luxir::api::ColStr>(
        req->docList("q")->columns.at("id_s").kind).v;
    out.sortedIds.assign(ids.begin(), ids.end());
    return out;
  };

  Snapshot before = snapshot();
  CollectionHelper::UpdateBuilder request;
  request.commit(true, 1);
  ASSERT_TRUE(helper.submit(request).success);
  EXPECT_EQ(before, snapshot());
}

TEST_F(OrdColFormatTest, indexedStringModesPreserveValuesAndSelectExtrema) {
  for (auto format : {OrdColWriter::FormatOverride::DIRECT, OrdColWriter::FormatOverride::PREDICTED}) {
    OrdFormatGuard guard(format);
    for (bool array : {false, true}) {
      CollectionHelper helper;
      helper.clear();
      SchemaBuilder schema;
      auto& field = schema.field("value");
      field.type = api::FieldDef::FieldClass::STRING;
      field.multi = array ? api::FieldDef::MultiMode::ARRAY : api::FieldDef::MultiMode::SET;
      schema.normalizer(field, {"nfkc_cf"});
      schema.set(helper.collection());
      ASSERT_TRUE(helper.index(flatdoc("id", "a", "value", vecs("Z", "b", "B", "", "a"),
                                      "ref_ss", vecs("z", "b", "", "a")), UpdateMessage::NO_COMMIT).success);
      ASSERT_TRUE(helper.index(flatdoc("id", "gone", "value", vecs("zz", "zz"), "ref_ss", vecs("zz")), UpdateMessage::COMMIT).success);
      ASSERT_TRUE(helper.index(flatdoc("id", "b", "value", vecs("y", "m", "y"),
                                      "ref_ss", vecs("y", "m")), UpdateMessage::NO_COMMIT).success);
      ASSERT_TRUE(helper.index(flatdoc("id", "c", "value", "q", "ref_ss", "q"), UpdateMessage::NO_COMMIT).success);
      ASSERT_TRUE(helper.index(flatdoc("id", "d", "value", vecs()), UpdateMessage::NO_COMMIT).success);
      ASSERT_TRUE(helper.index(flatdoc("id", "e"), UpdateMessage::COMMIT).success);
      ASSERT_TRUE(helper.deleteById("gone", UpdateMessage::COMMIT).success);
      for (int merge = 0; merge < 2; merge++) {
        auto req = localReq(helper.getSearchEngine());
        auto& top = req->collection("main").topDocs("q").allQuery().limit(-1).fields({"id", "value"});
        qb::sort(top, "id", qb::ASC);
        req->execute(false);
        ASSERT_OK(req);
        const auto& rows = std::get<api::ArrArrStr>(req->docList("q")->columns.at("value").kind).v;
        ASSERT_EQ(5u, rows.size());
        EXPECT_EQ(array ? (std::vector<std::string>{"z", "b", "b", "", "a"})
                        : (std::vector<std::string>{"", "a", "b", "z"}),
                  std::vector<std::string>(rows[0].v.begin(), rows[0].v.end()));
        EXPECT_EQ(array ? (std::vector<std::string>{"y", "m", "y"})
                        : (std::vector<std::string>{"m", "y"}),
                  std::vector<std::string>(rows[1].v.begin(), rows[1].v.end()));
        EXPECT_EQ("q", rows[2].v[0]);
        EXPECT_TRUE(rows[3].v.empty());
        EXPECT_TRUE(rows[4].v.empty());
        for (auto sortMode : {StringSortMode::SEGMENT, StringSortMode::GLOBAL})
        for (bool parallel : {false, true}) for (auto dir : {qb::ASC, qb::DESC}) for (int limit : {2, -1}) {
          OrdSortModeGuard sortGuard(sortMode);
          auto run = [&](std::string_view name) {
            auto sorted = localReq(helper.getSearchEngine());
            auto& docs = sorted->collection("main").topDocs("q").allQuery().limit(limit).fields({"id"});
            qb::sort(docs, name, dir);
            sorted->execute(parallel);
            EXPECT_TRUE(sorted->ok()) << sorted->errorMsg();
            auto ids = std::get<api::ColStr>(sorted->docList("q")->columns.at("id").kind).v;
            return std::vector<std::string>(ids.begin(), ids.end());
          };
          EXPECT_EQ(run("ref_ss"), run("value"));
        }
        auto match = localReq(helper.getSearchEngine());
        match->collection("main").topDocs("q").matchQuery("value", "b").countLimit();
        match->topDocs("exists").existsQuery("value").countLimit();
        match->execute(false);
        ASSERT_OK(match);
        EXPECT_EQ(1, match->getMatchCount("q"));
        EXPECT_EQ(3, match->getMatchCount("exists"));
        auto scores = localReq(helper.getSearchEngine());
        scores->collection("main").topDocs("value").matchQuery("value", "b").getScores();
        scores->topDocs("reference").matchQuery("ref_ss", "b").getScores();
        scores->execute(false);
        ASSERT_OK(scores);
        auto scoreOf = [&](std::string_view op) {
          return std::get<api::ColFloat>(scores->docList(op)->columns.at("_score_").kind).v[0];
        };
        EXPECT_FLOAT_EQ(scoreOf("reference"), scoreOf("value"));
        auto reader = helper.getIndexWriter()->snapshots.readers.getReader();
        for (auto& segment : reader->segments()) if (auto* info = segment.postingsReader().fieldInfo("value")) {
          EXPECT_EQ(array, (info->flags & FieldType::MULTI_ARRAY) != 0);
          EXPECT_EQ(format == OrdColWriter::FormatOverride::DIRECT ? SegFieldInfo::ORD_DIRECT : SegFieldInfo::ORD_PREDICTED,
                    info->ordFormat);
        }
        if (!merge) {
          CollectionHelper::UpdateBuilder update;
          update.commit(true, 1);
          ASSERT_TRUE(helper.submit(update).success);
        }
      }
    }
  }
}

TEST_F(OrdColFormatTest, indexedStringMixedModesMergeKeepsSurvivingSequences) {
  CollectionHelper helper;
  auto setMode = [&](bool array) {
    SchemaBuilder schema;
    auto& f = schema.field("value");
    f.type = api::FieldDef::FieldClass::STRING;
    f.multi = array ? api::FieldDef::MultiMode::ARRAY : api::FieldDef::MultiMode::SET;
    schema.set(helper.collection());
  };
  setMode(true);
  ASSERT_TRUE(helper.index(flatdoc("id", "a", "value", vecs("z", "a", "z")), UpdateMessage::NO_COMMIT).success);
  ASSERT_TRUE(helper.index(flatdoc("id", "gone", "value", vecs("x", "b", "x")), UpdateMessage::COMMIT).success);
  setMode(false);
  ASSERT_TRUE(helper.index(flatdoc("id", "b", "value", vecs("y", "b", "y")), UpdateMessage::COMMIT).success);
  setMode(true);
  ASSERT_TRUE(helper.index(flatdoc("id", "missing"), UpdateMessage::NO_COMMIT).success);
  ASSERT_TRUE(helper.index(flatdoc("id", "c", "value", ""), UpdateMessage::COMMIT).success);
  ASSERT_TRUE(helper.deleteById("gone", UpdateMessage::COMMIT).success);
  for (int merge = 0; merge < 2; merge++) {
    auto req = localReq(helper.getSearchEngine());
    auto& docs = req->collection("main").topDocs("q").allQuery().fields({"id", "value"});
    qb::sort(docs, "id", qb::ASC);
    req->execute(false);
    ASSERT_OK(req);
    const auto& rows = std::get<api::ArrArrStr>(req->docList("q")->columns.at("value").kind).v;
    ASSERT_EQ(4u, rows.size());
    EXPECT_EQ((std::vector<std::string>{"z", "a", "z"}), std::vector<std::string>(rows[0].v.begin(), rows[0].v.end()));
    EXPECT_EQ((std::vector<std::string>{"b", "y"}), std::vector<std::string>(rows[1].v.begin(), rows[1].v.end()));
    ASSERT_EQ(1u, rows[2].v.size());
    EXPECT_EQ("", rows[2].v[0]);
    EXPECT_TRUE(rows[3].v.empty());
    if (!merge) {
      CollectionHelper::UpdateBuilder update;
      update.commit(true, 1);
      ASSERT_TRUE(helper.submit(update).success);
    }
  }
  auto reader = helper.getIndexWriter()->snapshots.readers.getReader();
  ASSERT_EQ(1u, reader->segments().size());
  EXPECT_NE(0, reader->segments()[0].postingsReader().fieldInfo("value")->flags & FieldType::MULTI_ARRAY);
}

TEST_F(OrdColFormatTest, arrayDocumentDedupChargesReusesAndReleasesScratch) {
  CollectionHelper helper;
  SchemaBuilder schema;
  auto& field = schema.field("values");
  field.type = api::FieldDef::FieldClass::STRING;
  field.multi = api::FieldDef::MultiMode::ARRAY;
  schema.set(helper.collection());
  std::vector<std::string> values;
  for (int i = 0; i < 256; i++) values.push_back(std::to_string(i));
  auto repeats = values;
  values.insert(values.end(), repeats.begin(), repeats.end());
  ASSERT_TRUE(helper.indexAll({flatdoc("values", values), flatdoc("values", values)}, UpdateMessage::COMMIT).success);
  auto index = helper.getIndexWriter()->snapshots.readers.getReader();
  auto& segment = index->segments()[0];
  OrdColReader column(segment.postingsReader(), *segment.postingsReader().fieldInfo("values"));
  for (size_t ceiling : {SIZE_MAX, size_t{1}}) {
    RequestMemTracker tracker(ceiling);
    {
      OrdValueBatches values(nullptr, column, OrdMap::SegToGlobal::identity(), segment.maxDoc(), false, nullptr);
      DistinctOrdValueBatches distinct(values, tracker, "values");
      OrdValueBatches::Batch batch;
      if (ceiling == 1) {
        EXPECT_THROW(distinct.next(batch), ResourceExhaustedError);
      } else {
        size_t peak = 0;
        int count = 0;
        while (distinct.next(batch)) {
          count += batch.size;
          EXPECT_GT(tracker.bytes(), 0u);
          if (peak == 0) peak = tracker.peak();
          EXPECT_EQ(peak, tracker.peak());
        }
        EXPECT_EQ(512, count);
      }
    }
    EXPECT_EQ(0u, tracker.bytes());
  }
}
