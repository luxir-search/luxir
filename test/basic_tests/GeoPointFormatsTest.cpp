// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include <gtest/gtest.h>

#include <algorithm>
#include <limits>
#include <memory_resource>
#include <string>
#include <string_view>
#include <vector>

#include "luxir/api/luxir_types.hpp"
#include "luxir/schema/GeoPointCoerce.h"
#include "test/CollectionHelper.h"
#include "test/QueryBuild.h"
#include "test/SchemaBuilder.h"
#include "test/LuxirTest.h"
#include "test/TestUtils.h"

using namespace luxir;
using namespace luxir::test;
using coerce::GeoPointInput;

namespace {

api::Val jsonVal(std::string_view json, std::pmr::memory_resource& mr) {
  api::Val val;
  std::string error;
  if (!api::read_json(val, json, mr, &error)) {
    throw std::runtime_error("bad test JSON: " + error);
  }
  return val;
}

std::vector<GeoPoint> points(const GeoPointInput& wire, std::string_view json) {
  std::pmr::monotonic_buffer_resource mr;
  std::vector<GeoPoint> out;
  wire.forEachPoint(jsonVal(json, mr), [&](const GeoPoint& p) { out.push_back(p); });
  return out;
}

GeoPoint one(const GeoPointInput& wire, std::string_view json) {
  auto out = points(wire, json);
  if (out.size() != 1) throw std::runtime_error("expected one point");
  return out[0];
}

std::string failure(const GeoPointInput& wire, std::string_view json) {
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

class GeoPointInputTest : public LuxirTest {};

TEST_F(GeoPointInputTest, everyPointForm) {
  GeoPointInput wire{"loc"};
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

TEST_F(GeoPointInputTest, geohashDecodesToCellCentre) {
  GeoPointInput wire{"loc"};
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
  // Numeric-looking strings are still geohashes.
  p = one(wire, R"("40")");
  EXPECT_EQ(-87.1875, p.latitude);
  EXPECT_EQ(-84.375, p.longitude);
  // Twelve characters is the limit; a, i, l and o are not in the alphabet.
  EXPECT_TRUE(GeoPointInput::decodeGeohash("u4pruydqqvjq").has_value());
  EXPECT_FALSE(GeoPointInput::decodeGeohash("u4pruydqqvjqq").has_value());
  EXPECT_FALSE(GeoPointInput::decodeGeohash("u4pa").has_value());
  EXPECT_FALSE(GeoPointInput::decodeGeohash("").has_value());
}

TEST_F(GeoPointInputTest, zCoordinateIsDroppedUnlessExplicitlyRejected) {
  GeoPointInput strict{"loc", false, false};
  for (std::string_view json : {"[1, 2, 3]", R"j("2,1,3")j", R"j("POINT (1 2 3)")j",
                                R"j("POINT Z (1 2 3)")j",
                                R"({"type": "Point", "coordinates": [1, 2, 3]})"}) {
    EXPECT_NE(std::string::npos, failure(strict, json).find("ignore_z_value")) << json;
    GeoPointInput lenient{"loc"};
    GeoPoint p = one(lenient, json);
    EXPECT_EQ(2.0, p.latitude) << json;
    EXPECT_EQ(1.0, p.longitude) << json;
  }
  // POINT Z requires the third coordinate.
  EXPECT_NE(std::string::npos,
            failure(GeoPointInput{"loc", false, true}, R"j("POINT Z (1 2)")j").find("WKT"));
}

TEST_F(GeoPointInputTest, malformedPointsFailWithTheFieldName) {
  GeoPointInput wire{"loc"};
  auto expectFailure = [&](std::string_view json, std::string_view detail) {
    std::string message = failure(wire, json);
    EXPECT_NE(std::string::npos, message.find("field 'loc'")) << json << ": " << message;
    EXPECT_NE(std::string::npos, message.find(detail)) << json << ": " << message;
  };
  expectFailure("[1]", "needs 2 coordinates");
  expectFailure("[1, 2, 3, 4]", "needs 2 coordinates");
  expectFailure("[1, 91]", "lat=91");
  expectFailure("[181, 1]", "lon=181");
  expectFailure(R"("91,0")", "lat=91");
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

TEST_F(GeoPointInputTest, errorsShowFormAndInterpretedCoordinates) {
  GeoPointInput input{"loc", false, false};
  for (auto [json, form] : {
      std::pair{"[1, 91]", "[lon, lat] array"},
      {R"("91,1")", "lat,lon string"},
      {R"j("POINT (1 91)")j", "WKT"},
      {R"({"lat":91,"lon":1})", "lat/lon object"},
      {R"({"type":"Point","coordinates":[1,91]})", "GeoJSON"},
      {"[1, 91, 0, 0]", "[lon, lat] array"},
      {R"("91,1,0,0")", "lat,lon string"},
      {R"j("POINT Z (1 91)")j", "WKT"}}) {
    std::string message = failure(input, json);
    EXPECT_NE(std::string::npos, message.find(form)) << message;
    EXPECT_NE(std::string::npos, message.find("lon=1 lat=91")) << message;
  }
  EXPECT_NE(std::string::npos, failure(input, "[1, 2, 3]").find("lon=1 lat=2"));
}

TEST_F(GeoPointInputTest, numberArraysAreOnePointOtherArraysListPoints) {
  GeoPointInput wire{"loc"};
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

TEST_F(GeoPointInputTest, ignoreMalformedSkipsOnlyBadPoints) {
  GeoPointInput wire{"loc", true, false};
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
  multi.multi = api::FieldDef::MultiMode::DEFAULT;  // column scan: exercises the unindexed path too
  b.set(helper.collection());

  CollectionHelper::UpdateBuilder update;
  addJsonDoc(update, R"({"id": "array", "loc": [10.5, 50.5]})");
  addJsonDoc(update, R"({"id": "object", "loc": {"lat": 50.6, "lon": 10.6}})");
  addJsonDoc(update, R"({"id": "geojson", "loc": {"type": "Point", "coordinates": [10.7, 50.7]}})");
  addJsonDoc(update, R"({"id": "latlon", "loc": "50.8,10.8"})");
  addJsonDoc(update, R"j({"id": "wkt", "loc": "POINT (10.9 50.9)"})j");
  addJsonDoc(update, R"({"id": "geohash", "loc": "u0zrq"})");  // about 50.5, 10.5
  addJsonDoc(update, R"({"id": "wrapped", "loc": ["50.55,10.55"]})");
  addJsonDoc(update, R"({"id": "z", "loc": [10.5,50.5,100]})");
  addJsonDoc(update, R"({"id": "far", "loc": [-74.0, 40.7]})");
  addJsonDoc(update, R"j({"id": "m_mixed", "locs": [[10.5, 50.5], "-33.9,151.2", "POINT (139.7 35.7)"]})j");
  addJsonDoc(update, R"({"id": "m_one", "locs": [139.7, 35.7]})");
  addJsonDoc(update, R"({"id": "m_object", "locs": {"lat": -33.9, "lon": 151.2}})");
  addJsonDoc(update, R"({"id": "m_strings", "locs": ["51.5,-0.1", "gcpvj0"]})");
  update.commit();
  IndexResult result = helper.submit(update);
  ASSERT_EQ(IndexResult::Status::OK, result.status) << result.error_message;

  EXPECT_EQ((Ids{"array", "geohash", "geojson", "latlon", "object", "wkt", "wrapped", "z"}),
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
  strict.ignore_z_value = false;
  auto& lenient = b.field("lenient");
  lenient.type = api::FieldDef::FieldClass::GEO_POINT;
  lenient.index = api::FieldDef::IndexMode::RANGE;
  lenient.multi = api::FieldDef::MultiMode::DEFAULT;
  lenient.ignore_malformed = true;
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
  EXPECT_NE(std::string::npos, result.errors[0].error_message.find("lat=91"));
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
    EXPECT_TRUE(((GeoPointFieldType*)type)->ignoreZValue) << name;
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
  for (auto name : {"single", "multi"}) {
    auto& f = b.field(name);
    f.type = api::FieldDef::FieldClass::GEO_POINT;
    f.ignore_malformed = true;
    if (std::string_view(name) == "multi") f.multi = api::FieldDef::MultiMode::DEFAULT;
    auto& copy = b.variant(f, "copy");
    copy.type = api::FieldDef::FieldClass::GEO_POINT;
    copy.index = api::FieldDef::IndexMode::RANGE;
  }
  b.set(helper.collection());

  CollectionHelper::UpdateBuilder update;
  addJsonDoc(update, R"({"id":"bad","single":"nowhere at all","multi":[[1,91]]})");
  addJsonDoc(update, R"({"id":"good","single":[[1,91],[10.5,50.5,3]],"multi":[[1,91],[10.5,50.5,3],"2,1"]})");
  addJsonDoc(update, R"({"id":"two","single":[[1,91],[1,2],[3,4]]})");
  update.commit();
  auto result = helper.submit(update);
  ASSERT_EQ(IndexResult::Status::PARTIAL, result.status);
  ASSERT_EQ(1u, result.errors.size());
  EXPECT_NE(std::string::npos, result.errors[0].error_message.find("single-valued"));
  EXPECT_EQ((Ids{"bad", "good"}), allIds());
  for (auto field : {"single", "single__copy", "multi", "multi__copy"}) {
    EXPECT_EQ((Ids{"good"}), boxIds(field, 50.0, 51.0, 10.0, 11.0)) << field;
  }
  for (auto field : {"multi", "multi__copy"}) {
    EXPECT_EQ((Ids{"good"}), boxIds(field, 1.5, 2.5, 0.5, 1.5)) << field;
  }
}

TEST_F(GeoPointFormatsTest, validatesBeforeAnyBranchMutates) {
  CollectionHelper helper;
  SchemaBuilder b;
  for (auto name : {"single", "multi"}) {
    auto& f = b.field(name);
    f.type = api::FieldDef::FieldClass::GEO_POINT;
    if (std::string_view(name) == "multi") f.multi = api::FieldDef::MultiMode::DEFAULT;
    auto& copy = b.variant(f, "copy");
    copy.type = api::FieldDef::FieldClass::GEO_POINT;
    copy.index = api::FieldDef::IndexMode::RANGE;
  }
  b.set(helper.collection());
  auto writer = helper.getIndexWriter();
  auto& inv = writer->obtainInverter();
  inv.startDoc();
  inv.getIndexHandler("id").index(inv, std::string_view("good"));
  std::pmr::monotonic_buffer_resource mr;
  for (auto name : {"single", "multi"}) {
    auto& input = inv.getIndexHandler(name);
    EXPECT_THROW(input.index(inv, jsonVal("[[10,20],[1,91]]", mr)), DocumentError);
    EXPECT_THROW(input.index(inv, 91, 1), DocumentError);
    EXPECT_THROW(input.index(inv, 1, 181), DocumentError);
    EXPECT_THROW(input.index(inv, std::numeric_limits<double>::infinity(), 1), DocumentError);
    EXPECT_THROW(input.index(inv, 1, std::numeric_limits<double>::quiet_NaN()), DocumentError);
    const GeoPoint badPoint{91, 1};
    EXPECT_THROW(input.index(inv, std::span<const GeoPoint>(&badPoint, 1)), DocumentError);
    const GeoPoint invalid[]{{20, 10}, {91, 1}};
    EXPECT_THROW(input.index(inv, std::span<const GeoPoint>(invalid)), DocumentError);
    // Retry in the same doc: a partial column from any failure corrupts this append.
    input.index(inv, 50.5, 10.5);
  }
  inv.finishDoc();
  writer->releaseInverter(inv);
  helper.commit();
  for (auto field : {"single", "single__copy", "multi", "multi__copy"}) {
    EXPECT_EQ((Ids{"good"}), boxIds(field, 50.0, 51.0, 10.0, 11.0)) << field;
    EXPECT_TRUE(boxIds(field, 19.0, 21.0, 9.0, 11.0).empty()) << field;
  }
}

TEST_F(GeoPointFormatsTest, storedGeoIsRejected) {
  SchemaBuilder b;
  auto& f = b.field("loc");
  f.type = api::FieldDef::FieldClass::GEO_POINT;
  f.stored = true;
  try {
    b.build();
    FAIL() << "stored geo field accepted";
  } catch (const SchemaError& e) {
    EXPECT_NE(std::string::npos, std::string_view(e.what()).find("stored=true"));
    EXPECT_NE(std::string::npos, std::string_view(e.what()).find("loc"));
  }
}
