// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include <gtest/gtest.h>

#include <algorithm>
#include <memory_resource>
#include <string>
#include <string_view>
#include <vector>

#include "luxir/api/luxir_types.hpp"
#include "luxir/index/handler/GeoPointWire.h"
#include "test/CollectionHelper.h"
#include "test/QueryBuild.h"
#include "test/SchemaBuilder.h"
#include "test/LuxirTest.h"
#include "test/TestUtils.h"

using namespace luxir;
using namespace luxir::test;
using handler::GeoPointWire;

namespace {

api::Val jsonVal(std::string_view json, std::pmr::memory_resource& mr) {
  api::Val val;
  std::string error;
  if (!api::read_json(val, json, mr, &error)) {
    throw std::runtime_error("bad test JSON: " + error);
  }
  return val;
}

std::vector<GeoPoint> points(const GeoPointWire& wire, std::string_view json) {
  std::pmr::monotonic_buffer_resource mr;
  std::vector<GeoPoint> out;
  wire.forEachPoint(jsonVal(json, mr), [&](const GeoPoint& p) { out.push_back(p); });
  return out;
}

GeoPoint one(const GeoPointWire& wire, std::string_view json) {
  auto out = points(wire, json);
  if (out.size() != 1) throw std::runtime_error("expected one point");
  return out[0];
}

std::string failure(const GeoPointWire& wire, std::string_view json) {
  try {
    points(wire, json);
  } catch (const DocumentError& e) {
    return e.what();
  }
  return "";
}

// Adds one document read from JSON. The literal outlives the submit.
void addJsonDoc(CollectionHelper::UpdateBuilder& update, std::string_view json) {
  update.addRaw([json](api::Map& doc, std::pmr::memory_resource& mr) {
    std::string error;
    if (!api::read_json(doc, json, mr, &error)) {
      throw std::runtime_error("bad test JSON: " + error);
    }
  });
}

std::vector<std::string> ids(LocalReq& req) {
  req.execute();
  EXPECT_OK((&req));
  std::vector<std::string> found;
  for (const auto& doc : req.getDocs()) {
    found.push_back(std::get<std::string>(*find(doc, "id")));
  }
  std::sort(found.begin(), found.end());
  return found;
}

std::vector<std::string> boxIds(std::string_view field, double minLat, double maxLat,
                                double minLon, double maxLon) {
  auto req = localReq(LuxirTest::luxirNode->getSearchEngine());
  auto& cur = req->collection("main").topDocs("q");
  cur.rawQuery() = qb::geoBox(cur.mr(), field, minLat, maxLat, minLon, maxLon);
  cur.fields({"id"}).limit(50);
  return ids(*req);
}

std::vector<std::string> allIds() {
  auto req = localReq(LuxirTest::luxirNode->getSearchEngine());
  auto& cur = req->collection("main").topDocs("q");
  cur.allQuery().fields({"id"}).limit(50);
  return ids(*req);
}

using Ids = std::vector<std::string>;

} // namespace

class GeoPointWireTest : public LuxirTest {};

TEST_F(GeoPointWireTest, everyPointForm) {
  GeoPointWire wire{"loc"};
  auto expect = [&](std::string_view json, double lat, double lon) {
    GeoPoint p = one(wire, json);
    EXPECT_EQ(lat, p.latitude) << json;
    EXPECT_EQ(lon, p.longitude) << json;
  };
  expect("[-74.5, 40.25]", 40.25, -74.5);
  expect("[10, 50]", 50.0, 10.0);
  expect(R"({"lat": 40.25, "lon": -74.5})", 40.25, -74.5);
  expect(R"({"lon": "-74.5", "lat": " 40.25"})", 40.25, -74.5);
  expect(R"({"type": "Point", "coordinates": [-74.5, 40.25]})", 40.25, -74.5);
  expect(R"({"type": "point", "coordinates": [10, 50]})", 50.0, 10.0);
  expect(R"("40.25,-74.5")", 40.25, -74.5);
  expect(R"(" 40.25 , -74.5 ")", 40.25, -74.5);
  expect(R"("+40.25,-74.5")", 40.25, -74.5);
  expect(R"({"lat": "+40.25", "lon": -74.5})", 40.25, -74.5);
  expect(R"j("POINT (-74.5 40.25)")j", 40.25, -74.5);
  expect(R"j("point(-74.5   40.25)")j", 40.25, -74.5);
  expect(R"j("  Point ( 10 50 ) ")j", 50.0, 10.0);
  // Boundaries are valid.
  expect("[180, 90]", 90.0, 180.0);
  expect(R"("-90,-180")", -90.0, -180.0);
}

TEST_F(GeoPointWireTest, geohashDecodesToCellCentre) {
  GeoPointWire wire{"loc"};
  // The Wikipedia example: u4pruydqqvj is 57.64911, 10.40744 (11 characters,
  // a cell about 15 cm wide).
  GeoPoint p = one(wire, R"("u4pruydqqvj")");
  EXPECT_NEAR(57.64911, p.latitude, 1e-5);
  EXPECT_NEAR(10.40744, p.longitude, 1e-5);
  // Case-insensitive.
  GeoPoint upper = one(wire, R"("U4PRUYDQQVJ")");
  EXPECT_EQ(p.latitude, upper.latitude);
  EXPECT_EQ(p.longitude, upper.longitude);
  // One character: "s" is the cell lat [0, 45], lon [0, 45].
  p = one(wire, R"("s")");
  EXPECT_EQ(22.5, p.latitude);
  EXPECT_EQ(22.5, p.longitude);
  // Two characters: "ez" is lat [39.375, 45], lon [-11.25, 0].
  p = one(wire, R"("ez")");
  EXPECT_EQ((39.375 + 45.0) / 2, p.latitude);
  EXPECT_EQ(-11.25 / 2, p.longitude);
  // Twelve characters is the limit; a, i, l and o are not in the alphabet.
  EXPECT_TRUE(GeoPointWire::decodeGeohash("u4pruydqqvjq").has_value());
  EXPECT_FALSE(GeoPointWire::decodeGeohash("u4pruydqqvjqq").has_value());
  EXPECT_FALSE(GeoPointWire::decodeGeohash("u4pa").has_value());
  EXPECT_FALSE(GeoPointWire::decodeGeohash("").has_value());
}

TEST_F(GeoPointWireTest, zCoordinateNeedsIgnoreZValue) {
  GeoPointWire strict{"loc"};
  for (std::string_view json : {"[1, 2, 3]", R"j("2,1,3")j", R"j("POINT (1 2 3)")j",
                                R"j("POINT Z (1 2 3)")j",
                                R"({"type": "Point", "coordinates": [1, 2, 3]})"}) {
    EXPECT_NE(std::string::npos, failure(strict, json).find("ignore_z_value")) << json;
    GeoPointWire lenient{"loc", false, true};
    GeoPoint p = one(lenient, json);
    EXPECT_EQ(2.0, p.latitude) << json;
    EXPECT_EQ(1.0, p.longitude) << json;
  }
  // POINT Z requires the third coordinate.
  EXPECT_NE(std::string::npos,
            failure(GeoPointWire{"loc", false, true}, R"j("POINT Z (1 2)")j").find("WKT"));
}

TEST_F(GeoPointWireTest, malformedPointsFailWithTheFieldName) {
  GeoPointWire wire{"loc"};
  auto expectFailure = [&](std::string_view json, std::string_view detail) {
    std::string message = failure(wire, json);
    EXPECT_NE(std::string::npos, message.find("field 'loc'")) << json << ": " << message;
    EXPECT_NE(std::string::npos, message.find(detail)) << json << ": " << message;
  };
  expectFailure("[1]", "needs 2 coordinates");
  expectFailure("[1, 2, 3, 4]", "needs 2 coordinates");
  expectFailure("[1, 91]", "latitude 91");
  expectFailure("[181, 1]", "longitude 181");
  expectFailure(R"("91,0")", "latitude 91");
  expectFailure(R"({"lat": 1})", "both lat and lon");
  expectFailure(R"({"lat": 1, "lon": 2, "alt": 3})", "unknown key \"alt\"");
  expectFailure(R"({"lat": "x", "lon": 2})", "\"lat\" must be a number");
  expectFailure(R"({"lat": 1, "lon": 2, "type": "Point"})", "not both");
  expectFailure(R"({"type": "LineString", "coordinates": [1, 2]})", "\"type\": \"Point\"");
  expectFailure(R"({"type": "Point"})", "\"coordinates\" array");
  expectFailure(R"("1,2,3,4")", "is not \"lat,lon\"");
  expectFailure(R"("1,x")", "is not \"lat,lon\"");
  expectFailure(R"j("POINT (1)")j", "WKT");
  expectFailure(R"j("POINT (1 2")j", "WKT");
  expectFailure(R"j("POINT (1 2) x")j", "WKT");
  expectFailure(R"j("POINT EMPTY")j", "POINT EMPTY");
  expectFailure(R"("not a place")", "geohash");
  expectFailure("true", "expected [lon, lat]");
  expectFailure("7", "expected [lon, lat]");
}

TEST_F(GeoPointWireTest, numberArraysAreOnePointOtherArraysListPoints) {
  GeoPointWire wire{"loc"};
  EXPECT_EQ(1u, points(wire, "[1, 2]").size());
  EXPECT_EQ(2u, points(wire, "[[1, 2], [3, 4]]").size());
  EXPECT_EQ(1u, points(wire, "[[1, 2]]").size());
  EXPECT_EQ(3u, points(wire, R"(["2,1", "s", {"lat": 2, "lon": 1}])").size());
  EXPECT_EQ(2u, points(wire, R"j(["2,1", "POINT (1 2)"])j").size());
  EXPECT_EQ(2u, points(wire, R"([[1, 2], null, "s"])").size());
  EXPECT_TRUE(points(wire, "[]").empty());
  EXPECT_TRUE(points(wire, "null").empty());
  // Nesting goes one level deep: an element must itself be a point.
  EXPECT_NE(std::string::npos, failure(wire, "[[[1, 2]]]").find("is not a number"));
  EXPECT_NE(std::string::npos, failure(wire, R"([1, "s"])").find("expected [lon, lat]"));
}

TEST_F(GeoPointWireTest, ignoreMalformedSkipsOnlyBadPoints) {
  GeoPointWire wire{"loc", true};
  EXPECT_TRUE(points(wire, "[1, 91]").empty());
  EXPECT_TRUE(points(wire, R"("nowhere at all")").empty());
  EXPECT_TRUE(points(wire, "[1, 2, 3]").empty());  // z without ignore_z_value
  auto kept = points(wire, R"([[1, 2], [1, 91], "bad", "4,3", {"lat": 1}])");
  ASSERT_EQ(2u, kept.size());
  EXPECT_EQ(2.0, kept[0].latitude);
  EXPECT_EQ(4.0, kept[1].latitude);
}

class GeoPointFormatsTest : public LuxirTest {};

TEST_F(GeoPointFormatsTest, ingestEveryFormSingleAndMulti) {
  CollectionHelper helper;
  SchemaBuilder b;
  auto& single = b.field("loc");
  single.type = api::FieldDef::FieldClass::GEO_POINT;
  single.index = api::FieldDef::IndexMode::RANGE;
  auto& multi = b.field("locs");
  multi.type = api::FieldDef::FieldClass::GEO_POINT;
  multi.multi = true;  // column scan: exercises the unindexed path too
  b.set(helper.collection());

  CollectionHelper::UpdateBuilder update;
  addJsonDoc(update, R"({"id": "array", "loc": [10.5, 50.5]})");
  addJsonDoc(update, R"({"id": "object", "loc": {"lat": 50.6, "lon": 10.6}})");
  addJsonDoc(update, R"({"id": "geojson", "loc": {"type": "Point", "coordinates": [10.7, 50.7]}})");
  addJsonDoc(update, R"({"id": "latlon", "loc": "50.8,10.8"})");
  addJsonDoc(update, R"j({"id": "wkt", "loc": "POINT (10.9 50.9)"})j");
  addJsonDoc(update, R"({"id": "geohash", "loc": "u0zrq"})");  // about 50.5, 10.5
  addJsonDoc(update, R"({"id": "wrapped", "loc": ["50.55,10.55"]})");
  addJsonDoc(update, R"({"id": "far", "loc": [-74.0, 40.7]})");
  addJsonDoc(update, R"j({"id": "m_mixed", "locs": [[10.5, 50.5], "-33.9,151.2", "POINT (139.7 35.7)"]})j");
  addJsonDoc(update, R"({"id": "m_one", "locs": [139.7, 35.7]})");
  addJsonDoc(update, R"({"id": "m_object", "locs": {"lat": -33.9, "lon": 151.2}})");
  addJsonDoc(update, R"({"id": "m_strings", "locs": ["51.5,-0.1", "gcpvj0"]})");
  update.commit();
  IndexResult result = helper.submit(update);
  ASSERT_EQ(IndexResult::Status::OK, result.status) << result.error_message;

  EXPECT_EQ((Ids{"array", "geohash", "geojson", "latlon", "object", "wkt", "wrapped"}),
            boxIds("loc", 50.0, 51.0, 10.0, 11.0));
  EXPECT_EQ((Ids{"far"}), boxIds("loc", 40.0, 41.0, -75.0, -73.0));
  EXPECT_EQ((Ids{"m_mixed", "m_one"}), boxIds("locs", 35.0, 36.0, 139.0, 140.0));
  EXPECT_EQ((Ids{"m_mixed", "m_object"}), boxIds("locs", -34.0, -33.0, 151.0, 152.0));
  EXPECT_EQ((Ids{"m_strings"}), boxIds("locs", 51.0, 52.0, -1.0, 0.0));
}

TEST_F(GeoPointFormatsTest, malformedPointsFailTheDocumentUnlessIgnored) {
  CollectionHelper helper;
  SchemaBuilder b;
  auto& strict = b.field("strict");
  strict.type = api::FieldDef::FieldClass::GEO_POINT;
  strict.index = api::FieldDef::IndexMode::RANGE;
  auto& lenient = b.field("lenient");
  lenient.type = api::FieldDef::FieldClass::GEO_POINT;
  lenient.index = api::FieldDef::IndexMode::RANGE;
  lenient.multi = true;
  lenient.ignore_malformed = true;
  lenient.ignore_z_value = true;
  b.set(helper.collection());

  CollectionHelper::UpdateBuilder update;
  addJsonDoc(update, R"({"id": "bad", "strict": [1, 91]})");
  addJsonDoc(update, R"({"id": "z", "strict": [1, 2, 3]})");
  addJsonDoc(update, R"({"id": "two", "strict": [[1, 2], [3, 4]]})");
  addJsonDoc(update, R"({"id": "skipped", "lenient": "nowhere at all", "name_s": "kept"})");
  addJsonDoc(update, R"({"id": "partial", "lenient": [[1, 91], [1, 2, 100], "not-a-point"]})");
  update.commit();
  IndexResult result = helper.submit(update);
  ASSERT_EQ(IndexResult::Status::PARTIAL, result.status);
  ASSERT_EQ(3u, result.errors.size());
  EXPECT_NE(std::string::npos, result.errors[0].error_message.find("latitude 91"));
  EXPECT_NE(std::string::npos, result.errors[1].error_message.find("ignore_z_value"));
  EXPECT_NE(std::string::npos, result.errors[2].error_message.find("single-valued"));

  // The skipped value leaves its document indexed; the z value was dropped.
  EXPECT_EQ((Ids{"partial", "skipped"}), allIds());
  EXPECT_EQ((Ids{"partial"}), boxIds("lenient", 1.5, 2.5, 0.5, 1.5));
  EXPECT_TRUE(boxIds("lenient", -90.0, 90.0, 1.5, 180.0).empty());
}

TEST_F(GeoPointFormatsTest, suffixTemplates) {
  auto schema = Schema::createDefaultSchema();
  for (auto [name, multi] : {std::pair{"home_p", false}, std::pair{"stops_ps", true}}) {
    FieldType* type = schema->resolveInput(name).fieldType;
    ASSERT_NE(nullptr, type) << name;
    EXPECT_EQ(FieldType::GEO_POINT, type->type()) << name;
    EXPECT_TRUE(type->rangeIndexed()) << name;
    EXPECT_TRUE(type->hasColumn()) << name;
    EXPECT_EQ(multi, type->multiValued()) << name;
  }

  CollectionHelper helper;
  CollectionHelper::UpdateBuilder update;
  addJsonDoc(update, R"({"id": "a", "home_p": "50.5,10.5", "stops_ps": [[10.5, 50.5], [-74.0, 40.7]]})");
  addJsonDoc(update, R"({"id": "b", "home_p": [-74.0, 40.7]})");
  update.commit();
  ASSERT_EQ(IndexResult::Status::OK, helper.submit(update).status);
  EXPECT_EQ((Ids{"a"}), boxIds("home_p", 50.0, 51.0, 10.0, 11.0));
  EXPECT_EQ((Ids{"a"}), boxIds("stops_ps", 40.0, 41.0, -75.0, -73.0));
}

TEST_F(GeoPointFormatsTest, optionsAreGeoOnlyAndInherit) {
  {
    SchemaBuilder b;
    auto& f = b.field("name");
    f.type = api::FieldDef::FieldClass::STRING;
    f.ignore_malformed = true;
    EXPECT_THROW(b.build(), SchemaError);
  }
  {
    SchemaBuilder b;
    auto& f = b.field("count");
    f.type = api::FieldDef::FieldClass::INT;
    f.ignore_z_value = false;
    EXPECT_THROW(b.build(), SchemaError);
  }
  SchemaBuilder b;
  auto& base = b.templ("_place");
  base.type = api::FieldDef::FieldClass::GEO_POINT;
  base.ignore_malformed = true;
  base.ignore_z_value = true;
  auto& child = b.field("office");
  child.parent = "_place";
  child.ignore_z_value = false;
  auto schema = b.build();
  auto* office = (GeoPointFieldType*)schema->resolveInput("office").fieldType;
  EXPECT_TRUE(office->ignoreMalformed);
  EXPECT_FALSE(office->ignoreZValue);
  auto* other = (GeoPointFieldType*)schema->resolveInput("other_place").fieldType;
  EXPECT_TRUE(other->ignoreMalformed);
  EXPECT_TRUE(other->ignoreZValue);
}

TEST_F(GeoPointFormatsTest, variantsFollowThePrimarysOptions) {
  {
    SchemaBuilder b;
    auto& f = b.field("loc");
    f.type = api::FieldDef::FieldClass::GEO_POINT;
    b.variant(f, "copy").ignore_malformed = true;
    EXPECT_THROW(b.build(), SchemaError);
  }
  CollectionHelper helper;
  SchemaBuilder b;
  auto& f = b.field("loc");
  f.type = api::FieldDef::FieldClass::GEO_POINT;
  f.ignore_malformed = true;
  auto& copy = b.variant(f, "copy");
  copy.type = api::FieldDef::FieldClass::GEO_POINT;
  copy.index = api::FieldDef::IndexMode::RANGE;
  auto schema = b.set(helper.collection());
  EXPECT_TRUE(((GeoPointFieldType*)schema->physical("loc__copy"))->ignoreMalformed);

  CollectionHelper::UpdateBuilder update;
  addJsonDoc(update, R"({"id": "bad", "loc": "nowhere at all"})");
  addJsonDoc(update, R"({"id": "good", "loc": [10.5, 50.5]})");
  update.commit();
  ASSERT_EQ(IndexResult::Status::OK, helper.submit(update).status);
  EXPECT_EQ((Ids{"bad", "good"}), allIds());
  EXPECT_EQ((Ids{"good"}), boxIds("loc__copy", 50.0, 51.0, 10.0, 11.0));
}
