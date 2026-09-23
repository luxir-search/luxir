// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <span>
#include <vector>

#include "luxir/query/NumericPredicateQuery.h"
#include "luxir/query/QueryPrep.h"
#include "luxir/reader/FieldReader.h"
#include "luxir/util/NumericUtils.h"
#include "luxir/util/random.h"
#include "test/CollectionHelper.h"
#include "test/LocalReq.h"
#include "test/QueryBuild.h"
#include "test/LuxirTest.h"
#include "test/TestIndex.h"
#include "test/TestUtils.h"

using namespace luxir;
using namespace luxir::test;

namespace {

struct RangeRun {
  std::vector<int32_t> pruned;
  std::vector<int32_t> fullScan;
  std::vector<int32_t> sparseVerify;
  std::vector<int32_t> bulk;
  int64_t count = -1;
  int64_t cost = -1;
  bool zoneBulkAvailable = false;
};

std::vector<int32_t> collect(Query::Scorer* scorer) {
  std::vector<int32_t> docs;
  if (scorer == nullptr) return docs;
  for (int32_t doc = scorer->next(); doc != PostingsReader::END;
       doc = scorer->next()) {
    docs.push_back(doc);
  }
  return docs;
}

RangeRun runRange(IndexReader& reader, std::string_view field,
                  int64_t lo, int64_t hi) {
  MemPool pool;
  Query::Context context(pool, reader);
  NumericPredicateQuery query(field, lo, hi);
  auto* weight = static_cast<NumericPredicateQuery::Weight*>(
      query.createWeight(context, 0));
  auto& segment = reader.segments()[0];

  RangeRun run;
  run.fullScan = collect(weight->createFullScanScorerForTests(pool, segment));
  run.pruned = collect(weight->createScorer(pool, segment));

  auto* sparseSupplier = weight->scorerSupplier(pool, segment);
  if (sparseSupplier != nullptr) {
    run.cost = sparseSupplier->cost();
    run.zoneBulkAvailable = sparseSupplier->bulkScorer(pool) != nullptr;
    auto* sparsePlan = resolveScorerPlanForTests(pool, *sparseSupplier, 0);
    auto* sparse = sparsePlan->build(pool);
    EXPECT_EQ(Query::ReportedTwoPhase::YES,
              sparsePlan->shape().reportedTwoPhase);
    run.sparseVerify = collect(sparse);
  }

  auto materialized = QueryPrep::materialize(*weight, nullptr, segment, nullptr);
  for (int32_t doc = 0; doc < segment.maxDoc(); doc++) {
    if (materialized->get(doc)) run.bulk.push_back(doc);
  }
  run.count = weight->count(segment);
  return run;
}

std::vector<int32_t> oracle(const std::vector<std::vector<int64_t>>& values,
                            int64_t lo, int64_t hi) {
  std::vector<int32_t> docs;
  for (int32_t doc = 0; doc < (int32_t)values.size(); doc++) {
    if (std::ranges::any_of(values[(size_t)doc], [=](int64_t value) {
          return lo <= value && value <= hi;
        })) {
      docs.push_back(doc);
    }
  }
  return docs;
}

std::vector<int32_t> collectDocSet(DocSet* set, int32_t maxDoc) {
  std::vector<int32_t> docs;
  for (int32_t doc = 0; doc < maxDoc; doc++) {
    if (set->get(doc)) docs.push_back(doc);
  }
  return docs;
}

std::vector<int32_t> intersectDomain(const std::vector<int32_t>& docs,
                                     DocSet& domain, int32_t minDoc = 0) {
  std::vector<int32_t> intersection;
  for (int32_t doc : docs) {
    if (doc >= minDoc && domain.get(doc)) intersection.push_back(doc);
  }
  return intersection;
}

void expectRun(const RangeRun& run, const std::vector<int32_t>& expected) {
  EXPECT_EQ(run.fullScan, expected);
  EXPECT_EQ(run.pruned, expected);
  EXPECT_EQ(run.sparseVerify, expected);
  EXPECT_EQ(run.bulk, expected);
  EXPECT_EQ(run.count, (int64_t)expected.size());
}

} // namespace

class NumericRangeZoneMapTest : public LuxirTest {};

TEST_F(NumericRangeZoneMapTest, randomizedMultiBlockOracleAndCount) {
  constexpr int32_t N = 33'000;
  constexpr int64_t MULTI_BLOCK = IntColReader::BLOCK_SIZE;

  std::vector<std::vector<int64_t>> dense((size_t)N);
  std::vector<std::vector<int64_t>> optional((size_t)N);
  std::vector<std::vector<int64_t>> multi((size_t)N);
  std::vector<std::vector<int64_t>> floats((size_t)N);
  std::vector<std::vector<int64_t>> doubles((size_t)N);
  std::vector<std::vector<int64_t>> dates((size_t)N);
  std::vector<std::vector<int64_t>> extremeGcd((size_t)N);

  int64_t nextMultiRank = 0;
  int32_t spanningDoc = -1;
  for (int32_t doc = 0; doc < N; doc++) {
    dense[(size_t)doc].push_back(-120'000 + (int64_t)doc * 6);
    if (doc % 4 != 0) {
      optional[(size_t)doc].push_back(-90'000 + (int64_t)doc * 10);
    }

    int32_t length = doc % 17 == 0 ? 0 : 5;
    if (nextMultiRank < MULTI_BLOCK
        && nextMultiRank + length == MULTI_BLOCK) {
      length++;
    }
    int64_t start = nextMultiRank;
    for (int32_t i = 0; i < length; i++) {
      multi[(size_t)doc].push_back(-300'000 + nextMultiRank * 2);
      nextMultiRank++;
    }
    if (start < MULTI_BLOCK && nextMultiRank > MULTI_BLOCK) spanningDoc = doc;

    int64_t numeric = (int64_t)doc - N / 2;
    floats[(size_t)doc].push_back((int64_t)floatToSortableInt32((float)numeric));
    doubles[(size_t)doc].push_back(doubleToSortableInt64((double)numeric));
    dates[(size_t)doc].push_back(946684800000LL + (int64_t)doc * 60'000);
    extremeGcd[(size_t)doc].push_back((doc & 1)
        ? std::numeric_limits<int64_t>::max()
        : std::numeric_limits<int64_t>::min());
  }
  ASSERT_GE(spanningDoc, 0);
  ASSERT_GT(nextMultiRank, 2 * MULTI_BLOCK);

  TestIndex index;
  Inverter& inverter = index.getInverter();
  auto& denseHandler = inverter.getIndexHandler("dense_i");
  auto& optionalHandler = inverter.getIndexHandler("optional_i");
  auto& multiHandler = inverter.getIndexHandler("multi_is");
  auto& floatHandler = inverter.getIndexHandler("zone_f");
  auto& doubleHandler = inverter.getIndexHandler("zone_d");
  auto& dateHandler = inverter.getIndexHandler("zone_dt");
  auto& extremeGcdHandler = inverter.getIndexHandler("extreme_gcd_i");

  for (int32_t doc = 0; doc < N; doc++) {
    inverter.startDoc();
    denseHandler.index(inverter, dense[(size_t)doc][0]);
    if (!optional[(size_t)doc].empty()) {
      optionalHandler.index(inverter, optional[(size_t)doc][0]);
    }
    multiHandler.index(inverter, std::span<const int64_t>(multi[(size_t)doc]));
    floatHandler.index(inverter, (int64_t)doc - N / 2);
    doubleHandler.index(inverter, (int64_t)doc - N / 2);
    dateHandler.index(inverter, dates[(size_t)doc][0]);
    extremeGcdHandler.index(inverter, extremeGcd[(size_t)doc][0]);
    inverter.finishDoc();
  }
  index.flush();
  index.initReader();
  ASSERT_EQ(index.reader->segments().size(), 1);

  const std::array<std::pair<int64_t, int64_t>, 7> intRanges = {{
      {1, 0},
      {-500'000, -400'000},
      {-120'000, -119'800},
      {-75'000, -60'000},
      {-30'000, 30'000},
      {-500'000, 500'000},
      {std::numeric_limits<int64_t>::min(),
       std::numeric_limits<int64_t>::max()},
  }};

  for (auto [lo, hi] : intRanges) {
    expectRun(runRange(*index.reader, "dense_i", lo, hi),
              oracle(dense, lo, hi));
    expectRun(runRange(*index.reader, "optional_i", lo, hi),
              oracle(optional, lo, hi));
    expectRun(runRange(*index.reader, "multi_is", lo, hi),
              oracle(multi, lo, hi));
  }

  // Random bound pairs exercise residual comparisons in crossing blocks.
  SplitMix64 rng(0x5a17e123);
  int32_t randomRanges = (int32_t)scaleTestWork(4);
  for (int32_t i = 0; i < randomRanges; i++) {
    int64_t a = -150'000 + (int64_t)rng.rint(350'000);
    int64_t b = -150'000 + (int64_t)rng.rint(350'000);
    int64_t lo = std::min(a, b);
    int64_t hi = std::max(a, b);
    expectRun(runRange(*index.reader, "dense_i", lo, hi),
              oracle(dense, lo, hi));
    expectRun(runRange(*index.reader, "optional_i", lo, hi),
              oracle(optional, lo, hi));
    expectRun(runRange(*index.reader, "multi_is", lo, hi),
              oracle(multi, lo, hi));
  }

  // The first value block ends in the middle of spanningDoc. Accepting that
  // whole block must emit the owner once even though its final values are in
  // the next OUTSIDE block.
  int64_t firstBlockMax = -300'000 + (MULTI_BLOCK - 1) * 2;
  auto insideBlockExpected = oracle(multi, std::numeric_limits<int64_t>::min(),
                                    firstBlockMax);
  auto insideBlockRun = runRange(*index.reader, "multi_is",
                                 std::numeric_limits<int64_t>::min(),
                                 firstBlockMax);
  expectRun(insideBlockRun, insideBlockExpected);
  EXPECT_EQ(std::count(insideBlockRun.pruned.begin(), insideBlockRun.pruned.end(),
                       spanningDoc), 1);

  struct TypedCase {
    std::string_view field;
    const std::vector<std::vector<int64_t>>* values;
    int64_t lo;
    int64_t hi;
  };
  std::array<TypedCase, 3> typed = {{
      {"zone_f", &floats, (int64_t)floatToSortableInt32(-1000.0f),
       (int64_t)floatToSortableInt32(1000.0f)},
      {"zone_d", &doubles, doubleToSortableInt64(-1000.0),
       doubleToSortableInt64(1000.0)},
      {"zone_dt", &dates, dates[5000][0], dates[20'000][0]},
  }};
  for (const auto& test : typed) {
    expectRun(runRange(*index.reader, test.field, test.lo, test.hi),
              oracle(*test.values, test.lo, test.hi));
  }

  // Each block contains only INT64_MIN/MAX. Its unsigned delta and gcd are
  // UINT64_MAX, so it stays compressed to one residual bit while exercising
  // saturating bound inversion across the complete signed domain.
  for (int64_t endpoint : {std::numeric_limits<int64_t>::min(),
                           std::numeric_limits<int64_t>::max()}) {
    expectRun(runRange(*index.reader, "extreme_gcd_i", endpoint, endpoint),
              oracle(extremeGcd, endpoint, endpoint));
  }

  // A selective correlated range must advertise its selective estimate.
  auto selective = runRange(*index.reader, "dense_i", -120'000, -119'800);
  EXPECT_GT(selective.cost, 0);
  EXPECT_LT(selective.cost, N);
}

TEST_F(NumericRangeZoneMapTest, deletedOnlyMatchesKeepScorerPresent) {
  constexpr int32_t N = 2 * IntColReader::BLOCK_SIZE;
  CollectionHelper helper;
  std::vector<Doc> docs;
  docs.reserve(N);
  for (int32_t doc = 0; doc < N; doc++) {
    int64_t value = doc == 0 ? 10
        : doc < (int32_t)IntColReader::BLOCK_SIZE ? 20 : 30;
    docs.push_back(flatdoc(
        "id", "deleted_range_" + std::to_string(doc),
        "deleted_only_i", value));
  }
  ASSERT_TRUE(helper.indexAll(docs, UpdateMessage::COMMIT).success);
  ASSERT_TRUE(helper.deleteById(
      "deleted_range_0", UpdateMessage::COMMIT).success);

  auto reader = helper.getIndexWriter()->snapshots.readers.getReader();
  ASSERT_EQ(1u, reader->segments().size());
  MemPool pool;
  Query::Context context(pool, *reader);
  NumericPredicateQuery query("deleted_only_i", 10, 10);
  auto* weight = static_cast<NumericPredicateQuery::Weight*>(
      query.createWeight(context, 0));
  auto* supplier = weight->scorerSupplier(pool, reader->segments()[0]);
  ASSERT_NE(nullptr, supplier);
  Query::PlanContext buildContext;
  buildContext.demand = Query::Demand::fromLeadCost(
      std::numeric_limits<int64_t>::max());
  EXPECT_EQ(Query::MatchState::UNKNOWN,
            supplier->describeScorer(buildContext).matchState);
  Query::Scorer* scorer = supplier->resolve(pool, buildContext)->build(pool);
  ASSERT_NE(nullptr, scorer);
  EXPECT_NE(nullptr,
            dynamic_cast<NumericPredicateQuery::ZoneMapScorer*>(scorer));
  EXPECT_EQ((std::vector<int32_t>{0}), collect(scorer));

  auto req = localReq(helper.getSearchEngine());
  req->collection("main");
  auto& topDocs = req->topDocs("q").getNumber().limit(0);
  topDocs.rawQuery() = qb::range(
      topDocs.mr(), "deleted_only_i", qb::valI64(topDocs.mr(), 10),
      nullptr, qb::valI64(topDocs.mr(), 10), nullptr);
  req->execute(false);
  ASSERT_TRUE(req->ok()) << req->errorMsg();
  EXPECT_EQ(0, req->getMatchCount("q"));
}

TEST_F(NumericRangeZoneMapTest, countDeclinesDeletedSegment) {
  TestIndex index;
  TestField field(index, "deleted_i");
  field.startIndexing();
  field.add(0, 10);
  field.add(1, 20);
  index.deleteDoc(0);
  index.flush();
  index.initReader();

  MemPool pool;
  Query::Context context(pool, *index.reader);
  NumericPredicateQuery query("deleted_i", 0, 100);
  auto* weight = query.createWeight(context, 0);
  ASSERT_NE(index.reader->segments()[0].liveDocs(), nullptr);
  EXPECT_EQ(weight->count(index.reader->segments()[0]), -1);
}

TEST_F(NumericRangeZoneMapTest, windowFilterFillAndProbeUseZoneMapScorer) {
  const int32_t nDocs = 2 * IntColReader::BLOCK_SIZE + 257;
  TestIndex index;
  Inverter& inverter = index.getInverter();
  auto& field = inverter.getIndexHandler("window_i");
  for (int32_t doc = 0; doc < nDocs; doc++) {
    inverter.startDoc();
    field.index(inverter, doc / IntColReader::BLOCK_SIZE);
    inverter.finishDoc();
  }
  index.flush();
  index.initReader();

  auto makeFilter = [&](MemPool& pool) {
    Query::Context context(pool, *index.reader);
    NumericPredicateQuery query("window_i", 1, 1);
    auto* weight = static_cast<NumericPredicateQuery::Weight*>(
        query.createWeight(context, 0));
    auto* scorer = weight->createZoneMapScorerForTests(
        pool, context.topReader.segments()[0]);
    auto scorers = pool.make_span<Query::Scorer*>(1);
    scorers[0] = scorer;
    return scorers;
  };
  auto expected = [](int32_t doc) {
    return doc / IntColReader::BLOCK_SIZE == 1;
  };

  MemPool fillPool;
  WindowFilter fill(fillPool, makeFilter(fillPool));
  int32_t start = IntColReader::BLOCK_SIZE - DocsEnumMeta::L1_DOCS / 2;
  int32_t end = start + DocsEnumMeta::L1_DOCS;
  fill.prepare(start, end);
  for (int32_t doc = start; doc < end; doc++) {
    EXPECT_EQ(fill.accepts(doc), expected(doc)) << "doc=" << doc;
  }

  MemPool probePool;
  WindowFilter probe(probePool, makeFilter(probePool), true);
  for (int32_t doc = 0; doc < nDocs; doc += 7) {
    EXPECT_EQ(probe.acceptsProbe(doc), expected(doc)) << "doc=" << doc;
  }
}

TEST_F(NumericRangeZoneMapTest, materializeFiltersBitAndArrayDomains) {
  constexpr int32_t N = 20'000;
  constexpr int32_t ODD_WINDOW_START = 37;

  TestIndex index;
  Inverter& inverter = index.getInverter();
  auto& field = inverter.getIndexHandler("domain_i");
  for (int32_t doc = 0; doc < N; doc++) {
    inverter.startDoc();
    field.index(inverter, doc);
    inverter.finishDoc();
  }
  index.flush();
  index.initReader();

  RAMBitDocSet bitDomain(N);
  std::vector<int32_t> arrayDocs;
  for (int32_t doc = 0; doc < N; doc++) {
    if (doc % 67 == 3 || doc % 127 == 65) {
      bitDomain.mutableBits().set(doc);
    }
    if (doc % 73 == 5 || doc % 131 == 66) arrayDocs.push_back(doc);
  }
  for (int32_t doc : {63, 64, 65, 4095, 4096, 4097, 8191, 8192, 8193}) {
    bitDomain.mutableBits().set(doc);
  }
  std::sort(arrayDocs.begin(), arrayDocs.end());
  arrayDocs.erase(std::unique(arrayDocs.begin(), arrayDocs.end()),
                  arrayDocs.end());
  ArrDocSet arrayDomain(std::move(arrayDocs));

  MemPool pool;
  Query::Context context(pool, *index.reader);
  NumericPredicateQuery query("domain_i", 0,
                          IntColReader::BLOCK_SIZE - 1);
  auto* weight = static_cast<NumericPredicateQuery::Weight*>(
      query.createWeight(context, 0));
  auto& segment = index.reader->segments()[0];
  auto expected = collect(weight->createFullScanScorerForTests(pool, segment));

  auto bitMaterialized = QueryPrep::materialize(
      *weight, nullptr, segment, &bitDomain);
  EXPECT_EQ(collectDocSet(bitMaterialized.get(), N),
            intersectDomain(expected, bitDomain));

  auto arrayMaterialized = QueryPrep::materialize(
      *weight, nullptr, segment, &arrayDomain);
  EXPECT_EQ(collectDocSet(arrayMaterialized.get(), N),
            intersectDomain(expected, arrayDomain));

  // QueryPrep starts at zero, so explicitly start a bulk count window at an
  // odd offset to cover applyBitFilter's cross-word source shift.
  auto* supplier = weight->scorerSupplier(pool, segment);
  ASSERT_NE(supplier, nullptr);
  auto* bulk = supplier->bulkScorer(pool);
  ASSERT_NE(bulk, nullptr);
  DocSetBuilder builder(N);
  int64_t count = 0;
  for (int32_t cursor = ODD_WINDOW_START; cursor != PostingsReader::END; ) {
    int32_t next = bulk->countNextWindow(count, &builder, &bitDomain,
                                         cursor, N);
    if (next == PostingsReader::END) break;
    ASSERT_GT(next, cursor);
    cursor = next;
  }
  auto shifted = builder.build();
  auto shiftedExpected = intersectDomain(expected, bitDomain,
                                         ODD_WINDOW_START);
  EXPECT_EQ(collectDocSet(shifted.get(), N), shiftedExpected);
  EXPECT_EQ(count, (int64_t)shiftedExpected.size());
}

TEST_F(NumericRangeZoneMapTest, shuffledFallbackMatchesAllCollectionPaths) {
  constexpr int32_t N = 40'000;
  std::vector<std::vector<int64_t>> values((size_t)N);

  TestIndex index;
  Inverter& inverter = index.getInverter();
  auto& field = inverter.getIndexHandler("shuffled_i");
  for (int32_t doc = 0; doc < N; doc++) {
    int64_t value = (doc & 1) ? N - 1 - doc / 2 : doc / 2;
    values[(size_t)doc].push_back(value);
    inverter.startDoc();
    field.index(inverter, value);
    inverter.finishDoc();
  }
  index.flush();
  index.initReader();

  constexpr int64_t LO = N / 2 - 5;
  constexpr int64_t HI = N / 2 + 5;
  auto expected = oracle(values, LO, HI);
  auto run = runRange(*index.reader, "shuffled_i", LO, HI);
  expectRun(run, expected);
  EXPECT_FALSE(run.zoneBulkAvailable);
}

TEST_F(NumericRangeZoneMapTest, predictedCrossingBlockComparesDecodedValues) {
  constexpr int32_t N = IntColReader::BLOCK_SIZE;
  constexpr int32_t OUTLIER = N / 2;
  std::vector<std::vector<int64_t>> values((size_t)N);

  TestIndex index;
  Inverter& inverter = index.getInverter();
  auto& field = inverter.getIndexHandler("predicted_cross_i");
  for (int32_t doc = 0; doc < N; doc++) {
    int64_t value = (int64_t)doc * 17 + (doc == OUTLIER ? 3 : 0);
    values[(size_t)doc].push_back(value);
    inverter.startDoc();
    field.index(inverter, value);
    inverter.finishDoc();
  }
  index.flush();
  index.initReader();

  auto& segment = index.reader->segments()[0];
  FieldReader fields(segment.postingsReader());
  ASSERT_TRUE(fields.seek("predicted_cross_i"));
  SegFieldInfo info;
  fields.readFieldInfo(info);
  IntColReader column(segment.postingsReader(), info);
  ASSERT_EQ(column.numBlocks(), 1);
  EXPECT_NE(column.blockInfo(0).scaledSlope, 0);

  int64_t target = values[(size_t)OUTLIER][0];
  auto expected = oracle(values, target, target);
  expectRun(runRange(*index.reader, "predicted_cross_i", target, target),
            expected);

  MemPool pool;
  Query::Context context(pool, *index.reader);
  NumericPredicateQuery query("predicted_cross_i", target, target);
  auto* weight = static_cast<NumericPredicateQuery::Weight*>(
      query.createWeight(context, 0));
  EXPECT_EQ(collect(weight->createZoneMapScorerForTests(pool, segment)),
            expected);
}
