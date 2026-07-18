#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include "solux/index/PointsWriter.h"
#include "solux/query/NumericRangeQuery.h"
#include "solux/query/QueryPrep.h"
#include "solux/reader/FieldReader.h"
#include "solux/reader/PointsReader.h"
#include "solux/util/random.h"
#include "test/CollectionHelper.h"
#include "test/SchemaBuilder.h"
#include "test/SoluxTest.h"

using namespace solux;
using namespace solux::test;

namespace {

struct RangeField {
  std::string_view name;
  bool multi = false;
};

struct QueryState {
  Query::Context context;
  NumericRangeQuery query;
  NumericRangeQuery::Weight* weight;

  QueryState(MemPool& pool, IndexReader& reader, std::string_view field,
             int64_t lo, int64_t hi)
      : context(pool, reader), query(field, lo, hi),
        weight(static_cast<NumericRangeQuery::Weight*>(
            query.createWeight(context, 0))) {}
};

void setRangeSchema(CollectionHelper& helper, std::span<const RangeField> fields) {
  SchemaBuilder b;
  for (const auto& fs : fields) {
    auto& f = b.field(fs.name);
    f.type = api::FieldDef::FieldClass::INT;
    f.index = api::FieldDef::IndexMode::RANGE;
    f.multi = fs.multi;
  }
  b.set(helper.collection());
}

std::vector<int32_t> collect(Query::Scorer* scorer) {
  std::vector<int32_t> docs;
  if (scorer == nullptr) return docs;
  for (int32_t doc = scorer->next(); doc != PostingsReader::END;
       doc = scorer->next()) {
    docs.push_back(doc);
  }
  return docs;
}

std::vector<int32_t> collectSet(DocSet& set, int32_t maxDoc) {
  std::vector<int32_t> docs;
  for (int32_t doc = 0; doc < maxDoc; doc++) {
    if (set.get(doc)) docs.push_back(doc);
  }
  return docs;
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

std::vector<int32_t> fullScan(IndexReader& reader, std::string_view field,
                              int64_t lo, int64_t hi) {
  MemPool pool;
  QueryState state(pool, reader, field, lo, hi);
  return collect(state.weight->createFullScanScorerForTests(
      pool, reader.segments()[0]));
}

std::vector<int32_t> selectedScorer(IndexReader& reader, std::string_view field,
                                    int64_t lo, int64_t hi, int64_t leadCost) {
  MemPool pool;
  QueryState state(pool, reader, field, lo, hi);
  auto* supplier = state.weight->scorerSupplier(pool, reader.segments()[0]);
  return supplier == nullptr ? std::vector<int32_t>{}
      : collect(supplier->get(pool, leadCost));
}

std::vector<int32_t> materialized(IndexReader& reader, std::string_view field,
                                  int64_t lo, int64_t hi,
                                  DocSet* domain = nullptr) {
  MemPool pool;
  QueryState state(pool, reader, field, lo, hi);
  auto result = QueryPrep::materialize(
      *state.weight, nullptr, reader.segments()[0], domain);
  return collectSet(*result, reader.segments()[0].maxDoc());
}

int64_t exactCount(IndexReader& reader, std::string_view field,
                   int64_t lo, int64_t hi) {
  MemPool pool;
  QueryState state(pool, reader, field, lo, hi);
  return state.weight->count(reader.segments()[0]);
}

std::optional<SegFieldInfo> tryFieldInfo(IndexReader::Segment& segment,
                                         std::string_view field) {
  MemPool pool;
  FieldReader fields(pool, segment.postingsReader());
  if (!fields.seek(field)) return std::nullopt;
  SegFieldInfo info;
  fields.readFieldInfo(info);
  return {info};
}

SegFieldInfo fieldInfo(IndexReader::Segment& segment, std::string_view field) {
  auto info = tryFieldInfo(segment, field);
  if (!info) throw std::runtime_error("missing test field");
  return *info;
}

std::vector<PointsReader::Point> readPoints(IndexReader::Segment& segment,
                                             std::string_view field) {
  SegFieldInfo info = fieldInfo(segment, field);
  if (info.pointsMetaOff == 0) return {};
  PointsReader points(segment.postingsReader(), info);
  points.validate();
  return points.readAll();
}

} // namespace

class NumericRangePointsTest : public SoluxTest {};

TEST_F(NumericRangePointsTest, randomizedOracleAcrossScorerAndBulkPaths) {
  constexpr int32_t N = 2600;
  CollectionHelper helper;
  const RangeField fields[] = {{"point_single"}, {"point_multi", true}};
  setRangeSchema(helper, fields);

  std::vector<std::vector<int64_t>> single(N);
  std::vector<std::vector<int64_t>> multi(N);
  auto writer = helper.getIndexWriter();
  Inverter& inverter = writer->obtainInverter();
  auto& singleHandler = inverter.getIndexHandler("point_single");
  auto& multiHandler = inverter.getIndexHandler("point_multi");
  for (int32_t doc = 0; doc < N; doc++) {
    inverter.startDoc();
    if (doc % 7 != 0) {
      single[(size_t)doc] = {(int64_t)((doc * 7919) % 10007) - 5000};
      singleHandler.index(inverter, single[(size_t)doc][0]);
    }
    int32_t count = doc % 11 == 0 ? 0 : doc % 5 + 1;
    for (int32_t i = 0; i < count; i++) {
      multi[(size_t)doc].push_back(
          (int64_t)((doc * 3571 + i * 1013) % 15013) - 7500);
    }
    multiHandler.index(inverter, std::span<const int64_t>(multi[(size_t)doc]));
    inverter.finishDoc();
  }
  writer->releaseInverter(inverter);
  writer->commit();
  auto reader = writer->getIndexReader();

  for (int32_t i = 0; i < 18; i++) {
    int64_t lo = -8000 + (int64_t)((i * 1237) % 12000);
    int64_t hi = lo + (i % 4 == 0 ? 0 : 50 + i * 193);
    for (auto [field, values] : {
           std::pair<std::string_view, const std::vector<std::vector<int64_t>>*>{
               "point_single", &single},
           {"point_multi", &multi}}) {
      auto expected = oracle(*values, lo, hi);
      EXPECT_EQ(fullScan(*reader, field, lo, hi), expected);
      EXPECT_EQ(selectedScorer(*reader, field, lo, hi,
                               std::numeric_limits<int64_t>::max()), expected);
      EXPECT_EQ(selectedScorer(*reader, field, lo, hi, 0), expected);
      EXPECT_EQ(materialized(*reader, field, lo, hi), expected);
      EXPECT_EQ(exactCount(*reader, field, lo, hi), (int64_t)expected.size());
    }
  }
}

TEST_F(NumericRangePointsTest, bitsetLeavesMatchEveryQueryArm) {
  constexpr int32_t N = 1400;
  CollectionHelper helper;
  const RangeField fields[] = {{"bitset_point"}};
  setRangeSchema(helper, fields);

  std::vector<std::vector<int64_t>> values(N);
  auto writer = helper.getIndexWriter();
  Inverter& inverter = writer->obtainInverter();
  auto& field = inverter.getIndexHandler("bitset_point");
  SplitMix64 rng(0x91c7a5e3);
  for (int32_t doc = 0; doc < N; doc++) {
    inverter.startDoc();
    if (rng.rint(2) == 0) {
      values[(size_t)doc] = {(int64_t)doc * 3};
      field.index(inverter, values[(size_t)doc][0]);
    }
    inverter.finishDoc();
  }
  writer->releaseInverter(inverter);
  writer->commit();
  auto reader = writer->getIndexReader();
  auto& segment = reader->segments()[0];
  SegFieldInfo info = fieldInfo(segment, "bitset_point");
  PointsReader points(segment.postingsReader(), info);
  points.validate();
  ASSERT_EQ(2u, points.leafCount());
  EXPECT_EQ(PointsWriter::DOC_BITSET, points.leafInfo(0).docCodec);
  EXPECT_EQ(PointsWriter::DOC_BITSET, points.leafInfo(1).docCodec);
  EXPECT_GT(points.leafInfo(0).valueGcd, 1);

  const std::array<std::pair<int64_t, int64_t>, 4> ranges = {{
    {0, 30},
    {300, 2700},
    {3000, 3300},
    {0, (int64_t)N * 3},
  }};
  for (auto [lo, hi] : ranges) {
    auto expected = oracle(values, lo, hi);
    MemPool pool;
    QueryState state(pool, *reader, "bitset_point", lo, hi);
    EXPECT_EQ(expected, collect(state.weight->createFullScanScorerForTests(
                            pool, segment)));
    EXPECT_EQ(expected, collect(state.weight->createPointsScorerForTests(
                            pool, segment)));
    EXPECT_EQ(expected, collect(state.weight->createComplementScorerForTests(
                            pool, segment)));
    EXPECT_EQ(expected, collect(state.weight->createZoneMapScorerForTests(
                            pool, segment)));
    EXPECT_EQ(expected, collect(state.weight->createScorer(pool, segment)));
    EXPECT_EQ(expected, materialized(*reader, "bitset_point", lo, hi));
  }
}

TEST_F(NumericRangePointsTest, allSelectionArmsAreReachable) {
  constexpr int32_t N = 33'000;
  CollectionHelper helper;
  const RangeField fields[] = {{"arm_sorted"}, {"arm_shuffled"}};
  setRangeSchema(helper, fields);

  auto writer = helper.getIndexWriter();
  Inverter& inverter = writer->obtainInverter();
  auto& sorted = inverter.getIndexHandler("arm_sorted");
  auto& shuffled = inverter.getIndexHandler("arm_shuffled");
  auto& scanSorted = inverter.getIndexHandler("arm_scan_sorted_i");
  auto& scanShuffled = inverter.getIndexHandler("arm_scan_shuffled_i");
  for (int32_t doc = 0; doc < N; doc++) {
    inverter.startDoc();
    sorted.index(inverter, doc);
    shuffled.index(inverter, (int64_t)((doc * 7919) % 33001));
    scanSorted.index(inverter, doc);
    scanShuffled.index(inverter, (int64_t)((doc * 7919) % 33001));
    inverter.finishDoc();
  }
  writer->releaseInverter(inverter);
  writer->commit();
  auto reader = writer->getIndexReader();
  auto& segment = reader->segments()[0];

  auto select = [&](std::string_view field, int64_t lo, int64_t hi,
                    int64_t leadCost) {
    MemPool pool;
    QueryState state(pool, *reader, field, lo, hi);
    auto* supplier = state.weight->scorerSupplier(pool, segment);
    EXPECT_NE(supplier, nullptr);
    SkipStats::reset();
    SkipStats::enabled = true;
    Query::Scorer* scorer = supplier->get(pool, leadCost);
    SkipStats::enabled = false;
    return std::tuple{collect(scorer),
                      SkipStats::numericRangeSparseVerifyArms,
                      SkipStats::numericRangeComplementArms,
                      SkipStats::numericRangePointsArms,
                      SkipStats::numericRangeZoneArms,
                      dynamic_cast<NumericRangeQuery::RangeScorer<
                          IntColReader::Iterator>*>(scorer) != nullptr};
  };

  auto sparse = select("arm_sorted", 100, 100, 0);
  EXPECT_EQ(std::get<1>(sparse), 1);
  auto points = select("arm_sorted", 100, 100,
                       std::numeric_limits<int64_t>::max());
  EXPECT_EQ(std::get<3>(points), 1);
  auto complement = select("arm_sorted", 0, 26'000,
                           std::numeric_limits<int64_t>::max());
  EXPECT_EQ(std::get<2>(complement), 1);
  auto midPoints = select("arm_sorted", 0,
                          Postings::NUMERIC_BLOCK_SIZE - 1,
                          std::numeric_limits<int64_t>::max());
  EXPECT_EQ(std::get<3>(midPoints), 1);
  EXPECT_EQ(std::get<4>(midPoints), 0);
  auto shuffledPoints = select("arm_shuffled", 0, 9'999,
                               std::numeric_limits<int64_t>::max());
  EXPECT_EQ(std::get<3>(shuffledPoints), 1);
  EXPECT_FALSE(std::get<5>(shuffledPoints));
  auto zone = select("arm_scan_sorted_i", 0,
                     Postings::NUMERIC_BLOCK_SIZE - 1,
                     std::numeric_limits<int64_t>::max());
  EXPECT_EQ(std::get<4>(zone), 1);
  EXPECT_EQ(std::get<3>(zone), 0);
  auto full = select("arm_scan_shuffled_i", 0, 9'999,
                     std::numeric_limits<int64_t>::max());
  EXPECT_TRUE(std::get<5>(full));
  EXPECT_EQ(std::get<3>(full), 0);

  EXPECT_EQ(std::get<0>(sparse), fullScan(*reader, "arm_sorted", 100, 100));
  EXPECT_EQ(std::get<0>(points), std::get<0>(sparse));
  EXPECT_EQ(std::get<0>(complement),
            fullScan(*reader, "arm_sorted", 0, 26'000));
  EXPECT_EQ(std::get<0>(midPoints), fullScan(
      *reader, "arm_sorted", 0, Postings::NUMERIC_BLOCK_SIZE - 1));
  EXPECT_EQ(std::get<0>(shuffledPoints),
            fullScan(*reader, "arm_shuffled", 0, 9'999));
  EXPECT_EQ(std::get<0>(zone), fullScan(
      *reader, "arm_scan_sorted_i", 0, Postings::NUMERIC_BLOCK_SIZE - 1));
  EXPECT_EQ(std::get<0>(full),
            fullScan(*reader, "arm_scan_shuffled_i", 0, 9'999));

  auto checkScoredArm = [&](std::string_view field, int64_t lo, int64_t hi,
                            int64_t leadCost, bool expectBulk) {
    MemPool pool;
    Query::Context context(pool, *reader);
    NumericRangeQuery query(field, lo, hi);
    auto* weight = static_cast<NumericRangeQuery::Weight*>(
        query.createWeight(context, Query::NEED_SCORES, 3.0f));
    auto* supplier = weight->scorerSupplier(pool, segment);
    ASSERT_NE(nullptr, supplier);
    auto* scorer = supplier->get(pool, leadCost);
    ASSERT_NE(nullptr, scorer);
    EXPECT_FLOAT_EQ(3.0f, scorer->getMaxScore(PostingsReader::END));
    EXPECT_FLOAT_EQ(3.0f,
                    scorer->getMaxScoreForSetup(PostingsReader::END));
    scorer->setMinCompetitiveScore(3.0f);
    ASSERT_NE(PostingsReader::END, scorer->next());
    EXPECT_FLOAT_EQ(3.0f, scorer->score());

    BulkScorer* bulk = supplier->bulkScorer(pool);
    if (!expectBulk) {
      EXPECT_EQ(nullptr, bulk);
      return;
    }
    ASSERT_NE(nullptr, bulk);
    ScoreWindow window;
    bulk->scoreNextWindow(window, nullptr, 0, segment.maxDoc(), 3.0f);
    ASSERT_GT(window.size, 0);
    for (int32_t i = 0; i < window.size; i++) {
      EXPECT_FLOAT_EQ(3.0f, window.scores[(size_t)i]);
    }
  };

  checkScoredArm("arm_sorted", 100, 100, 0, true);  // sparse pull, points bulk
  checkScoredArm("arm_sorted", 100, 100,
                 std::numeric_limits<int64_t>::max(), true);  // points array
  checkScoredArm("arm_sorted", 0, Postings::NUMERIC_BLOCK_SIZE - 1,
                 std::numeric_limits<int64_t>::max(), true);  // points bitset
  checkScoredArm("arm_sorted", 0, 26'000,
                 std::numeric_limits<int64_t>::max(), true);  // complement
  checkScoredArm("arm_scan_sorted_i", 0,
                 Postings::NUMERIC_BLOCK_SIZE - 1,
                 std::numeric_limits<int64_t>::max(), true);  // zone map
  checkScoredArm("arm_scan_shuffled_i", 0, 9'999,
                 std::numeric_limits<int64_t>::max(), false);  // full scan
}

TEST_F(NumericRangePointsTest, optionalComplementAndMultiLeafDedup) {
  constexpr int32_t N = 1300;
  CollectionHelper helper;
  const RangeField fields[] = {{"optional_point"}, {"multi_leaf", true}};
  setRangeSchema(helper, fields);

  std::vector<std::vector<int64_t>> optional(N);
  std::vector<std::vector<int64_t>> multi(N);
  auto writer = helper.getIndexWriter();
  Inverter& inverter = writer->obtainInverter();
  auto& optionalHandler = inverter.getIndexHandler("optional_point");
  auto& multiHandler = inverter.getIndexHandler("multi_leaf");
  for (int32_t doc = 0; doc < N; doc++) {
    inverter.startDoc();
    if (doc % 4 != 0) {
      optional[(size_t)doc] = {doc};
      optionalHandler.index(inverter, doc);
    }
    multi[(size_t)doc] = doc == 0
        ? std::vector<int64_t>{-10'000, 10'000}
        : std::vector<int64_t>{doc};
    multiHandler.index(inverter, std::span<const int64_t>(multi[(size_t)doc]));
    inverter.finishDoc();
  }
  writer->releaseInverter(inverter);
  writer->commit();
  auto reader = writer->getIndexReader();

  auto optionalExpected = oracle(optional, 0, 900);
  EXPECT_EQ(selectedScorer(*reader, "optional_point", 0, 900,
                           std::numeric_limits<int64_t>::max()),
            optionalExpected);
  EXPECT_EQ(materialized(*reader, "optional_point", 0, 900), optionalExpected);

  auto multiExpected = oracle(multi, 9000, 11'000);
  ASSERT_EQ(multiExpected, std::vector<int32_t>{0});
  auto result = selectedScorer(*reader, "multi_leaf", 9000, 11'000,
                               std::numeric_limits<int64_t>::max());
  EXPECT_EQ(result, multiExpected);
  EXPECT_EQ(std::count(result.begin(), result.end(), 0), 1);
}

TEST_F(NumericRangePointsTest, fanoutCostAndArrayBitsetBoundary) {
  constexpr int32_t N = 16;
  CollectionHelper helper;
  const RangeField fields[] = {{"fanout", true}};
  setRangeSchema(helper, fields);

  auto writer = helper.getIndexWriter();
  Inverter& inverter = writer->obtainInverter();
  auto& handler = inverter.getIndexHandler("fanout");
  for (int32_t doc = 0; doc < N; doc++) {
    inverter.startDoc();
    std::vector<int64_t> values;
    for (int32_t i = 0; i < 80; i++) values.push_back(1000 + doc * 100 + i);
    if (doc == 0) values.push_back(1);
    if (doc < 2) values.push_back(2);
    handler.index(inverter, std::span<const int64_t>(values));
    inverter.finishDoc();
  }
  writer->releaseInverter(inverter);
  writer->commit();
  auto reader = writer->getIndexReader();
  auto& segment = reader->segments()[0];

  MemPool pool;
  QueryState state(pool, *reader, "fanout", 2, 2);
  auto* supplier = state.weight->scorerSupplier(pool, segment);
  ASSERT_NE(supplier, nullptr);
  EXPECT_EQ(supplier->cost(), N);
  auto* dense = supplier->get(pool, std::numeric_limits<int64_t>::max());
  EXPECT_NE(dynamic_cast<NumericRangeQuery::PointsBitScorer*>(dense), nullptr);
  EXPECT_EQ(collect(dense), (std::vector<int32_t>{0, 1}));

  MemPool sparsePool;
  QueryState sparseState(sparsePool, *reader, "fanout", 1, 1);
  auto* sparseSupplier = sparseState.weight->scorerSupplier(sparsePool, segment);
  ASSERT_NE(sparseSupplier, nullptr);
  auto* sparse = sparseSupplier->get(
      sparsePool, std::numeric_limits<int64_t>::max());
  EXPECT_NE(dynamic_cast<NumericRangeQuery::PointsArrayScorer*>(sparse), nullptr);
  EXPECT_EQ(collect(sparse), (std::vector<int32_t>{0}));
}

TEST_F(NumericRangePointsTest, exactCountDuplicatesAndBulkDomain) {
  constexpr int32_t N = 1200;
  CollectionHelper helper;
  const RangeField fields[] = {{"duplicate_endpoint"}, {"domain_point"}};
  setRangeSchema(helper, fields);

  auto writer = helper.getIndexWriter();
  Inverter& inverter = writer->obtainInverter();
  auto& handler = inverter.getIndexHandler("duplicate_endpoint");
  auto& domainHandler = inverter.getIndexHandler("domain_point");
  for (int32_t doc = 0; doc < N; doc++) {
    inverter.startDoc();
    handler.index(inverter, doc < 600 ? 7 : 9);
    domainHandler.index(inverter, doc);
    inverter.finishDoc();
  }
  writer->releaseInverter(inverter);
  writer->commit();
  auto reader = writer->getIndexReader();
  EXPECT_EQ(exactCount(*reader, "duplicate_endpoint", 7, 7), 600);
  EXPECT_EQ(exactCount(*reader, "duplicate_endpoint", 7, 9), N);

  RAMBitDocSet bitDomain(N);
  std::vector<int32_t> arrayDocs;
  for (int32_t doc = 0; doc < N; doc++) {
    if (doc % 11 == 0) bitDomain.mutableBits().set(doc);
    if (doc % 13 == 0) arrayDocs.push_back(doc);
  }
  ArrDocSet arrayDomain(std::move(arrayDocs));
  auto filter = [](const std::vector<int32_t>& docs, DocSet& domain) {
    std::vector<int32_t> out;
    for (int32_t doc : docs) if (domain.get(doc)) out.push_back(doc);
    return out;
  };
  auto denseExpected = fullScan(*reader, "domain_point", 100, 199);
  EXPECT_EQ(materialized(*reader, "domain_point", 100, 199, &bitDomain),
            filter(denseExpected, bitDomain));
  auto sparseExpected = fullScan(*reader, "domain_point", 100, 110);
  EXPECT_EQ(materialized(*reader, "domain_point", 100, 110, &arrayDomain),
            filter(sparseExpected, arrayDomain));
}

// A doc with several in-range values must surface once from the sparse ARRAY
// materialization (the bitset path dedups structurally; the array path's
// explicit dedup is only reachable when the match count stays under the
// array/bitset threshold, which the randomized corpus rarely hits).
TEST_F(NumericRangePointsTest, arrayPathDedupsMultiValuedDuplicates) {
  constexpr int32_t N = 1400;
  CollectionHelper helper;
  const RangeField fields[] = {{"dup_multi", true}};
  setRangeSchema(helper, fields);

  std::vector<std::vector<int64_t>> values(N);
  auto writer = helper.getIndexWriter();
  Inverter& inverter = writer->obtainInverter();
  auto& handler = inverter.getIndexHandler("dup_multi");
  for (int32_t doc = 0; doc < N; doc++) {
    inverter.startDoc();
    values[(size_t)doc] = doc == 3
        ? std::vector<int64_t>{500'000, 500'000, 500'001}
        : std::vector<int64_t>{doc, doc + N};
    handler.index(inverter, std::span<const int64_t>(values[(size_t)doc]));
    inverter.finishDoc();
  }
  writer->releaseInverter(inverter);
  writer->commit();
  auto reader = writer->getIndexReader();

  auto expected = oracle(values, 500'000, 500'001);
  ASSERT_EQ(expected, std::vector<int32_t>{3});
  MemPool pool;
  QueryState state(pool, *reader, "dup_multi", 500'000, 500'001);
  auto* supplier = state.weight->scorerSupplier(pool, reader->segments()[0]);
  ASSERT_NE(supplier, nullptr);
  auto* scorer = supplier->get(pool, std::numeric_limits<int64_t>::max());
  ASSERT_NE(dynamic_cast<NumericRangeQuery::PointsArrayScorer*>(scorer), nullptr);
  EXPECT_EQ(collect(scorer), expected);
  EXPECT_EQ(materialized(*reader, "dup_multi", 500'000, 500'001), expected);
}

// Query bounds that land strictly inside a gcd step must round toward the
// range (ceil on the lower residual): even-valued leaves probed with odd
// bounds, plus a >32-bit-residual raw leaf probed off-value.  All other test
// data has gcd == 1, which cannot distinguish ceil from floor.
TEST_F(NumericRangePointsTest, boundaryInsideGcdStepAndRawLeaf) {
  constexpr int32_t N = 200;
  CollectionHelper helper;
  const RangeField fields[] = {{"even_point"}, {"wide_point"}};
  setRangeSchema(helper, fields);

  std::vector<std::vector<int64_t>> even(N);
  std::vector<std::vector<int64_t>> wide(N);
  auto writer = helper.getIndexWriter();
  Inverter& inverter = writer->obtainInverter();
  auto& evenHandler = inverter.getIndexHandler("even_point");
  auto& wideHandler = inverter.getIndexHandler("wide_point");
  for (int32_t doc = 0; doc < N; doc++) {
    inverter.startDoc();
    even[(size_t)doc] = {2 * doc};
    evenHandler.index(inverter, 2 * doc);
    // Spread exceeds 32 bits of residual and the +doc%3 breaks the gcd (a
    // pure doc<<33 progression has gcd 2^33 and would FOR-pack after all).
    int64_t wideValue = ((int64_t)doc << 33) + doc % 3;
    wide[(size_t)doc] = {wideValue};
    wideHandler.index(inverter, wideValue);
    inverter.finishDoc();
  }
  writer->releaseInverter(inverter);
  writer->commit();
  auto reader = writer->getIndexReader();

  for (auto [lo, hi] : {std::pair<int64_t, int64_t>{3, 5}, {3, 4}, {4, 5},
                        {3, 3}, {1, 399}, {-1, 1}, {395, 399}}) {
    auto expected = oracle(even, lo, hi);
    EXPECT_EQ(selectedScorer(*reader, "even_point", lo, hi,
                             std::numeric_limits<int64_t>::max()), expected)
        << "even [" << lo << "," << hi << "]";
    EXPECT_EQ(exactCount(*reader, "even_point", lo, hi),
              (int64_t)expected.size()) << "even [" << lo << "," << hi << "]";
  }
  for (auto [lo, hi] : {std::pair<int64_t, int64_t>{(1LL << 33) + 1, (5LL << 33) - 1},
                        {(1LL << 33) + 1, (5LL << 33) + 1},
                        {3LL << 33, 3LL << 33}}) {
    auto expected = oracle(wide, lo, hi);
    EXPECT_EQ(selectedScorer(*reader, "wide_point", lo, hi,
                             std::numeric_limits<int64_t>::max()), expected)
        << "wide [" << lo << "," << hi << "]";
    EXPECT_EQ(exactCount(*reader, "wide_point", lo, hi),
              (int64_t)expected.size()) << "wide [" << lo << "," << hi << "]";
  }
}

TEST_F(NumericRangePointsTest, mergedSegmentRetainsPoints) {
  constexpr int32_t DOCS_PER_SEGMENT = 350;
  CollectionHelper helper;
  const RangeField fields[] = {{"merged_range"}};
  setRangeSchema(helper, fields);
  auto writer = helper.getIndexWriter();

  for (int32_t segmentNum = 0; segmentNum < 2; segmentNum++) {
    Inverter& inverter = writer->obtainInverter();
    auto& handler = inverter.getIndexHandler("merged_range");
    for (int32_t doc = 0; doc < DOCS_PER_SEGMENT; doc++) {
      inverter.startDoc();
      handler.index(inverter, segmentNum * DOCS_PER_SEGMENT + doc);
      inverter.finishDoc();
    }
    writer->releaseInverter(inverter);
    writer->commit();
  }
  writer->mergeSegments();
  auto reader = writer->getIndexReader();
  ASSERT_EQ(reader->segments().size(), 1);
  SegFieldInfo info = fieldInfo(reader->segments()[0], "merged_range");
  ASSERT_NE(info.pointsMetaOff, 0);
  PointsReader points(reader->segments()[0].postingsReader(), info);
  points.validate();
  EXPECT_EQ(points.leafCount(), 2);
  EXPECT_EQ(points.leafInfo(0).count,
            PointsWriter::DEFAULT_MAX_POINTS_PER_LEAF);
  EXPECT_EQ(selectedScorer(*reader, "merged_range", 20, 59,
                           std::numeric_limits<int64_t>::max()),
            fullScan(*reader, "merged_range", 20, 59));
}

TEST_F(NumericRangePointsTest, mergedPointsMatchFreshRebuild) {
  CollectionHelper helper;
  auto writer = helper.getIndexWriter();

  std::vector<Doc> source1 = {
      flatdoc("id", "s1-a", "merge_values_is", vec_i(2, 2, 10)),
      flatdoc("id", "s1-b", "merge_values_is", vec_i(14))};
  ASSERT_TRUE(helper.indexAll(source1, UpdateMessage::COMMIT).success);
  auto preRangeReader = writer->getIndexReader();
  ASSERT_EQ(preRangeReader->segments().size(), 1);
  EXPECT_EQ(fieldInfo(preRangeReader->segments()[0],
                      "merge_values_is").pointsMetaOff, 0);

  const RangeField fields[] = {{"merge_values_is", true}};
  setRangeSchema(helper, fields);
  std::vector<Doc> source2 = {
      flatdoc("id", "s2-a", "merge_values_is", vec_i(4, 8)),
      flatdoc("id", "s2-delete", "merge_values_is", vec_i(12, 12)),
      flatdoc("id", "s2-b", "merge_values_is", vec_i(16, 20))};
  std::vector<Doc> source3 = {
      flatdoc("id", "s3-absent-a"),
      flatdoc("id", "s3-absent-b")};
  std::vector<Doc> source4 = {
      flatdoc("id", "s4-a", "merge_values_is", vec_i(6, 6, 18)),
      flatdoc("id", "s4-b", "merge_values_is", vec_i(22, 26))};
  ASSERT_TRUE(helper.indexAll(source2, UpdateMessage::COMMIT).success);
  ASSERT_TRUE(helper.indexAll(source3, UpdateMessage::COMMIT).success);
  ASSERT_TRUE(helper.indexAll(source4, UpdateMessage::COMMIT).success);
  ASSERT_TRUE(helper.deleteById("s2-delete", UpdateMessage::COMMIT).success);

  auto sourceReader = writer->getIndexReader();
  ASSERT_EQ(sourceReader->segments().size(), 4);
  int32_t pointSources = 0;
  int32_t synthesizedSources = 0;
  int32_t absentSources = 0;
  for (auto& segment : sourceReader->segments()) {
    auto info = tryFieldInfo(segment, "merge_values_is");
    if (!info) absentSources++;
    else if (info->pointsMetaOff == 0) synthesizedSources++;
    else pointSources++;
  }
  EXPECT_EQ(pointSources, 2);
  EXPECT_EQ(synthesizedSources, 1);
  EXPECT_EQ(absentSources, 1);

  writer->mergeSegments();
  auto mergedReader = writer->getIndexReader();
  ASSERT_EQ(mergedReader->segments().size(), 1);
  auto& mergedSegment = mergedReader->segments()[0];
  SegFieldInfo mergedInfo = fieldInfo(mergedSegment, "merge_values_is");
  ASSERT_NE(mergedInfo.pointsMetaOff, 0);
  PointsReader mergedPoints(mergedSegment.postingsReader(), mergedInfo);
  mergedPoints.validate();

  CollectionHelper fresh("points_merge_fresh");
  fresh.clear();
  setRangeSchema(fresh, fields);
  std::vector<Doc> liveDocs = {source1[0], source1[1], source2[0], source2[2],
                               source3[0], source3[1], source4[0], source4[1]};
  ASSERT_TRUE(fresh.indexAll(liveDocs, UpdateMessage::COMMIT).success);
  auto freshReader = fresh.getIndexWriter()->getIndexReader();
  ASSERT_EQ(freshReader->segments().size(), 1);

  auto merged = mergedPoints.readAll();
  auto rebuilt = readPoints(freshReader->segments()[0], "merge_values_is");
  EXPECT_EQ(merged, rebuilt);
  EXPECT_EQ(std::count(merged.begin(), merged.end(),
                       PointsReader::Point{6, 6}), 2);
  EXPECT_EQ(std::count_if(merged.begin(), merged.end(), [](const auto& point) {
              return point.value == 12;
            }), 0);
  EXPECT_GT(mergedPoints.leafInfo(0).valueGcd, 1);
  EXPECT_EQ(selectedScorer(*mergedReader, "merge_values_is", 5, 19,
                           std::numeric_limits<int64_t>::max()),
            fullScan(*mergedReader, "merge_values_is", 5, 19));
}
