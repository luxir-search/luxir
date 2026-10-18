// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include <gtest/gtest.h>
#include "luxir/query/TermQuery.h"
#include "luxir/reader/SkipStats.h"
#include "test/CollectionHelper.h"
#include "test/LocalReq.h"
#include "test/LuxirTest.h"
#include "test/QueryBuild.h"
#include "test/SchemaBuilder.h"

using namespace luxir;
using namespace luxir::test;

class CountProbeTest : public LuxirTest {
protected:
  CollectionHelper helper;

  void index(bool geo = false) {
    if (geo) {
      SchemaBuilder schema;
      auto& field = schema.field("point");
      field.type = api::FieldDef::FieldClass::GEO_POINT;
      field.index = api::FieldDef::IndexMode::RANGE;
      schema.set(helper.collection());
    }
    auto writer = helper.getIndexWriter();
    writer->snapshots.readers.filterCache = std::make_shared<FilterCache>(
        FilterCacheConfig{.maxBytes = 1024 * 1024, .minSegmentDocs = 0, .admissionThreshold = 2});
    writer->mergePolicy->setMergeFactor(100);
    for (int seg = 0; seg < 4; seg++) {
      auto& inverter = writer->obtainInverter();
      for (int i = 0; i < 160; i++) {
        std::string text = i % 2 ? "common present" : "common";
        if (seg % 2 && i % 8 == 0) text += " rare";
        if (seg % 2 == 0) text += " even";
        inverter.startDoc();
        inverter.getIndexHandler("id").index(inverter, std::to_string(seg * 160 + i));
        inverter.getIndexHandler("body_w").index(inverter, text);
        if (geo && seg % 2 && i % 2) inverter.getIndexHandler("point").index(inverter, 0.0, 0.0);
        inverter.finishDoc();
      }
      writer->releaseInverter(inverter);
      writer->commit();
    }
    ASSERT_EQ(4u, writer->snapshots.readers.getReader()->segments().size());
  }

  template <typename MakeQuery>
  void checkCached(MakeQuery makeQuery, int64_t found) {
    for (int repeat = 0; repeat < 4; repeat++) {
      auto req = localReq(helper.getSearchEngine());
      req->topDocs("q").limit(0).getNumber().rawQuery() = makeQuery(req->mr);
      SkipStatsScope stats;
      helper.getSearchEngine().dispatch(*req.get(), 0);
      ASSERT_OK(req);
      EXPECT_EQ(found, req->getMatchCount());
      if (repeat >= 2) {
        EXPECT_EQ(0, SkipStats::termDictionarySeeks);
        EXPECT_EQ(4, SkipStats::wholeCountHits);
        EXPECT_EQ(1, SkipStats::cacheFirstMembershipWeightSkips);
      }
    }
  }
};

TEST_F(CountProbeTest, cachedConjunctionIncludesEmptySegmentsWithoutTermSeeks) {
  index();
  for (bool rare : {false, true}) {
    checkCached([&](auto& mr) {
      return qb::boolean(mr, {qb::match(mr, "body_w", "common"),
          qb::match(mr, "body_w", rare ? "rare" : "present")});
    }, rare ? 40 : 320);
  }
}

TEST_F(CountProbeTest, emptyConjunctionBecomesResident) {
  index();
  checkCached([](auto& mr) {
    return qb::boolean(mr, {qb::match(mr, "body_w", "even"), qb::match(mr, "body_w", "rare")});
  }, 0);
}

TEST_F(CountProbeTest, missingTermWithDeletionsBecomesResident) {
  index();
  for (int seg = 0; seg < 4; seg++) {
    ASSERT_TRUE(helper.deleteById(std::to_string(seg * 160), UpdateMessage::COMMIT).success);
  }
  auto reader = helper.getIndexWriter()->snapshots.readers.getReader();
  ASSERT_EQ(4u, reader->segments().size());
  for (auto& segment : reader->segments()) ASSERT_NE(nullptr, segment.liveDocs());
  checkCached([](auto& mr) { return qb::match(mr, "body_w", "rare"); }, 38);
}

TEST_F(CountProbeTest, missingGeoFieldBecomesResident) {
  index(true);
  checkCached([](auto& mr) { return qb::geoBox(mr, "point", -1, 1, -1, 1); }, 160);
}

TEST_F(CountProbeTest, emptyDomainCachesRawMembership) {
  index();
  auto reader = helper.getIndexWriter()->snapshots.readers.getReader();
  auto& segment = reader->segments().front();
  auto empty = DocSetBuilder(segment.maxDoc()).build();
  TermQuery query("body_w", "present");
  for (int repeat = 0; repeat < 3; repeat++) {
    MemPool pool;
    Query::Context context(pool, *reader);
    auto* weight = query.createWeight(context, 0);
    auto* use = context.getFilterUse(query, FilterCache::AdmissionLane::WHOLE);
    ASSERT_NE(nullptr, use);
    QueryPrep::WholeMembershipPlan plan(*weight, use, context.filterUses);
    SkipStatsScope stats;
    auto result = plan.resolve(*reader, segment, empty.get(), nullptr,
        [](Query::Weight&) -> Query::ScorerSupplier* {
          ADD_FAILURE() << "metadata zero must not build a fallback supplier";
          return nullptr;
        });
    EXPECT_TRUE(result.available);
    EXPECT_EQ(0, result.count);
    EXPECT_EQ(repeat == 0 ? 1 : 0, SkipStats::wholeCountBypasses);
    EXPECT_EQ(repeat == 1 ? 1 : 0, SkipStats::wholeCountBuilds);
    EXPECT_EQ(repeat == 2 ? 1 : 0, SkipStats::wholeCountHits);
    if (repeat == 0) EXPECT_EQ(nullptr, use->rawDocSet(segment.ord));
    else {
      ASSERT_NE(nullptr, use->rawDocSet(segment.ord));
      EXPECT_EQ(80, use->rawDocSet(segment.ord)->card());
    }
  }
}
