// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include <benchmark/benchmark.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "bench/luxir_bench.h"
#include "luxir/query/GeoBoxQuery.h"
#include "luxir/query/DistanceFeatureQuery.h"
#include "luxir/query/GeoDistanceQuery.h"
#include "luxir/query/GeoNearestQuery.h"
#include "luxir/reader/BKDReader.h"
#include "luxir/reader/PostingsReader.h"
#include "luxir/reader/SkipStats.h"
#include "luxir/schema/Schema.h"
#include "luxir/search/GeoDistanceComparator.h"
#include "luxir/util/geo.h"
#include "luxir/util/random.h"
#include "test/CollectionHelper.h"
#include "test/LocalReq.h"
#include "test/SchemaBuilder.h"
#include "test/TestIndex.h"
#include "test/TestUtils.h"

using namespace luxir;
using namespace luxir::test;

namespace {

constexpr std::string_view FIELD = "geo";
constexpr int32_t PRODUCTION_DOCS = 500'000;
constexpr int32_t UNIT_TEST_DOCS = 5'000;
constexpr double PI = 3.14159265358979323846;

enum CorpusShape : uint8_t {
  UNIFORM,
  UNIFORM_MULTI,
  CLUSTERED,
  DIAGONAL,
  LATBAND
};

enum BoxCase : uint8_t {
  TINY,
  CITY,
  METRO,
  REGION,
  CONTINENT,
  HEMISPHERE,
  GLOBAL_MINUS_SLIVER,
  LAT_BAND_SLICE,
  LON_BAND_SLICE,
  DATELINE,
  DISJOINT,
  BOX_CASE_COUNT
};

enum GeoOp : uint8_t {
  QUERY,
  SCAN_FORCED,
  COUNT,
  ESTIMATE
};

enum DistanceCase : uint8_t {
  DISTANCE_TINY,
  DISTANCE_CITY,
  DISTANCE_METRO,
  DISTANCE_REGION,
  DISTANCE_CONTINENT,
  DISTANCE_HEMISPHERE,
  DISTANCE_GLOBAL_MINUS_SLIVER,
  DISTANCE_CASE_COUNT
};

struct CorpusSpec {
  CorpusShape shape;
  std::string_view name;
  bool multi;
};

struct BoxSpec {
  double minLat;
  double maxLat;
  double minLon;
  double maxLon;
};

struct CircleSpec {
  double lat;
  double lon;
  double radiusMeters;
};

struct RawPoint {
  double lat;
  double lon;
};

constexpr std::array<CorpusSpec, 5> CORPORA{{
    {UNIFORM, "uniform", false},
    {UNIFORM_MULTI, "uniform_multi", true},
    {CLUSTERED, "clustered", false},
    {DIAGONAL, "diagonal", false},
    {LATBAND, "latband", false},
}};

constexpr std::array<std::string_view, BOX_CASE_COUNT> BOX_NAMES{{
    "tiny", "city", "metro", "region", "continent", "hemisphere",
    "global_minus_sliver", "lat_band_slice", "lon_band_slice",
    "dateline", "disjoint",
}};

constexpr std::array<std::string_view, DISTANCE_CASE_COUNT> DISTANCE_NAMES{{
    "tiny", "city", "metro", "region", "continent", "hemisphere",
    "global_minus_sliver",
}};

constexpr std::array<RawPoint, 20> CITY_CENTERS{{
    {40.7128, -74.0060}, {34.0522, -118.2437}, {19.4326, -99.1332},
    {-23.5505, -46.6333}, {51.5074, -0.1278}, {48.8566, 2.3522},
    {30.0444, 31.2357}, {6.5244, 3.3792}, {55.7558, 37.6173},
    {28.6139, 77.2090}, {39.9042, 116.4074}, {35.6762, 139.6503},
    {1.3521, 103.8198}, {-33.8688, 151.2093}, {-33.9249, 18.4241},
    {-1.2921, 36.8219}, {-34.6037, -58.3816}, {21.3069, -157.8583},
    {61.2181, -149.9003}, {-33.4489, -70.6693},
}};

int32_t documentCount() {
  return luxir::unit_tests
      ? (int32_t)LuxirTest::scaleTestWork(UNIT_TEST_DOCS)
      : PRODUCTION_DOCS;
}

std::string_view opName(GeoOp op) {
  switch (op) {
    case QUERY: return "query";
    case SCAN_FORCED: return "scan_forced";
    case COUNT: return "count";
    case ESTIMATE: return "estimate";
  }
  return "unknown";
}

std::array<BoxSpec, BOX_CASE_COUNT> boxesFor(CorpusShape shape) {
  if (shape == UNIFORM || shape == UNIFORM_MULTI) {
    return {{{-0.01, 0.01, -0.01, 0.01},
             {-4.025, 4.025, -4.025, 4.025},
             {-12.73, 12.73, -12.73, 12.73},
             {-32.4, 32.4, -50.0, 50.0},
             {-63.0, 63.0, -90.0, 90.0},
             {-90.0, 90.0, -90.0, 90.0},
             {-90.0, 90.0, -162.0, 162.0},
             {-1.0, 1.0, -180.0, 180.0},
             {-90.0, 90.0, -2.0, 2.0},
             {-90.0, 90.0, 178.2, -178.2},
             {89.999, 89.9995, 179.999, 179.9995}}};
  }
  if (shape == CLUSTERED) {
    return {{{40.7103, 40.7153, -74.0085, -74.0035},
             {40.7036, 40.7220, -74.0152, -73.9968},
             {40.6822, 40.7434, -74.0366, -73.9754},
             {20.0, 62.0, -130.0, -55.0},
             {-40.0, 70.0, -130.0, 50.0},
             {-90.0, 90.0, -180.0, 0.0},
             {-90.0, 90.0, -144.0, 180.0},
             {-2.0, 2.0, -180.0, 180.0},
             {-90.0, 90.0, -2.0, 2.0},
             {-90.0, 90.0, 178.2, -178.2},
             {89.999, 89.9995, 179.999, 179.9995}}};
  }
  if (shape == DIAGONAL) {
    return {{{-0.01, 0.01, -0.01, 0.01},
             {-0.09, 0.09, -0.12, 0.12},
             {-0.9, 0.9, -0.93, 0.93},
             {-9.0, 9.0, -9.03, 9.03},
             {-31.5, 31.5, -31.53, 31.53},
             {-90.0, 0.0, -180.0, 180.0},
             {-81.0, 81.0, -180.0, 180.0},
             {-1.0, 1.0, -180.0, 180.0},
             {-90.0, 90.0, -1.0, 1.0},
             {-90.0, 90.0, 178.2, -178.2},
             {89.999, 89.9995, 179.999, 179.9995}}};
  }
  return {{{-0.01, 0.01, -0.01, 0.01},
           {-1.0, 1.0, -0.18, 0.18},
           {-1.0, 1.0, -1.8, 1.8},
           {-1.0, 1.0, -18.0, 18.0},
           {-1.0, 1.0, -63.0, 63.0},
           {-1.0, 1.0, -90.0, 90.0},
           {-1.0, 1.0, -162.0, 162.0},
           {-0.1, 0.1, -180.0, 180.0},
           {-1.0, 1.0, -2.0, 2.0},
           {-1.0, 1.0, 178.2, -178.2},
           {89.999, 89.9995, 179.999, 179.9995}}};
}

std::array<CircleSpec, DISTANCE_CASE_COUNT> circlesFor(CorpusShape shape) {
  // Calibrated against the deterministic generators to the box ladder's
  // approximate selectivities: tiny, 0.1%, 1%, 10%, 35%, 50%, and 90%.
  if (shape == CLUSTERED) {
    return {{{40.7128, -74.0060, 275.0},
             {40.7128, -74.0060, 1000.0},
             {40.7128, -74.0060, 3400.0},
             {40.0, -98.0, 2200000.0},
             {15.0, -40.0, 7000000.0},
             {0.0, -90.0, 10007557.0},
             {0.0, 0.0, 15000000.0}}};
  }
  if (shape == DIAGONAL) {
    return {{{0.0, 0.0, 1573.0},
             {0.0, 0.0, 14153.0},
             {0.0, 0.0, 141526.0},
             {0.0, 0.0, 1412098.0},
             {0.0, 0.0, 4790000.0},
             {0.0, 0.0, 6672000.0},
             {0.0, 0.0, 9800000.0}}};
  }
  if (shape == LATBAND) {
    return {{{0.0, 0.0, 1250.0},
             {0.0, 0.0, 53000.0},
             {0.0, 0.0, 210000.0},
             {0.0, 0.0, 2002000.0},
             {0.0, 0.0, 7005000.0},
             {0.0, 0.0, 10007557.0},
             {0.0, 0.0, 18013600.0}}};
  }
  return {{{0.0, 0.0, 1250.0},
           {0.0, 0.0, 500000.0},
           {0.0, 0.0, 1600000.0},
           {0.0, 0.0, 5000000.0},
           {0.0, 0.0, 8700000.0},
           {0.0, 0.0, 10007557.0},
           {0.0, 0.0, 15000000.0}}};
}

class CorpusGenerator {
  SplitMix64 rng;
  CorpusShape shape;

  double unit() {
    return (double)(rng() >> 11) * (1.0 / 9007199254740992.0);
  }

  double gaussian() {
    double u1 = std::max(unit(), std::numeric_limits<double>::min());
    return std::sqrt(-2.0 * std::log(u1)) * std::cos(2.0 * PI * unit());
  }

  static double wrapLongitude(double lon) {
    while (lon < -180.0) lon += 360.0;
    while (lon >= 180.0) lon -= 360.0;
    return lon;
  }

public:
  CorpusGenerator(CorpusShape shape, uint64_t seed) : rng(seed), shape(shape) {}

  RawPoint next() {
    if (shape == UNIFORM || shape == UNIFORM_MULTI) {
      return {-90.0 + 180.0 * unit(), -180.0 + 360.0 * unit()};
    }
    if (shape == CLUSTERED) {
      if (unit() < 0.95) {
        const RawPoint& center = CITY_CENTERS[(size_t)rng.rint(
            (int64_t)CITY_CENTERS.size())];
        double lat = std::clamp(center.lat + gaussian() * 0.05,
                                -90.0, std::nextafter(90.0, 0.0));
        double lon = wrapLongitude(center.lon + gaussian() * 0.05);
        return {lat, lon};
      }
      return {-90.0 + 180.0 * unit(), -180.0 + 360.0 * unit()};
    }
    if (shape == DIAGONAL) {
      double lat = -89.9 + 179.8 * unit();
      return {lat, std::clamp(lat + gaussian() * 0.02,
                              -180.0, std::nextafter(180.0, 0.0))};
    }
    return {-1.0 + 2.0 * unit(), -180.0 + 360.0 * unit()};
  }
};

std::shared_ptr<Schema> geoSchema(bool multi) {
  SchemaBuilder b;
  auto& field = b.field(FIELD);
  field.type = api::FieldDef::FieldClass::GEO_POINT;
  field.index = api::FieldDef::IndexMode::RANGE;
  field.multi = multi;
  auto base = Schema::createDefaultSchema();
  return b.build(base.get());
}

void populate(Inverter& inverter, CorpusShape shape, bool multi,
              int32_t docs, uint64_t seed,
              std::vector<std::array<int64_t, 2>>* packed = nullptr) {
  auto& handler = inverter.getIndexHandler(FIELD);
  CorpusGenerator generator(shape, seed);
  if (packed != nullptr) packed->resize((size_t)docs);
  int32_t valuesPerDoc = multi ? 2 : 1;
  for (int32_t doc = 0; doc < docs; doc++) {
    std::array<GeoPoint, 2> points;
    inverter.startDoc();
    for (int32_t value = 0; value < valuesPerDoc; value++) {
      RawPoint point = generator.next();
      points[(size_t)value] = {point.lat, point.lon};
      if (packed != nullptr) {
        (*packed)[(size_t)doc][(size_t)value] =
            geo::encodePoint(point.lat, point.lon);
      }
    }
    if (multi) {
      handler.index(inverter, std::span<const GeoPoint>(points));
    } else {
      handler.index(inverter, points[0].latitude, points[0].longitude);
    }
    inverter.finishDoc();
  }
}

bool packedMatches(int64_t packed, const GeoBoxQuery& query) {
  int32_t lat = geo::unpackLatitude(packed);
  int32_t lon = geo::unpackLongitude(packed);
  return !query.isEmpty()
      && query.getMinLatitude() <= lat && lat <= query.getMaxLatitude()
      && geo::longitudeInRange(lon, query.getMinLongitude(),
                               query.getMaxLongitude());
}

bool packedMatches(int64_t packed, double centerLat, double centerLon,
                   double sortKey) {
  return geo::haversinSortKey(
      centerLat, centerLon,
      geo::decodeLatitude(geo::unpackLatitude(packed)),
      geo::decodeLongitude(geo::unpackLongitude(packed))) <= sortKey;
}

class GeoFixture {
  TestIndex index;
  std::shared_ptr<Schema> schema;
  CorpusSpec corpus;
  int32_t docs;
  uint64_t points;
  std::array<BoxSpec, BOX_CASE_COUNT> boxes;
  std::array<CircleSpec, DISTANCE_CASE_COUNT> circles;
  std::array<int32_t, BOX_CASE_COUNT> expectedBoxes{};
  std::array<int32_t, DISTANCE_CASE_COUNT> expectedDistances{};

public:
  explicit GeoFixture(CorpusSpec corpus)
      : corpus(corpus), docs(documentCount()),
        points((uint64_t)docs * (corpus.multi ? 2ULL : 1ULL)),
        boxes(boxesFor(corpus.shape)), circles(circlesFor(corpus.shape)) {
    schema = geoSchema(corpus.multi);
    index.iw = std::make_unique<IndexWriter>(
        index.snapshots, schema);
    Inverter& inverter = index.getInverter();
    std::vector<std::array<int64_t, 2>> packed;
    populate(inverter, corpus.shape, corpus.multi, docs,
             0x7ac5d2e184b9360fULL + (uint64_t)corpus.shape, &packed);
    index.flush();
    index.initReader();
    if (index.reader->segments().size() != 1) {
      throw std::logic_error("geo benchmark fixture is not single segment");
    }

    int32_t valuesPerDoc = corpus.multi ? 2 : 1;
    for (int32_t boxIndex = 0; boxIndex < BOX_CASE_COUNT; boxIndex++) {
      const BoxSpec& box = boxes[(size_t)boxIndex];
      GeoBoxQuery query(FIELD, box.minLat, box.maxLat,
                        box.minLon, box.maxLon);
      int32_t matches = 0;
      for (const auto& docPoints : packed) {
        for (int32_t value = 0; value < valuesPerDoc; value++) {
          if (packedMatches(docPoints[(size_t)value], query)) {
            matches++;
            break;
          }
        }
      }
      expectedBoxes[(size_t)boxIndex] = matches;
    }
    for (int32_t circleIndex = 0;
         circleIndex < DISTANCE_CASE_COUNT; circleIndex++) {
      const CircleSpec& circle = circles[(size_t)circleIndex];
      double sortKey = geo::distanceQuerySortKey(circle.radiusMeters);
      int32_t matches = 0;
      for (const auto& docPoints : packed) {
        for (int32_t value = 0; value < valuesPerDoc; value++) {
          if (packedMatches(docPoints[(size_t)value], circle.lat, circle.lon,
                            sortKey)) {
            matches++;
            break;
          }
        }
      }
      expectedDistances[(size_t)circleIndex] = matches;
    }
  }

  TestIndex& testIndex() { return index; }
  const BoxSpec& box(BoxCase boxCase) const { return boxes[(size_t)boxCase]; }
  int32_t expectedCount(BoxCase boxCase) const {
    return expectedBoxes[(size_t)boxCase];
  }
  const CircleSpec& circle(DistanceCase distanceCase) const {
    return circles[(size_t)distanceCase];
  }
  int32_t expectedCount(DistanceCase distanceCase) const {
    return expectedDistances[(size_t)distanceCase];
  }
  int32_t docCount() const { return docs; }
  uint64_t pointCount() const { return points; }
  bool multiValued() const { return corpus.multi; }

  SegFieldInfo fieldInfo() {
    MemPool pool;
    auto& segment = index.reader->segments()[0];
    auto* fields = segment.postingsReader().fieldInfo(FIELD);
    if (!fields) {
      throw std::logic_error("geo benchmark field is missing");
    }
    const auto& info = *fields;
    return info;
  }
};

GeoFixture& fixture(CorpusShape shape) {
  static std::array<std::unique_ptr<GeoFixture>, CORPORA.size()> fixtures;
  auto& value = fixtures[(size_t)shape];
  if (value == nullptr) value = std::make_unique<GeoFixture>(CORPORA[(size_t)shape]);
  return *value;
}

int32_t countMatches(Query::Scorer* scorer) {
  int32_t count = 0;
  if (scorer != nullptr) {
    while (scorer->next() != PostingsReader::END) count++;
  }
  return count;
}

bool unitTimingCase(CorpusShape shape, BoxCase box) {
  if (!luxir::unit_tests) return true;
  bool corpusSelected = shape == UNIFORM || shape == CLUSTERED;
  bool boxSelected = box == CITY || box == REGION || box == DATELINE;
  return corpusSelected && boxSelected;
}

bool unitTimingCase(CorpusShape shape, DistanceCase circle) {
  if (!luxir::unit_tests) return true;
  bool corpusSelected = shape == UNIFORM || shape == CLUSTERED;
  bool circleSelected = circle == DISTANCE_CITY || circle == DISTANCE_REGION
                     || circle == DISTANCE_GLOBAL_MINUS_SLIVER;
  return corpusSelected && circleSelected;
}

void skipReduced(benchmark::State& state) {
  state.SkipWithMessage("reduced geo unit-test matrix");
}

void BM_GeoIterate(benchmark::State& state, CorpusShape shape, GeoOp op) {
  BoxCase boxCase = (BoxCase)state.range(0);
  if (!unitTimingCase(shape, boxCase)) {
    skipReduced(state);
    return;
  }

  GeoFixture& data = fixture(shape);
  const BoxSpec& box = data.box(boxCase);
  int32_t expected = data.expectedCount(boxCase);
  auto& segment = data.testIndex().reader->segments()[0];
  int64_t armBKD = 0;
  int64_t armScan = 0;

  if (op == QUERY) {
    MemPool pool;
    Query::Context context(pool, *data.testIndex().reader);
    GeoBoxQuery query(FIELD, box.minLat, box.maxLat, box.minLon, box.maxLon);
    auto* weight = static_cast<GeoBoxQuery::Weight*>(
        query.createWeight(context, 0));
    bool saved = SkipStats::enabled;
    SkipStats::enabled = true;
    int64_t bkdBefore = SkipStats::geoBKDArms;
    int64_t scanBefore = SkipStats::geoScanArms;
    Query::Scorer* scorer = weight->createScorer(pool, segment);
    armBKD = SkipStats::geoBKDArms - bkdBefore;
    armScan = SkipStats::geoScanArms - scanBefore;
    SkipStats::enabled = saved;
    if (countMatches(scorer) != expected) {
      state.SkipWithError("geo selected scorer differs from quantized oracle");
      return;
    }
  }

  for (auto _ : state) {
    MemPool pool;
    Query::Context context(pool, *data.testIndex().reader);
    GeoBoxQuery query(FIELD, box.minLat, box.maxLat, box.minLon, box.maxLon);
    auto* weight = static_cast<GeoBoxQuery::Weight*>(
        query.createWeight(context, 0));
    Query::Scorer* scorer = op == SCAN_FORCED
        ? weight->createScanScorerForTests(pool, segment)
        : weight->createScorer(pool, segment);
    int32_t count = countMatches(scorer);
    benchmark::DoNotOptimize(count);
    if (count != expected) {
      state.SkipWithError("geo scorer differs from quantized oracle");
      break;
    }
  }
  state.counters["docs"] = data.docCount();
  state.counters["matches"] = expected;
  if (op == QUERY) {
    state.counters["arm_bkd"] = armBKD != 0 ? 1 : 0;
    state.counters["arm_scan"] = armScan != 0 ? 1 : 0;
  }
}

void BM_GeoCount(benchmark::State& state, CorpusShape shape) {
  BoxCase boxCase = (BoxCase)state.range(0);
  if (!unitTimingCase(shape, boxCase)) {
    skipReduced(state);
    return;
  }
  GeoFixture& data = fixture(shape);
  const BoxSpec& box = data.box(boxCase);
  int32_t expected = data.expectedCount(boxCase);
  auto& segment = data.testIndex().reader->segments()[0];

  for (auto _ : state) {
    MemPool pool;
    Query::Context context(pool, *data.testIndex().reader);
    GeoBoxQuery query(FIELD, box.minLat, box.maxLat, box.minLon, box.maxLon);
    auto* weight = static_cast<GeoBoxQuery::Weight*>(
        query.createWeight(context, 0));
    int64_t count = countForTests(weight, segment);
    benchmark::DoNotOptimize(count);
    if (count != expected) {
      state.SkipWithError("geo count differs from quantized oracle");
      break;
    }
  }
  state.counters["docs"] = data.docCount();
  state.counters["matches"] = expected;
}

void BM_GeoEstimate(benchmark::State& state, CorpusShape shape) {
  if (luxir::unit_tests) {
    skipReduced(state);
    return;
  }
  BoxCase boxCase = (BoxCase)state.range(0);
  GeoFixture& data = fixture(shape);
  const BoxSpec& box = data.box(boxCase);
  SegFieldInfo info = data.fieldInfo();
  auto& segment = data.testIndex().reader->segments()[0];
  BKDReader reader(segment.postingsReader(), info);
  GeoBoxQuery query(FIELD, box.minLat, box.maxLat, box.minLon, box.maxLon);
  BKDBoxRelation relation(query.getMinLatitude(), query.getMaxLatitude(),
                          query.getMinLongitude(), query.getMaxLongitude());
  BKDReader::EstimateResult estimate{};
  for (auto _ : state) {
    estimate = reader.estimateIntersect(relation);
    benchmark::DoNotOptimize(estimate);
  }
  state.counters["estimated_count"] = (double)estimate.estimatedCount;
  state.counters["upper_bound"] = (double)estimate.upperBound;
  state.counters["exact_count"] = data.expectedCount(boxCase);
}

void BM_GeoDistanceIterate(benchmark::State& state, CorpusShape shape,
                           GeoOp op) {
  DistanceCase distanceCase = (DistanceCase)state.range(0);
  if (!unitTimingCase(shape, distanceCase)) {
    skipReduced(state);
    return;
  }
  GeoFixture& data = fixture(shape);
  const CircleSpec& circle = data.circle(distanceCase);
  int32_t expected = data.expectedCount(distanceCase);
  auto& segment = data.testIndex().reader->segments()[0];
  int64_t armBKD = 0;
  int64_t armScan = 0;

  if (op == QUERY) {
    MemPool pool;
    Query::Context context(pool, *data.testIndex().reader);
    GeoDistanceQuery query(FIELD, circle.lat, circle.lon,
                           circle.radiusMeters);
    auto* weight = static_cast<GeoDistanceQuery::Weight*>(
        query.createWeight(context, 0));
    bool saved = SkipStats::enabled;
    SkipStats::enabled = true;
    int64_t bkdBefore = SkipStats::geoBKDArms;
    int64_t scanBefore = SkipStats::geoScanArms;
    Query::Scorer* scorer = weight->createScorer(pool, segment);
    armBKD = SkipStats::geoBKDArms - bkdBefore;
    armScan = SkipStats::geoScanArms - scanBefore;
    SkipStats::enabled = saved;
    if (countMatches(scorer) != expected) {
      state.SkipWithError(
          "geo distance selected scorer differs from quantized oracle");
      return;
    }
  }

  for (auto _ : state) {
    MemPool pool;
    Query::Context context(pool, *data.testIndex().reader);
    GeoDistanceQuery query(FIELD, circle.lat, circle.lon,
                           circle.radiusMeters);
    auto* weight = static_cast<GeoDistanceQuery::Weight*>(
        query.createWeight(context, 0));
    Query::Scorer* scorer = op == SCAN_FORCED
        ? weight->createScanScorerForTests(pool, segment)
        : weight->createScorer(pool, segment);
    int32_t count = countMatches(scorer);
    benchmark::DoNotOptimize(count);
    if (count != expected) {
      state.SkipWithError("geo distance scorer differs from quantized oracle");
      break;
    }
  }
  state.counters["docs"] = data.docCount();
  state.counters["matches"] = expected;
  if (op == QUERY) {
    state.counters["arm_bkd"] = armBKD != 0 ? 1 : 0;
    state.counters["arm_scan"] = armScan != 0 ? 1 : 0;
  }
}

void BM_GeoDistanceCount(benchmark::State& state, CorpusShape shape) {
  DistanceCase distanceCase = (DistanceCase)state.range(0);
  if (!unitTimingCase(shape, distanceCase)) {
    skipReduced(state);
    return;
  }
  GeoFixture& data = fixture(shape);
  const CircleSpec& circle = data.circle(distanceCase);
  int32_t expected = data.expectedCount(distanceCase);
  auto& segment = data.testIndex().reader->segments()[0];
  for (auto _ : state) {
    MemPool pool;
    Query::Context context(pool, *data.testIndex().reader);
    GeoDistanceQuery query(FIELD, circle.lat, circle.lon,
                           circle.radiusMeters);
    auto* weight = static_cast<GeoDistanceQuery::Weight*>(
        query.createWeight(context, 0));
    int64_t count = countForTests(weight, segment);
    benchmark::DoNotOptimize(count);
    if (count != expected) {
      state.SkipWithError("geo distance count differs from quantized oracle");
      break;
    }
  }
  state.counters["docs"] = data.docCount();
  state.counters["matches"] = expected;
}

void BM_GeoDistanceEstimate(benchmark::State& state, CorpusShape shape) {
  if (luxir::unit_tests) {
    skipReduced(state);
    return;
  }
  DistanceCase distanceCase = (DistanceCase)state.range(0);
  GeoFixture& data = fixture(shape);
  const CircleSpec& circle = data.circle(distanceCase);
  SegFieldInfo info = data.fieldInfo();
  auto& segment = data.testIndex().reader->segments()[0];
  BKDReader reader(segment.postingsReader(), info);
  GeoDistanceQuery query(FIELD, circle.lat, circle.lon,
                         circle.radiusMeters);
  BKDDistanceRelation relation = query.makeRelation();
  BKDReader::EstimateResult estimate{};
  for (auto _ : state) {
    estimate = reader.estimateIntersect(relation);
    benchmark::DoNotOptimize(estimate);
  }
  state.counters["estimated_count"] = (double)estimate.estimatedCount;
  state.counters["upper_bound"] = (double)estimate.upperBound;
  state.counters["exact_count"] = data.expectedCount(distanceCase);
}

void BM_GeoSize(benchmark::State& state, CorpusShape shape) {
  if (luxir::unit_tests) {
    skipReduced(state);
    return;
  }
  GeoFixture& data = fixture(shape);
  SegFieldInfo info = data.fieldInfo();
  auto& segment = data.testIndex().reader->segments()[0];
  BKDReader reader(segment.postingsReader(), info);
  for (auto _ : state) benchmark::DoNotOptimize(reader);
  uint64_t bytes = reader.sizeInBytes();
  state.counters["bytes_per_point"] =
      (double)bytes / (double)data.pointCount();
  state.counters["points"] = (double)data.pointCount();
  state.counters["size_bytes"] = (double)bytes;
}

void BM_GeoBuild(benchmark::State& state, CorpusShape shape) {
  if (luxir::unit_tests) {
    state.SkipWithMessage("geo build benchmark is production-only");
    return;
  }
  const CorpusSpec& corpus = CORPORA[(size_t)shape];
  int32_t docs = documentCount();
  uint64_t pointCount = (uint64_t)docs * (corpus.multi ? 2ULL : 1ULL);
  for (auto _ : state) {
    state.PauseTiming();
    auto dir = std::make_unique<RAMDir>();
    auto schema = geoSchema(corpus.multi);
    CommitSnapshotRegistry snapshots(*dir);
    auto writer = std::make_unique<IndexWriter>(snapshots, schema);
    Inverter& inverter = writer->obtainInverter();
    populate(inverter, shape, corpus.multi, docs,
             0x3e8c7b1d956a204fULL + (uint64_t)shape);
    state.ResumeTiming();
    writer->releaseInverter(inverter, true);
    writer->commit();
    state.PauseTiming();
    auto reader = writer->snapshots.readers.getReader();
    if (reader->segments().size() != 1) {
      state.SkipWithError("geo build did not produce one segment");
    }
    reader.reset();
    writer.reset();
    dir.reset();
    state.ResumeTiming();
  }
  state.counters["points_per_second"] = benchmark::Counter(
      (double)pointCount, benchmark::Counter::kIsRate);
}

void BM_GeoMerge(benchmark::State& state, CorpusShape shape) {
  if (luxir::unit_tests) {
    state.SkipWithMessage("geo merge benchmark is production-only");
    return;
  }
  const CorpusSpec& corpus = CORPORA[(size_t)shape];
  int32_t docs = documentCount();
  int32_t firstHalf = docs / 2;
  uint64_t pointCount = (uint64_t)docs * (corpus.multi ? 2ULL : 1ULL);
  for (auto _ : state) {
    state.PauseTiming();
    auto dir = std::make_unique<RAMDir>();
    auto schema = geoSchema(corpus.multi);
    CommitSnapshotRegistry snapshots(*dir);
    auto writer = std::make_unique<IndexWriter>(snapshots, schema);
    for (int32_t half = 0; half < 2; half++) {
      int32_t halfDocs = half == 0 ? firstHalf : docs - firstHalf;
      Inverter& inverter = writer->obtainInverter();
      populate(inverter, shape, corpus.multi, halfDocs,
               0xb27d4e619ac3805fULL + (uint64_t)shape * 2 + half);
      writer->releaseInverter(inverter, true);
      writer->commit();
    }
    auto before = writer->snapshots.readers.getReader();
    if (before->segments().size() != 2) {
      state.SkipWithError("geo merge setup did not produce two segments");
    }
    before.reset();
    state.ResumeTiming();
    writer->mergeSegments();
    state.PauseTiming();
    auto after = writer->snapshots.readers.getReader();
    if (after->segments().size() != 1) {
      state.SkipWithError("geo merge did not produce one segment");
    }
    after.reset();
    writer.reset();
    dir.reset();
    state.ResumeTiming();
  }
  state.counters["points_per_second"] = benchmark::Counter(
      (double)pointCount, benchmark::Counter::kIsRate);
}

std::string benchName(const CorpusSpec& corpus, std::string_view op,
                      std::string_view box = {}) {
  std::string name = "BM_Geo/" + std::string(corpus.name) + "/"
                   + std::string(op);
  if (!box.empty()) name += "/" + std::string(box);
  return name;
}

// Query points for nearest-k and distance sorts: twenty cities, offset so
// they do not sit on a clustered corpus's centers.
constexpr std::array<RawPoint, 20> NEAREST_CENTERS{{
    {40.7258, -74.0271}, {34.0652, -118.2647}, {19.4456, -99.1542},
    {-23.5375, -46.6543}, {51.5204, -0.1488}, {48.8696, 2.3312},
    {30.0574, 31.2147}, {6.5374, 3.3582}, {55.7688, 37.5963},
    {28.6269, 77.1880}, {39.9172, 116.3864}, {35.6892, 139.6293},
    {1.3651, 103.7988}, {-33.8558, 151.1883}, {-33.9119, 18.4031},
    {-1.2791, 36.8009}, {-34.5907, -58.4026}, {21.3199, -157.8793},
    {61.2311, -149.9213}, {-33.4359, -70.6903},
}};

// The k nearest docs by a full column pass: what a nearest-k costs without
// the points walk.
std::vector<int32_t> bruteNearest(IndexReader& reader, const SegFieldInfo& info,
                                  RawPoint center, int32_t k, bool multi) {
  auto& segment = reader.segments()[0];
  IntColReader column(segment.postingsReader(), info);
  IntColReader::Iterator iter(column);
  GeoNearestQuery::NearestHeap heap((size_t)k, std::numeric_limits<double>::infinity(), multi);
  auto key = [&](int64_t packed) {
    return geo::haversinSortKey(center.lat, center.lon,
                                geo::decodeLatitude(geo::unpackLatitude(packed)),
                                geo::decodeLongitude(geo::unpackLongitude(packed)));
  };
  for (int32_t doc = iter.next(); doc != PostingsReader::END; doc = iter.next()) {
    if (!column.multiValued()) {
      heap.offer(key(iter.value()), 0, doc);
      continue;
    }
    auto [start, end] = column.getStartEndValueRank(iter.rank());
    for (int64_t rank = start; rank < end; rank++) {
      heap.offer(key(iter.values().valueAt(rank)), 0, doc);
    }
  }
  std::vector<int32_t> docs;
  heap.forEach([&](double, int32_t, int32_t doc) { docs.push_back(doc); });
  std::sort(docs.begin(), docs.end());
  return docs;
}

std::vector<int32_t> walkNearest(IndexReader& reader, RawPoint center, int32_t k,
                                 bool multi) {
  MemPool pool;
  Query::Context context(pool, reader);
  GeoNearestQuery query(FIELD, center.lat, center.lon, k,
                        std::numeric_limits<double>::infinity(), multi);
  auto* weight = query.createWeight(context, Query::NEED_SCORES);
  Query::Weight::PrepareContext prepare(reader, {}, false, nullptr);
  auto prepared = weight->prepare(prepare);
  std::vector<int32_t> docs;
  Query::Scorer* scorer = prepared->createScorer(pool, reader.segments()[0]);
  for (int32_t doc = scorer ? scorer->next() : PostingsReader::END;
       doc != PostingsReader::END; doc = scorer->next()) {
    docs.push_back(doc);
  }
  return docs;
}

void BM_GeoNearest(benchmark::State& state, CorpusShape shape, bool walk) {
  if (luxir::unit_tests && shape != UNIFORM && shape != CLUSTERED) {
    skipReduced(state);
    return;
  }
  int32_t k = (int32_t)state.range(0);
  GeoFixture& data = fixture(shape);
  IndexReader& reader = *data.testIndex().reader;
  SegFieldInfo info = data.fieldInfo();
  {
    SkipStatsScope stats;
    for (RawPoint center : NEAREST_CENTERS) {
      if (walkNearest(reader, center, k, data.multiValued())
          != bruteNearest(reader, info, center, k, data.multiValued())) {
        state.SkipWithError("geo_nearest differs from the column pass");
        return;
      }
    }
    state.counters["leaves_per_query"] =
        (double)SkipStats::geoNearestLeaves / (double)NEAREST_CENTERS.size();
  }
  for (auto _ : state) {
    for (RawPoint center : NEAREST_CENTERS) {
      auto docs = walk ? walkNearest(reader, center, k, data.multiValued())
                       : bruteNearest(reader, info, center, k, data.multiValued());
      benchmark::DoNotOptimize(docs.data());
    }
  }
  state.counters["docs"] = data.docCount();
  state.counters["us_per_query"] = benchmark::Counter(
      (double)NEAREST_CENTERS.size(),
      benchmark::Counter::kIsIterationInvariantRate | benchmark::Counter::kInvert);
}

// Distance sorts through the whole search path, over one segment of
// uniform points: every doc, or the tenth of them that a filter admits.
// pruning: 2 = box test and points-index competitive set (the default), 1 =
// box test only, 0 = neither.
void BM_GeoSort(benchmark::State& state, bool filtered, int32_t pruning) {
  int32_t docs = luxir::unit_tests ? (int32_t)LuxirTest::scaleTestWork(5000) : 1'000'000;
  CollectionHelper helper("geo_sort_bm");
  std::vector<int32_t> shape{docs};
  if (!helper.indexMatchesShape(shape)) {
    helper.clear();
    CorpusGenerator generator(UNIFORM, 0x6e0d15);
    std::vector<Doc> batch;
    batch.reserve((size_t)docs);
    for (int32_t doc = 0; doc < docs; doc++) {
      RawPoint point = generator.next();
      batch.push_back(flatdoc("id", std::to_string(doc), "tag_s", doc % 10 == 0 ? "a" : "b",
                              "loc_p", std::vector<double>{point.lon, point.lat}));
    }
    ASSERT_TRUE(helper.indexAll(batch, UpdateMessage::COMMIT, false, 1).success);
  }
  int32_t limit = (int32_t)state.range(0);
  bool savedBox = GeoDistanceComparator::disableBoxForTests;
  bool savedIndex = GeoDistanceComparator::disableIndexForTests;
  auto restore = scope_guard([&]() {
    GeoDistanceComparator::disableBoxForTests = savedBox;
    GeoDistanceComparator::disableIndexForTests = savedIndex;
  });
  GeoDistanceComparator::disableBoxForTests = pruning < 1;
  GeoDistanceComparator::disableIndexForTests = pruning < 2;
  size_t center = 0;
  auto request = [&]() {
    RawPoint origin = NEAREST_CENTERS[center++ % NEAREST_CENTERS.size()];
    std::string json = R"({"collection":"geo_sort_bm","query":)"
        + std::string(filtered ? R"({"match":{"field":"tag_s","val":"a"}})" : R"({"all":true})")
        + ",\"sort\":[\"geo_distance(loc_p, " + std::to_string(origin.lat) + ", "
        + std::to_string(origin.lon) + ")\"],\"fields\":[\"id\",\"_sort_\"],\"limit\":"
        + std::to_string(limit) + "}";
    auto req = localReq(LuxirTest::luxirNode->getSearchEngine());
    EXPECT_TRUE(luxir::api::read_json(req->rawRequest(), json, req->mr)) << json;
    req->execute(false);
    EXPECT_TRUE(req->ok()) << req->errorMsg();
    return req;
  };
  {
    SkipStatsScope stats;
    request();
    state.counters["circle_builds"] = (double)SkipStats::geoSortCircleBuilds;
  }
  for (auto _ : state) {
    auto req = request();
    benchmark::DoNotOptimize(req->responses.size());
  }
  state.counters["matches"] = filtered ? docs / 10 : docs;
}

// Text plus proximity top-10 over one segment of uniform points whose body
// text matches half the docs, with distance_feature skipping on or off.
// shape: 0 = distance_feature alone, 1 = required text + optional
// distance_feature, 2 = optional text + optional distance_feature.
void BM_GeoProximity(benchmark::State& state, int32_t shape, bool skipping) {
  int32_t docs = luxir::unit_tests ? (int32_t)LuxirTest::scaleTestWork(5000) : 1'000'000;
  CollectionHelper helper("geo_proximity_bm");
  std::vector<int32_t> segments{docs};
  if (!helper.indexMatchesShape(segments)) {
    helper.clear();
    CorpusGenerator generator(UNIFORM, 0x9e0f2a);
    SplitMix64 rng(0x51);
    std::vector<Doc> batch;
    batch.reserve((size_t)docs);
    for (int32_t doc = 0; doc < docs; doc++) {
      RawPoint point = generator.next();
      std::string body = doc % 2 == 0 ? "coffee" : "tea";
      for (int32_t repeat = (int32_t)(rng() % 3); repeat > 0; repeat--) body += " coffee";
      body += " house";
      batch.push_back(flatdoc("id", std::to_string(doc), "body_t", body,
                              "loc_p", std::vector<double>{point.lon, point.lat}));
    }
    ASSERT_TRUE(helper.indexAll(batch, UpdateMessage::COMMIT, false, 1).success);
  }
  bool saved = DistanceFeatureQuery::disableSkippingForTests;
  auto restore = scope_guard([&]() { DistanceFeatureQuery::disableSkippingForTests = saved; });
  DistanceFeatureQuery::disableSkippingForTests = !skipping;
  size_t center = 0;
  auto request = [&]() {
    RawPoint origin = NEAREST_CENTERS[center++ % NEAREST_CENTERS.size()];
    std::string df = R"({"distance_feature":{"field":"loc_p","lat":)" + std::to_string(origin.lat)
        + R"(,"lon":)" + std::to_string(origin.lon) + R"(,"pivot_meters":50000}})";
    std::string text = R"({"match":{"field":"body_t","val":"coffee"}})";
    std::string query = shape == 0 ? df
        : shape == 1 ? R"({"boolean":{"required":[)" + text + R"(],"optional":[)" + df + "]}}"
                     : R"({"boolean":{"optional":[)" + text + "," + df + "]}}";
    std::string json = R"({"collection":"geo_proximity_bm","query":)" + query
        + R"(,"fields":["id"],"get_scores":true,"limit":10})";
    auto req = localReq(LuxirTest::luxirNode->getSearchEngine());
    EXPECT_TRUE(luxir::api::read_json(req->rawRequest(), json, req->mr)) << json;
    req->execute(false);
    EXPECT_TRUE(req->ok()) << req->errorMsg();
    return req;
  };
  {
    SkipStatsScope stats;
    request();
    state.counters["circle_builds"] = (double)SkipStats::distanceFeatureCircleBuilds;
  }
  for (auto _ : state) {
    auto req = request();
    benchmark::DoNotOptimize(req->responses.size());
  }
  state.counters["docs"] = docs;
}

std::string distanceBenchName(const CorpusSpec& corpus, std::string_view op,
                              std::string_view circle) {
  return "BM_GeoDistance/" + std::string(corpus.name) + "/"
       + std::string(op) + "/" + std::string(circle);
}

void registerCase(const CorpusSpec& corpus, GeoOp op, BoxCase box) {
  auto* registration = benchmark::RegisterBenchmark(
      benchName(corpus, opName(op), BOX_NAMES[(size_t)box]),
      [shape = corpus.shape, op](benchmark::State& state) {
        if (op == COUNT) {
          BM_GeoCount(state, shape);
        } else if (op == ESTIMATE) {
          BM_GeoEstimate(state, shape);
        } else {
          BM_GeoIterate(state, shape, op);
        }
      });
  registration->ArgName("box_id")->Arg((int32_t)box)->UseRealTime();
  if (op == ESTIMATE) registration->Iterations(1);
}

void registerDistanceCase(const CorpusSpec& corpus, GeoOp op,
                          DistanceCase circle) {
  auto* registration = benchmark::RegisterBenchmark(
      distanceBenchName(corpus, opName(op),
                        DISTANCE_NAMES[(size_t)circle]),
      [shape = corpus.shape, op](benchmark::State& state) {
        if (op == COUNT) {
          BM_GeoDistanceCount(state, shape);
        } else if (op == ESTIMATE) {
          BM_GeoDistanceEstimate(state, shape);
        } else {
          BM_GeoDistanceIterate(state, shape, op);
        }
      });
  registration->ArgName("circle_id")->Arg((int32_t)circle)->UseRealTime();
  if (op == ESTIMATE) registration->Iterations(1);
}

void registerSize(const CorpusSpec& corpus) {
  benchmark::RegisterBenchmark(
      benchName(corpus, "size"),
      [shape = corpus.shape](benchmark::State& state) {
        BM_GeoSize(state, shape);
      })->Iterations(1)->UseRealTime();
}

void registerBuildMerge(const CorpusSpec& corpus, bool merge) {
  benchmark::RegisterBenchmark(
      benchName(corpus, merge ? "merge" : "build"),
      [shape = corpus.shape, merge](benchmark::State& state) {
        if (merge) BM_GeoMerge(state, shape);
        else BM_GeoBuild(state, shape);
      })->Iterations(1)->UseRealTime()->Unit(benchmark::kMillisecond);
}

[[maybe_unused]] bool benchmarksRegistered = [] {
  for (const CorpusSpec& corpus : CORPORA) {
    for (bool walk : {true, false}) {
      benchmark::RegisterBenchmark(
          "BM_GeoNearest/" + std::string(corpus.name) + "/"
              + (walk ? "bkd_best_first" : "column_pass"),
          [shape = corpus.shape, walk](benchmark::State& state) {
            BM_GeoNearest(state, shape, walk);
          })->ArgName("k")->Arg(10)->Arg(100)->UseRealTime()
            ->Unit(benchmark::kMicrosecond);
    }
  }
  for (bool filtered : {false, true}) {
    for (int32_t pruning : {2, 1, 0}) {
      std::string_view mode = pruning == 2 ? "box_and_index" : pruning == 1 ? "box_only" : "none";
      benchmark::RegisterBenchmark(
          "BM_GeoSort/" + std::string(filtered ? "filter_10pct" : "all") + "/"
              + std::string(mode),
          [filtered, pruning](benchmark::State& state) {
            BM_GeoSort(state, filtered, pruning);
          })->ArgName("limit")->Arg(10)->Arg(100)->UseRealTime()
            ->Unit(benchmark::kMicrosecond);
    }
  }
  for (int32_t shape : {0, 1, 2}) {
    std::string_view name = shape == 0 ? "alone" : shape == 1 ? "must_text" : "should_text";
    for (bool skipping : {true, false}) {
      benchmark::RegisterBenchmark(
          "BM_GeoProximity/" + std::string(name) + "/" + (skipping ? "skipping" : "no_skipping"),
          [shape, skipping](benchmark::State& state) {
            BM_GeoProximity(state, shape, skipping);
          })->UseRealTime()->Unit(benchmark::kMicrosecond);
    }
  }
  for (const CorpusSpec& corpus : CORPORA) {
    for (int32_t box = 0; box < BOX_CASE_COUNT; box++) {
      BoxCase boxCase = (BoxCase)box;
      registerCase(corpus, QUERY, boxCase);
      registerCase(corpus, SCAN_FORCED, boxCase);
      if (!corpus.multi) registerCase(corpus, COUNT, boxCase);
      registerCase(corpus, ESTIMATE, boxCase);
    }
    for (int32_t circle = 0; circle < DISTANCE_CASE_COUNT; circle++) {
      DistanceCase distanceCase = (DistanceCase)circle;
      registerDistanceCase(corpus, QUERY, distanceCase);
      registerDistanceCase(corpus, SCAN_FORCED, distanceCase);
      if (!corpus.multi) registerDistanceCase(corpus, COUNT, distanceCase);
      registerDistanceCase(corpus, ESTIMATE, distanceCase);
    }
    registerSize(corpus);
    registerBuildMerge(corpus, false);
    registerBuildMerge(corpus, true);
  }
  return true;
}();

} // namespace
