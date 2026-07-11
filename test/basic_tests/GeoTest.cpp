#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory_resource>
#include <span>
#include <string_view>
#include <vector>

#include "solux/api/build.h"
#include "solux/query/GeoBoxQuery.h"
#include "solux/reader/BKDReader.h"
#include "solux/reader/FieldReader.h"
#include "solux/reader/IntColReader.h"
#include "solux/schema/Schema.h"
#include "solux/util/geo.h"
#include "solux/util/random.h"
#include "test/CollectionHelper.h"
#include "test/SoluxTest.h"

using namespace solux;
using namespace solux::test;

namespace {

struct Box {
  double minLat;
  double maxLat;
  double minLon;
  double maxLon;
};

struct QuantizedPoint {
  double latitude;
  double longitude;
};

double unitDouble(SplitMix64& rng) {
  return (double)(rng() >> 11) * (1.0 / 9007199254740992.0);
}

QuantizedPoint quantize(double latitude, double longitude) {
  return {geo::decodeLatitude(geo::encodeLatitude(latitude)),
          geo::decodeLongitude(geo::encodeLongitude(longitude))};
}

bool oracleMatches(const QuantizedPoint& point, const Box& box) {
  if (point.latitude < box.minLat || point.latitude > box.maxLat) return false;
  if (box.minLon <= box.maxLon) {
    return box.minLon <= point.longitude && point.longitude <= box.maxLon;
  }
  return point.longitude >= box.minLon || point.longitude <= box.maxLon;
}

std::vector<int32_t> oracle(
    const std::vector<std::vector<QuantizedPoint>>& points, const Box& box) {
  std::vector<int32_t> docs;
  for (int32_t doc = 0; doc < (int32_t)points.size(); doc++) {
    if (std::ranges::any_of(points[(size_t)doc], [&](const auto& point) {
          return oracleMatches(point, box);
        })) {
      docs.push_back(doc);
    }
  }
  return docs;
}

struct QueryState {
  Query::Context context;
  GeoBoxQuery query;
  GeoBoxQuery::Weight* weight;

  QueryState(MemPool& pool, IndexReader& reader, std::string_view field,
             const Box& box)
    : context(pool, reader),
      query(field, box.minLat, box.maxLat, box.minLon, box.maxLon),
      weight((GeoBoxQuery::Weight*)query.createWeight(context, 0)) {}
};

std::vector<int32_t> collect(Query::Scorer* scorer, bool twoPhase) {
  std::vector<int32_t> docs;
  if (scorer == nullptr) return docs;
  if (!twoPhase) {
    for (int32_t doc = scorer->next(); doc != PostingsReader::END;
         doc = scorer->next()) {
      docs.push_back(doc);
    }
    return docs;
  }
  EXPECT_TRUE(scorer->hasTwoPhase());
  for (int32_t doc = scorer->approximationNext(); doc != PostingsReader::END;
       doc = scorer->approximationNext()) {
    if (scorer->matches()) docs.push_back(doc);
  }
  return docs;
}

std::vector<int32_t> run(IndexReader& reader, std::string_view field,
                         const Box& box, bool twoPhase) {
  MemPool pool;
  QueryState state(pool, reader, field, box);
  return collect(state.weight->createScorer(pool, reader.segments()[0]),
                 twoPhase);
}

void setGeoSchema(CollectionHelper& helper) {
  std::pmr::monotonic_buffer_resource arena;
  api::SchemaDef def;
  api::FieldDef* fields = api::build::allocArray(def.fields, 2, arena);
  fields[0].name = "geo_single";
  fields[0].field_class = api::FieldDef::FieldClass::GEO_POINT;
  fields[0].index = api::FieldDef::IndexMode::RANGE;
  fields[1].name = "geo_multi";
  fields[1].field_class = api::FieldDef::FieldClass::GEO_POINT;
  fields[1].index = api::FieldDef::IndexMode::RANGE;
  fields[1].multi_valued = true;
  helper.collection().setSchema(
      Schema::fromProto(def, helper.collection().getSchema().get()));
}

SegFieldInfo fieldInfo(IndexReader::Segment& segment, std::string_view field) {
  MemPool pool;
  FieldReader fields(pool, segment.postingsReader());
  if (!fields.seek(field)) throw std::runtime_error("missing geo test field");
  SegFieldInfo info;
  fields.readFieldInfo(info);
  return info;
}

} // namespace

class GeoEncodingTest : public SoluxTest {};

TEST_F(GeoEncodingTest, endpointsZerosValidationAndPacking) {
  EXPECT_EQ(INT32_MIN, geo::encodeLatitude(-90.0));
  EXPECT_EQ(INT32_MAX, geo::encodeLatitude(90.0));
  EXPECT_EQ(INT32_MIN, geo::encodeLatitudeCeil(-90.0));
  EXPECT_EQ(INT32_MAX, geo::encodeLatitudeCeil(90.0));
  EXPECT_EQ(INT32_MIN, geo::encodeLongitude(-180.0));
  EXPECT_EQ(INT32_MAX, geo::encodeLongitude(180.0));
  EXPECT_EQ(INT32_MIN, geo::encodeLongitudeCeil(-180.0));
  EXPECT_EQ(INT32_MAX, geo::encodeLongitudeCeil(180.0));
  EXPECT_EQ(0, geo::encodeLatitude(-0.0));
  EXPECT_EQ(0, geo::encodeLatitude(+0.0));
  EXPECT_EQ(0, geo::encodeLatitudeCeil(-0.0));
  EXPECT_EQ(0, geo::encodeLatitudeCeil(+0.0));
  EXPECT_EQ(0, geo::encodeLongitude(-0.0));
  EXPECT_EQ(0, geo::encodeLongitude(+0.0));
  EXPECT_EQ(0, geo::encodeLongitudeCeil(-0.0));
  EXPECT_EQ(0, geo::encodeLongitudeCeil(+0.0));

  EXPECT_DOUBLE_EQ(-90.0, geo::decodeLatitude(INT32_MIN));
  EXPECT_DOUBLE_EQ(-180.0, geo::decodeLongitude(INT32_MIN));
  EXPECT_LT(geo::decodeLatitude(INT32_MAX), 90.0);
  EXPECT_LT(geo::decodeLongitude(INT32_MAX), 180.0);

  // Ceil saturates for ANY coordinate inside the last quantization cell, not
  // just the exact endpoint (Java (int) narrowing semantics; the unclamped
  // C cast is UB here).
  EXPECT_EQ(INT32_MAX, geo::encodeLatitudeCeil(std::nextafter(90.0, 0.0)));
  EXPECT_EQ(INT32_MAX, geo::encodeLongitudeCeil(std::nextafter(180.0, 0.0)));
  EXPECT_EQ(INT32_MAX, geo::encodeLatitudeCeil(89.9999999999));
  EXPECT_EQ(INT32_MAX, geo::encodeLongitudeCeil(179.9999999999));

  EXPECT_THROW(geo::encodeLatitude(std::numeric_limits<double>::quiet_NaN()),
               std::invalid_argument);
  EXPECT_THROW(geo::encodeLatitude(std::numeric_limits<double>::infinity()),
               std::invalid_argument);
  EXPECT_THROW(geo::encodeLatitude(90.0001), std::invalid_argument);
  EXPECT_THROW(geo::encodeLongitude(-180.0001), std::invalid_argument);

  int64_t packed = geo::pack(-123456789, 987654321);
  EXPECT_EQ(-123456789, geo::unpackLatitude(packed));
  EXPECT_EQ(987654321, geo::unpackLongitude(packed));
}

TEST_F(GeoEncodingTest, quantizationBoundariesAndRandomRoundTrips) {
  double lat = geo::decodeLatitude(123456) + geo::LAT_DECODE * 0.25;
  double lon = geo::decodeLongitude(-654321) + geo::LON_DECODE * 0.25;
  EXPECT_EQ(123456, geo::encodeLatitude(lat));
  EXPECT_EQ(123457, geo::encodeLatitudeCeil(lat));
  EXPECT_EQ(-654321, geo::encodeLongitude(lon));
  EXPECT_EQ(-654320, geo::encodeLongitudeCeil(lon));

  SplitMix64 rng(0x8a771c42);
  for (int32_t i = 0; i < 2000; i++) {
    double rawLat = -90.0 + unitDouble(rng) * 180.0;
    double rawLon = -180.0 + unitDouble(rng) * 360.0;
    int32_t encodedLat = geo::encodeLatitude(rawLat);
    int32_t encodedLon = geo::encodeLongitude(rawLon);
    double decodedLat = geo::decodeLatitude(encodedLat);
    double decodedLon = geo::decodeLongitude(encodedLon);
    EXPECT_LE(decodedLat, rawLat);
    EXPECT_LT(rawLat, geo::decodeLatitude(encodedLat + 1));
    EXPECT_LE(decodedLon, rawLon);
    EXPECT_LT(rawLon, geo::decodeLongitude(encodedLon + 1));
    EXPECT_LT(rawLat - decodedLat, 4.2e-8);
    EXPECT_LT(rawLon - decodedLon, 8.4e-8);
  }
}

class GeoBoxQueryTest : public SoluxTest {};

TEST_F(GeoBoxQueryTest, randomizedQuantizedOracleSingleAndMulti) {
  constexpr int32_t N = 700;
  CollectionHelper helper;
  helper.clear();
  setGeoSchema(helper);

  std::vector<std::vector<QuantizedPoint>> single(N);
  std::vector<std::vector<QuantizedPoint>> multi(N);
  auto writer = helper.getIndexWriter();
  Inverter& inverter = writer->obtainInverter();
  auto& singleHandler = inverter.getIndexHandler("geo_single");
  auto& multiHandler = inverter.getIndexHandler("geo_multi");
  SplitMix64 rng(0x6be920d1);
  const std::array<GeoPoint, 4> clusters = {{{40.7128, -74.0060},
                                             {51.5074, -0.1278},
                                             {35.6762, 139.6503},
                                             {-33.8688, 151.2093}}};

  for (int32_t doc = 0; doc < N; doc++) {
    inverter.startDoc();
    auto makePoint = [&](int32_t salt) {
      if ((doc + salt) % 3 == 0) {
        const auto& center = clusters[(size_t)((doc + salt) % clusters.size())];
        double lat = center.latitude + (unitDouble(rng) - 0.5) * 0.4;
        double lon = center.longitude + (unitDouble(rng) - 0.5) * 0.4;
        return GeoPoint{lat, lon};
      }
      return GeoPoint{-90.0 + unitDouble(rng) * 180.0,
                      -180.0 + unitDouble(rng) * 360.0};
    };

    if (doc % 5 != 0) {
      GeoPoint point = makePoint(0);
      singleHandler.index(inverter, point.latitude, point.longitude);
      single[(size_t)doc].push_back(quantize(point.latitude, point.longitude));
    }

    if (doc % 7 != 0) {
      std::vector<GeoPoint> points;
      int32_t count = 1 + doc % 3;
      for (int32_t i = 0; i < count; i++) points.push_back(makePoint(i + 1));
      multiHandler.index(inverter, std::span<const GeoPoint>(points));
      for (const auto& point : points) {
        multi[(size_t)doc].push_back(quantize(point.latitude, point.longitude));
      }
    }
    inverter.finishDoc();
  }

  writer->releaseInverter(inverter);
  writer->commit();
  auto reader = writer->getIndexReader();
  ASSERT_EQ(1u, reader->segments().size());
  auto& segment = reader->segments()[0];
  SegFieldInfo singleInfo = fieldInfo(segment, "geo_single");
  ASSERT_NE(0, singleInfo.pointsMetaOff);
  BKDReader singleBKD(segment.postingsReader(), singleInfo);
  EXPECT_NO_THROW(singleBKD.validate());
  SegFieldInfo multiInfo = fieldInfo(segment, "geo_multi");
  ASSERT_NE(0, multiInfo.pointsMetaOff);
  BKDReader multiBKD(segment.postingsReader(), multiInfo);
  EXPECT_NO_THROW(multiBKD.validate());
  {
    IntColReader column(segment.postingsReader(), multiInfo);
    IntColReader::Iterator iter(column);
    for (int32_t doc = 0; doc < N; doc++) {
      if (multi[(size_t)doc].empty()) continue;
      ASSERT_EQ(doc, iter.next());
      auto [start, end] = column.getStartEndValueRank(iter.rank());
      ASSERT_EQ((int64_t)multi[(size_t)doc].size(), end - start) << doc;
      for (int64_t rank = start; rank < end; rank++) {
        int64_t packed = iter.values().valueAt(rank);
        const auto& expected = multi[(size_t)doc][(size_t)(rank - start)];
        EXPECT_DOUBLE_EQ(expected.latitude,
                         geo::decodeLatitude(geo::unpackLatitude(packed))) << doc;
        EXPECT_DOUBLE_EQ(expected.longitude,
                         geo::decodeLongitude(geo::unpackLongitude(packed))) << doc;
      }
    }
    EXPECT_EQ(PostingsReader::END, iter.next());
  }

  QuantizedPoint exact = single[1][0];
  const std::array<Box, 9> boxes = {{{40.70, 40.72, -74.02, -73.99},
                                     {25.0, 60.0, -130.0, -55.0},
                                     {-90.0, 90.0, -180.0, 180.0},
                                     {-5.0, 5.0, -180.0, 180.0},
                                     {-90.0, 90.0, -10.0, 10.0},
                                     {-90.0, 90.0, 170.0, -170.0},
                                     {exact.latitude, exact.latitude,
                                      exact.longitude, exact.longitude},
                                     {exact.latitude, exact.latitude,
                                      exact.longitude, 180.0},
                                     {-33.9, -33.8, 151.1, 151.3}}};

  for (size_t boxIndex = 0; boxIndex < boxes.size(); boxIndex++) {
    const auto& box = boxes[boxIndex];
    for (auto [field, values] : {
           std::pair<std::string_view,
                     const std::vector<std::vector<QuantizedPoint>>*>{
               "geo_single", &single},
           {"geo_multi", &multi}}) {
      SCOPED_TRACE(::testing::Message() << "box=" << boxIndex << " field=" << field);
      auto expected = oracle(*values, box);
      EXPECT_EQ(expected, run(*reader, field, box, false));
      EXPECT_EQ(expected, run(*reader, field, box, true));
    }
  }

  MemPool pool;
  QueryState state(pool, *reader, "geo_single", boxes[2]);
  auto* supplier = state.weight->scorerSupplier(pool, segment);
  ASSERT_NE(nullptr, supplier);
  EXPECT_EQ((int64_t)(N - (N + 4) / 5), supplier->cost());
  EXPECT_EQ(-1, state.weight->count(segment));
}

TEST_F(GeoBoxQueryTest, rejectsInvalidBoxesAndTreatsUnrepresentableEdgesAsEmpty) {
  EXPECT_THROW(GeoBoxQuery("geo", -91.0, 0.0, 0.0, 1.0),
               std::invalid_argument);
  EXPECT_THROW(GeoBoxQuery("geo", 10.0, -10.0, 0.0, 1.0),
               std::invalid_argument);
  EXPECT_THROW(GeoBoxQuery("geo", 0.0, 1.0,
                           std::numeric_limits<double>::infinity(), 1.0),
               std::invalid_argument);
  EXPECT_TRUE(GeoBoxQuery("geo", 90.0, 90.0, -180.0, 180.0).isEmpty());
  EXPECT_TRUE(GeoBoxQuery("geo", -90.0, 90.0, 180.0, 180.0).isEmpty());
}

TEST_F(GeoBoxQueryTest, flushWritesBKDButMergeDefersGeoPointsToPassC) {
  CollectionHelper helper;
  helper.clear();
  setGeoSchema(helper);
  auto writer = helper.getIndexWriter();

  for (int32_t segmentNumber = 0; segmentNumber < 2; segmentNumber++) {
    Inverter& inverter = writer->obtainInverter();
    auto& handler = inverter.getIndexHandler("geo_single");
    for (int32_t doc = 0; doc < 20; doc++) {
      inverter.startDoc();
      handler.index(inverter, -40.0 + doc,
                    -170.0 + segmentNumber * 100.0 + doc);
      inverter.finishDoc();
    }
    writer->releaseInverter(inverter);
    writer->commit();
  }

  auto beforeMerge = writer->getIndexReader();
  ASSERT_EQ(2u, beforeMerge->segments().size());
  for (auto& segment : beforeMerge->segments()) {
    SegFieldInfo info = fieldInfo(segment, "geo_single");
    ASSERT_NE(0, info.pointsMetaOff);
    BKDReader bkd(segment.postingsReader(), info);
    EXPECT_NO_THROW(bkd.validate());
  }

  writer->mergeSegments();
  auto afterMerge = writer->getIndexReader();
  ASSERT_EQ(1u, afterMerge->segments().size());
  EXPECT_EQ(0, fieldInfo(afterMerge->segments()[0], "geo_single").pointsMetaOff);
}
