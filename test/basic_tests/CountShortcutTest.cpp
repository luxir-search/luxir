// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include <gtest/gtest.h>
#include <array>
#include "luxir/query/AllQuery.h"
#include "luxir/query/BooleanQuery.h"
#include "luxir/query/BoostQuery.h"
#include "luxir/query/ConstantScoreQuery.h"
#include "luxir/query/ExistsQuery.h"
#include "luxir/query/ForcePrepareQuery.h"
#include "luxir/query/GeoBoxQuery.h"
#include "luxir/query/GeoDistanceQuery.h"
#include "luxir/query/MatchNoDocsQuery.h"
#include "luxir/query/NumericPredicateQuery.h"
#include "luxir/query/PrefixQuery.h"
#include "luxir/query/RescoreQuery.h"
#include "luxir/query/TermQuery.h"
#include "luxir/value/ValueExprParser.h"
#include "test/CollectionHelper.h"
#include "test/LocalReq.h"
#include "test/LuxirTest.h"
#include "test/QueryBuild.h"
#include "test/SchemaBuilder.h"
#include "test/TestUtils.h"

using namespace luxir;
using namespace luxir::test;

namespace {
struct CountStats {
  bool previous = SkipStats::enabled;
  CountStats() { SkipStats::enabled = true; SkipStats::reset(); }
  ~CountStats() { SkipStats::enabled = previous; }
};
}

class CountShortcutTest : public LuxirTest, public ::testing::WithParamInterface<bool> {
protected:
  CollectionHelper helper;
  std::shared_ptr<IndexReader> reader;
  std::shared_ptr<FilterCache> cache;
  static constexpr int DOCS = 160;

  void SetUp() override {
    SchemaBuilder schema;
    for (auto name : {"number", "numbers", "scan", "middle"}) {
      auto& field = schema.field(name);
      field.type = api::FieldDef::FieldClass::INT;
      field.multi = std::string_view(name) == "numbers";
      field.index = std::string_view(name) == "scan"
          ? api::FieldDef::IndexMode::NONE : api::FieldDef::IndexMode::RANGE;
    }
    for (auto name : {"geo", "geos", "geo_scan", "geo_full"}) {
      auto& field = schema.field(name);
      field.type = api::FieldDef::FieldClass::GEO_POINT;
      field.multi = std::string_view(name) == "geos";
      field.index = std::string_view(name) == "geo_scan"
          ? api::FieldDef::IndexMode::NONE : api::FieldDef::IndexMode::RANGE;
    }
    schema.set(helper.collection());
    auto writer = helper.getIndexWriter();
    cache = std::make_shared<FilterCache>(FilterCacheConfig{
        .maxBytes = GetParam() ? 64u << 20 : 0, .minSegmentDocs = 0,
        .admissionThreshold = 2});
    writer->snapshots.readers.filterCache = cache;
    writer->mergePolicy->setMergeFactor(100);
    for (int seg = 0; seg < 3; seg++) {
      auto& inverter = writer->obtainInverter();
      auto& id = inverter.getIndexHandler("id");
      auto& text = inverter.getIndexHandler("body_w");
      for (int doc = 0; doc < DOCS; doc++) {
        inverter.startDoc();
        id.index(inverter, std::to_string(seg * DOCS + doc));
        text.index(inverter, doc % 2 ? "alpha beta" : "beta");
        inverter.getIndexHandler("geo_full").index(inverter, (double)(doc % 60 - 30), (double)(doc % 80 - 40));
        if (doc % 5) {
          int64_t value = doc % 80 - 40 + seg * 10;
          for (auto name : {"number", "scan"}) inverter.getIndexHandler(name).index(inverter, value);
          std::array<int64_t, 3> values{value, value, value + 3};
          inverter.getIndexHandler("numbers").index(inverter, std::span<const int64_t>(values));
          if (seg == 1) inverter.getIndexHandler("middle").index(inverter, value);
          GeoPoint point{(double)(doc % 60 - 30), (double)(doc % 80 - 40)};
          inverter.getIndexHandler("geo").index(inverter, point.latitude, point.longitude);
          inverter.getIndexHandler("geo_scan").index(inverter, point.latitude, point.longitude);
          std::array<GeoPoint, 2> points{point, GeoPoint{point.latitude + 1, point.longitude + 1}};
          inverter.getIndexHandler("geos").index(inverter, std::span<const GeoPoint>(points));
        }
        inverter.finishDoc();
      }
      writer->releaseInverter(inverter);
      writer->commit();
    }
    reader = writer->snapshots.readers.getReader();
    ASSERT_EQ(3u, reader->segments().size());
  }

  void deleteSome() {
    for (int seg = 0; seg < 3; seg++) {
      ASSERT_TRUE(helper.deleteById(std::to_string(seg * DOCS + 2), UpdateMessage::COMMIT).success);
    }
    reader = helper.getIndexWriter()->snapshots.readers.getReader();
    ASSERT_EQ(3u, reader->segments().size());
  }

  int64_t iterated(Query::ScorerSupplier* supplier, MemPool& pool,
                   IndexReader::Segment& segment, DocSet* domain) {
    if (!supplier) return 0;
    auto* scorer = buildScorerForTests(pool, *supplier, INT64_MAX);
    if (!scorer) return 0;
    int64_t count = 0;
    for (int32_t doc = scorer->next(); doc != PostingsReader::END; doc = scorer->next()) {
      if (segment.liveDocs() && !segment.liveDocs()->docset().get(doc)) continue;
      if (!domain || domain->get(doc)) ++count;
    }
    return count;
  }

  void check(Query& query, Query::CountHow unrestricted) {
    for (int flags : {0, Query::NEED_SCORES}) {
      MemPool pool;
      Query::Context context(pool, *reader);
      auto* weight = query.createWeight(context, flags);
      for (auto& segment : reader->segments()) {
        DocSetBuilder selected(segment.maxDoc()), unfiltered(segment.maxDoc()), empty(segment.maxDoc());
        for (int doc = 0; doc < segment.maxDoc(); doc += 3) {
          unfiltered.add(std::min(doc + 2, segment.maxDoc() - 1));
          if (!segment.liveDocs() || segment.liveDocs()->docset().get(doc)) selected.add(doc);
        }
        auto domain = selected.build();
        auto rawDomain = unfiltered.build();
        auto noDocs = empty.build();
        for (DocSet* eligibility : { (DocSet*)nullptr, domain.get(), rawDomain.get(), noDocs.get() }) {
          auto shortcut = weight->countShortcut(segment, eligibility);
          if (!eligibility && !segment.liveDocs() && shortcut.maxMatches != 0) {
            EXPECT_EQ(unrestricted, shortcut.how);
          }
          auto* supplier = weight->scorerSupplier(pool, segment);
          auto resolved = QueryPrep::resolveCount(shortcut, [&](Query::Weight& resolver) {
            return &resolver == weight ? supplier : resolver.scorerSupplier(pool, segment);
          }, Query::CountHow::INDEX_WALK);
          auto executorCount = eligibility || segment.liveDocs()
              ? std::nullopt : QueryPrep::supplierCount(supplier);
          int64_t count = iterated(supplier, pool, segment, eligibility);
          EXPECT_GE(shortcut.maxMatches, count);
          if (shortcut.how != Query::CountHow::ITERATE) { EXPECT_TRUE(resolved.has_value()); }
          if (resolved) { EXPECT_EQ(count, *resolved); }
          if (!eligibility && !segment.liveDocs() && query.getKind() == QueryKind::NUMERIC_PREDICATE) {
            EXPECT_TRUE(executorCount.has_value());
          }
          if (executorCount) { EXPECT_EQ(count, *executorCount); }
          if (shortcut.how == Query::CountHow::METADATA) { EXPECT_EQ(count, shortcut.maxMatches); }
        }
      }
    }
  }
};

TEST_P(CountShortcutTest, metadataLeavesAndWrappersAgree) {
  for (bool deleted : {false, true}) {
    if (deleted) deleteSome();
    AllQuery all;
    MatchNoDocsQuery none;
    TermQuery term("body_w", "alpha"), absent("body_w", "missing");
    ExistsQuery exists("number"), missing("missing_i");
    ConstantScoreQuery constant(&term, 2.0f);
    BoostQuery boost(&term, 2.0f);
    ForcePrepareQuery prepared(&term);
    google::protobuf::Arena arena;
    ValueExprOptions options{reader->schema().get(), {}};
    auto* program = ValueExprParser(options, arena).parse("add(score,1)");
    RescoreQuery rescore(&term, program);
    for (Query* query : std::array<Query*, 10>{&all, &none, &term, &absent, &exists, &missing,
                          &constant, &boost, &prepared, &rescore}) {
      check(*query, Query::CountHow::METADATA);
    }
    PrefixQuery prefix("body_w", "a");
    check(prefix, Query::CountHow::ITERATE);
  }
}

TEST_P(CountShortcutTest, numericLevelsAgree) {
  for (bool deleted : {false, true}) {
    if (deleted) deleteSome();
    for (auto field : {"number", "numbers", "scan", "middle", "missing_i"}) {
      for (auto [lo, hi] : {std::pair<int64_t,int64_t>{-1000,1000}, {2000,3000},
                            {-10,10}, {2,2}, {10,-10}}) {
        SCOPED_TRACE(std::format("{} [{},{}] deleted={}", field, lo, hi, deleted));
        NumericPredicateQuery query(field, lo, hi);
        auto expected = lo > hi || lo > 100 || lo < -100
            ? Query::CountHow::METADATA
            : std::string_view(field) == "numbers"
                ? Query::CountHow::ITERATE : Query::CountHow::SUPPLIER;
        check(query, expected);
      }
    }
  }
}

TEST_P(CountShortcutTest, geoLevelsAgree) {
  for (bool deleted : {false, true}) {
    if (deleted) deleteSome();
    for (auto field : {"geo", "geos", "geo_scan", "missing_geo"}) {
      auto expected = std::string_view(field) == "geos" || std::string_view(field) == "geo_scan"
          ? Query::CountHow::ITERATE : Query::CountHow::INDEX_WALK;
      for (auto [low, high] : {std::pair<double,double>{-90,90}, {-10,10}, {80,85}, {90,90}}) {
        GeoBoxQuery box(field, low, high, -180, 180);
        check(box, expected);
      }
      GeoDistanceQuery distance(field, 0, 0, 1500000);
      check(distance, expected);
    }
  }
}

TEST_P(CountShortcutTest, booleanBoundsAndSingletonsAgree) {
  for (bool deleted : {false, true}) {
    if (deleted) deleteSome();
    TermQuery alpha("body_w", "alpha"), beta("body_w", "beta"), dead("body_w", "absent");
    NumericPredicateQuery numeric("number", -10, 10), empty("number", 2000, 3000);
    std::array<Query*, 2> either{&alpha, &beta}, singleton{&numeric, &empty};
    std::array<Query*, 1> required{&alpha}, optional{&beta}, prohibited{&alpha};
    BooleanQuery disjunction({}, either, {}, {});
    BooleanQuery conjunction(either, {}, {}, {});
    BooleanQuery one({}, singleton, {}, {});
    BooleanQuery mandOpt(required, optional, {}, {});
    BooleanQuery excluded(required, {}, prohibited, {});
    BooleanQuery gated(required, optional, {}, {}, 1);
    AllQuery all;
    GeoBoxQuery geo("geo", -10, 10, -20, 20);
    std::array<Query*, 1> allRequired{&all}, rangeFilter{&numeric}, geoFilter{&geo};
    BooleanQuery rangeOnly({}, {}, {}, rangeFilter), geoOnly({}, {}, {}, geoFilter);
    BooleanQuery allRange(allRequired, {}, {}, rangeFilter), allGeo(allRequired, {}, {}, geoFilter);
    check(disjunction, Query::CountHow::ITERATE);
    check(conjunction, Query::CountHow::ITERATE);
    check(one, Query::CountHow::SUPPLIER);
    check(mandOpt, Query::CountHow::METADATA);
    check(excluded, Query::CountHow::ITERATE);
    check(gated, Query::CountHow::ITERATE);
    check(rangeOnly, Query::CountHow::SUPPLIER);
    check(allRange, Query::CountHow::SUPPLIER);
    check(geoOnly, Query::CountHow::INDEX_WALK);
    check(allGeo, Query::CountHow::INDEX_WALK);
  }
}

TEST_P(CountShortcutTest, probesStopBeforeIndexWalks) {
  MemPool pool;
  Query::Context context(pool, *reader);
  NumericPredicateQuery numeric("number", -10, 10);
  GeoBoxQuery geo("geo_full", -10, 10, -20, 20);
  std::array<Query*, 1> prohibited{&geo}, required{&numeric};
  BooleanQuery boolean(required, {}, prohibited, {});
  for (Query* query : std::array<Query*, 3>{&numeric, &geo, &boolean}) {
    auto* weight = query->createWeight(context, 0);
    for (auto& segment : reader->segments()) {
      RAMBitDocSet unknown(segment.maxDoc());
      unknown.mutableBits().set(1);
      EXPECT_EQ(-1, unknown.cachedCard());
      weight->countShortcut(segment, &unknown);
      EXPECT_EQ(-1, unknown.cachedCard());
      int builds = 0;
      auto shortcut = weight->countShortcut(segment, nullptr);
      auto count = QueryPrep::resolveCount(shortcut, [&](Query::Weight& resolver) {
        ++builds;
        EXPECT_EQ(Query::CountHow::SUPPLIER, shortcut.how);
        return resolver.scorerSupplier(pool, segment);
      });
      EXPECT_EQ(query == &numeric ? 1 : 0, builds);
      EXPECT_EQ(query == &numeric, count.has_value());
      QueryPrep::WholeMembershipPlan plan(*weight, nullptr, {});
      EXPECT_EQ(query == &numeric, resolveWholeMembershipForTests(plan, *reader, segment, nullptr).available);
    }
  }
}

TEST_P(CountShortcutTest, resolverReusesOwnSupplierAndBuildsOnlyLeaf) {
  MemPool pool;
  Query::Context context(pool, *reader);
  NumericPredicateQuery range("number", -10, 10);
  AllQuery all;
  std::array<Query*, 1> required{&all}, filter{&range};
  BooleanQuery filtered(required, {}, {}, filter);
  ConstantScoreQuery constant(&filtered, 2.0f);
  for (Query* query : std::array<Query*, 3>{&range, &filtered, &constant}) {
    auto* weight = query->createWeight(context, 0);
    for (auto& segment : reader->segments()) {
      auto shortcut = weight->countShortcut(segment, nullptr);
      ASSERT_EQ(Query::CountHow::SUPPLIER, shortcut.how);
      ASSERT_NE(nullptr, dynamic_cast<NumericPredicateQuery::Weight*>(shortcut.resolver));
      Query::ScorerSupplier* supplier = nullptr;
      if (shortcut.resolver == weight) supplier = weight->scorerSupplier(pool, segment);
      int builds = supplier ? 1 : 0;
      bool reused = false;
      auto count = QueryPrep::resolveCount(shortcut, [&](Query::Weight& resolver) {
        if (&resolver == weight && supplier) {
          reused = true;
          return supplier;
        }
        ++builds;
        return resolver.scorerSupplier(pool, segment);
      });
      ASSERT_TRUE(count);
      EXPECT_EQ(1, builds);
      EXPECT_EQ(shortcut.resolver == weight, reused);
      EXPECT_EQ(0u, cache->counters().buildAttempts);
    }
  }
}

TEST_P(CountShortcutTest, topDocsResolvesPointsBeforeFilterCache) {
  for (auto field : {"number", "scan"})
  for (int shape = 0; shape < 3; shape++) {
    for (int repeat = 0; repeat < 3; repeat++) {
      CountStats stats;
      auto req = localReq(luxirNode->getSearchEngine());
      req->collection("main");
      auto& op = req->topDocs("q").limit(0).getNumber();
      auto query = qb::range(op.mr(), field, qb::valI64(op.mr(), -10), nullptr,
                             qb::valI64(op.mr(), 10), nullptr);
      if (shape == 2) {
        op.allQuery().filter(query);
      } else {
        op.rawQuery() = shape == 1 ? qb::constantScore(op.mr(), query, 2.0f) : query;
      }
      req->execute(false);
      ASSERT_OK(req);
      EXPECT_EQ(3, SkipStats::wholeCountConstant);
      EXPECT_EQ(0, SkipStats::wholeCountBuilds + SkipStats::wholeCountHits);
      EXPECT_EQ(0, SkipStats::numericPredicatePointsArms);
      EXPECT_EQ(0u, cache->counters().buildAttempts);
      MemPool pool;
      Query::Context context(pool, *reader);
      NumericPredicateQuery reference(field, -10, 10);
      auto* weight = reference.createWeight(context, 0);
      int64_t expected = 0;
      for (auto& segment : reader->segments()) {
        expected += iterated(weight->scorerSupplier(pool, segment), pool, segment, nullptr);
      }
      EXPECT_EQ(expected, req->getMatchCount());
    }
  }
}

TEST_P(CountShortcutTest, topDocsKeepsNumericCountExecutor) {
  for (auto field : {"numbers"}) {
    CountStats stats;
    auto req = localReq(luxirNode->getSearchEngine());
    req->collection("main");
    auto& op = req->topDocs("q").limit(0).getNumber();
    op.rawQuery() = qb::range(op.mr(), field, qb::valI64(op.mr(), -10), nullptr,
                              qb::valI64(op.mr(), 10), nullptr);
    req->execute(false);
    ASSERT_OK(req);
    EXPECT_EQ(0, SkipStats::numericPredicatePointsArms);
    EXPECT_EQ(0, SkipStats::numericPredicateZoneArms);
    EXPECT_EQ(0, SkipStats::numericPredicateScanArms);
    MemPool pool;
    Query::Context context(pool, *reader);
    NumericPredicateQuery reference(field, -10, 10);
    auto* weight = reference.createWeight(context, 0);
    int64_t expected = 0;
    for (auto& segment : reader->segments()) {
      EXPECT_EQ(Query::CountHow::ITERATE, weight->countShortcut(segment, nullptr).how);
      expected += iterated(weight->scorerSupplier(pool, segment), pool, segment, nullptr);
    }
    EXPECT_EQ(expected, req->getMatchCount());
  }
}

TEST_P(CountShortcutTest, geoWholeCountsKeepCacheRouting) {
  for (bool filtered : {false, true}) {
    int bound = filtered ? 12 : 10;
    int64_t expected = 0;
    MemPool pool;
    Query::Context context(pool, *reader);
    GeoBoxQuery reference("geo", -bound, bound, -20, 20);
    auto* weight = reference.createWeight(context, 0);
    for (auto& segment : reader->segments()) {
      expected += iterated(weight->scorerSupplier(pool, segment), pool, segment, nullptr);
    }
    for (int repeat = 0; repeat < 3; repeat++) {
      CountStats stats;
      auto req = localReq(luxirNode->getSearchEngine());
      req->collection("main");
      auto& op = req->topDocs("q").limit(0).getNumber();
      auto query = qb::geoBox(op.mr(), "geo", -bound, bound, -20, 20);
      if (filtered) op.allQuery().filter(query);
      else op.rawQuery() = query;
      req->execute(false);
      ASSERT_OK(req);
      EXPECT_EQ(expected, req->getMatchCount());
      EXPECT_EQ(0, SkipStats::wholeCountConstant);
      if (GetParam()) {
        EXPECT_EQ(repeat == 0 ? 3 : 0, SkipStats::wholeCountBypasses);
        EXPECT_EQ(repeat == 1 ? 3 : 0, SkipStats::wholeCountBuilds);
        EXPECT_EQ(repeat == 2 ? 3 : 0, SkipStats::wholeCountHits);
      }
    }
  }
}

TEST_P(CountShortcutTest, executionCanDiscoverEmptyChildren) {
  MemPool pool;
  Query::Context context(pool, *reader);
  TermQuery term("body_w", "alpha");
  GeoBoxQuery emptyGeo("geo", 80, 85, 100, 110);
  NumericPredicateQuery emptyPoints("number", 0, 0);
  auto* numeric = emptyPoints.createWeight(context, 0);
  for (auto& segment : reader->segments()) {
    auto shortcut = numeric->countShortcut(segment, nullptr);
    EXPECT_EQ(Query::CountHow::SUPPLIER, shortcut.how);
    EXPECT_GT(shortcut.maxMatches, 0);
    EXPECT_EQ(0, QueryPrep::supplierCount(numeric->scorerSupplier(pool, segment)));
  }
  for (Query* empty : std::array<Query*, 2>{&emptyGeo, &emptyPoints}) {
    std::array<Query*, 2> clauses{&term, empty};
    BooleanQuery query({}, clauses, {}, {});
    auto* weight = query.createWeight(context, 0);
    auto* termWeight = term.createWeight(context, 0);
    for (auto& segment : reader->segments()) {
      EXPECT_EQ(Query::CountHow::ITERATE, weight->countShortcut(segment, nullptr).how);
      auto count = QueryPrep::supplierCount(weight->scorerSupplier(pool, segment));
      ASSERT_TRUE(count);
      EXPECT_EQ(termWeight->countShortcut(segment, nullptr).maxMatches, *count);
    }
  }
}

TEST_P(CountShortcutTest, pointsEmptyDisjunctionUsesWholeCacheRouting) {
  for (int repeat = 0; repeat < 3; repeat++) {
    CountStats stats;
    auto req = localReq(luxirNode->getSearchEngine());
    req->collection("main");
    auto& op = req->topDocs("q").limit(0).getNumber();
    op.rawQuery() = qb::boolean(op.mr(), {},
        {qb::match(op.mr(), "body_w", "alpha"),
         qb::range(op.mr(), "number", qb::valI64(op.mr(), 0), nullptr,
                   qb::valI64(op.mr(), 0), nullptr)});
    req->execute(false);
    ASSERT_OK(req);
    EXPECT_EQ(3 * DOCS / 2, req->getMatchCount());
    EXPECT_EQ(0, SkipStats::wholeCountConstant);
    EXPECT_EQ(GetParam() && repeat == 0 ? 3 : 0, SkipStats::wholeCountBypasses);
    EXPECT_EQ(GetParam() && repeat == 1 ? 3 : 0, SkipStats::wholeCountBuilds);
    EXPECT_EQ(GetParam() && repeat == 2 ? 3 : 0, SkipStats::wholeCountHits);
  }
}

TEST_P(CountShortcutTest, queryBucketsResolveFilterLeaves) {
  MemPool pool;
  Query::Context context(pool, *reader);
  NumericPredicateQuery range("number", -10, 10);
  GeoBoxQuery geo("geo", -10, 10, -20, 20);
  std::array<int64_t, 2> expected{};
  std::array<Query*, 2> queries{&range, &geo};
  for (size_t i = 0; i < queries.size(); i++) {
    auto* weight = queries[i]->createWeight(context, 0);
    for (auto& segment : reader->segments()) {
      expected[i] += iterated(weight->scorerSupplier(pool, segment), pool, segment, nullptr);
    }
  }
  for (int repeat = 0; repeat < 3; repeat++) {
    CountStats stats;
    auto req = localReq(luxirNode->getSearchEngine());
    req->collection("main");
    auto& facet = req->facet("filters", "number").rawOp().kind.emplace<api::QueryFacet>();
    auto* buckets = api::build::allocArray(facet.buckets, 2, req->mr);
    for (size_t i = 0; i < queries.size(); i++) {
      buckets[i].name = i == 0 ? "range" : "geo";
      auto filter = i == 0
          ? qb::range(req->mr, "number", qb::valI64(req->mr, -10), nullptr,
                      qb::valI64(req->mr, 10), nullptr)
          : qb::geoBox(req->mr, "geo", -10, 10, -20, 20);
      auto* query = api::build::allocMessage<api::Query>(req->mr);
      *query = qb::boolean(req->mr, {qb::all()}, {}, {}, {filter});
      buckets[i].query = query;
    }
    req->execute(false);
    ASSERT_OK(req);
    EXPECT_EQ(0u, cache->counters().buildAttempts);
    EXPECT_EQ(0, SkipStats::numericPredicatePointsArms);
    EXPECT_EQ(0, SkipStats::geoBKDArms);
    auto* result = req->responses[0]->proto.ops.at("filters")->facetResult();
    ASSERT_NE(nullptr, result);
    ASSERT_EQ(2u, result->counts.size());
    const auto& names = std::get<api::ColStr>(result->bucket_ids->kind).v;
    for (size_t i = 0; i < names.size(); i++) {
      EXPECT_EQ(expected[names[i] == "range" ? 0 : 1], result->counts[i]);
    }
  }
}

TEST_P(CountShortcutTest, preparedSourcesKeepCountArms) {
  MemPool pool;
  Query::Context context(pool, *reader);
  TermQuery term("body_w", "alpha");
  ForcePrepareQuery force(&term);
  ConstantScoreQuery constant(&force, 2.0f);
  google::protobuf::Arena arena;
  ValueExprOptions options{reader->schema().get(), {}};
  auto* program = ValueExprParser(options, arena).parse("add(score,1)");
  RescoreQuery rescore(&force, program);
  std::vector<DocSet*> domains(reader->segments().size());
  Query::Weight::PrepareContext prepare{*reader, domains, false, nullptr};
  for (Query* query : std::array<Query*, 3>{&force, &constant, &rescore}) {
    auto* weight = query->createWeight(context, 0);
    auto prepared = weight->prepare(prepare);
    ASSERT_NE(nullptr, prepared);
    for (auto& segment : reader->segments()) {
      auto count = QueryPrep::supplierCount(prepared->scorerSupplier(pool, segment));
      ASSERT_TRUE(count);
      EXPECT_EQ(weight->countShortcut(segment, nullptr).maxMatches, *count);
    }
  }
}

TEST_P(CountShortcutTest, queryBucketsUseNumericCountAndRangePointsArms) {
  CountStats stats;
  auto req = localReq(luxirNode->getSearchEngine());
  req->collection("main");
  auto& cursor = req->facet("queries", "number");
  auto& facet = cursor.rawOp().kind.emplace<api::QueryFacet>();
  auto* buckets = api::build::allocArray(facet.buckets, 4, req->mr);
  std::array fields{"number", "numbers", "scan", "cover"};
  for (size_t i = 0; i < fields.size(); i++) {
    buckets[i].name = fields[i];
    auto* query = api::build::allocMessage<api::Query>(req->mr);
    bool cover = std::string_view(fields[i]) == "cover";
    *query = qb::range(req->mr, cover ? "number" : fields[i],
                      qb::valI64(req->mr, cover ? -1000 : -10), nullptr,
                      qb::valI64(req->mr, cover ? 1000 : 10), nullptr);
    buckets[i].query = query;
  }
  req->rangeFacet("ranges", "number").range(-40, 80, 20);
  req->execute(false);
  ASSERT_OK(req);
  EXPECT_EQ(0, SkipStats::numericPredicatePointsArms);
  EXPECT_EQ(0, SkipStats::numericPredicateZoneArms);
  EXPECT_EQ(0, SkipStats::numericPredicateScanArms);
  EXPECT_EQ(3, SkipStats::rangeFacetPointsArms);
  auto* result = req->responses[0]->proto.ops.at("queries")->facetResult();
  ASSERT_NE(nullptr, result);
  ASSERT_EQ(4u, result->counts.size());
  const auto& names = std::get<api::ColStr>(result->bucket_ids->kind).v;
  for (size_t i = 0; i < names.size(); i++) {
    MemPool pool;
    Query::Context context(pool, *reader);
    bool cover = names[i] == "cover";
    NumericPredicateQuery query(cover ? "number" : names[i], cover ? -1000 : -10,
                                cover ? 1000 : 10);
    auto* weight = query.createWeight(context, 0);
    int64_t expected = 0;
    for (auto& segment : reader->segments()) {
      expected += iterated(weight->scorerSupplier(pool, segment), pool, segment, nullptr);
    }
    EXPECT_EQ(expected, result->counts[i]);
  }
}

TEST_P(CountShortcutTest, topDocsCountsMatchIterationAcrossDeletes) {
  for (bool deleted : {false, true}) {
    if (deleted) deleteSome();
    for (int kind = 0; kind < 4; kind++) {
      CountStats stats;
      auto req = localReq(luxirNode->getSearchEngine());
      req->collection("main");
      auto& op = req->topDocs("q").limit(0).getNumber();
      AllQuery all;
      TermQuery term("body_w", "alpha");
      ExistsQuery exists("number");
      GeoBoxQuery geo("geo", -10, 10, -20, 20);
      std::array<Query*, 4> references{&all, &term, &exists, &geo};
      switch (kind) {
        case 0: op.allQuery(); break;
        case 1: op.matchQuery("body_w", "alpha"); break;
        case 2: op.existsQuery("number"); break;
        case 3: op.rawQuery() = qb::geoBox(op.mr(), "geo", -10, 10, -20, 20); break;
      }
      req->execute(false);
      ASSERT_OK(req);
      MemPool pool;
      Query::Context context(pool, *reader);
      auto* weight = references[kind]->createWeight(context, 0);
      int64_t expected = 0;
      for (auto& segment : reader->segments()) {
        DocSet* domain = segment.liveDocs() ? &segment.liveDocs()->docset() : nullptr;
        expected += iterated(weight->scorerSupplier(pool, segment), pool, segment, domain);
      }
      EXPECT_EQ(expected, req->getMatchCount());
    }
  }
}

INSTANTIATE_TEST_SUITE_P(CacheModes, CountShortcutTest, ::testing::Bool());
