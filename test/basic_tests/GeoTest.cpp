// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <deque>
#include <cmath>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <memory_resource>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

#include "luxir/api/build.h"
#include "luxir/query/GeoBoxQuery.h"
#include "luxir/query/GeoDistanceQuery.h"
#include "luxir/query/GeoPolygonQuery.h"
#include "luxir/query/TermQuery.h"
#include "luxir/query/BooleanQuery.h"
#include "luxir/reader/BKDReader.h"
#include "luxir/reader/PostingsReader.h"
#include "luxir/reader/IntColReader.h"
#include "luxir/server/HttpServer.h"
#include "luxir/util/geo.h"
#include "luxir/util/random.h"
#include "test/CollectionHelper.h"
#include "test/HttpReq.h"
#include "test/QueryBuild.h"
#include "test/SchemaBuilder.h"
#include "test/LuxirTest.h"
#include "test/TestUtils.h"

using namespace luxir;
using namespace luxir::test;

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

// Runs any geo query on the first segment, through the planned arm or the
// column scan.
template <class Q>
std::vector<int32_t> runQuery(IndexReader& reader, Q& query, bool scan,
                              bool twoPhase) {
  MemPool pool;
  Query::Context context(pool, reader);
  auto* weight = (typename Q::Weight*)query.createWeight(context, 0);
  auto& segment = reader.segments()[0];
  return collect(scan ? weight->createScanScorerForTests(pool, segment)
                      : weight->createScorer(pool, segment),
                 twoPhase);
}

using EncodedPoints = std::vector<std::vector<std::pair<int32_t, int32_t>>>;

// Docs with any point the relation's per-point check accepts.
template <class Relation>
std::vector<int32_t> relationOracle(const EncodedPoints& points,
                                    const Relation& relation) {
  std::vector<int32_t> docs;
  for (int32_t doc = 0; doc < (int32_t)points.size(); doc++) {
    if (std::ranges::any_of(points[(size_t)doc], [&](const auto& point) {
          return relation.matches(point.first, point.second);
        })) {
      docs.push_back(doc);
    }
  }
  return docs;
}

void addEncoded(EncodedPoints& points, int32_t doc, double lat, double lon) {
  points[(size_t)doc].push_back({geo::encodeLatitude(lat), geo::encodeLongitude(lon)});
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
  multi.multi = api::FieldDef::MultiMode::DEFAULT;
  b.set(helper.collection());
}

SegFieldInfo fieldInfo(IndexReader::Segment& segment, std::string_view field) {
  MemPool pool;
  auto* fields = segment.postingsReader().fieldInfo(field);
  if (!fields) throw std::runtime_error("missing geo test field");
  const auto& info = *fields;
  return info;
}

template <typename Fill>
void addWireDoc(CollectionHelper::UpdateBuilder& builder, std::string_view id,
                std::string_view field, Fill&& fill) {
  builder.addRaw([=](api::Map& doc, std::pmr::memory_resource& mr) mutable {
    using Pair = std::pair<std::string_view, ::hpp_proto::indirect_view<api::Val>>;
    Pair* fields = api::build::allocArray(doc.fields, 2, mr);
    auto* idVal = (api::Val*)mr.allocate(sizeof(api::Val), alignof(api::Val));
    new (idVal) api::Val();
    idVal->kind = api::build::arenaStr(mr, id);
    auto* fieldVal = (api::Val*)mr.allocate(sizeof(api::Val), alignof(api::Val));
    new (fieldVal) api::Val();
    fill(*fieldVal, mr);
    fields[0] = Pair{api::build::arenaStr(mr, "id"), {idVal}};
    fields[1] = Pair{api::build::arenaStr(mr, field), {fieldVal}};
  });
}

void setDoubles(api::Val& val, std::pmr::memory_resource& mr,
                std::initializer_list<double> values) {
  auto& arr = val.kind.emplace<api::ArrDouble>();
  double* out = api::build::allocArray(arr.v, values.size(), mr);
  std::copy(values.begin(), values.end(), out);
}

void setInts(api::Val& val, std::pmr::memory_resource& mr,
             std::initializer_list<int64_t> values) {
  auto& arr = val.kind.emplace<api::ArrInt>();
  int64_t* out = api::build::allocArray(arr.v, values.size(), mr);
  std::copy(values.begin(), values.end(), out);
}

template <typename Fill>
void setValues(api::Val& val, std::pmr::memory_resource& mr, size_t size,
               Fill&& fill) {
  auto& arr = val.kind.emplace<api::ArrVal>();
  api::Val* out = api::build::allocArray(arr.v, size, mr);
  for (size_t i = 0; i < size; i++) fill(out[i], i, mr);
}

using Ring = std::vector<geo::LatLon>;

// Polygons as vertex lists, kept alive with the pool for the GeoPolygonQuery
// that borrows them.
struct PolygonSet {
  MemPool pool;
  std::deque<Ring> rings;
  std::deque<std::vector<std::span<const geo::LatLon>>> holes;
  std::vector<geo::PolygonRings> polygons;

  PolygonSet& add(Ring outer, std::vector<Ring> holeRings = {}) {
    auto& spans = holes.emplace_back();
    for (auto& ring : holeRings) spans.push_back(rings.emplace_back(std::move(ring)));
    polygons.push_back({rings.emplace_back(std::move(outer)), spans});
    return *this;
  }
};

Ring square(double minLat, double maxLat, double minLon, double maxLon) {
  return {{minLat, minLon}, {maxLat, minLon}, {maxLat, maxLon}, {minLat, maxLon}};
}

} // namespace

class GeoEncodingTest : public LuxirTest {};

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

class GeoBoxQueryTest : public LuxirTest {};

TEST_F(GeoBoxQueryTest, publicWireIngestAndProtoQueryRoundTrip) {
  CollectionHelper helper;
  setGeoSchema(helper);

  CollectionHelper::UpdateBuilder update;
  addWireDoc(update, "ny", "geo_single", [](api::Val& val, auto& mr) {
    setDoubles(val, mr, {-74.0060, 40.7128});
  });
  addWireDoc(update, "order", "geo_single", [](api::Val& val, auto& mr) {
    setInts(val, mr, {10, 50});
  });
  addWireDoc(update, "tokyo", "geo_single", [](api::Val& val, auto& mr) {
    setValues(val, mr, 2, [](api::Val& elem, size_t i, auto&) {
      if (i == 0) elem.kind = 139.6503;
      else elem.kind = 35.6762f;
    });
  });
  addWireDoc(update, "multi", "geo_multi", [](api::Val& val, auto& mr) {
    setValues(val, mr, 3, [](api::Val& point, size_t i, auto& innerMr) {
      if (i == 0) {
        setDoubles(point, innerMr, {179.0, 0.0});
      } else if (i == 1) {
        setInts(point, innerMr, {-179, 1});
      } else {
        setValues(point, innerMr, 2, [](api::Val& elem, size_t j, auto&) {
          if (j == 0) elem.kind = 151.2093;
          else elem.kind = -33.8688f;
        });
      }
    });
  });
  update.commit();
  IndexResult result = helper.submit(update);
  ASSERT_EQ(IndexResult::Status::OK, result.status);

  const std::vector<std::string> ids{"ny", "order", "tokyo", "multi"};
  std::vector<std::vector<QuantizedPoint>> single(4), multi(4);
  single[0].push_back(quantize(40.7128, -74.0060));
  single[1].push_back(quantize(50.0, 10.0));
  single[2].push_back(quantize((double)35.6762f, 139.6503));
  multi[3].push_back(quantize(0.0, 179.0));
  multi[3].push_back(quantize(1.0, -179.0));
  multi[3].push_back(quantize((double)-33.8688f, 151.2093));

  auto queryIds = [&](std::string_view field, const Box& box) {
    auto req = localReq(luxirNode->getSearchEngine());
    auto& cur = req->collection("main").topDocs("q");
    cur.rawQuery() = qb::geoBox(cur.mr(), field, box.minLat, box.maxLat,
                                box.minLon, box.maxLon);
    cur.fields({"id"}).limit(20);
    req->execute();
    EXPECT_OK(req);
    std::vector<std::string> found;
    for (const auto& doc : req->getDocs()) {
      found.push_back(std::get<std::string>(*find(doc, "id")));
    }
    std::sort(found.begin(), found.end());
    return found;
  };
  auto expectedIds = [&](const auto& points, const Box& box) {
    std::vector<std::string> expected;
    for (int32_t doc : oracle(points, box)) expected.push_back(ids[(size_t)doc]);
    std::sort(expected.begin(), expected.end());
    return expected;
  };

  for (const Box& box : {Box{40.0, 41.0, -75.0, -73.0},
                         Box{49.0, 51.0, 9.0, 11.0},
                         Box{35.0, 36.0, 139.0, 140.0}}) {
    EXPECT_EQ(expectedIds(single, box), queryIds("geo_single", box));
  }
  Box dateline{-5.0, 5.0, 170.0, -170.0};
  EXPECT_EQ(expectedIds(multi, dateline), queryIds("geo_multi", dateline));

  // Order proof: VALUE arrays are [lon, lat], while query bounds are named.
  EXPECT_EQ((std::vector<std::string>{"order"}),
            queryIds("geo_single", Box{49.0, 51.0, 9.0, 11.0}));
  EXPECT_TRUE(queryIds("geo_single", Box{9.0, 11.0, 49.0, 51.0}).empty());
}

TEST_F(GeoBoxQueryTest, publicWireRejectsBadPointsWithoutCorruptingLaterDocs) {
  CollectionHelper helper;
  setGeoSchema(helper);

  CollectionHelper::UpdateBuilder update;
  addWireDoc(update, "short", "geo_single", [](api::Val& val, auto& mr) {
    setDoubles(val, mr, {1.0});
  });
  addWireDoc(update, "long", "geo_single", [](api::Val& val, auto& mr) {
    setInts(val, mr, {1, 2, 3, 4});
  });
  addWireDoc(update, "text", "geo_single", [](api::Val& val, auto& mr) {
    setValues(val, mr, 2, [](api::Val& elem, size_t i, auto&) {
      elem.kind = i == 0 ? std::string_view("a") : std::string_view("b");
    });
  });
  addWireDoc(update, "nested", "geo_single", [](api::Val& val, auto& mr) {
    setValues(val, mr, 2, [](api::Val& point, size_t i, auto& innerMr) {
      setDoubles(point, innerMr, {(double)i, (double)i});
    });
  });
  addWireDoc(update, "good", "geo_single", [](api::Val& val, auto& mr) {
    setDoubles(val, mr, {10.0, 50.0});
  });
  update.commit();
  IndexResult result = helper.submit(update);
  ASSERT_EQ(IndexResult::Status::PARTIAL, result.status);
  ASSERT_EQ(4u, result.errors.size());
  for (const auto& error : result.errors) {
    EXPECT_NE(std::string::npos, error.error_message.find("'geo_single'"));
  }
  EXPECT_NE(std::string::npos, result.errors[0].error_message.find("needs 2 coordinates"));
  EXPECT_NE(std::string::npos, result.errors[1].error_message.find("needs 2 coordinates"));
  EXPECT_NE(std::string::npos, result.errors[2].error_message.find("geohash"));
  EXPECT_NE(std::string::npos, result.errors[3].error_message.find("single-valued"));

  auto req = localReq(luxirNode->getSearchEngine());
  auto& cur = req->collection("main").topDocs("q");
  cur.rawQuery() = qb::geoBox(cur.mr(), "geo_single", 49.0, 51.0, 9.0, 11.0);
  cur.fields({"id"}).limit(10);
  req->execute();
  ASSERT_OK(req);
  auto docs = req->getDocs();
  ASSERT_EQ(1u, docs.size());
  EXPECT_EQ("good", std::get<std::string>(*find(docs[0], "id")));
}

TEST_F(GeoBoxQueryTest, randomizedQuantizedOracleSingleAndMulti) {
  constexpr int32_t N = 700;
  CollectionHelper helper;
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
  auto reader = writer->snapshots.readers.getReader();
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

  SkipStats::reset();
  SkipStats::enabled = true;
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
      MemPool countPool;
      QueryState countState(countPool, *reader, field, box);
      EXPECT_EQ(field == "geo_single" ? (int64_t)expected.size() : -1,
                countForTests(countState.weight, segment));
    }
  }
  SkipStats::enabled = false;
  EXPECT_GT(SkipStats::geoBKDArms, 0);
  EXPECT_EQ(0, SkipStats::geoScanArms);

  MemPool pool;
  QueryState state(pool, *reader, "geo_single", boxes[2]);
  auto* supplier = state.weight->scorerSupplier(pool, segment);
  ASSERT_NE(nullptr, supplier);
  EXPECT_EQ((int64_t)(N - (N + 4) / 5), supplier->cost());
  EXPECT_EQ((int64_t)oracle(single, boxes[2]).size(),
            countForTests(state.weight, segment));
  int64_t sparseBefore = SkipStats::geoSparseVerifyArms;
  SkipStats::enabled = true;
  auto* sparsePlan = resolveScorerPlanForTests(pool, *supplier, 0);
  auto* sparse = sparsePlan->build(pool);
  SkipStats::enabled = false;
  ASSERT_NE(nullptr, sparse);
  EXPECT_EQ(Query::ReportedTwoPhase::YES,
            sparsePlan->shape().reportedTwoPhase);
  EXPECT_EQ(sparseBefore + 1, SkipStats::geoSparseVerifyArms);

  QueryState multiState(pool, *reader, "geo_multi", boxes[2]);
  EXPECT_EQ(-1, countForTests(multiState.weight, segment));
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

TEST_F(GeoBoxQueryTest, scanFallbackWithoutPointsUsesDenseColumnPath) {
  CollectionHelper helper;
  setGeoSchema(helper, false);
  auto writer = helper.getIndexWriter();
  Inverter& inverter = writer->obtainInverter();
  auto& handler = inverter.getIndexHandler("geo_single");
  std::vector<std::vector<QuantizedPoint>> points(30);
  for (int32_t doc = 0; doc < 30; doc++) {
    inverter.startDoc();
    double lat = -20.0 + doc;
    double lon = 150.0 + doc;
    handler.index(inverter, lat, lon);
    points[(size_t)doc].push_back(quantize(lat, lon));
    inverter.finishDoc();
  }
  writer->releaseInverter(inverter);
  writer->commit();
  auto reader = writer->snapshots.readers.getReader();
  auto& segment = reader->segments()[0];
  EXPECT_EQ(0, fieldInfo(segment, "geo_single").pointsMetaOff);
  Box box{-10.0, 10.0, 160.0, -170.0};
  SkipStats::reset();
  SkipStats::enabled = true;
  EXPECT_EQ(oracle(points, box), run(*reader, "geo_single", box, false));
  SkipStats::enabled = false;
  EXPECT_EQ(1, SkipStats::geoScanArms);
}

// A 64 x 64 grid of single points, latitude -80 + 2.5 * (doc % 64), with a
// body of terms t3, t5 and t7 on every third, fifth and seventh doc. Calls
// `check` with the index reader.
template <typename Check>
void withGrid(bool range, Check&& check) {
  CollectionHelper helper;
  setGeoSchema(helper, range);
  auto writer = helper.getIndexWriter();
  Inverter& inverter = writer->obtainInverter();
  auto& handler = inverter.getIndexHandler("geo_single");
  auto& body = inverter.getIndexHandler("body_w");
  for (int32_t doc = 0; doc < 4096; doc++) {
    inverter.startDoc();
    handler.index(inverter, -80.0 + 2.5 * (doc % 64), -170.0 + 5.0 * (doc / 64));
    std::string text = "pad";
    for (int32_t k : {3, 5, 7}) if (doc % k == 0) text += " t" + std::to_string(k);
    body.index(inverter, std::string_view(text));
    inverter.finishDoc();
  }
  writer->releaseInverter(inverter);
  writer->commit();
  auto reader = writer->snapshots.readers.getReader();
  check(*reader);
}

// The southern half of the grid (2,112 docs): its supplier and a probe that
// resolves it for a pull consumer's expected candidates and spared work,
// returning whether the plan verifies (two-phase).
template <typename Check>
void withGridBox(bool range, Check&& check) {
  withGrid(range, [&](IndexReader& reader) {
    constexpr int32_t N = 4096;
    MemPool pool;
    QueryState state(pool, reader, "geo_single", Box{-80.0, 0.0, -180.0, 180.0});
    auto* supplier = state.weight->scorerSupplier(pool, reader.segments()[0]);
    ASSERT_NE(nullptr, supplier);
    auto verifies = [&](double candidates, double spared) {
      Query::Demand demand = Query::Demand::fromLeadCost(N);
      demand.expectedCandidates = candidates;
      demand.sparedWork = spared;
      auto* plan = supplier->resolve(pool, supplier->makePlanContext(demand));
      return plan->shape().reportedTwoPhase == Query::ReportedTwoPhase::YES;
    };
    check(*supplier, verifies, pool, N);
  });
}

// With the candidates a pull consumer expects to reach it, or an exposure's
// candidates, the box verifies or produces its BKD by point emissions:
// candidates times the verify price plus the consumer's spared work, against
// materializing about 2,100 matches into a bitset. Otherwise it verifies for
// a lead sparser than its matches.
TEST_F(GeoBoxQueryTest, armChoicePricesVerificationAgainstBKDProduction) {
  withGridBox(true, [](Query::ScorerSupplier& supplier, auto& verifies, MemPool& pool,
                       int32_t n) {
    EXPECT_TRUE(verifies(10, 0));
    EXPECT_FALSE(verifies(1000, 0));
    EXPECT_FALSE(verifies(10, 10000));
    auto verifiesFor = [&](int64_t lead, bool exposure) {
      auto context = supplier.makePlanContext(Query::Demand::fromCandidatesAndSpan(
          lead, n, exposure ? Query::ExecutionUse::MATCH_WINDOWS : Query::ExecutionUse::PULL));
      if (exposure) context.expectedSpan = n;
      auto* plan = supplier.resolve(pool, context);
      return plan->shape().reportedTwoPhase == Query::ReportedTwoPhase::YES;
    };
    EXPECT_TRUE(verifiesFor(1000, false));
    EXPECT_FALSE(verifiesFor(3000, false));
    // Filling windows over an exposure, the box prices that exposure's
    // candidates: verifying 1,000 costs more than producing.
    EXPECT_FALSE(verifiesFor(1000, true));
  });
}

// Without a BKD both arms verify; the scan iterates the column once the
// consumer reaches every document with a value.
TEST_F(GeoBoxQueryTest, armChoiceWithoutPointsScansAFullReach) {
  withGridBox(false, [](Query::ScorerSupplier& supplier, auto& verifies, MemPool& pool,
                        int32_t n) {
    EXPECT_TRUE(verifies(10, 0));
    SkipStats::reset();
    SkipStats::enabled = true;
    supplier.resolve(pool, supplier.makePlanContext(Query::Demand::fromLeadCost(n)))
        ->build(pool);
    SkipStats::enabled = false;
    EXPECT_EQ(1, SkipStats::geoScanArms);
  });
}

// A box holding 3,392 docs under a 3-term union of about 2,800 candidates: the
// union's cost is below the box's matches, but verifying every union candidate
// costs far more than producing the box, so the conjunction builds the BKD,
// and describing the Boolean reports the same (no verification).
TEST_F(GeoBoxQueryTest, boxUnderAUnionProducesItsBKD) {
  withGrid(true, [](IndexReader& reader) {
    constexpr int32_t N = 4096;
    MemPool pool;
    Query::Context context(pool, reader);
    GeoBoxQuery box("geo_single", -90.0, 51.0, -180.0, 180.0);
    TermQuery t3("body_w", "t3"), t5("body_w", "t5"), t7("body_w", "t7");
    std::array<Query*, 3> optional{&t3, &t5, &t7};
    std::array<Query*, 1> filters{&box};
    BooleanQuery query({}, optional, {}, filters, 1);
    auto* supplier = query.createWeight(context, 0)->scorerSupplier(pool, reader.segments()[0]);
    ASSERT_NE(nullptr, supplier);
    EXPECT_EQ(Query::VerificationWork::ABSENT,
              supplier->verificationWork(supplier->makePlanContext(Query::Demand::fromLeadCost(N))));
    SkipStats::reset();
    SkipStats::enabled = true;
    auto* scorer = resolveScorerPlanForTests(pool, *supplier, N)->build(pool);
    SkipStats::enabled = false;
    EXPECT_EQ(1, SkipStats::geoBKDArms);
    EXPECT_EQ(0, SkipStats::geoSparseVerifyArms);
    std::vector<int32_t> docs, expected;
    for (int32_t doc = scorer->next(); doc != PostingsReader::END; doc = scorer->next()) docs.push_back(doc);
    for (int32_t doc = 0; doc < N; doc++) {
      if (doc % 64 <= 52 && (doc % 3 == 0 || doc % 5 == 0 || doc % 7 == 0)) expected.push_back(doc);
    }
    EXPECT_EQ(expected, docs);
  });
}

TEST_F(GeoBoxQueryTest, mergeRebuildsSingleAndMultiBKDWithDeletes) {
  CollectionHelper helper;
  setGeoSchema(helper);
  auto writer = helper.getIndexWriter();

  for (int32_t segmentNumber = 0; segmentNumber < 2; segmentNumber++) {
    Inverter& inverter = writer->obtainInverter();
    auto& singleHandler = inverter.getIndexHandler("geo_single");
    auto& multiHandler = inverter.getIndexHandler("geo_multi");
    for (int32_t doc = 0; doc < 20; doc++) {
      inverter.startDoc();
      double lat = -40.0 + segmentNumber * 35.0 + doc;
      double lon = -170.0 + segmentNumber * 300.0 + doc;
      singleHandler.index(inverter, lat, lon);
      std::array<GeoPoint, 2> values{{{lat, lon}, {-lat, -lon}}};
      multiHandler.index(inverter, std::span<const GeoPoint>(values));
      inverter.finishDoc();
    }
    if (segmentNumber == 1) inverter.deleteDoc(5);
    writer->releaseInverter(inverter);
    writer->commit();
  }

  auto beforeMerge = writer->snapshots.readers.getReader();
  ASSERT_EQ(2u, beforeMerge->segments().size());
  for (auto& segment : beforeMerge->segments()) {
    for (std::string_view field : {"geo_single", "geo_multi"}) {
      SegFieldInfo info = fieldInfo(segment, field);
      ASSERT_NE(0, info.pointsMetaOff);
      BKDReader bkd(segment.postingsReader(), info);
      EXPECT_NO_THROW(bkd.validate());
    }
  }

  Box all{-90.0, 90.0, -180.0, 180.0};
  MemPool deletedCountPool;
  QueryState deletedCountState(deletedCountPool, *beforeMerge,
                               "geo_single", all);
  EXPECT_EQ(-1, countForTests(deletedCountState.weight, beforeMerge->segments()[1]));

  writer->mergeSegments();
  auto afterMerge = writer->snapshots.readers.getReader();
  ASSERT_EQ(1u, afterMerge->segments().size());
  auto& merged = afterMerge->segments()[0];
  for (std::string_view field : {"geo_single", "geo_multi"}) {
    SegFieldInfo info = fieldInfo(merged, field);
    ASSERT_NE(0, info.pointsMetaOff);
    BKDReader bkd(merged.postingsReader(), info);
    EXPECT_NO_THROW(bkd.validate());

    std::vector<BKDReader::Point> expected;
    IntColReader column(merged.postingsReader(), info);
    IntColReader::Iterator iter(column);
    for (int32_t doc = iter.next(); doc != PostingsReader::END;
         doc = iter.next()) {
      if (!column.multiValued()) {
        expected.push_back({geo::unpackLatitude(iter.value()),
                            geo::unpackLongitude(iter.value()), doc});
      } else {
        auto [start, end] = column.getStartEndValueRank(iter.rank());
        for (int64_t rank = start; rank < end; rank++) {
          int64_t packed = iter.values().valueAt(rank);
          expected.push_back({geo::unpackLatitude(packed),
                              geo::unpackLongitude(packed), doc});
        }
      }
    }
    auto actual = bkd.readAll();
    std::vector<std::vector<QuantizedPoint>> surviving(
        (size_t)merged.maxDoc());
    for (const auto& point : expected) {
      surviving[(size_t)point.docid].push_back(
          {geo::decodeLatitude(point.lat), geo::decodeLongitude(point.lon)});
    }
    const std::array<Box, 4> boxes = {{{-90.0, 90.0, -180.0, 180.0},
                                       {-30.0, 5.0, 160.0, -160.0},
                                       {-10.0, 10.0, 120.0, 170.0},
                                       {20.0, 45.0, -180.0, -120.0}}};
    for (const Box& box : boxes) {
      EXPECT_EQ(oracle(surviving, box), run(*afterMerge, field, box, false));
    }
    auto byPoint = [](const auto& left, const auto& right) {
      return std::tie(left.lat, left.lon, left.docid)
           < std::tie(right.lat, right.lon, right.docid);
    };
    std::sort(expected.begin(), expected.end(), byPoint);
    std::sort(actual.begin(), actual.end(), byPoint);
    EXPECT_EQ(expected, actual);
  }

  MemPool countPool;
  QueryState singleCountState(countPool, *afterMerge, "geo_single", all);
  EXPECT_EQ(39, countForTests(singleCountState.weight, merged));
  QueryState multiCountState(countPool, *afterMerge, "geo_multi", all);
  EXPECT_EQ(-1, countForTests(multiCountState.weight, merged));
}

// The column scan arm agrees with the per-point check for every geo relation
// over clustered points.
TEST_F(GeoBoxQueryTest, scanAgreesWithPointChecks) {
  constexpr int32_t N = 4 * 512 + 100;
  CollectionHelper helper;
  setGeoSchema(helper, false);
  auto writer = helper.getIndexWriter();
  Inverter& inverter = writer->obtainInverter();
  auto& handler = inverter.getIndexHandler("geo_single");
  const std::array<GeoPoint, 5> clusters = {{{40.7, -74.0}, {51.5, -0.1},
                                             {35.7, 139.7}, {-33.9, 151.2},
                                             {0.0, 179.9}}};
  EncodedPoints points(N);
  SplitMix64 rng(0x2b0e5a17);
  for (int32_t doc = 0; doc < N; doc++) {
    const auto& center = clusters[(size_t)(doc / 512)];
    double lat = center.latitude + (unitDouble(rng) - 0.5) * 0.2;
    double lon = std::min(180.0, center.longitude + (unitDouble(rng) - 0.5) * 0.2);
    inverter.startDoc();
    handler.index(inverter, lat, lon);
    addEncoded(points, doc, lat, lon);
    inverter.finishDoc();
  }
  writer->releaseInverter(inverter);
  writer->commit();
  auto reader = writer->snapshots.readers.getReader();

  auto check = [&](auto& query) {
    auto expected = relationOracle(points, query.makeRelation());
    EXPECT_EQ(expected, runQuery(*reader, query, true, false));
    EXPECT_EQ(expected, runQuery(*reader, query, true, true));
    return expected.size();
  };
  for (const Box& box : {Box{40.0, 41.0, -75.0, -73.0}, Box{40.7, 51.5, -74.0, 0.0},
                         Box{-1.0, 1.0, 179.85, -179.0}, Box{-90.0, 90.0, -180.0, 180.0}}) {
    GeoBoxQuery query("geo_single", box.minLat, box.maxLat, box.minLon, box.maxLon);
    EXPECT_GT(check(query), 0u);
  }
  for (double radius : {3000.0, 8000.0, 6000000.0}) {
    GeoDistanceQuery query("geo_single", 40.7, -74.0, radius);
    EXPECT_GT(check(query), 0u);
  }
  PolygonSet set;
  set.add(square(35.0, 36.0, 139.0, 140.0),
          {{{35.65, 139.65}, {35.75, 139.65}, {35.7, 139.75}}});
  set.add(square(51.45, 51.5, -0.15, -0.1));
  GeoPolygonQuery polygon(set.pool, "geo_single", set.polygons);
  EXPECT_GT(check(polygon), 0u);
  EXPECT_LT(check(polygon), 1024u);
}

class GeoPolygonQueryTest : public LuxirTest {};

TEST_F(GeoPolygonQueryTest, containmentHolesAndValidation) {
  // A concave "C" around New York with a triangular hole in its top bar;
  // the ring repeats its first vertex, which the query drops.
  PolygonSet set;
  set.add({{40.0, -75.0}, {41.5, -75.0}, {41.5, -73.0}, {41.0, -73.0}, {41.0, -74.5},
           {40.5, -74.5}, {40.5, -73.0}, {40.0, -73.0}, {40.0, -75.0}},
          {{{41.1, -74.9}, {41.4, -74.9}, {41.4, -74.6}}});
  GeoPolygonQuery query(set.pool, "geo", set.polygons);
  EXPECT_EQ(8u, query.getPolygons()[0].outer.size());
  const BKDPolygonRelation& relation = query.makeRelation();
  auto matches = [](const BKDPolygonRelation& r, double lat, double lon) {
    return r.matches(geo::encodeLatitude(lat), geo::encodeLongitude(lon));
  };
  EXPECT_TRUE(matches(relation, 40.25, -74.0));   // bottom bar
  EXPECT_TRUE(matches(relation, 41.25, -73.5));   // top bar
  EXPECT_TRUE(matches(relation, 40.75, -74.75));  // left bar
  EXPECT_FALSE(matches(relation, 40.75, -73.5));  // the notch
  EXPECT_FALSE(matches(relation, 41.35, -74.85)); // the hole
  EXPECT_FALSE(matches(relation, 42.0, -74.0));

  auto compare = [](const BKDPolygonRelation& r, double minLat, double maxLat,
                    double minLon, double maxLon) {
    return r.compare(geo::encodeLatitude(minLat), geo::encodeLatitude(maxLat),
                     geo::encodeLongitude(minLon), geo::encodeLongitude(maxLon));
  };
  EXPECT_EQ(BKDRelation::INSIDE, compare(relation, 40.1, 40.4, -74.9, -73.1));
  EXPECT_EQ(BKDRelation::OUTSIDE, compare(relation, 40.6, 40.9, -74.4, -73.1));
  EXPECT_EQ(BKDRelation::OUTSIDE, compare(relation, 10.0, 20.0, 10.0, 20.0));
  EXPECT_EQ(BKDRelation::CROSSES, compare(relation, 40.4, 40.6, -74.0, -73.5));
  EXPECT_EQ(BKDRelation::CROSSES, compare(relation, 41.2, 41.3, -74.85, -74.7));

  // A union: the second square covers part of the first one's hole, which
  // applies to its own polygon only.
  PolygonSet two;
  two.add(square(0.0, 10.0, 0.0, 10.0), {square(4.0, 6.0, 4.0, 6.0)});
  two.add(square(5.0, 7.0, 5.0, 7.0));
  GeoPolygonQuery both(two.pool, "geo", two.polygons);
  const BKDPolygonRelation& unionRelation = both.makeRelation();
  EXPECT_TRUE(matches(unionRelation, 8.0, 8.0));
  EXPECT_TRUE(matches(unionRelation, 5.5, 5.5));
  EXPECT_FALSE(matches(unionRelation, 4.5, 4.5));
  EXPECT_FALSE(matches(unionRelation, 20.0, 20.0));
  EXPECT_EQ(BKDRelation::INSIDE, compare(unionRelation, 5.2, 5.8, 5.2, 5.8));
  EXPECT_EQ(BKDRelation::OUTSIDE, compare(unionRelation, 4.2, 4.8, 4.2, 4.8));
  EXPECT_EQ(BKDRelation::CROSSES, compare(unionRelation, 4.5, 5.5, 4.5, 5.5));

  PolygonSet same;
  same.add(square(0.0, 10.0, 0.0, 10.0), {square(4.0, 6.0, 4.0, 6.0)});
  same.add(square(5.0, 7.0, 5.0, 7.0));
  GeoPolygonQuery equal(same.pool, "geo", same.polygons);
  EXPECT_TRUE(both.equals(equal));
  EXPECT_EQ(both.hash(), equal.hash());
  EXPECT_FALSE(both.equals(query));

  auto rejects = [](Ring outer, std::vector<Ring> holes = {}) {
    PolygonSet bad;
    bad.add(std::move(outer), std::move(holes));
    EXPECT_THROW(GeoPolygonQuery(bad.pool, "geo", bad.polygons), std::invalid_argument);
  };
  rejects({{0.0, 0.0}, {1.0, 0.0}});
  rejects({{0.0, 0.0}, {0.0, 0.0}, {0.0, 0.0}});
  rejects({{0.0, 0.0}, {1.0, 0.0}, {0.0, 0.0}});
  rejects({{0.0, 0.0}, {1.0, 0.0}, {0.0, 0.0}, {1.0, 0.0}});
  rejects({{0.0, 0.0}, {91.0, 0.0}, {0.0, 1.0}});
  rejects({{0.0, 0.0}, {std::numeric_limits<double>::quiet_NaN(), 0.0}, {0.0, 1.0}});
  rejects(square(0.0, 1.0, 0.0, 1.0), {{{0.5, 0.5}, {0.5, 0.5}, {0.6, 0.6}}});
  MemPool pool;
  EXPECT_THROW(GeoPolygonQuery(pool, "geo", {}), std::invalid_argument);
}

TEST_F(GeoPolygonQueryTest, bkdAndScanAgreeWithPointChecks) {
  constexpr int32_t N = 1500;
  CollectionHelper helper;
  setGeoSchema(helper);
  auto writer = helper.getIndexWriter();
  Inverter& inverter = writer->obtainInverter();
  auto& singleHandler = inverter.getIndexHandler("geo_single");
  auto& multiHandler = inverter.getIndexHandler("geo_multi");
  EncodedPoints single(N), multi(N);
  SplitMix64 rng(0x70f1a6b3);
  auto makePoint = [&](int32_t salt) {
    if (salt % 2 == 0) {
      return GeoPoint{40.7 + (unitDouble(rng) - 0.5) * 2.0,
                      -74.0 + (unitDouble(rng) - 0.5) * 2.0};
    }
    return GeoPoint{-90.0 + unitDouble(rng) * 180.0, -180.0 + unitDouble(rng) * 360.0};
  };
  for (int32_t doc = 0; doc < N; doc++) {
    inverter.startDoc();
    GeoPoint point = makePoint(doc);
    singleHandler.index(inverter, point.latitude, point.longitude);
    addEncoded(single, doc, point.latitude, point.longitude);
    std::array<GeoPoint, 2> values{{makePoint(doc + 1), makePoint(doc)}};
    multiHandler.index(inverter, std::span<const GeoPoint>(values));
    for (const auto& value : values) addEncoded(multi, doc, value.latitude, value.longitude);
    inverter.finishDoc();
  }
  writer->releaseInverter(inverter);
  writer->commit();
  auto reader = writer->snapshots.readers.getReader();

  PolygonSet withHole;
  withHole.add({{40.0, -75.0}, {41.5, -75.0}, {41.5, -73.0}, {41.0, -73.0},
                {41.0, -74.5}, {40.5, -74.5}, {40.5, -73.0}, {40.0, -73.0}},
               {{{41.1, -74.9}, {41.4, -74.9}, {41.4, -74.6}}});
  PolygonSet world;
  world.add({{-60.0, -170.0}, {70.0, -150.0}, {80.0, 20.0}, {10.0, 175.0}, {-70.0, 60.0}});
  PolygonSet several;
  several.add(square(40.2, 40.9, -74.6, -73.3), {square(40.4, 40.7, -74.2, -73.8)});
  several.add(square(40.5, 41.2, -74.0, -73.0));
  several.add(square(-10.0, 30.0, 100.0, 140.0));
  for (std::string_view field : {"geo_single", "geo_multi"}) {
    const EncodedPoints& points = field == "geo_single" ? single : multi;
    GeoPolygonQuery concave(withHole.pool, field, withHole.polygons);
    GeoPolygonQuery large(world.pool, field, world.polygons);
    GeoPolygonQuery parts(several.pool, field, several.polygons);
    for (GeoPolygonQuery* query : {&concave, &large, &parts}) {
      auto expected = relationOracle(points, query->makeRelation());
      EXPECT_FALSE(expected.empty()) << field;
      EXPECT_EQ(expected, runQuery(*reader, *query, false, false)) << field;
      EXPECT_EQ(expected, runQuery(*reader, *query, true, true)) << field;
    }
  }
}
// The public request path: a geo_polygon query over each cardinality.
TEST_F(GeoPolygonQueryTest, protoQuerySelectsDocsInside) {
  CollectionHelper helper;
  setGeoSchema(helper);
  CollectionHelper::UpdateBuilder update;
  addWireDoc(update, "ny", "geo_single", [](api::Val& val, auto& mr) {
    setDoubles(val, mr, {-74.0060, 40.7128});
  });
  addWireDoc(update, "london", "geo_single", [](api::Val& val, auto& mr) {
    setDoubles(val, mr, {-0.1278, 51.5074});
  });
  addWireDoc(update, "tour", "geo_multi", [](api::Val& val, auto& mr) {
    setValues(val, mr, 2, [](api::Val& point, size_t i, auto& innerMr) {
      if (i == 0) setDoubles(point, innerMr, {-74.0, 40.7});
      else setDoubles(point, innerMr, {151.2093, -33.8688});
    });
  });
  update.commit();
  ASSERT_EQ(IndexResult::Status::OK, helper.submit(update).status);

  auto search = [&](std::string_view field) {
    auto req = localReq(luxirNode->getSearchEngine());
    auto& cur = req->collection("main").topDocs("q");
    qb::GeoPolygonRings box{{{40.0, -75.0}, {41.0, -75.0}, {41.0, -73.0}, {40.0, -73.0}}};
    cur.rawQuery() = qb::geoPolygon(cur.mr(), field, {box});
    cur.fields({"id"}).limit(10);
    req->execute();
    EXPECT_OK(req);
    return req->getDocs();
  };
  auto single = search("geo_single");
  ASSERT_EQ(1u, single.size());
  EXPECT_EQ("ny", std::get<std::string>(*find(single[0], "id")));
  auto multi = search("geo_multi");
  ASSERT_EQ(1u, multi.size());
  EXPECT_EQ("tour", std::get<std::string>(*find(multi[0], "id")));
}

// The HTTP JSON form: named vertices, a union of polygons whose holes cut
// only their own polygon, and malformed polygons rejected as bad requests.
TEST_F(GeoPolygonQueryTest, jsonQueryMatchesUnion) {
  CollectionHelper helper;
  setGeoSchema(helper);
  HttpServer server(*luxirNode, 1, 0);
  server.start();
  auto post = [&](std::string_view path, const std::string& body) {
    return httpRequest(server.getPort(), http::verb::post, path, body);
  };
  auto update = post("/collections/main/_update", R"({"docs":[
      {"id":"ny","geo_single":[-74.006,40.7128]},
      {"id":"london","geo_single":[-0.1278,51.5074]},
      {"id":"sydney","geo_single":[151.2093,-33.8688]}],"commit":{}})");
  ASSERT_EQ(200, update.result_int()) << update.body();
  auto search = [&](const std::string& polygons) {
    return post("/collections/main/_search",
                R"({"query":{"geo_polygon":{"field":"geo_single","polygons":)" + polygons
                    + R"(}},"fields":["id"],"sort":"id asc"})");
  };
  auto ids = [&](const std::string& polygons) {
    auto res = search(polygons);
    EXPECT_EQ(200, res.result_int()) << res.body();
    glz::generic_i64 root;
    EXPECT_FALSE(glz::read_json(root, res.body())) << res.body();
    std::vector<std::string> out;
    for (const auto& doc : root["docs"].get_array()) out.push_back(doc["id"].get_string());
    return out;
  };
  const std::string ny =
      R"({"points":[{"lat":40,"lon":-75},{"lat":41,"lon":-75},{"lat":41,"lon":-73},{"lat":40,"lon":-73}]})";
  const std::string london =
      R"("points":[{"lat":51,"lon":-1},{"lat":52,"lon":-1},{"lat":52,"lon":1},{"lat":51,"lon":1}])";
  const std::string aroundLondon =
      R"("holes":[{"points":[{"lat":51.4,"lon":-0.2},{"lat":51.6,"lon":-0.2},{"lat":51.6,"lon":0},{"lat":51.4,"lon":0}]}])";
  EXPECT_EQ((std::vector<std::string>{"london", "ny"}), ids("[" + ny + ",{" + london + "}]"));
  EXPECT_EQ((std::vector<std::string>{"ny"}),
            ids("[" + ny + ",{" + london + "," + aroundLondon + "}]"));

  auto repeated = search(R"([{"points":[{"lat":1,"lon":1},{"lat":1,"lon":1},{"lat":1,"lon":1}]}])");
  EXPECT_EQ(400, repeated.result_int()) << repeated.body();
  EXPECT_NE(std::string::npos, repeated.body().find("3 distinct vertices")) << repeated.body();
  auto none = search("[]");
  EXPECT_EQ(400, none.result_int()) << none.body();
  EXPECT_NE(std::string::npos, none.body().find("at least one polygon")) << none.body();
  server.shutdown();
}

class GeoProjectionTest : public LuxirTest {};

// A projected geo field has the ingest shape its cardinality reads: [lon, lat]
// for a single-valued field, [[lon, lat], ...] for a multi-valued one even
// with one point, and no value when missing. Indexing what a search returns,
// as gRPC Vals in either document format or as HTTP JSON, gives back the same
// quantized points.
TEST_F(GeoProjectionTest, projectedPointsIndexBackUnchanged) {
  struct Input {
    std::string id;
    std::string field;
    std::vector<GeoPoint> points;
  };
  const std::vector<Input> inputs = {
      {"single", "geo_single", {{40.712776123456, -74.005974321987}}},
      {"corner", "geo_single", {{-90.0, 180.0}}},
      {"one", "geo_multi", {{-33.8688197, 151.2092955}}},
      {"many", "geo_multi",
       {{51.5072178, -0.1275862}, {35.6764225, 139.6500231}, {51.5072178, -0.1275862}}},
  };
  auto otherField = [](const Input& input) {
    return input.field == "geo_single" ? "geo_multi" : "geo_single";
  };
  CollectionHelper helper;
  setGeoSchema(helper);
  CollectionHelper::UpdateBuilder originals;
  for (const auto& input : inputs) {
    addWireDoc(originals, input.id, input.field, [&input](api::Val& val, auto& mr) {
      auto lonLat = [](api::Val& out, auto& outMr, const GeoPoint& p) {
        setDoubles(out, outMr, {p.longitude, p.latitude});
      };
      if (input.field == "geo_single") return lonLat(val, mr, input.points[0]);
      setValues(val, mr, input.points.size(), [&](api::Val& point, size_t i, auto& innerMr) {
        lonLat(point, innerMr, input.points[i]);
      });
    });
  }
  originals.commit();
  ASSERT_EQ(IndexResult::Status::OK, helper.submit(originals).status);
  auto original = [&](std::string_view id) {
    return std::ranges::find(inputs, id, &Input::id) != inputs.end();
  };

  // gRPC: the returned Val of each original, from row maps and from columns.
  for (auto [format, suffix] : {std::pair{api::DocFormat::ROWS, "-rows"},
                                std::pair{api::DocFormat::COLUMNS, "-cols"}}) {
    auto req = localReq(luxirNode->getSearchEngine());
    req->collection("main").topDocs("q").allQuery()
        .fields({"id", "geo_single", "geo_multi"}).documentFormat(format).limit(100);
    req->execute();
    ASSERT_OK(req);
    const api::DocList& list = *req->docList();
    auto docs = req->getDocs();
    auto rowVal = [&](size_t row, std::string_view field) -> const api::Val* {
      for (const auto& [name, column] : list.columns) {
        if (name != field) continue;
        const auto& slot = std::get<api::ColVal>(column.kind).v[row];
        return slot.isNull() ? nullptr : &slot;
      }
      if (row >= list.docs.size()) return nullptr;
      for (const auto& [name, val] : list.docs[row].fields) {
        if (name == field) return &*val;
      }
      return nullptr;
    };
    CollectionHelper::UpdateBuilder copies;
    for (size_t row = 0; row < docs.size(); row++) {
      auto id = std::get<std::string>(*find(docs[row], "id"));
      if (!original(id)) continue;
      const Input& input = *std::ranges::find(inputs, id, &Input::id);
      const api::Val* val = rowVal(row, input.field);
      ASSERT_NE(nullptr, val) << id;
      EXPECT_EQ(nullptr, rowVal(row, otherField(input))) << id;
      EXPECT_EQ(input.field == "geo_multi", std::holds_alternative<api::ArrVal>(val->kind)) << id;
      addWireDoc(copies, id + suffix, input.field, [val](api::Val& out, auto&) { out = *val; });
    }
    copies.commit();
    ASSERT_EQ(IndexResult::Status::OK, helper.submit(copies).status);
  }

  // HTTP JSON: wildcard selection includes geo fields; post each value back.
  HttpServer server(*luxirNode, 1, 0);
  server.start();
  auto found = httpRequest(server.getPort(), http::verb::post, "/collections/main/_search",
                           R"({"query":"*:*","fields":["*"],"limit":100})");
  ASSERT_EQ(200, found.result_int()) << found.body();
  glz::generic_i64 root;
  ASSERT_FALSE(glz::read_json(root, found.body())) << found.body();
  std::string body = R"({"docs":[)";
  for (const auto& doc : root["docs"].get_array()) {
    const auto& id = doc["id"].get_string();
    if (!original(id)) continue;
    const Input& input = *std::ranges::find(inputs, id, &Input::id);
    ASSERT_TRUE(doc.contains(input.field)) << id;
    EXPECT_TRUE(!doc.contains(otherField(input)) || doc[otherField(input)].is_null()) << id;
    const auto& value = doc[input.field];
    EXPECT_EQ(input.field == "geo_multi", value[0].is_array()) << id;
    if (body.back() != '[') body += ',';
    body += R"({"id":")" + id + R"(-json",")" + input.field + R"(":)"
        + glz::write_json(value).value() + "}";
  }
  body += R"(],"commit":{}})";
  auto posted = httpRequest(server.getPort(), http::verb::post, "/collections/main/_update", body);
  ASSERT_EQ(200, posted.result_int()) << posted.body();
  server.shutdown();

  auto quantized = [](const FieldVal& val) {
    std::vector<std::vector<double>> points;
    if (const auto* one = std::get_if<std::vector<double>>(&val)) points.push_back(*one);
    else points = std::get<std::vector<std::vector<double>>>(val);
    std::vector<std::pair<int32_t, int32_t>> out;
    for (const auto& point : points) {
      EXPECT_EQ(2u, point.size());
      out.push_back({geo::encodeLatitude(point[1]), geo::encodeLongitude(point[0])});
    }
    return out;
  };
  auto req = localReq(luxirNode->getSearchEngine());
  req->collection("main").topDocs("q").allQuery()
      .fields({"id", "geo_single", "geo_multi"}).limit(100);
  req->execute();
  ASSERT_OK(req);
  auto docs = req->getDocs();
  EXPECT_EQ(4 * inputs.size(), docs.size());
  for (const auto& input : inputs) {
    std::vector<std::pair<int32_t, int32_t>> expected;
    for (const auto& p : input.points) {
      expected.push_back({geo::encodeLatitude(p.latitude), geo::encodeLongitude(p.longitude)});
    }
    for (std::string suffix : {"", "-rows", "-cols", "-json"}) {
      std::string id = input.id + suffix;
      auto doc = std::ranges::find_if(docs, [&](const Doc& d) {
        return std::get<std::string>(*find(d, "id")) == id;
      });
      ASSERT_NE(docs.end(), doc) << id;
      const FieldVal* val = find(*doc, input.field);
      ASSERT_NE(nullptr, val) << id;
      EXPECT_EQ(input.field == "geo_multi",
                std::holds_alternative<std::vector<std::vector<double>>>(*val)) << id;
      EXPECT_EQ(expected, quantized(*val)) << id;
      EXPECT_EQ(nullptr, find(*doc, otherField(input))) << id;
    }
  }
}
