// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <format>
#include <limits>
#include <numbers>
#include <optional>
#include <string>
#include <vector>

#include "luxir/query/GeoNearestQuery.h"
#include "luxir/reader/SkipStats.h"
#include "luxir/util/geo.h"
#include "luxir/util/random.h"
#include "test/CollectionHelper.h"
#include "test/LocalReq.h"
#include "test/LuxirTest.h"
#include "test/QueryBuild.h"
#include "test/SchemaBuilder.h"
#include "test/TestUtils.h"

using namespace luxir;
using namespace luxir::test;

namespace {

constexpr double INF = std::numeric_limits<double>::infinity();

double unit(SplitMix64& rng) {
  return (double)(rng() >> 11) * (1.0 / 9007199254740992.0);
}

double wrapLon(double lon) {
  while (lon < -180.0) lon += 360.0;
  while (lon > 180.0) lon -= 360.0;
  return lon;
}

// The center clamped into the box, keyed like the research prototype. Not a
// bound across the date line or past 90 degrees of longitude.
double clampedKey(double lat, double lon, double minLat, double maxLat,
                  double minLon, double maxLon) {
  return geo::haversinSortKey(lat, lon, std::clamp(lat, minLat, maxLat),
                              std::clamp(lon, minLon, maxLon));
}

// Smallest key over a dense sampling of the box edges plus its interior.
double sampledMin(double lat, double lon, double minLat, double maxLat,
                  double minLon, double maxLon, SplitMix64& rng) {
  double best = INF;
  auto visit = [&](double pointLat, double pointLon) {
    best = std::min(best, geo::haversinSortKey(lat, lon, pointLat, pointLon));
  };
  constexpr int STEPS = 400;
  for (int i = 0; i <= STEPS; i++) {
    double t = (double)i / STEPS;
    double edgeLat = minLat + (maxLat - minLat) * t;
    double edgeLon = minLon + (maxLon - minLon) * t;
    visit(edgeLat, minLon);
    visit(edgeLat, maxLon);
    visit(minLat, edgeLon);
    visit(maxLat, edgeLon);
    if (minLon <= lon && lon <= maxLon) visit(edgeLat, lon);
  }
  for (int i = 0; i < 64; i++) {
    visit(minLat + (maxLat - minLat) * unit(rng),
          minLon + (maxLon - minLon) * unit(rng));
  }
  return best;
}

class GeoNearestBoundTest : public LuxirTest {};

TEST_F(GeoNearestBoundTest, boxLowerBoundNeverExceedsAPointAndIsTight) {
  SplitMix64 rng(0x5eed1234);
  int64_t iterations = scaleTestWork(4000);
  for (int64_t iter = 0; iter < iterations; iter++) {
    double minLat;
    double maxLat;
    double minLon;
    double maxLon;
    double width = std::pow(10.0, -4.0 + 6.0 * unit(rng));  // 1e-4 .. 100 degrees
    switch (iter % 5) {
      case 0:  // anywhere
        minLat = -90.0 + 180.0 * unit(rng);
        minLon = -180.0 + 360.0 * unit(rng);
        break;
      case 1:  // touching a pole
        minLat = unit(rng) < 0.5 ? 90.0 - width : -90.0;
        minLon = -180.0 + 360.0 * unit(rng);
        break;
      case 2:  // against the date line
        minLat = -90.0 + 180.0 * unit(rng);
        minLon = unit(rng) < 0.5 ? -180.0 : 180.0 - width;
        break;
      case 3:  // every longitude
        minLat = -90.0 + 180.0 * unit(rng);
        minLon = -180.0;
        width = std::max(width, 1.0);
        break;
      default:  // a degenerate point or line
        minLat = -90.0 + 180.0 * unit(rng);
        minLon = -180.0 + 360.0 * unit(rng);
        width = unit(rng) < 0.5 ? 0.0 : width;
        break;
    }
    maxLat = std::min(90.0, minLat + width);
    maxLon = iter % 5 == 3 ? 180.0 : std::min(180.0, minLon + width * (0.5 + unit(rng)));
    double lat;
    double lon;
    switch (iter % 4) {
      case 0:
        lat = -90.0 + 180.0 * unit(rng);
        lon = -180.0 + 360.0 * unit(rng);
        break;
      case 1:  // near the box, possibly across the date line
        lat = std::clamp(minLat + (unit(rng) - 0.5) * 4.0 * std::max(width, 1e-3), -90.0, 90.0);
        lon = wrapLon(minLon + (unit(rng) - 0.5) * 4.0 * std::max(width, 1e-3) + (unit(rng) < 0.3 ? 360.0 : 0.0));
        break;
      case 2:  // a pole
        lat = unit(rng) < 0.5 ? 90.0 : -90.0;
        lon = -180.0 + 360.0 * unit(rng);
        break;
      default:  // far around the globe, past 90 degrees of longitude
        lat = -90.0 + 180.0 * unit(rng);
        lon = wrapLon(minLon + 90.0 + 180.0 * unit(rng));
        break;
    }
    double bound = geo::boxSortKeyLowerBound(lat, lon, minLat, maxLat, minLon, maxLon);
    double sampled = sampledMin(lat, lon, minLat, maxLat, minLon, maxLon, rng);
    ASSERT_LE(bound, sampled) << "center " << lat << "," << lon << " box [" << minLat << ","
                              << maxLat << "]x[" << minLon << "," << maxLon << "]";
    // The dense edge sampling approaches the true minimum from above; the
    // bound must sit just below it, within a sampling step, a meter, and
    // the slack boxSortKeyLowerBound takes off, not collapse toward zero.
    double step = geo::haversinMeters(0.0, 0.0, 0.0, std::max(maxLat - minLat, maxLon - minLon) / 400.0);
    double floorMeters = std::max(0.0, geo::haversinMeters(sampled) - step - 1.0);
    double floorKey = 1.0 - std::cos(floorMeters / geo::EARTH_MEAN_RADIUS_METERS);
    EXPECT_GE(bound, floorKey * (1.0 - 1e-12) - 2e-15)
        << "center " << lat << "," << lon << " box [" << minLat << "," << maxLat << "]x["
        << minLon << "," << maxLon << "]";
  }
}

TEST_F(GeoNearestBoundTest, clampingIsNotABoundAcrossTheDateLineOrPastTheMeridian) {
  // East of the date line, the box's west edge -180 is 0.1 degree away;
  // clamping picks its east edge, 80 degrees away.
  double bound = geo::boxSortKeyLowerBound(0.0, 179.9, -1.0, 1.0, -180.0, -100.0);
  double nearest = geo::haversinSortKey(0.0, 179.9, 0.0, -180.0);
  EXPECT_LE(bound, nearest);
  EXPECT_NEAR(geo::haversinMeters(bound), geo::haversinMeters(nearest), 1.0);
  EXPECT_GT(clampedKey(0.0, 179.9, -1.0, 1.0, -180.0, -100.0), 100 * nearest);

  // 170 degrees of longitude away, the nearest point of a box below the
  // pole is its top edge, nowhere near the clamped corner, and the true
  // distance goes over the pole.
  double lat = 80.0;
  double polar = geo::boxSortKeyLowerBound(lat, 0.0, 60.0, 85.0, 170.0, 175.0);
  double overPole = geo::haversinSortKey(lat, 0.0, 85.0, 170.0);
  EXPECT_LE(polar, overPole);
  EXPECT_NEAR(geo::haversinMeters(polar), geo::haversinMeters(overPole), 1.0);
  EXPECT_GT(clampedKey(lat, 0.0, 60.0, 85.0, 170.0, 175.0), polar);
  SplitMix64 rng(7);
  EXPECT_LE(polar, sampledMin(lat, 0.0, 60.0, 85.0, 170.0, 175.0, rng));

  // A center inside the box, or on its meridians, is at most its latitude
  // gap away.
  EXPECT_EQ(0.0, geo::boxSortKeyLowerBound(10.0, 10.0, 0.0, 20.0, 0.0, 20.0));
  EXPECT_NEAR(geo::haversinMeters(geo::boxSortKeyLowerBound(-89.0, 120.0, 80.0, 90.0, 100.0, 130.0)),
              geo::haversinMeters(-89.0, 120.0, 80.0, 120.0), 1.0);
}

// ----------------------------------------------------------------------------

enum class Shape { UNIFORM, CLUSTERED, DATELINE, POLAR };

struct Point {
  double lat;
  double lon;
};

Point corpusPoint(Shape shape, SplitMix64& rng) {
  double a = unit(rng);
  double b = unit(rng);
  switch (shape) {
    case Shape::UNIFORM:
      return {-90.0 + 180.0 * a, -180.0 + 360.0 * b};
    case Shape::CLUSTERED:
      return {40.7 + (a - 0.5) * 0.2, -74.0 + (b - 0.5) * 0.2};
    case Shape::DATELINE:
      return {(a - 0.5) * 20.0, wrapLon(170.0 + 20.0 * b)};
    case Shape::POLAR:
      return {std::min(90.0, 85.0 + 5.0 * a), -180.0 + 360.0 * b};
  }
  return {0.0, 0.0};
}

Point quantized(Point point) {
  return {geo::decodeLatitude(geo::encodeLatitude(point.lat)),
          geo::decodeLongitude(geo::encodeLongitude(point.lon))};
}

std::string pointText(Point point) {
  return std::format("{:.17g},{:.17g}", point.lat, point.lon);
}

struct CorpusDoc {
  std::string id;
  std::string tag;
  std::optional<Point> single;
  std::vector<Point> multi;
  bool deleted = false;
};

void setSchema(CollectionHelper& helper) {
  SchemaBuilder b;
  for (auto [name, index, multi] : {std::tuple{"geo", true, false},
                                    std::tuple{"geos", true, true},
                                    std::tuple{"geon", false, false}}) {
    auto& field = b.field(name);
    field.type = api::FieldDef::FieldClass::GEO_POINT;
    field.index = index ? api::FieldDef::IndexMode::RANGE : api::FieldDef::IndexMode::NONE;
    field.multi = multi;
  }
  b.set(helper.collection());
}

// Indexes `count` docs over `segments` commits, deleting about one in nine
// of all but the last segment's.
// Every 13th doc repeats an earlier point so distances tie.
std::vector<CorpusDoc> buildCorpus(CollectionHelper& helper, Shape shape, int32_t count,
                                   int32_t segments, uint64_t seed) {
  SplitMix64 rng(seed);
  std::vector<CorpusDoc> docs;
  for (int32_t i = 0; i < count; i++) {
    CorpusDoc doc;
    doc.id = "d" + std::to_string(i);
    doc.tag = i % 50 == 0 ? "rare" : (i % 3 == 0 ? "c" : "ab");
    if (i % 17 != 0) {
      Point point = i % 13 == 0 && !docs.empty() && docs.back().single
          ? *docs.back().single : corpusPoint(shape, rng);
      doc.single = point;
    }
    if (i % 11 != 0) {
      int32_t points = 1 + (int32_t)(rng() % 3);
      for (int32_t p = 0; p < points; p++) doc.multi.push_back(corpusPoint(shape, rng));
    }
    docs.push_back(doc);
  }
  // Deletes hit the earlier segments; the last one is indexed after them,
  // so it has no deleted docs and an unfiltered search gives it no domain.
  int32_t perSegment = (count + segments - 1) / segments;
  auto indexRange = [&](int32_t start, int32_t end) {
    std::vector<Doc> batch;
    for (int32_t i = start; i < end; i++) {
      const CorpusDoc& doc = docs[(size_t)i];
      Doc out{{"id", doc.id}, {"tag_s", doc.tag}};
      if (doc.single) {
        out.push_back({"geo", pointText(*doc.single)});
        out.push_back({"geon", pointText(*doc.single)});
      }
      if (!doc.multi.empty()) {
        std::vector<std::string> texts;
        for (Point point : doc.multi) texts.push_back(pointText(point));
        out.push_back({"geos", texts});
      }
      batch.push_back(std::move(out));
    }
    EXPECT_EQ(IndexResult::Status::OK, helper.indexAll(batch, UpdateMessage::COMMIT).status);
  };
  int32_t lastStart = perSegment * (segments - 1);
  for (int32_t start = 0; start < lastStart; start += perSegment) {
    indexRange(start, std::min(lastStart, start + perSegment));
  }
  std::vector<std::string> deletes;
  for (int32_t i = 4; i < lastStart; i += 9) {
    docs[(size_t)i].deleted = true;
    deletes.push_back(docs[(size_t)i].id);
  }
  EXPECT_EQ(IndexResult::Status::OK, helper.deleteByIds(deletes, UpdateMessage::COMMIT).status);
  indexRange(lastStart, count);
  return docs;
}

// The doc's key for a distance from (lat, lon): nearest point by default.
std::optional<double> docKey(const CorpusDoc& doc, std::string_view field, double lat,
                             double lon, bool farthest = false) {
  std::vector<Point> points;
  if (field == "geos") points = doc.multi;
  else if (doc.single) points.push_back(*doc.single);
  if (points.empty()) return std::nullopt;
  double best = farthest ? -INF : INF;
  for (Point point : points) {
    Point q = quantized(point);
    double key = geo::haversinSortKey(lat, lon, q.lat, q.lon);
    best = farthest ? std::max(best, key) : std::min(best, key);
  }
  return best;
}

struct Hits {
  std::vector<std::string> ids;
  std::vector<float> scores;
  std::vector<std::optional<double>> sortValues;
};

Hits readHits(LocalReq& req) {
  Hits hits;
  for (const auto& response : req.responses) {
  const auto* op = response->proto.ops.find("q");
  if (op == nullptr || (*op)->docList() == nullptr) continue;
  const auto* list = (*op)->docList();
  if (const auto* id = list->columns.find("id")) {
    for (auto value : std::get<api::ColStr>(id->kind).v) hits.ids.emplace_back(value);
  }
  if (const auto* score = list->columns.find("_score_")) {
    for (float value : std::get<api::ColFloat>(score->kind).v) hits.scores.push_back(value);
  }
  if (const auto* sort = list->columns.find("_sort_")) {
    for (const auto& value : std::get<api::ColVal>(sort->kind).v) {
      const auto& arr = std::get<api::ArrVal>(value.kind);
      const auto* meters = std::get_if<double>(&arr.v[0].kind);
      hits.sortValues.push_back(meters ? std::optional<double>(*meters) : std::nullopt);
    }
  }
  }
  return hits;
}

const CorpusDoc& byId(const std::vector<CorpusDoc>& docs, const std::string& id) {
  return docs[(size_t)std::stoi(id.substr(1))];
}

class GeoNearestQueryTest : public LuxirTest {};

TEST_F(GeoNearestQueryTest, matchesBruteForceAcrossShapesFieldsFiltersAndSegments) {
  SkipStatsScope stats;
  for (Shape shape : {Shape::UNIFORM, Shape::CLUSTERED, Shape::DATELINE, Shape::POLAR}) {
    CollectionHelper helper;
    helper.clear();
    setSchema(helper);
    int32_t count = (int32_t)scaleTestWork(1500);
    auto docs = buildCorpus(helper, shape, count, 3, 0xabc + (uint64_t)shape);
    std::vector<Point> centers{{0.0, 0.0}, {40.7128, -74.0060}, {0.5, 179.95},
                               {0.5, -179.95}, {90.0, 0.0}, {-90.0, 10.0}, {88.0, -170.0}};
    for (Point center : centers) {
      for (std::string_view field : {"geo", "geos", "geon"}) {
        for (int32_t k : {1, 7, 64, count + 10}) {
          for (std::string_view tag : {"", "c", "rare"}) {
            for (double maxMeters : {INF, 500000.0}) {
              std::vector<double> expected;
              for (const CorpusDoc& doc : docs) {
                if (doc.deleted || (!tag.empty() && doc.tag != tag)) continue;
                auto key = docKey(doc, field, center.lat, center.lon);
                if (!key || (maxMeters != INF && *key > geo::distanceQuerySortKey(maxMeters))) continue;
                expected.push_back(*key);
              }
              std::sort(expected.begin(), expected.end());
              if ((int64_t)expected.size() > k) expected.resize((size_t)k);

              auto req = localReq(luxirNode->getSearchEngine());
              auto& cur = req->collection("main").topDocs("q");
              cur.rawQuery() = qb::geoNearest(cur.mr(), field, center.lat, center.lon, k,
                  maxMeters == INF ? std::nullopt : std::optional<double>(maxMeters));
              if (!tag.empty()) cur.matchFilter("tag_s", tag);
              cur.fields({"id"}).getScores().limit(-1)
                  .documentFormat(api::DocFormat::COLUMNS);
              req->execute();
              ASSERT_TRUE(req->ok()) << req->toString();
              Hits hits = readHits(*req);
              std::vector<double> actual;
              for (const auto& id : hits.ids) {
                actual.push_back(*docKey(byId(docs, id), field, center.lat, center.lon));
              }
              std::string where = std::format("shape {} center {},{} field {} k {} tag '{}' max {}",
                  (int)shape, center.lat, center.lon, field, k, tag, maxMeters);
              ASSERT_EQ(expected.size(), actual.size()) << where;
              // Hits rank by score, which falls with distance.
              for (size_t i = 0; i + 1 < hits.scores.size(); i++) {
                ASSERT_GE(hits.scores[i], hits.scores[i + 1]) << where;
                if (hits.scores[i] > hits.scores[i + 1]) {
                  ASSERT_LE(actual[i], actual[i + 1]) << where;
                }
              }
              for (size_t i = 0; i < actual.size(); i++) {
                EXPECT_FLOAT_EQ((float)(1.0 / (1.0 + geo::haversinMeters(actual[i]))),
                                hits.scores[i]) << where;
              }
              std::sort(actual.begin(), actual.end());
              EXPECT_EQ(expected, actual) << where;
            }
          }
        }
      }
    }
  }
  EXPECT_GT(SkipStats::geoNearestWalks, 0);
  EXPECT_GT(SkipStats::geoNearestSparse, 0);
  EXPECT_GT(SkipStats::geoNearestScans, 0);
}

TEST_F(GeoNearestQueryTest, walkStopsEarlyAndTiesKeepIndexOrder) {
  SkipStatsScope stats;
  CollectionHelper helper;
  setSchema(helper);
  std::vector<Doc> batch;
  int32_t count = 20000;
  SplitMix64 rng(99);
  for (int32_t i = 0; i < count; i++) {
    // Docs 0..9 share one point; the rest spread over the globe.
    Point point = i < 10 ? Point{10.0, 10.0} : corpusPoint(Shape::UNIFORM, rng);
    batch.push_back({{"id", "d" + std::to_string(i)}, {"geo", pointText(point)}});
  }
  ASSERT_EQ(IndexResult::Status::OK, helper.indexAll(batch, UpdateMessage::COMMIT).status);
  auto req = localReq(luxirNode->getSearchEngine());
  auto& cur = req->collection("main").topDocs("q");
  cur.rawQuery() = qb::geoNearest(cur.mr(), "geo", 10.0, 10.0, 4, std::nullopt);
  cur.fields({"id"}).getScores().documentFormat(api::DocFormat::COLUMNS);
  req->execute();
  Hits hits = readHits(*req);
  EXPECT_EQ((std::vector<std::string>{"d0", "d1", "d2", "d3"}), hits.ids);
  EXPECT_EQ(1, SkipStats::geoNearestWalks);
  // 20000 points are 40 leaves; the walk decodes the one holding the ties.
  EXPECT_LE(SkipStats::geoNearestLeaves, 2);
}

TEST_F(GeoNearestQueryTest, rejectsBadArguments) {
  CollectionHelper helper;
  setSchema(helper);
  helper.index({{"id", "a"}, {"geo", "1,2"}, {"tag_s", "x"}}, UpdateMessage::COMMIT);
  auto run = [&](std::string_view field, double lat, double lon, int32_t k,
                 std::optional<double> max) {
    auto req = localReq(luxirNode->getSearchEngine());
    auto& cur = req->collection("main").topDocs("q");
    cur.rawQuery() = qb::geoNearest(cur.mr(), field, lat, lon, k, max);
    req->execute();
    return req->ok() ? std::string() : req->errorMsg();
  };
  EXPECT_EQ("", run("geo", 1, 2, 1, std::nullopt));
  EXPECT_NE("", run("tag_s", 1, 2, 1, std::nullopt));
  EXPECT_NE("", run("geo", 91, 2, 1, std::nullopt));
  EXPECT_NE("", run("geo", 1, 181, 1, std::nullopt));
  EXPECT_NE("", run("geo", 1, 2, 0, std::nullopt));
  EXPECT_NE("", run("geo", 1, 2, 1, -1.0));
}

// ----------------------------------------------------------------------------

class GeoDistanceSortTest : public LuxirTest {};

TEST_F(GeoDistanceSortTest, matchesBruteForceWithAndWithoutPruning) {
  for (Shape shape : {Shape::UNIFORM, Shape::CLUSTERED, Shape::DATELINE, Shape::POLAR}) {
    CollectionHelper helper;
    helper.clear();
    setSchema(helper);
    int32_t count = (int32_t)scaleTestWork(3000);
    auto docs = buildCorpus(helper, shape, count, 2, 0xdef + (uint64_t)shape);
    for (Point center : {Point{0.0, 0.0}, Point{40.7, -74.0}, Point{0.0, 179.99},
                         Point{89.9, 100.0}}) {
      struct Case { std::string expr; std::string_view field; bool farthest; bool desc; };
      std::string origin = std::format("{}, {}", center.lat, center.lon);
      std::vector<Case> cases{
          {"geo_distance(geo, " + origin + ")", "geo", false, false},
          {"geo_distance(geo, " + origin + ")", "geo", false, true},
          {"geo_distance(geon, " + origin + ")", "geon", false, false},
          {"min(geo_distance(geos, " + origin + "))", "geos", false, false},
          {"max(geo_distance(geos, " + origin + "))", "geos", true, false},
          {"max(geo_distance(geos, " + origin + "))", "geos", true, true}};
      for (const Case& c : cases) {
        for (int32_t limit : {1, 10, 100}) {
          for (std::string_view tag : {"", "ab"}) {
            std::vector<std::pair<double, std::string>> expected;
            int64_t missing = 0;
            for (const CorpusDoc& doc : docs) {
              if (doc.deleted || (!tag.empty() && doc.tag != tag)) continue;
              auto key = docKey(doc, c.field, center.lat, center.lon, c.farthest);
              if (!key) {
                missing++;
                continue;
              }
              expected.push_back({c.desc ? -*key : *key, doc.id});
            }
            std::sort(expected.begin(), expected.end());
            std::vector<std::optional<double>> expectedMeters;
            for (const auto& [key, id] : expected) {
              if ((int32_t)expectedMeters.size() == limit) break;
              expectedMeters.push_back(geo::haversinMeters(c.desc ? -key : key));
            }
            while ((int32_t)expectedMeters.size() < limit && missing-- > 0) {
              expectedMeters.push_back(std::nullopt);
            }
            for (bool exactCount : {false, true}) {
              SkipStatsScope stats;
              auto req = localReq(luxirNode->getSearchEngine());
              auto& cur = req->collection("main").topDocs("q");
              cur.allQuery();
              if (!tag.empty()) cur.matchFilter("tag_s", tag);
              cur.sortBy(c.expr, c.desc);
              cur.fields({"id", "_sort_"}).limit(limit).documentFormat(api::DocFormat::COLUMNS);
              if (exactCount) cur.countLimit(-1);
              req->execute();
              ASSERT_TRUE(req->ok()) << req->toString();
              Hits hits = readHits(*req);
              std::string where = std::format("shape {} sort '{}' desc {} limit {} tag '{}' exact {}",
                  (int)shape, c.expr, c.desc, limit, tag, exactCount);
              ASSERT_EQ(expectedMeters.size(), hits.sortValues.size()) << where;
              for (size_t i = 0; i < hits.ids.size(); i++) {
                ASSERT_EQ(expectedMeters[i].has_value(), hits.sortValues[i].has_value()) << where;
                if (!expectedMeters[i]) continue;
                EXPECT_DOUBLE_EQ(*expectedMeters[i], *hits.sortValues[i]) << where << " rank " << i;
                auto own = docKey(byId(docs, hits.ids[i]), c.field, center.lat, center.lon, c.farthest);
                EXPECT_DOUBLE_EQ(geo::haversinMeters(*own), *hits.sortValues[i]) << where;
              }
            }
          }
        }
      }
    }
  }
}

TEST_F(GeoDistanceSortTest, pointsIndexRestrictsAscendingTopK) {
  CollectionHelper helper;
  setSchema(helper);
  std::vector<Doc> batch;
  int32_t count = 60000;
  SplitMix64 rng(5);
  std::vector<std::string> deletes;
  for (int32_t i = 0; i < count; i++) {
    std::string point = pointText(corpusPoint(Shape::UNIFORM, rng));
    std::string second = pointText(corpusPoint(Shape::UNIFORM, rng));
    batch.push_back({{"id", "d" + std::to_string(i)}, {"geo", point}, {"geon", point},
                     {"geos", std::vector<std::string>{point, second}}});
    if (i % 7 == 0) deletes.push_back("d" + std::to_string(i));
  }
  ASSERT_EQ(IndexResult::Status::OK, helper.indexAll(batch, UpdateMessage::COMMIT).status);
  ASSERT_EQ(IndexResult::Status::OK, helper.deleteByIds(deletes, UpdateMessage::COMMIT).status);
  auto run = [&](std::string_view expr, int32_t limit) {
    auto req = localReq(luxirNode->getSearchEngine());
    auto& cur = req->collection("main").topDocs("q");
    cur.allQuery();
    cur.sortBy(expr);
    cur.fields({"id", "_sort_"}).limit(limit).documentFormat(api::DocFormat::COLUMNS);
    req->execute();
    EXPECT_TRUE(req->ok()) << req->toString();
    return readHits(*req);
  };
  int64_t builds = 0;
  for (Point center : {Point{12.5, -40.25}, Point{89.99, 0.0}, Point{0.0, 180.0},
                       Point{-90.0, 0.0}}) {
    for (int32_t limit : {1, 10, 100}) {
      std::string origin = std::format("{}, {}", center.lat, center.lon);
      // geon holds the same points without a points index, so it never prunes.
      Hits plain;
      {
        SkipStatsScope stats;
        plain = run("geo_distance(geon, " + origin + ")", limit);
        EXPECT_EQ(0, SkipStats::geoSortCircleBuilds);
      }
      SkipStatsScope stats;
      Hits pruned = run("geo_distance(geo, " + origin + ")", limit);
      Hits multi = run("min(geo_distance(geos, " + origin + "))", limit);
      builds += SkipStats::geoSortCircleBuilds;
      EXPECT_EQ(plain.ids, pruned.ids) << origin << " limit " << limit;
      EXPECT_EQ(plain.sortValues, pruned.sortValues) << origin << " limit " << limit;
      ASSERT_EQ((size_t)limit, multi.ids.size());
      for (size_t i = 0; i < multi.ids.size(); i++) {
        // Every doc's geo point is one of its geos points, so the i-th
        // nearest doc by geos is no farther than the i-th by geo.
        EXPECT_LE(*multi.sortValues[i], *pruned.sortValues[i] + 1e-6) << origin;
        if (i > 0) {
          EXPECT_LE(*multi.sortValues[i - 1], *multi.sortValues[i]);
        }
      }
    }
  }
  EXPECT_GT(builds, 0);
}

TEST_F(GeoDistanceSortTest, valueFunctionAndErrors) {
  CollectionHelper helper;
  setSchema(helper);
  helper.indexAll({{{"id", "a"}, {"geo", "0,0"}, {"tag_s", "x"}},
                   {{"id", "b"}, {"geo", "0,1"}, {"tag_s", "x"}},
                   {{"id", "c"}, {"tag_s", "x"}}},
                  UpdateMessage::COMMIT);
  auto run = [&](std::string_view expr, bool desc = false) {
    auto req = localReq(luxirNode->getSearchEngine());
    auto& cur = req->collection("main").topDocs("q");
    cur.allQuery();
    cur.sortBy(expr, desc);
    cur.fields({"id", "_sort_"}).documentFormat(api::DocFormat::COLUMNS);
    req->execute();
    std::string error = req->ok() ? std::string() : req->errorMsg();
    return std::pair(std::move(req), error);
  };
  {
    auto [req, error] = run("geo_distance(geo, 0, 0.9)");
    ASSERT_EQ("", error);
    Hits hits = readHits(*req);
    EXPECT_EQ((std::vector<std::string>{"b", "a", "c"}), hits.ids);
    EXPECT_NEAR(geo::haversinMeters(0, 0.9, 0, 1), *hits.sortValues[0], 0.01);
    EXPECT_FALSE(hits.sortValues[2].has_value());
  }
  {
    // Missing values sort last descending too.
    auto [req, error] = run("geo_distance(geo, 0, 0.9)", true);
    ASSERT_EQ("", error);
    EXPECT_EQ((std::vector<std::string>{"a", "b", "c"}), readHits(*req).ids);
  }
  {
    // A wider expression evaluates per doc, without the comparator.
    auto [req, error] = run("def(geo_distance(geo, 0, 0.9), 0) * 2");
    ASSERT_EQ("", error);
    EXPECT_EQ((std::vector<std::string>{"c", "b", "a"}), readHits(*req).ids);
  }
  EXPECT_NE("", run("geo_distance(geos, 0, 0)").second);  // multi-valued needs a reducer
  EXPECT_NE("", run("geo_distance(tag_s, 0, 0)").second);
  EXPECT_NE("", run("geo_distance(geo, 91, 0)").second);
  EXPECT_NE("", run("geo").second);  // a geo column alone is not sortable
}

} // namespace
