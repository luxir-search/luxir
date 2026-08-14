#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <limits>
#include <numbers>
#include <string_view>
#include <vector>

#include "luxir/query/GeoDistanceQuery.h"
#include "luxir/reader/BKDReader.h"
#include "luxir/reader/FieldReader.h"
#include "luxir/reader/IntColReader.h"
#include "luxir/util/geo.h"
#include "luxir/util/random.h"
#include "test/CollectionHelper.h"
#include "test/SchemaBuilder.h"
#include "test/LuxirTest.h"
#include "test/TestUtils.h"

using namespace luxir;
using namespace luxir::test;

namespace {

double unitDouble(SplitMix64& rng) {
  return (double)(rng() >> 11) * (1.0 / 9007199254740992.0);
}

double normalizeLon(double lon) {
  lon = std::remainder(lon, 360.0);
  return lon == -180.0 ? 180.0 : lon;
}

GeoPoint destination(double lat, double lon, double meters, double bearing) {
  double lat1 = lat * (std::numbers::pi / 180.0);
  double lon1 = lon * (std::numbers::pi / 180.0);
  double distance = meters / geo::EARTH_MEAN_RADIUS_METERS;
  double lat2 = std::asin(std::sin(lat1) * std::cos(distance)
      + std::cos(lat1) * std::sin(distance) * std::cos(bearing));
  double lon2 = lon1 + std::atan2(
      std::sin(bearing) * std::sin(distance) * std::cos(lat1),
      std::cos(distance) - std::sin(lat1) * std::sin(lat2));
  return {lat2 * (180.0 / std::numbers::pi),
          normalizeLon(lon2 * (180.0 / std::numbers::pi))};
}

bool boxContains(const geo::BoundingBox& box, const GeoPoint& point) {
  if (point.latitude < box.minLat || point.latitude > box.maxLat) return false;
  if (box.minLon <= box.maxLon) {
    return box.minLon <= point.longitude && point.longitude <= box.maxLon;
  }
  return point.longitude >= box.minLon || point.longitude <= box.maxLon;
}

double exactMeters(double lat1, double lon1, double lat2, double lon2) {
  double x1 = lat1 * (std::numbers::pi / 180.0);
  double x2 = lat2 * (std::numbers::pi / 180.0);
  double h = (1.0 - std::cos(x1 - x2))
      + std::cos(x1) * std::cos(x2)
          * (1.0 - std::cos((lon1 - lon2)
                            * (std::numbers::pi / 180.0)));
  return geo::EARTH_MEAN_RADIUS_METERS * 2.0
      * std::asin(std::min(1.0, std::sqrt(h * 0.5)));
}

SegFieldInfo fieldInfo(IndexReader::Segment& segment, std::string_view field) {
  MemPool pool;
  FieldReader fields(segment.postingsReader());
  if (!fields.seek(field)) throw std::runtime_error("missing geo test field");
  SegFieldInfo info;
  fields.readFieldInfo(info);
  return info;
}

void setGeoSchema(CollectionHelper& helper, bool range = true) {
  SchemaBuilder b;
  api::FieldDef::IndexMode index = range ? api::FieldDef::IndexMode::RANGE
                                         : api::FieldDef::IndexMode::NONE;
  auto& single = b.field("geo_single");
  single.type = api::FieldDef::FieldClass::GEO_POINT;
  single.index = index;
  auto& multi = b.field("geo_multi");
  multi.type = api::FieldDef::FieldClass::GEO_POINT;
  multi.index = index;
  multi.multi = true;
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

struct DistanceState {
  Query::Context context;
  GeoDistanceQuery query;
  GeoDistanceQuery::Weight* weight;

  DistanceState(MemPool& pool, IndexReader& reader, std::string_view field,
                double lat, double lon, double radius, int32_t flags = 0,
                float multiplier = 1.0f)
      : context(pool, reader), query(field, lat, lon, radius),
        weight((GeoDistanceQuery::Weight*)query.createWeight(
            context, flags, multiplier)) {}
};

std::vector<int32_t> bruteColumn(IndexReader::Segment& segment,
                                 std::string_view field,
                                 const GeoDistanceQuery& query) {
  SegFieldInfo info = fieldInfo(segment, field);
  IntColReader column(segment.postingsReader(), info);
  IntColReader::Iterator iter(column);
  double key = geo::distanceQuerySortKey(query.getRadiusMeters());
  std::vector<int32_t> docs;
  for (int32_t doc = iter.next(); doc != PostingsReader::END;
       doc = iter.next()) {
    auto matches = [&](int64_t packed) {
      return geo::haversinSortKey(
          query.getCenterLatitude(), query.getCenterLongitude(),
          geo::decodeLatitude(geo::unpackLatitude(packed)),
          geo::decodeLongitude(geo::unpackLongitude(packed))) <= key;
    };
    bool match = false;
    if (!column.multiValued()) {
      match = matches(iter.value());
    } else {
      auto [start, end] = column.getStartEndValueRank(iter.rank());
      for (int64_t rank = start; rank < end && !match; rank++) {
        match = matches(iter.values().valueAt(rank));
      }
    }
    if (match) docs.push_back(doc);
  }
  return docs;
}

enum class Shape { UNIFORM, CLUSTERED, DATELINE, POLAR };

GeoPoint corpusPoint(Shape shape, SplitMix64& rng) {
  double a = unitDouble(rng);
  double b = unitDouble(rng);
  if (shape == Shape::CLUSTERED) {
    return {40.7128 + (a - 0.5) * 2.0, -74.0060 + (b - 0.5) * 2.0};
  }
  if (shape == Shape::DATELINE) {
    return {(a - 0.5) * 8.0, normalizeLon(179.0 + (b - 0.5) * 8.0)};
  }
  if (shape == Shape::POLAR) {
    return {84.0 + a * 5.9, -180.0 + b * 360.0};
  }
  return {-90.0 + a * 180.0, -180.0 + b * 360.0};
}

GeoPoint shapeCenter(Shape shape) {
  if (shape == Shape::CLUSTERED) return {40.7128, -74.0060};
  if (shape == Shape::DATELINE) return {0.0, 179.0};
  if (shape == Shape::POLAR) return {89.0, 45.0};
  return {0.0, 0.0};
}

} // namespace

class GeoMathTest : public LuxirTest {};

TEST_F(GeoMathTest, haversinAndDistanceSortKey) {
  EXPECT_EQ(0.0, geo::haversinSortKey(12.0, -34.0, 12.0, -34.0));
  EXPECT_DOUBLE_EQ(geo::haversinSortKey(10.0, 20.0, -30.0, 40.0),
                   geo::haversinSortKey(-30.0, 40.0, 10.0, 20.0));
  EXPECT_EQ(0u, std::bit_cast<uint64_t>(
                    geo::haversinSortKey(10.0, 20.0, -30.0, 40.0)) & 7u);
  EXPECT_NEAR(111195.08, geo::haversinMeters(0.0, 0.0, 1.0, 0.0), 0.1);
  EXPECT_NEAR(5570229.9,
              geo::haversinMeters(40.7128, -74.0060, 51.5074, -0.1278),
              1.0);

  for (double radius : {0.001, 1.0, 1000.123, 1000000.5, 19000000.0}) {
    double key = geo::distanceQuerySortKey(radius);
    EXPECT_GE(geo::haversinMeters(key), radius);
    uint64_t bits = std::bit_cast<uint64_t>(key);
    ASSERT_GT(bits, 0u);
    double predecessor = std::bit_cast<double>(bits - 1);
    if (geo::haversinMeters(key) != radius) {
      EXPECT_LT(geo::haversinMeters(predecessor), radius);
    } else {
      EXPECT_LE(geo::haversinMeters(predecessor), radius);
    }
  }
}

TEST_F(GeoMathTest, axisLatitudeAndCircleBoundingBox) {
  for (auto [lat, lon, radius] : {
           std::tuple{0.0, 0.0, 1000000.0},
           std::tuple{40.0, -74.0, 2500000.0},
           std::tuple{-55.0, 130.0, 500000.0}}) {
    constexpr int32_t SAMPLES = 10000;
    double bestDelta = -1.0;
    double bestBearing = 0.0;
    for (int32_t i = 0; i < SAMPLES; i++) {
      double bearing = 2.0 * std::numbers::pi * (double)i / SAMPLES;
      GeoPoint point = destination(lat, lon, radius, bearing);
      double delta = std::abs(std::remainder(point.longitude - lon, 360.0));
      if (delta > bestDelta) {
        bestDelta = delta;
        bestBearing = bearing;
      }
    }
    double step = 2.0 * std::numbers::pi / SAMPLES;
    double left = bestBearing - step;
    double right = bestBearing + step;
    auto longitudeDelta = [&](double bearing) {
      GeoPoint point = destination(lat, lon, radius, bearing);
      return std::abs(std::remainder(point.longitude - lon, 360.0));
    };
    for (int32_t i = 0; i < 80; i++) {
      double third = (right - left) / 3.0;
      double m1 = left + third;
      double m2 = right - third;
      if (longitudeDelta(m1) < longitudeDelta(m2)) left = m1;
      else right = m2;
    }
    double bruteAxis = destination(lat, lon, radius,
                                   (left + right) * 0.5).latitude;
    EXPECT_NEAR(bruteAxis, geo::axisLat(lat, radius),
                geo::AXISLAT_ERROR);
  }

  for (auto [lat, lon, radius] : {
           std::tuple{0.0, 179.9, 1000000.0},
           std::tuple{89.0, 20.0, 500000.0},
           std::tuple{-88.0, -170.0, 300000.0},
           std::tuple{35.0, -120.0, 0.0}}) {
    geo::BoundingBox box = geo::circleBoundingBox(lat, lon, radius);
    for (int32_t i = 0; i < 2000; i++) {
      GeoPoint point = destination(lat, lon, radius,
          2.0 * std::numbers::pi * (double)i / 2000.0);
      EXPECT_TRUE(boxContains(box, point)) << lat << ' ' << lon << ' ' << i;
    }
    if (std::abs(lat) + radius / geo::EARTH_MEAN_RADIUS_METERS
                              * (180.0 / std::numbers::pi) >= 90.0) {
      EXPECT_EQ(-180.0, box.minLon);
      EXPECT_EQ(180.0, box.maxLon);
    }
  }
}

class BKDDistanceRelationTest : public LuxirTest {};

TEST_F(BKDDistanceRelationTest, randomizedCellAndPointOracle) {
  SplitMix64 rng(0x70d18a4c);
  double earthDistance = geo::haversinMeters(
      std::numeric_limits<double>::max());
  int64_t iterations = scaleTestWork(500);
  for (int64_t iteration = 0; iteration < iterations; iteration++) {
    double centerLat = -90.0 + unitDouble(rng) * 180.0;
    double centerLon = -180.0 + unitDouble(rng) * 360.0;
    double radius;
    switch (iteration % 6) {
      case 0: radius = 0.0; break;
      case 1: radius = unitDouble(rng) * 100.0; break;
      case 2: radius = unitDouble(rng) * earthDistance; break;
      case 3:
        centerLon = iteration % 2 == 0 ? 179.9 : -179.9;
        radius = unitDouble(rng) * 2000000.0;
        break;
      case 4:
        centerLat = iteration % 2 == 0 ? 89.9 : -89.9;
        radius = unitDouble(rng) * 2000000.0;
        break;
      default: radius = earthDistance; break;
    }
    BKDDistanceRelation relation(centerLat, centerLon, radius);

    int32_t lat1 = geo::encodeLatitude(-90.0 + unitDouble(rng) * 180.0);
    int32_t lat2 = geo::encodeLatitude(-90.0 + unitDouble(rng) * 180.0);
    int32_t lon1 = geo::encodeLongitude(-180.0 + unitDouble(rng) * 360.0);
    int32_t lon2 = geo::encodeLongitude(-180.0 + unitDouble(rng) * 360.0);
    if (lat2 < lat1) std::swap(lat1, lat2);
    if (lon2 < lon1) std::swap(lon1, lon2);
    BKDRelation cellRelation = relation.compare(lat1, lat2, lon1, lon2);
    for (int32_t y = 0; y < 5; y++) {
      int32_t lat = (int32_t)((int64_t)lat1
          + ((int64_t)lat2 - lat1) * y / 4);
      for (int32_t x = 0; x < 5; x++) {
        int32_t lon = (int32_t)((int64_t)lon1
            + ((int64_t)lon2 - lon1) * x / 4);
        bool brute = geo::haversinSortKey(
            centerLat, centerLon, geo::decodeLatitude(lat),
            geo::decodeLongitude(lon)) <= geo::distanceQuerySortKey(radius);
        EXPECT_EQ(brute, relation.matches(lat, lon));
        if (cellRelation == BKDRelation::INSIDE) {
          EXPECT_TRUE(brute);
        }
        if (cellRelation == BKDRelation::OUTSIDE) {
          EXPECT_FALSE(brute);
        }
        double meters = exactMeters(centerLat, centerLon,
                                    geo::decodeLatitude(lat),
                                    geo::decodeLongitude(lon));
        if (std::abs(meters - radius) > 0.2) {
          EXPECT_EQ(meters <= radius, relation.matches(lat, lon));
        }
      }
    }
  }
}

TEST_F(BKDDistanceRelationTest, validatesInputs) {
  EXPECT_THROW(BKDDistanceRelation(91.0, 0.0, 1.0), std::invalid_argument);
  EXPECT_THROW(BKDDistanceRelation(0.0, 181.0, 1.0), std::invalid_argument);
  EXPECT_THROW(BKDDistanceRelation(0.0, 0.0, -1.0), std::invalid_argument);
  EXPECT_THROW(BKDDistanceRelation(
      0.0, 0.0, std::numeric_limits<double>::infinity()),
      std::invalid_argument);
}

class GeoDistanceQueryTest : public LuxirTest {};

TEST_F(GeoDistanceQueryTest, uniformScoreAndBoundsAcrossExecutionArms) {
  CollectionHelper helper;
  setGeoSchema(helper);
  auto writer = helper.getIndexWriter();
  Inverter& inverter = writer->obtainInverter();
  auto& single = inverter.getIndexHandler("geo_single");
  for (int32_t doc = 0; doc < 40; doc++) {
    inverter.startDoc();
    single.index(inverter, (double)doc - 20.0, (double)doc * 2.0 - 40.0);
    inverter.finishDoc();
  }
  writer->releaseInverter(inverter);
  writer->commit();
  auto reader = writer->getIndexReader();
  auto& segment = reader->segments()[0];

  auto check = [](Query::Scorer* scorer) {
    ASSERT_NE(nullptr, scorer);
    EXPECT_FLOAT_EQ(4.0f, scorer->getMaxScore(PostingsReader::END));
    EXPECT_FLOAT_EQ(4.0f,
                    scorer->getMaxScoreForSetup(PostingsReader::END));
    scorer->setMinCompetitiveScore(4.0f);
    ASSERT_NE(PostingsReader::END, scorer->next());
    EXPECT_FLOAT_EQ(4.0f, scorer->score());
  };

  MemPool pool;
  DistanceState state(pool, *reader, "geo_single", 0.0, 0.0, 2'000'000.0,
                      Query::NEED_SCORES, 4.0f);
  auto* supplier = state.weight->scorerSupplier(pool, segment);
  ASSERT_NE(nullptr, supplier);
  check(buildScorerForTests(pool, *supplier, 0));  // sparse verifier
  check(buildScorerForTests(
      pool, *supplier, std::numeric_limits<int64_t>::max()));  // BKD
  check(state.weight->createScanScorerForTests(pool, segment));

  BulkScorer* bulk = supplier->bulkScorer(pool);
  ASSERT_NE(nullptr, bulk);
  ScoreWindow window;
  bulk->scoreNextWindow(window, nullptr, 0, segment.maxDoc(), 4.0f);
  ASSERT_GT(window.size, 0);
  for (int32_t i = 0; i < window.size; i++) {
    EXPECT_FLOAT_EQ(4.0f, window.scores[(size_t)i]);
  }

  MemPool noScorePool;
  DistanceState noScore(noScorePool, *reader, "geo_single", 0.0, 0.0,
                        2'000'000.0);
  auto* unscored = noScore.weight->createScorer(noScorePool, segment);
  ASSERT_NE(nullptr, unscored);
  EXPECT_FLOAT_EQ(0.0f,
                  unscored->getMaxScoreForSetup(PostingsReader::END));
  ASSERT_NE(PostingsReader::END, unscored->next());
  EXPECT_FLOAT_EQ(0.0f, unscored->score());
}

TEST_F(GeoDistanceQueryTest, bkdScanAndColumnOraclesAcrossCorpora) {
  for (Shape shape : {Shape::UNIFORM, Shape::CLUSTERED,
                      Shape::DATELINE, Shape::POLAR}) {
    CollectionHelper helper;
    helper.clear();
    setGeoSchema(helper);
    auto writer = helper.getIndexWriter();
    Inverter& inverter = writer->obtainInverter();
    auto& single = inverter.getIndexHandler("geo_single");
    auto& multi = inverter.getIndexHandler("geo_multi");
    SplitMix64 rng(0x34ab2109 + (uint64_t)shape);
    GeoPoint exact{};
    bool haveExact = false;
    for (int32_t doc = 0; doc < 350; doc++) {
      inverter.startDoc();
      GeoPoint point = corpusPoint(shape, rng);
      if (doc % 7 != 0) {
        single.index(inverter, point.latitude, point.longitude);
        if (!haveExact) {
          exact = {geo::decodeLatitude(geo::encodeLatitude(point.latitude)),
                   geo::decodeLongitude(geo::encodeLongitude(point.longitude))};
          haveExact = true;
        }
      }
      if (doc % 11 != 0) {
        GeoPoint far{-point.latitude, normalizeLon(point.longitude + 180.0)};
        std::array<GeoPoint, 2> values{{point, far}};
        multi.index(inverter, std::span<const GeoPoint>(values));
      }
      inverter.finishDoc();
    }
    writer->releaseInverter(inverter);
    writer->commit();
    auto reader = writer->getIndexReader();
    ASSERT_EQ(1u, reader->segments().size());
    auto& segment = reader->segments()[0];
    GeoPoint center = shapeCenter(shape);
    std::array<std::tuple<double, double, double>, 3> circles{{
        {center.latitude, center.longitude, 300000.0},
        {exact.latitude, exact.longitude, 0.0},
        {0.0, 0.0,
         geo::haversinMeters(std::numeric_limits<double>::max())}}};

    for (auto [lat, lon, radius] : circles) {
      for (std::string_view field : {"geo_single", "geo_multi"}) {
        MemPool pool;
        DistanceState state(pool, *reader, field, lat, lon, radius);
        auto expected = bruteColumn(segment, field, state.query);
        EXPECT_EQ(expected, collect(state.weight->createScorer(pool, segment)));
        EXPECT_EQ(expected, collect(state.weight->createScanScorerForTests(
                                pool, segment)));
        EXPECT_EQ(field == "geo_single" ? (int64_t)expected.size() : -1,
                  state.weight->count(segment));

        SegFieldInfo info = fieldInfo(segment, field);
        BKDReader bkd(segment.postingsReader(), info);
        auto estimate = bkd.estimateIntersect(state.query.makeRelation());
        EXPECT_GE(estimate.upperBound, expected.size());
        auto* supplier = state.weight->scorerSupplier(pool, segment);
        if (!expected.empty()) {
          ASSERT_NE(nullptr, supplier);
          EXPECT_LE(supplier->cost(), 350);
          // leadCost 0 forces the sparse-verify arm through the same oracle.
          EXPECT_EQ(expected, collect(buildScorerForTests(
                                  pool, *supplier, 0)));
        }
      }
    }
  }
}

TEST_F(GeoDistanceQueryTest, scanFallbackAndCountGating) {
  CollectionHelper helper;
  setGeoSchema(helper, false);
  auto writer = helper.getIndexWriter();
  Inverter& inverter = writer->obtainInverter();
  auto& single = inverter.getIndexHandler("geo_single");
  for (int32_t doc = 0; doc < 40; doc++) {
    inverter.startDoc();
    single.index(inverter, (double)doc - 20.0, (double)doc * 4.0 - 80.0);
    inverter.finishDoc();
  }
  writer->releaseInverter(inverter);
  writer->commit();
  auto reader = writer->getIndexReader();
  auto& segment = reader->segments()[0];
  ASSERT_EQ(0, fieldInfo(segment, "geo_single").pointsMetaOff);
  MemPool pool;
  DistanceState state(pool, *reader, "geo_single", 0.0, 0.0, 1000000.0);
  auto expected = bruteColumn(segment, "geo_single", state.query);
  EXPECT_EQ(expected, collect(state.weight->createScorer(pool, segment)));
  EXPECT_EQ(-1, state.weight->count(segment));
}

TEST_F(GeoDistanceQueryTest, countRejectsDeletedSegment) {
  CollectionHelper helper;
  setGeoSchema(helper);
  auto writer = helper.getIndexWriter();
  Inverter& inverter = writer->obtainInverter();
  auto& single = inverter.getIndexHandler("geo_single");
  for (int32_t doc = 0; doc < 20; doc++) {
    inverter.startDoc();
    single.index(inverter, (double)doc, (double)doc);
    inverter.finishDoc();
  }
  inverter.deleteDoc(3);
  writer->releaseInverter(inverter);
  writer->commit();
  auto reader = writer->getIndexReader();
  auto& segment = reader->segments()[0];
  ASSERT_NE(nullptr, segment.liveDocs());
  MemPool pool;
  DistanceState state(pool, *reader, "geo_single", 0.0, 0.0, 1000.0);
  EXPECT_EQ(-1, state.weight->count(segment));
}

TEST_F(GeoDistanceQueryTest, rejectsInvalidArguments) {
  EXPECT_THROW(GeoDistanceQuery("geo", 91.0, 0.0, 1.0),
               std::invalid_argument);
  EXPECT_THROW(GeoDistanceQuery("geo", 0.0, -181.0, 1.0),
               std::invalid_argument);
  EXPECT_THROW(GeoDistanceQuery("geo", 0.0, 0.0, -1.0),
               std::invalid_argument);
  EXPECT_THROW(GeoDistanceQuery(
      "geo", 0.0, 0.0, std::numeric_limits<double>::quiet_NaN()),
      std::invalid_argument);
}
