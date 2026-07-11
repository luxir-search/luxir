#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <memory_resource>
#include <span>
#include <string_view>
#include <tuple>
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
#include "test/QueryBuild.h"
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
  if (!twoPhase || !scorer->hasTwoPhase()) {
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

void setGeoSchema(CollectionHelper& helper, bool range = true) {
  std::pmr::monotonic_buffer_resource arena;
  api::SchemaDef def;
  api::FieldDef* fields = api::build::allocArray(def.fields, 2, arena);
  fields[0].name = "geo_single";
  fields[0].field_class = api::FieldDef::FieldClass::GEO_POINT;
  fields[0].index = range ? api::FieldDef::IndexMode::RANGE
                          : api::FieldDef::IndexMode::NONE;
  fields[1].name = "geo_multi";
  fields[1].field_class = api::FieldDef::FieldClass::GEO_POINT;
  fields[1].index = range ? api::FieldDef::IndexMode::RANGE
                          : api::FieldDef::IndexMode::NONE;
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

TEST_F(GeoBoxQueryTest, publicWireIngestAndProtoQueryRoundTrip) {
  CollectionHelper helper;
  helper.clear();
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
    auto req = localReq(soluxNode->getSearchEngine());
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
  helper.clear();
  setGeoSchema(helper);

  CollectionHelper::UpdateBuilder update;
  addWireDoc(update, "short", "geo_single", [](api::Val& val, auto& mr) {
    setDoubles(val, mr, {1.0});
  });
  addWireDoc(update, "long", "geo_single", [](api::Val& val, auto& mr) {
    setInts(val, mr, {1, 2, 3});
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
    EXPECT_NE(std::string::npos, error.error_message.find("field 'geo_single'"));
    EXPECT_NE(std::string::npos,
              error.error_message.find("[x, y] = [lon, lat]"));
  }

  auto req = localReq(soluxNode->getSearchEngine());
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
                countState.weight->count(segment));
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
            state.weight->count(segment));
  int64_t sparseBefore = SkipStats::geoSparseVerifyArms;
  SkipStats::enabled = true;
  auto* sparse = supplier->get(pool, 0);
  SkipStats::enabled = false;
  ASSERT_NE(nullptr, sparse);
  EXPECT_TRUE(sparse->hasTwoPhase());
  EXPECT_EQ(sparseBefore + 1, SkipStats::geoSparseVerifyArms);

  QueryState multiState(pool, *reader, "geo_multi", boxes[2]);
  EXPECT_EQ(-1, multiState.weight->count(segment));
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
  helper.clear();
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
  auto reader = writer->getIndexReader();
  auto& segment = reader->segments()[0];
  EXPECT_EQ(0, fieldInfo(segment, "geo_single").pointsMetaOff);
  Box box{-10.0, 10.0, 160.0, -170.0};
  SkipStats::reset();
  SkipStats::enabled = true;
  EXPECT_EQ(oracle(points, box), run(*reader, "geo_single", box, false));
  SkipStats::enabled = false;
  EXPECT_EQ(1, SkipStats::geoScanArms);
}

TEST_F(GeoBoxQueryTest, mergeRebuildsSingleAndMultiBKDWithDeletes) {
  CollectionHelper helper;
  helper.clear();
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

  auto beforeMerge = writer->getIndexReader();
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
  EXPECT_EQ(-1, deletedCountState.weight->count(beforeMerge->segments()[1]));

  writer->mergeSegments();
  auto afterMerge = writer->getIndexReader();
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
  EXPECT_EQ(39, singleCountState.weight->count(merged));
  QueryState multiCountState(countPool, *afterMerge, "geo_multi", all);
  EXPECT_EQ(-1, multiCountState.weight->count(merged));
}
