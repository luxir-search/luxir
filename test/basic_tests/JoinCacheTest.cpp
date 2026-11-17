// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include "test/CollectionHelper.h"
#include "test/LocalReq.h"
#include "test/LuxirTest.h"
#include "test/QueryBuild.h"
#include "test/SchemaBuilder.h"
#include "luxir/reader/SkipStats.h"

namespace luxir::test {
namespace {

class JoinCacheTest : public LuxirTest {
protected:
  CollectionHelper helper;
  std::shared_ptr<FilterCache> cache;

  void SetUp() override {
    LuxirTest::SetUp();
    cache = std::make_shared<FilterCache>(FilterCacheConfig{
        .maxBytes = 1024 * 1024, .minSegmentDocs = 0, .admissionThreshold = 2});
    helper.getIndexWriter()->snapshots.readers.filterCache = cache;
    CollectionHelper::UpdateBuilder input;
    for (int32_t i = 0; i < 24; i++) {
      auto json = "{\"id\":\"" + std::to_string(i) + "\",\"items_nested\":[{\"color_s\":\""
          + (i % 2 ? "blue" : "red") + "\",\"size_s\":\"" + (i % 3 ? "S" : "L")
          + "\",\"deep_nested\":{\"text_w\":\"" + (i % 4 ? "other" : "deep common")
          + "\"}},{\"color_s\":\"blue\",\"size_s\":\"L\"}]}";
      input.addRaw([&](auto& doc, auto& arena) { ASSERT_TRUE(api::read_json(doc, json, arena)); });
    }
    input.commit();
    ASSERT_TRUE(helper.submit(input).success);
  }

  int64_t count(int32_t position = 0, bool maximum = false, int32_t kind = 0,
                bool restricted = false, bool prepared = false) {
    auto req = localReq(helper.getSearchEngine());
    req->testForcePrepare = prepared;
    auto& mr = req->mr;
    auto inner = kind == 1 ? qb::boolean(mr, {qb::match(mr, "color_s", "red"), qb::match(mr, "size_s", "L")})
        : kind == 4 ? qb::fuzzy(mr, "color_s", "red", 1) : qb::match(mr, "color_s", "red");
    if (kind == 2 || kind == 3) inner = qb::phraseText(mr, "text_w", "deep common");
    if (kind == 3) inner = qb::toParent(mr, "deep_nested", inner, "none");
    auto join = qb::toParent(mr, kind == 2 ? "items_nested.deep_nested" : "items_nested", inner,
        maximum ? "max" : "none");
    auto& op = req->topDocs().countLimit().limit(0);
    if (position == 0) op.rawQuery() = join;
    else if (position == 1) op.allQuery().filter(join);
    else if (position == 2) op.rawQuery() = qb::boolean(req->mr, {}, {}, {}, {join});
    else op.rawQuery() = qb::boolean(req->mr, {qb::all()}, {}, {join});
    if (restricted) op.matchFilter("id", "0");
    req->execute(1);
    EXPECT_OK(req);
    return req->getMatchCount();
  }
};

TEST_F(JoinCacheTest, explicitFilterAdmitsAndServesReducerIndependentMembership) {
  EXPECT_EQ(12, count(1));
  EXPECT_EQ(0u, cache->counters().admissions);
  EXPECT_EQ(12, count(1));
  EXPECT_GE(cache->counters().admissions, 1u);
  EXPECT_GE(cache->counters().builds, 1u);
  auto hits = cache->counters().hits;
  EXPECT_EQ(12, count(1, true));
  EXPECT_GT(cache->counters().hits, hits);
}

TEST_F(JoinCacheTest, mainFilterAndProhibitedPositionsAdmitOnSecondSighting) {
  for (int32_t position : {0, 1, 2, 3}) {
    cache->clear();
    cache->resetAdmission();
    cache->resetCounters();
    EXPECT_EQ(12, count(position));
    EXPECT_EQ(0u, cache->counters().admissions);
    EXPECT_EQ(12, count(position));
    EXPECT_GT(cache->counters().admissions, 0u);
    EXPECT_GT(cache->counters().builds, 0u);
    auto hits = cache->counters().hits;
    SkipStatsScope stats;
    EXPECT_EQ(12, count(position, true));
    EXPECT_GT(cache->counters().hits, hits);
    if (position == 0) { EXPECT_EQ(1, SkipStats::cacheFirstMembershipWeightSkips); }
  }
}

TEST_F(JoinCacheTest, canonicalPreparedJoinUsesTheSameRawWholeMembershipLane) {
  EXPECT_EQ(12, count(0, false, 0, false, true));
  EXPECT_EQ(12, count(0, false, 0, false, true));
  SkipStatsScope stats;
  EXPECT_EQ(12, count(0, false, 0, false, true));
  EXPECT_EQ(1, SkipStats::wholeCountHits);
  EXPECT_EQ(0, SkipStats::cacheFirstMembershipWeightSkips);
}

TEST_F(JoinCacheTest, sameItemAndTwoLevelQueriesHaveTheirOwnOwnerEntries) {
  for (int32_t kind : {1, 2, 3}) {
    int64_t expected = kind == 1 ? 4 : 6;
    EXPECT_EQ(expected, count(0, false, kind));
    EXPECT_EQ(expected, count(0, false, kind));
    SkipStatsScope stats;
    EXPECT_EQ(expected, count(0, true, kind));
    EXPECT_EQ(1, SkipStats::cacheFirstMembershipWeightSkips);
    EXPECT_EQ(1, SkipStats::wholeCountHits);
  }
}

TEST_F(JoinCacheTest, scoredRankingReusesUnscoredOwnerMembershipForItsCount) {
  EXPECT_EQ(12, count());
  EXPECT_EQ(12, count());
  auto req = localReq(helper.getSearchEngine());
  req->topDocs().countLimit().getScores().limit(3).fields({"id"}).rawQuery() = qb::toParent(req->mr,
      "items_nested", qb::match(req->mr, "color_s", "red"), "max");
  SkipStatsScope stats;
  req->execute(1);
  ASSERT_OK(req);
  EXPECT_EQ(12, req->getMatchCount());
  EXPECT_EQ(3u, req->getDocs().size());
  EXPECT_GT(SkipStats::wholeTopKCountHits, 0);
}

TEST_F(JoinCacheTest, prunedUniformMaximumRoutesResidentChildMembershipByDensity) {
  for (bool sparse : {false, true}) {
    SCOPED_TRACE(sparse);
    if (sparse) {
      std::string json = R"({"id":"padding","items_nested":{"deep_nested":[)";
      for (int i = 0; i < 2048; i++) {
        if (i) json += ',';
        json += R"({"text_w":"other"})";
      }
      json += "]}}";
      CollectionHelper::UpdateBuilder input;
      input.addRaw([&](auto& doc, auto& arena) { ASSERT_TRUE(api::read_json(doc, json, arena)); });
      input.commit(true, 1); ASSERT_TRUE(helper.submit(input).success);
    }
    for (int pass = 0; pass < 3; pass++) {
      auto req = localReq(helper.getSearchEngine());
      auto& op = req->topDocs().countLimit().limit(0);
      op.rawQuery() = qb::phraseText(req->mr, "text_w", "deep common");
      op.rawOp().domain.emplace().to = "items_nested.deep_nested";
      req->execute(1); ASSERT_OK(req);
      EXPECT_EQ(6, req->getMatchCount());
    }
    auto hits = cache->counters().hits;
    auto req = localReq(helper.getSearchEngine());
    req->topDocs().getScores().fields({"id"}).limit(3).rawQuery() = qb::boost(req->mr,
        qb::toParent(req->mr, "items_nested.deep_nested", qb::constantScore(req->mr,
            qb::phraseText(req->mr, "text_w", "deep common"), 1.0f)), 2.5f);
    req->execute(1); ASSERT_OK(req);
    if (sparse) { EXPECT_EQ(hits, cache->counters().hits); }
    else { EXPECT_GT(cache->counters().hits, hits); }
    auto scores = std::get<api::ColFloat>(req->docList()->columns.at("_score_").kind).v;
    ASSERT_EQ(3u, scores.size());
    for (auto score : scores) EXPECT_EQ(2.5f, score);
  }
}

TEST_F(JoinCacheTest, rawMembershipSurvivesRootDeletesAndAnUntouchedSegmentReopen) {
  EXPECT_EQ(1, count(1, false, 0, true));
  EXPECT_EQ(1, count(1, false, 0, true));
  SkipStatsScope stats;
  EXPECT_EQ(12, count());
  EXPECT_EQ(1, SkipStats::cacheFirstMembershipWeightSkips);
  auto first = helper.getIndexWriter()->snapshots.readers.getReader();
  auto* core = &first->segments()[0].segmentCore();
  auto hits = cache->counters().hits;
  ASSERT_TRUE(helper.deleteById("0", UpdateMessage::COMMIT).success);
  EXPECT_EQ(11, count());
  EXPECT_GT(cache->counters().hits, hits);
  ASSERT_TRUE(helper.index(flatdoc("id", "later"), UpdateMessage::COMMIT).success);
  auto next = helper.getIndexWriter()->snapshots.readers.getReader();
  ASSERT_EQ(2u, next->segments().size());
  EXPECT_EQ(core, &next->segments()[0].segmentCore());
  hits = cache->counters().hits;
  EXPECT_EQ(11, count());
  EXPECT_GT(cache->counters().hits, hits);
  cache->resize(0);
  EXPECT_EQ(11, count());
}

TEST_F(JoinCacheTest, joinDoesNotPopulateRawChildMembershipForAnotherJoin) {
  EXPECT_EQ(6, count(0, false, 2));
  EXPECT_EQ(6, count(0, false, 2));
  auto warm = cache->counters();
  // The composed join is cold. A join only borrows a resident child set and
  // does not populate raw child membership, so the first left no child hit.
  EXPECT_EQ(6, count(0, false, 3));
  EXPECT_EQ(warm.admissions, cache->counters().admissions);
  EXPECT_EQ(warm.builds, cache->counters().builds);
  EXPECT_EQ(warm.hits, cache->counters().hits);
  cache->resize(0);
  EXPECT_EQ(6, count(0, false, 2));
  EXPECT_EQ(6, count(0, false, 3));
}

TEST_F(JoinCacheTest, ownerAdmissionDoesNotRequireAnAdmittedChildEntry) {
  CollectionHelper small("join_cache_small");
  auto smallCache = std::make_shared<FilterCache>(FilterCacheConfig{
      .maxBytes = 1024 * 1024, .minSegmentDocs = 3, .admissionThreshold = 2});
  small.getIndexWriter()->snapshots.readers.filterCache = smallCache;
  CollectionHelper::UpdateBuilder input;
  for (auto json : {R"({"id":"a","items_nested":{"color_s":"red"}})", R"({"id":"b"})"}) {
    input.addRaw([&](auto& doc, auto& arena) { ASSERT_TRUE(api::read_json(doc, json, arena)); });
  }
  input.commit();
  ASSERT_TRUE(small.submit(input).success);
  for (int i = 0; i < 3; i++) {
    auto req = localReq(small.getSearchEngine());
    req->collection("join_cache_small").topDocs().countLimit().limit(0).rawQuery() = qb::toParent(req->mr,
        "items_nested", qb::match(req->mr, "color_s", "red"), "none");
    req->execute(1);
    ASSERT_OK(req);
    EXPECT_EQ(1, req->getMatchCount());
  }
  EXPECT_EQ(1u, smallCache->counters().builds);
  EXPECT_EQ(1u, smallCache->dump(10, nullptr).size());
  EXPECT_GT(smallCache->counters().hits, 0u);
}

TEST_F(JoinCacheTest, coreStableChildKeepsItsStricterScope) {
  EXPECT_EQ(12, count(0, false, 4));
  EXPECT_EQ(12, count(0, false, 4));
  // Only owner membership is cached; the join does not populate its raw child.
  auto entries = cache->dump(10, nullptr);
  ASSERT_EQ(1u, entries.size());
  for (const auto& entry : entries) { EXPECT_EQ(FilterKeyScope::CORE_STABLE, entry.scope); }
  SkipStatsScope stats;
  EXPECT_EQ(12, count(0, false, 4));
  EXPECT_EQ(1, SkipStats::cacheFirstMembershipWeightSkips);
}

TEST_F(JoinCacheTest, readerStableChildAndOwnerSelectionRespectLiveEligibility) {
  SchemaBuilder schema;
  auto& vector = schema.field("items_nested.embedding_v");
  vector.type = api::FieldDef::FieldClass::VECTOR;
  vector.dims = 2;
  vector.column = true;
  vector.metric = api::VectorMetric::L2;
  schema.set(helper.collection());
  CollectionHelper::UpdateBuilder input;
  for (auto json : {R"({"id":"v0","items_nested":{"embedding_v":[1,0]}})",
                    R"({"id":"v1","items_nested":{"embedding_v":[2,0]}})"}) {
    input.addRaw([&](auto& doc, auto& arena) { ASSERT_TRUE(api::read_json(doc, json, arena)); });
  }
  input.commit();
  ASSERT_TRUE(helper.submit(input).success);
  auto selected = [&](bool restricted, bool countOnly = false) {
    auto req = localReq(helper.getSearchEngine());
    auto& op = req->topDocs().fields({"id"}).countLimit();
    op.rawQuery() = qb::toParent(req->mr, "items_nested",
        qb::knn(req->mr, "embedding_v", {0.0f, 0.0f}, 1, 1), "none");
    if (restricted) op.matchFilter("id", "v1");
    if (countOnly) op.limit(0);
    req->execute(1);
    EXPECT_OK(req);
    EXPECT_EQ(1, req->getMatchCount());
    return req->getDocs();
  };
  selected(false, true);
  selected(false, true);
  EXPECT_EQ(2u, cache->dump(10, nullptr).size());
  for (int i = 0; i < 3; i++) { EXPECT_TRUE(containsDoc(selected(false), flatdoc("id", "v0"))); }
  EXPECT_GT(cache->counters().readerStableHits, 0u);
  for (const auto& entry : cache->dump(10, nullptr)) {
    EXPECT_EQ(FilterKeyScope::READER_STABLE, entry.scope);
    EXPECT_TRUE(entry.readerValue);
  }
  EXPECT_TRUE(containsDoc(selected(true), flatdoc("id", "v1")));
  ASSERT_TRUE(helper.deleteById("v0", UpdateMessage::COMMIT).success);
  EXPECT_TRUE(containsDoc(selected(false), flatdoc("id", "v1")));
  EXPECT_GT(cache->counters().readerStableRetires, 0u);
  cache->resize(0);
  EXPECT_TRUE(containsDoc(selected(false), flatdoc("id", "v1")));
}

} // namespace
} // namespace luxir::test
