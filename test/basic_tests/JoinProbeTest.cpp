// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include "luxir/search/OwnershipRelation.h"
#include "luxir/search/ops/TopDocsReq.h"
#include "luxir/query/NumericPredicateQuery.h"
#include "test/CollectionHelper.h"
#include "test/LocalReq.h"
#include "test/NumericVerification.h"
#include "test/QueryBuild.h"

using namespace luxir;
using namespace luxir::test;

class JoinProbeTest : public LuxirTest {
protected:
  CollectionHelper helper;
};

TEST_F(JoinProbeTest, emptyFamiliesDoNotVerifyInACompoundChild) {
  CollectionHelper::UpdateBuilder input;
  for (auto json : {
      R"({"id":"no","items_nested":[{"color_s":"red"},{"color_s":"blue","parts_nested":{"color_s":"blue"}}]})",
      R"({"id":"yes","items_nested":{"color_s":"red","parts_nested":{"color_s":"blue"}}})"}) {
    input.addRaw([&](auto& doc, auto& arena) { ASSERT_TRUE(api::read_json(doc, json, arena)); });
  }
  input.commit(); ASSERT_TRUE(helper.submit(input).success);
  auto req = localReq(helper.getSearchEngine());
  req->topDocs().countLimit().limit(-1).fields({"id"}).rawQuery() = qb::toParent(req->mr, "items_nested",
      qb::boolean(req->mr, {qb::match(req->mr, "color_s", "red"),
          qb::toParent(req->mr, "parts_nested", qb::match(req->mr, "color_s", "blue"))}), "none");
  req->execute(1); ASSERT_OK(req);
  EXPECT_EQ(1, req->getMatchCount());
}

TEST_F(JoinProbeTest, sparseFamiliesUsePointChecksAndBoundedCandidateMasks) {
  constexpr int32_t N = 2048, LARGE = 1020;
  helper.getIndexWriter()->snapshots.readers.filterCache = std::make_shared<FilterCache>(FilterCacheConfig{.maxBytes = 0});
  CollectionHelper::UpdateBuilder input;
  std::vector<int32_t> rootOfItem, rootOfPart;
  for (int32_t owner = 0; owner < N; owner++) {
    int32_t count = owner == LARGE ? 20000 : owner % 11 == 0 ? 0 : owner % 7 == 0 ? 31 : 2;
    std::string json = "{\"id\":\"" + std::to_string(owner) + "\",\"tag_s\":\""
        + (owner % 17 == 0 ? "lead" : "other") + "\",\"items_nested\":[";
    for (int32_t row = 0; row < count; row++) {
      if (row) json += ',';
      int value = owner % 5 && row == count - 1 ? 1 : 100;
      json += "{\"n_i\":" + std::to_string(value) + ",\"parts_nested\":[{}, {\"n_i\":"
          + std::to_string(value) + "}]}";
      rootOfItem.push_back(owner);
      rootOfPart.push_back(owner); rootOfPart.push_back(owner);
    }
    json += "]}";
    input.addRaw([&](auto& doc, auto& arena) { ASSERT_TRUE(api::read_json(doc, json, arena)); });
  }
  input.commit(); ASSERT_TRUE(helper.submit(input).success);
  ASSERT_TRUE(helper.deleteById("17", UpdateMessage::COMMIT).success);
  auto root = helper.getIndexWriter()->snapshots.readers.getReader();
  for (auto path : {"items_nested", "items_nested.parts_nested"}) for (int high : {1, 101}) {
    SCOPED_TRACE(::testing::Message() << path << '/' << high);
    auto many = root->view(path);
    const auto& rootOfRow = many->path() == "items_nested" ? rootOfItem : rootOfPart;
    MemPool pool;
    Query::Context context(pool, *many);
    OwnershipRelation relation(*many, *root, context.planningContext().memoryTracker);
    std::string field = std::string(path) + ".n_i";
    NumericPredicateQuery numeric(field, 0, high);
    auto* source = numeric.createWeight(context, 0)->scorerSupplier(pool, many->segments()[0]);
    auto* supplier = relation.reduce(pool, 0, *source, root->segments()[0].liveDocSet(), RowRelation::Reduction::NONE, 1.0f);
    auto planContext = Query::PlanContext::fromLeadCost(N / 17 + 1, Query::ExecutionUse::COUNT_WINDOWS);
    planContext.demand.span = N;
    planContext.membershipConsumption = Query::MembershipConsumption::CANDIDATES;
    SkipStatsScope stats;
    auto* scorer = supplier->resolve(pool, planContext)->build(pool);
    EXPECT_GT(SkipStats::numericPredicateSparseVerifyArms, 0);
    EXPECT_EQ(0, SkipStats::numericPredicateScanArms);
    int32_t verified = 0;
    NumericVerificationObserver observer([&](int32_t row) {
      ASSERT_GE(row, 0); ASSERT_LT((size_t)row, rootOfRow.size());
      EXPECT_EQ(0, rootOfRow[(size_t)row] % 17);
      EXPECT_NE(17, rootOfRow[(size_t)row]);
      verified++;
    });
    std::array<uint64_t, N / 64> bits{};
    for (int32_t owner = 0; owner < N; owner += 17) bits[(size_t)owner >> 6] |= 1ULL << (owner & 63);
    scorer->intersectCandidateBits(bits, 0);
    for (int32_t owner = 0; owner < N; owner++) {
      bool expected = owner % 17 == 0 && owner != 17 && owner % 11 != 0 && (high == 101 || owner % 5);
      EXPECT_EQ(expected, (bits[(size_t)owner >> 6] & (1ULL << (owner & 63))) != 0) << owner;
    }
    auto* probe = supplier->resolve(pool, planContext)->build(pool);
    for (int32_t owner = 0; owner < N; owner += 17) {
      bool expected = owner != 17 && owner % 11 != 0 && (high == 101 || owner % 5);
      EXPECT_EQ(expected, probe->advanceWindow(owner, owner + 1) == owner) << owner;
      if (expected) {
        int32_t before = verified;
        EXPECT_TRUE(probe->matches()); EXPECT_TRUE(probe->matches());
        EXPECT_EQ(owner, probe->docId()); EXPECT_FLOAT_EQ(1.0f, probe->score());
        EXPECT_EQ(before, verified);
      }
    }
    for (int32_t owner : {17, LARGE}) {
      auto* single = supplier->resolve(pool, planContext)->build(pool);
      std::array<uint64_t, N / 64> one{};
      one[(size_t)owner >> 6] = 1ULL << (owner & 63);
      single->intersectCandidateBits(one, 0);
      EXPECT_EQ(owner == LARGE && high == 101,
          (one[(size_t)owner >> 6] & (1ULL << (owner & 63))) != 0);
    }
    EXPECT_GT(verified, 0);
    EXPECT_LE(verified, 2 * (int32_t)rootOfRow.size());
  }
}

TEST_F(JoinProbeTest, firstPageDoesNotVerifyAWholePostingWindow) {
  constexpr int32_t N = 8192;
  SearchOverridesGuard ranges(TopDocsReq::boundedCountRangeDocsForTests);
  TopDocsReq::boundedCountRangeDocsForTests = 64;
  helper.getIndexWriter()->snapshots.readers.filterCache = std::make_shared<FilterCache>(FilterCacheConfig{.maxBytes = 0});
  CollectionHelper::UpdateBuilder input;
  for (int32_t owner = 0; owner < N; owner++) {
    std::string json = "{\"id\":\"" + std::to_string(owner) + "\",\"tag_s\":\""
        + (owner % 4 == 0 ? "lead" : "other")
        + "\",\"items_nested\":[{\"n_i\":0},{\"n_i\":100}]}";
    input.addRaw([&](auto& doc, auto& arena) { ASSERT_TRUE(api::read_json(doc, json, arena)); });
  }
  input.commit(); ASSERT_TRUE(helper.submit(input).success);
  for (int count : {0, 10, 100, -1}) {
    SCOPED_TRACE(count);
    int checked = 0;
    NumericVerificationObserver observer([&](int32_t row) { EXPECT_EQ(0, (row / 2) % 4); checked++; });
    SkipStatsScope stats;
    auto req = localReq(helper.getSearchEngine());
    auto& op = req->topDocs().exprQuery("tag_s:lead AND items_nested.n_i:[0 TO 10]").limit(10).fields({"id"});
    if (count) op.countLimit(count);
    req->execute(1); ASSERT_OK(req);
    if (count >= 0) {
      EXPECT_LT(checked, count ? 600 : 20);
      EXPECT_GT(SkipStats::numericPredicateSparseVerifyArms, 0);
      EXPECT_EQ(0, SkipStats::numericPredicateScanArms);
    } else { EXPECT_EQ(N / 4, req->getMatchCount()); }
    if (!count) { EXPECT_EQ(0, SkipStats::conjDenseScoredWindows); }
  }
}

TEST_F(JoinProbeTest, childProofProjectsEmptyZonesWithoutRejectingPossibleOwners) {
  constexpr int32_t N = 5000;
  CollectionHelper::UpdateBuilder input;
  for (int32_t owner = 0; owner < N; owner++) {
    std::string json = "{\"id\":\"" + std::to_string(owner) + "\",\"items_nested\":["
        + (owner % 11 ? "{\"n_i\":" + std::to_string(owner < N / 2 ? 0 : 100) + "}" : "") + "]}";
    input.addRaw([&](auto& doc, auto& arena) { ASSERT_TRUE(api::read_json(doc, json, arena)); });
  }
  input.commit(); ASSERT_TRUE(helper.submit(input).success);
  auto root = helper.getIndexWriter()->snapshots.readers.getReader();
  auto many = root->view("items_nested");
  MemPool pool;
  Query::Context context(pool, *many);
  OwnershipRelation relation(*many, *root, context.planningContext().memoryTracker);
  NumericPredicateQuery numeric("items_nested.n_i", 0, 1);
  auto* source = numeric.createWeight(context, 0)->scorerSupplier(pool, many->segments()[0]);
  auto* supplier = relation.reduce(pool, 0, *source, nullptr, RowRelation::Reduction::NONE, 1.0f);
  auto planContext = Query::PlanContext::fromLeadCost(N, Query::ExecutionUse::COUNT_WINDOWS);
  planContext.membershipConsumption = Query::MembershipConsumption::CANDIDATES;
  auto* plan = supplier->resolve(pool, planContext);
  auto* proof = plan->buildMembershipProof(pool);
  ASSERT_NE(nullptr, proof);
  for (int32_t owner = 0; owner < N; owner++) {
    int32_t possible = proof->nextPossible(owner);
    if (owner < N / 2 && owner % 11) { EXPECT_EQ(owner, possible); }
    if (owner > N * 3 / 4) { EXPECT_EQ(PostingsReader::END, possible); }
  }
  EXPECT_EQ(1, proof->nextPossible(1));
  proof->reset();
  EXPECT_EQ(1, proof->nextPossible(1));
}
