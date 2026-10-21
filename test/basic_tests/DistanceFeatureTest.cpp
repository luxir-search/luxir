// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <format>
#include <limits>
#include <optional>
#include <string>
#include <vector>

#include "luxir/query/BooleanQuery.h"
#include "luxir/query/DistanceFeatureQuery.h"
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

double unit(SplitMix64& rng) {
  return (double)(rng() >> 11) * (1.0 / 9007199254740992.0);
}

struct Point {
  double lat;
  double lon;
};

struct CorpusDoc {
  std::string id;
  std::vector<Point> points;
  bool deleted = false;
};

std::string pointText(Point point) {
  return std::format("{:.17g},{:.17g}", point.lat, point.lon);
}

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

// Points near New York, so a pivot of a few kilometers spreads the scores.
// Body text repeats "coffee" 0-3 times so text scores vary too.
std::vector<CorpusDoc> buildCorpus(CollectionHelper& helper, int32_t count, int32_t segments,
                                   uint64_t seed) {
  SplitMix64 rng(seed);
  std::vector<CorpusDoc> docs;
  int32_t perSegment = (count + segments - 1) / segments;
  for (int32_t start = 0; start < count; start += perSegment) {
    std::vector<Doc> batch;
    for (int32_t i = start; i < std::min(count, start + perSegment); i++) {
      CorpusDoc doc;
      doc.id = "d" + std::to_string(i);
      Doc out{{"id", doc.id}};
      std::string body = i % 5 == 0 ? "tea" : "coffee";
      for (int32_t r = 0; r < (int32_t)(rng() % 3); r++) body += " coffee";
      body += i % 2 == 0 ? " shop" : " bar";
      out.push_back({"body_t", body});
      if (i % 13 != 0) {
        int32_t points = 1 + (int32_t)(rng() % 2);
        std::vector<std::string> texts;
        for (int32_t p = 0; p < points; p++) {
          doc.points.push_back({40.7 + (unit(rng) - 0.5) * 2.0, -74.0 + (unit(rng) - 0.5) * 2.0});
          texts.push_back(pointText(doc.points.back()));
        }
        out.push_back({"geo", pointText(doc.points[0])});
        out.push_back({"geon", pointText(doc.points[0])});
        out.push_back({"geos", texts});
      }
      batch.push_back(std::move(out));
      docs.push_back(std::move(doc));
    }
    EXPECT_EQ(IndexResult::Status::OK, helper.indexAll(batch, UpdateMessage::COMMIT).status);
  }
  std::vector<std::string> deletes;
  for (int32_t i = 3; i < count; i += 10) {
    docs[(size_t)i].deleted = true;
    deletes.push_back(docs[(size_t)i].id);
  }
  EXPECT_EQ(IndexResult::Status::OK, helper.deleteByIds(deletes, UpdateMessage::COMMIT).status);
  return docs;
}

std::optional<double> nearestMeters(const CorpusDoc& doc, std::string_view field, Point origin) {
  if (doc.points.empty()) return std::nullopt;
  size_t points = field == "geos" ? doc.points.size() : 1;
  double best = std::numeric_limits<double>::infinity();
  for (size_t i = 0; i < points; i++) {
    double lat = geo::decodeLatitude(geo::encodeLatitude(doc.points[i].lat));
    double lon = geo::decodeLongitude(geo::encodeLongitude(doc.points[i].lon));
    best = std::min(best, geo::haversinSortKey(origin.lat, origin.lon, lat, lon));
  }
  return geo::haversinMeters(best);
}

struct Hit {
  std::string id;
  float score;
};

std::vector<Hit> search(api::Query query, int64_t limit, bool exactCount,
                        float boost = 1.0f, std::optional<float> minScore = std::nullopt) {
  auto req = localReq(LuxirTest::luxirNode->getSearchEngine());
  auto& cur = req->collection("main").topDocs("q");
  if (boost != 1.0f) {
    auto* child = (api::Query*)cur.mr().allocate(sizeof(api::Query), alignof(api::Query));
    new (child) api::Query(query);
    api::Query wrapped;
    auto& b = wrapped.kind.emplace<api::BoostQuery>();
    b.query = child;
    b.boost = boost;
    query = wrapped;
  }
  cur.rawQuery() = query;
  cur.fields({"id"}).getScores().limit(limit).documentFormat(api::DocFormat::COLUMNS);
  if (exactCount) cur.countLimit(-1);
  if (minScore) cur.minScore(*minScore);
  req->execute();
  EXPECT_TRUE(req->ok()) << req->toString();
  std::vector<Hit> hits;
  for (const auto& response : req->responses) {
    const auto* op = response->proto.ops.find("q");
    if (op == nullptr || (*op)->docList() == nullptr) continue;
    const auto* list = (*op)->docList();
    const auto* ids = list->columns.find("id");
    const auto* scores = list->columns.find("_score_");
    if (ids == nullptr || scores == nullptr) continue;
    const auto& idCol = std::get<api::ColStr>(ids->kind).v;
    const auto& scoreCol = std::get<api::ColFloat>(scores->kind).v;
    for (size_t i = 0; i < idCol.size(); i++) hits.push_back({std::string(idCol[i]), scoreCol[i]});
  }
  return hits;
}

class DistanceFeatureTest : public LuxirTest {};

TEST_F(DistanceFeatureTest, scoresMatchBruteForceAndTopKIsUnchangedBySkipping) {
  CollectionHelper helper;
  setSchema(helper);
  int32_t count = (int32_t)scaleTestWork(20000);
  auto docs = buildCorpus(helper, count, 3, 42);
  for (Point origin : {Point{40.7128, -74.0060}, Point{41.5, -73.2}, Point{0.0, 0.0}}) {
    for (std::string_view field : {"geo", "geos", "geon"}) {
      int64_t builds = 0;
      for (double pivot : {50.0, 2000.0, 1e6}) {
        for (float boost : {1.0f, 3.5f}) {
          std::string where = std::format("origin {},{} field {} pivot {} boost {}",
                                          origin.lat, origin.lon, field, pivot, boost);
          // Every scored doc, exhaustively: its score is the formula's.
          std::pmr::monotonic_buffer_resource mr;
          api::Query query = qb::distanceFeature(mr, field, origin.lat, origin.lon, pivot);
          auto all = search(query, -1, true, boost);
          int64_t live = 0;
          for (const CorpusDoc& doc : docs) live += !doc.deleted && !doc.points.empty();
          ASSERT_EQ(live, (int64_t)all.size()) << where;
          for (const Hit& hit : all) {
            const CorpusDoc& doc = docs[(size_t)std::stoi(hit.id.substr(1))];
            ASSERT_FALSE(doc.deleted) << where;
            float expected = DistanceFeatureQuery::scoreAt(
                boost, pivot, *nearestMeters(doc, field, origin));
            ASSERT_FLOAT_EQ(expected, hit.score) << where << " " << hit.id;
          }
          for (int64_t k : {1, 10, 100}) {
            SkipStatsScope stats;
            auto pruned = search(query, k, false, boost);
            builds += SkipStats::distanceFeatureCircleBuilds;
            ASSERT_EQ((size_t)k, pruned.size()) << where;
            for (size_t i = 0; i < pruned.size(); i++) {
              EXPECT_EQ(all[i].id, pruned[i].id) << where << " k " << k << " rank " << i;
              EXPECT_EQ(all[i].score, pruned[i].score) << where << " k " << k;
            }
          }
        }
      }
      // Only a points index can narrow the iteration, and from (0, 0) every
      // doc is about as far, so no circle excludes enough of them.
      if (field == "geon") {
        EXPECT_EQ(0, builds) << origin.lat << "," << origin.lon;
      } else if (origin.lat != 0.0) {
        EXPECT_GT(builds, 0) << field << " " << origin.lat << "," << origin.lon;
      }
    }
  }
}

// min_score seeds the floor before collection; a doc scoring exactly the
// cut-off still matches, so the count is exact.
TEST_F(DistanceFeatureTest, minScoreCountsMatchBruteForce) {
  CollectionHelper helper;
  setSchema(helper);
  auto docs = buildCorpus(helper, (int32_t)scaleTestWork(20000), 2, 11);
  std::pmr::monotonic_buffer_resource mr;
  Point origin{40.7128, -74.0060};
  for (std::string_view field : {"geo", "geos"}) {
    for (double pivot : {200.0, 1e6, 5e6}) {
      auto query = qb::distanceFeature(mr, field, origin.lat, origin.lon, pivot);
      std::vector<float> scores;
      for (const CorpusDoc& doc : docs) {
        if (doc.deleted || doc.points.empty()) continue;
        scores.push_back(DistanceFeatureQuery::scoreAt(1.0f, pivot, *nearestMeters(doc, field, origin)));
      }
      std::sort(scores.begin(), scores.end(), std::greater<>());
      // Cut-offs at existing scores make ties at the floor.
      for (size_t rank : {(size_t)0, (size_t)9, scores.size() / 3}) {
        float cutoff = scores[rank];
        int64_t expected = std::count_if(scores.begin(), scores.end(),
                                         [&](float score) { return score >= cutoff; });
        SkipStatsScope stats;
        auto hits = search(query, -1, false, 1.0f, cutoff);
        EXPECT_EQ(expected, (int64_t)hits.size()) << field << " pivot " << pivot << " rank " << rank;
      }
    }
  }
}


// Proximity as a clause beside text: pruned top-k (the floor reaches the
// distance_feature scorer through the boolean scorer, which shrinks its
// circle) must equal the exhaustive ranking.
TEST_F(DistanceFeatureTest, textAndProximityTopKMatchesExhaustive) {
  CollectionHelper helper;
  setSchema(helper);
  auto docs = buildCorpus(helper, (int32_t)scaleTestWork(40000), 2, 7);
  std::pmr::monotonic_buffer_resource mr;
  for (std::string_view field : {"geo", "geos"}) {
    for (double pivot : {300.0, 5000.0}) {
      auto df = qb::distanceFeature(mr, field, 40.7128, -74.0060, pivot);
      auto text = qb::match(mr, "body_t", "coffee");
      auto shop = qb::match(mr, "body_t", "shop");
      std::vector<std::pair<std::string, api::Query>> shapes{
          {"alone", df},
          {"must_text_should_df", qb::boolean(mr, {text}, {df})},
          {"filter_text_should_df", qb::boolean(mr, {}, {df}, {}, {text})},
          {"should_text_df", qb::boolean(mr, {}, {text, df})},
          {"should_two_texts_df", qb::boolean(mr, {}, {text, shop, df})},
          {"should_text_two_dfs", qb::boolean(mr, {}, {text, df,
              qb::distanceFeature(mr, field, 40.9, -73.7, pivot * 3)})},
      };
      for (auto& [name, query] : shapes) {
       // Bulk scorers by default; pull scorers with the MandOpt bulk off.
       for (bool pull : {false, true}) {
        BooleanQuery::disableMandOptBulkForTests = pull;
        int64_t builds = 0;
        for (float boost : {1.0f, 4.0f}) {
          std::vector<Hit> all;
          {
            SkipStatsScope stats;
            DistanceFeatureQuery::disableSkippingForTests = true;
            all = search(query, 300, false, boost);
            DistanceFeatureQuery::disableSkippingForTests = false;
            EXPECT_EQ(0, SkipStats::distanceFeatureCircleBuilds) << name;
          }
          for (int64_t k : {1, 10, 100}) {
            SkipStatsScope stats;
            auto pruned = search(query, k, false, boost);
            builds += SkipStats::distanceFeatureCircleBuilds;
            std::string where = std::format("{} field {} pivot {} boost {} k {} pull {}",
                                            name, field, pivot, boost, k, pull);
            ASSERT_EQ((size_t)k, pruned.size()) << where;
            for (size_t i = 0; i < pruned.size(); i++) {
              EXPECT_EQ(all[i].id, pruned[i].id) << where << " rank " << i;
              // A disjunction's float sum depends on its clause order per
              // window, so scores may differ in the last bit across paths.
              EXPECT_FLOAT_EQ(all[i].score, pruned[i].score) << where << " rank " << i;
            }
          }
        }
        BooleanQuery::disableMandOptBulkForTests = false;
        // Two distance_feature clauses can each add up to their boost, so
        // with a small pivot neither one's floor rises above zero.
        if (name != "should_text_two_dfs") {
          EXPECT_GT(builds, 0) << name << " " << field << " " << pivot << " pull " << pull;
        }
       }
      }
    }
  }
}

TEST_F(DistanceFeatureTest, rejectsBadArguments) {
  CollectionHelper helper;
  setSchema(helper);
  helper.index({{"id", "a"}, {"geo", "1,2"}, {"body_t", "x"}}, UpdateMessage::COMMIT);
  auto run = [&](std::string_view field, double lat, double lon, double pivot) {
    auto req = localReq(luxirNode->getSearchEngine());
    auto& cur = req->collection("main").topDocs("q");
    cur.rawQuery() = qb::distanceFeature(cur.mr(), field, lat, lon, pivot);
    req->execute();
    return req->ok() ? std::string() : req->errorMsg();
  };
  EXPECT_EQ("", run("geo", 1, 2, 10));
  EXPECT_NE("", run("body_t", 1, 2, 10));
  EXPECT_NE("", run("geo", 91, 2, 10));
  EXPECT_NE("", run("geo", 1, 2, 0));
  EXPECT_NE("", run("geo", 1, 2, -5));
}

}  // namespace
