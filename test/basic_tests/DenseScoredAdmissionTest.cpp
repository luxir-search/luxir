// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include <gtest/gtest.h>

#include <array>
#include <string>
#include <string_view>
#include <vector>

#include "luxir/query/BooleanQuery.h"
#include "luxir/query/TermQuery.h"
#include "luxir/reader/DocsEnum.h"
#include "luxir/reader/SkipStats.h"
#include "luxir/search/Collector.h"
#include "test/CollectionHelper.h"
#include "test/LocalReq.h"
#include "test/QueryBuild.h"
#include "test/LuxirTest.h"
#include "test/TestUtils.h"

using namespace luxir;
using namespace luxir::test;

namespace {

struct DenseScoredGuard {
  bool saved =
      BooleanQuery::ConjunctionBulkScorer::disableDenseScoredForTests;

  explicit DenseScoredGuard(bool disabled) {
    BooleanQuery::ConjunctionBulkScorer::disableDenseScoredForTests = disabled;
  }

  ~DenseScoredGuard() {
    BooleanQuery::ConjunctionBulkScorer::disableDenseScoredForTests = saved;
  }
};

struct SkipStatsGuard {
  bool saved = SkipStats::enabled;

  SkipStatsGuard() {
    SkipStats::enabled = true;
    SkipStats::reset();
  }

  ~SkipStatsGuard() {
    SkipStats::enabled = saved;
    SkipStats::reset();
  }
};

struct Shape {
  std::string_view lead;
  std::string_view other;
  int32_t leadCount;
  int32_t survivorCount;
};

struct AdmissionRun {
  int64_t count;
  int64_t windows;
  int64_t admits;
  int64_t latchBacks;
  int64_t densityRejects;
};

void appendToken(std::string& body, std::string_view token) {
  body.push_back(' ');
  body.append(token);
}

std::vector<Doc> makeSegment(
    std::string_view idPrefix, std::span<const Shape> shapes) {
  std::vector<Doc> docs;
  docs.reserve((size_t) DocsEnumMeta::L1_DOCS);
  for (int32_t doc = 0; doc < DocsEnumMeta::L1_DOCS; doc++) {
    std::string body = "filler";
    for (const Shape& shape : shapes) {
      if (doc < shape.leadCount) {
        appendToken(body, shape.lead);
      }
      int32_t otherOnlyEnd =
          shape.leadCount + shape.leadCount - shape.survivorCount + 1;
      if (doc < shape.survivorCount
          || (doc >= shape.leadCount && doc < otherOnlyEnd)) {
        appendToken(body, shape.other);
      }
    }
    docs.push_back(flatdoc(
        "id", std::string(idPrefix) + std::to_string(doc),
        "body_w", body));
  }
  return docs;
}

AdmissionRun runShape(CollectionHelper& helper, const Shape& shape,
                      int32_t topK, bool exactCount, bool disabled) {
  DenseScoredGuard denseGuard(disabled);
  SkipStatsGuard statsGuard;

  auto req = localReq(helper.getSearchEngine());
  req->collection("main");
  auto& topDocs = req->topDocs("q");
  topDocs.getScores().limit(topK);
  if (exactCount) {
    topDocs.getNumber();
  }
  topDocs.rawQuery() = qb::boolean(
      topDocs.mr(),
      {qb::match(topDocs.mr(), "body_w", shape.lead),
       qb::match(topDocs.mr(), "body_w", shape.other)});
  req->execute(false);
  EXPECT_TRUE(req->ok()) << req->errorMsg();
  return {
      .count = req->getMatchCount(),
      .windows = SkipStats::conjDenseScoredWindows,
      .admits = SkipStats::conjDenseScoredAdmits,
      .latchBacks = SkipStats::conjDenseScoredLatchBacks,
      .densityRejects = SkipStats::conjDenseScoredDensityRejects,
  };
}

int64_t runPullCount(CollectionHelper& helper, const Shape& shape,
                     int32_t topK) {
  auto reader = helper.getIndexWriter()->getIndexReader();
  MemPool pool;
  Query::Context context(pool, *reader);
  TermQuery lead("body_w", shape.lead);
  TermQuery other("body_w", shape.other);
  std::array<Query*, 2> mandatory = {&lead, &other};
  BooleanQuery query(mandatory, {}, {}, {});
  auto* weight = query.createWeight(context, Query::NEED_SCORES);
  TopDocsCollector collector(topK);
  for (auto& segment : context.topReader.segments()) {
    auto* scorer = weight->createScorer(pool, segment);
    if (scorer != nullptr) {
      collectTopK(segment.ord, scorer, nullptr, nullptr, collector,
                  /*allowPruning=*/false);
    }
  }
  return collector.totalHits();
}

class DenseScoredAdmissionTest : public LuxirTest {
public:
  static constexpr Shape shallowInside{
      "shallowinsidelead", "shallowinsideother", 100, 10};
  static constexpr Shape shallowOutside{
      "shallowoutsidelead", "shallowoutsideother", 100, 11};
  static constexpr Shape deepInside{
      "deepinsidelead", "deepinsideother", 100, 60};
  static constexpr Shape deepOutside{
      "deepoutsidelead", "deepoutsideother", 100, 61};
  static constexpr Shape highSurvivors{
      "highsurvivorslead", "highsurvivorsother", 512, 256};
  static constexpr Shape belowLeadFloor{
      "belowfloorlead", "belowfloorother", 31, 1};

  CollectionHelper helper;

  void SetUp() override {
    LuxirTest::SetUp();
    constexpr std::array shapes = {
        shallowInside, shallowOutside, deepInside,
        deepOutside, highSurvivors, belowLeadFloor};
    helper.indexAll(makeSegment("shape", shapes), UpdateMessage::COMMIT);
  }
};

TEST_F(DenseScoredAdmissionTest, top100DensityBoundary) {
  AdmissionRun inside = runShape(helper, shallowInside, 100, false, false);
  EXPECT_EQ(inside.windows, 1);
  EXPECT_EQ(inside.admits, 1);
  EXPECT_EQ(inside.latchBacks, 0);
  EXPECT_EQ(inside.densityRejects, 0);

  AdmissionRun outside = runShape(helper, shallowOutside, 100, false, false);
  EXPECT_EQ(outside.windows, 1);
  EXPECT_EQ(outside.admits, 0);
  EXPECT_EQ(outside.latchBacks, 1);
  EXPECT_EQ(outside.densityRejects, 1);
}

TEST_F(DenseScoredAdmissionTest, top1000DensityBoundaryAndAbsoluteCapRemoval) {
  AdmissionRun shallowDepth = runShape(
      helper, deepInside, 100, false, false);
  EXPECT_EQ(shallowDepth.admits, 0);
  EXPECT_EQ(shallowDepth.latchBacks, 1);
  EXPECT_EQ(shallowDepth.densityRejects, 1);

  AdmissionRun inside = runShape(helper, deepInside, 1000, false, false);
  EXPECT_EQ(inside.admits, 1);
  EXPECT_EQ(inside.latchBacks, 0);
  EXPECT_EQ(inside.densityRejects, 0);

  AdmissionRun outside = runShape(helper, deepOutside, 1000, false, false);
  EXPECT_EQ(outside.admits, 0);
  EXPECT_EQ(outside.latchBacks, 1);
  EXPECT_EQ(outside.densityRejects, 1);

  AdmissionRun high = runShape(
      helper, highSurvivors, 1000, false, false);
  EXPECT_EQ(high.admits, 1);
  EXPECT_EQ(high.latchBacks, 0);
  EXPECT_EQ(high.densityRejects, 0);
}

TEST_F(DenseScoredAdmissionTest, leadFloorAndDisabledOracle) {
  AdmissionRun sparse = runShape(
      helper, belowLeadFloor, 1000, true, false);
  EXPECT_EQ(sparse.count, 1);
  EXPECT_EQ(sparse.admits, 0);
  EXPECT_EQ(sparse.latchBacks, 1);
  EXPECT_EQ(sparse.densityRejects, 0);

  constexpr std::array shapes = {
      shallowInside, shallowOutside, deepInside,
      deepOutside, highSurvivors, belowLeadFloor};
  for (const Shape& shape : shapes) {
    EXPECT_EQ(runShape(helper, shape, 1000, true, false).count,
              runShape(helper, shape, 1000, true, true).count)
        << shape.lead;
  }
}

TEST_F(DenseScoredAdmissionTest, exactCountUsesDeepestCalibratedDepth) {
  AdmissionRun ordinary = runShape(
      helper, shallowOutside, 100, false, false);
  EXPECT_EQ(ordinary.admits, 0);
  EXPECT_EQ(ordinary.latchBacks, 1);
  EXPECT_EQ(ordinary.densityRejects, 1);

  AdmissionRun exact = runShape(
      helper, shallowOutside, 100, true, false);
  EXPECT_EQ(exact.count, 11);
  EXPECT_EQ(exact.admits, 1);
  EXPECT_EQ(exact.latchBacks, 0);
  EXPECT_EQ(exact.densityRejects, 0);
  EXPECT_EQ(exact.count, runPullCount(helper, shallowOutside, 100));
}

// Requested depth below the entry gate: with pruning the shape never samples,
// but an exact count prunes nothing at any depth, so the effective depth (and
// with it the gate) reads as the deepest calibrated one.
TEST_F(DenseScoredAdmissionTest, exactCountAdmitsBelowTheShallowEntryGate) {
  AdmissionRun pruned = runShape(helper, shallowOutside, 10, false, false);
  EXPECT_EQ(pruned.windows, 0);
  EXPECT_EQ(pruned.admits, 0);
  EXPECT_EQ(pruned.latchBacks, 0);

  AdmissionRun exact = runShape(helper, shallowOutside, 10, true, false);
  EXPECT_EQ(exact.admits, 1);
  EXPECT_EQ(exact.latchBacks, 0);
  EXPECT_EQ(exact.densityRejects, 0);
  EXPECT_EQ(exact.count, runPullCount(helper, shallowOutside, 10));
}

TEST_F(DenseScoredAdmissionTest, segmentsDecideIndependently) {
  helper.clear();
  constexpr Shape segmentInside{
      "segmentlead", "segmentother", 100, 10};
  constexpr Shape segmentOutside{
      "segmentlead", "segmentother", 100, 11};
  helper.indexAll(
      makeSegment("inside", std::span{&segmentInside, 1}),
      UpdateMessage::COMMIT);
  helper.indexAll(
      makeSegment("outside", std::span{&segmentOutside, 1}),
      UpdateMessage::COMMIT);
  ASSERT_EQ(helper.getIndexWriter()->getIndexReader()->segments().size(), 2);

  AdmissionRun enabled = runShape(
      helper, segmentInside, 100, false, false);
  EXPECT_EQ(enabled.windows, 2);
  EXPECT_EQ(enabled.admits, 1);
  EXPECT_EQ(enabled.latchBacks, 1);
  EXPECT_EQ(enabled.densityRejects, 1);
}

} // namespace
