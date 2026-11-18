// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include <bit>
#include "test/CollectionHelper.h"
#include "test/LocalReq.h"
#include "test/LuxirTest.h"
#include "test/QueryBuild.h"
#include "test/SchemaBuilder.h"
#include "luxir/reader/SkipStats.h"
#include "luxir/query/QueryPrep.h"
#include "luxir/query/BooleanQuery.h"
#include "luxir/query/ParseContext.h"
#include "luxir/query/ProtobufQueryParser.h"

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

  Query* parseQuery(LocalReq& req, const api::Query& wire, std::string_view path = "") {
    req.reader = helper.getIndexWriter()->snapshots.readers.getReader();
    req.schema = req.reader->schema();
    req.filterUses = std::make_shared<FilterCache::UseRegistry>(cache.get(), *req.reader);
    auto view = req.pinView(path);
    ParseView parseView(*req.schema, nullptr, &view, &req);
    ParseContext parseContext(req.requestPool, *req.schema, req.arena,
        CoerceContext{}, "q", nullptr, &parseView);
    return ProtobufQueryParser(parseContext).parse(wire);
  }

  void publishSource(const api::Query& wire) {
    fundSource("", wire);
    auto req = localReq(helper.getSearchEngine());
    auto* query = parseQuery(*req, wire);
    auto* context = Query::Context::create(&req->arena, *req->createPlanningContext(*req->reader));
    auto* use = context->getFilterUse(*query);
    auto* weight = query->createWeight(*context, 0);
    auto probe = use->probe(0);
    ASSERT_EQ(FilterCache::Probe::Kind::BUILD, probe.kind());
    auto docs = QueryPrep::materializeRawFilter(*weight, nullptr, req->reader->segments()[0]);
    use->publishRaw(0, probe, std::move(docs), 1'000'000);
  }

  void indexRangeOwners(int32_t size = 128) {
    CollectionHelper::UpdateBuilder input;
    for (int32_t i = 0; i < size; i++) {
      auto json = "{\"id\":\"r" + std::to_string(i) + "\",\"tag_w\":\"common\",\"band_w\":\""
          + (i < 3 ? "below" : i < 7 ? "at" : "other") + "\",\"rows_nested\":[{\"value_i\":"
          + std::to_string(i % 4) + "},{\"value_i\":9}]}";
      input.addRaw([&](auto& doc, auto& arena) { ASSERT_TRUE(api::read_json(doc, json, arena)); });
    }
    input.commit(true, 1); ASSERT_TRUE(helper.submit(input).success);
  }

  void fundSource(std::string_view path, const api::Query& wire) {
    auto req = localReq(helper.getSearchEngine());
    req->reader = helper.getIndexWriter()->snapshots.readers.getReader();
    req->schema = req->reader->schema();
    req->filterUses = std::make_shared<FilterCache::UseRegistry>(cache.get(), *req->reader);
    auto view = req->pinView(path);
    ParseView parseView(*req->schema, nullptr, &view, req.get());
    ParseContext parseContext(req->requestPool, *req->schema, req->arena,
        CoerceContext{}, "q", nullptr, &parseView);
    auto* query = ProtobufQueryParser(parseContext).parse(wire);
    auto* planning = req->createPlanningContext(*view);
    auto* use = planning->getFilterUse(*query);
    ASSERT_NE(nullptr, use);
    EXPECT_EQ(FilterCache::Probe::Kind::BYPASS, use->probe(0, FilterCache::Work::pending()).kind());
    planning->filterUses->addSegmentNanos(0, 1'000'000'000);
    planning->filterUses->settle(true);
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

TEST_F(JoinCacheTest, fixedJoinWeightKnowsItsConstantContribution) {
  CollectionHelper::UpdateBuilder input;
  input.addRaw([](auto& doc, auto& arena) {
    ASSERT_TRUE(api::read_json(doc, R"({"id":"range","items_nested":{"value_i":2}})", arena));
  });
  input.commit(); ASSERT_TRUE(helper.submit(input).success);
  auto req = localReq(helper.getSearchEngine());
  req->reader = helper.getIndexWriter()->snapshots.readers.getReader();
  req->schema = req->reader->schema();
  ParseView view(*req->schema, nullptr, &req->reader, req.get());
  ParseContext parseContext(req->requestPool, *req->schema, req->arena,
      CoerceContext{}, "q", nullptr, &view);
  auto* context = Query::Context::create(&req->arena, *req->createPlanningContext(*req->reader));
  for (auto mode : {"max", "none", "sum"}) for (float boost : {1.0f, 2.5f}) {
    auto child = qb::range(req->mr, "value_i", qb::valI64(req->mr, 1), nullptr,
        qb::valI64(req->mr, 3), nullptr);
    auto wire = qb::boost(req->mr, qb::toParent(req->mr, "items_nested", child, mode), boost);
    auto* query = ProtobufQueryParser(parseContext).parse(wire);
    auto* scored = query->createWeight(*context, Query::NEED_SCORES);
    auto expected = std::string_view(mode) == "sum" ? std::nullopt
        : std::optional<float>(std::string_view(mode) == "none" ? 0.0f : boost);
    EXPECT_EQ(expected, scored->constantContribution()) << mode << ' ' << boost;
    EXPECT_EQ(0.0f, query->createWeight(*context, 0)->constantContribution());
  }
}

TEST_F(JoinCacheTest, bulkFallbackReusesMandatorySuppliers) {
  cache->resize(0);
  for (bool prepared : {false, true}) for (bool multiple : {false, true}) {
    SCOPED_TRACE(prepared);
    SCOPED_TRACE(multiple);
    auto req = localReq(helper.getSearchEngine());
    req->reader = helper.getIndexWriter()->snapshots.readers.getReader();
    req->schema = req->reader->schema();
    ParseView view(*req->schema, nullptr, &req->reader, req.get());
    ParseContext parseContext(req->requestPool, *req->schema, req->arena,
        CoerceContext{}, "q", nullptr, &view);
    auto join = qb::toParent(req->mr, "items_nested", qb::match(req->mr, "color_s", "red"), "max");
    auto wire = multiple
        ? qb::boolean(req->mr, {join, qb::match(req->mr, "id", "0")}, {}, {}, {qb::exists(req->mr, "id")})
        : qb::boolean(req->mr, {join}, {}, {}, {qb::exists(req->mr, "id")});
    auto* query = ProtobufQueryParser(parseContext).parse(wire);
    auto* context = Query::Context::create(&req->arena, *req->createPlanningContext(*req->reader));
    auto* weight = query->createWeight(*context, Query::NEED_SCORES);
    Query::Weight::PrepareContext prepareContext{*req->reader, {}, nullptr, nullptr};
    auto selection = prepared ? weight->prepare(prepareContext) : nullptr;
    auto& segment = req->reader->segments()[0];
    SkipStatsScope stats;
    Query::SegmentSource* source = weight;
    if (selection) source = selection.get();
    auto* supplier = source->scorerSupplier(req->requestPool, segment);
    ASSERT_NE(nullptr, supplier);
    EXPECT_EQ(1, SkipStats::joinChildSuppliers);
    auto plan = supplier->planBulk(Query::ExecutionUse::SCORED_WINDOWS, {});
    ASSERT_EQ(Query::ScorerSupplier::BulkAnswer::NO, plan.available);
    supplier->recordBulkPlanCommitment(Query::ExecutionUse::SCORED_WINDOWS, {}, plan);
    EXPECT_EQ(1, SkipStats::joinChildSuppliers);
    auto* scorer = supplier->resolve(req->requestPool, supplier->makePlanContext(
        Query::Demand::fromLeadCost(segment.maxDoc())))->build(req->requestPool);
    ASSERT_NE(nullptr, scorer);
    int32_t count = 0;
    while (scorer->next() != PostingsReader::END) { count++; EXPECT_GE(scorer->score(), 0.0f); }
    EXPECT_EQ(multiple ? 1 : 12, count);
    EXPECT_EQ(1, SkipStats::joinChildSuppliers);
  }
}

TEST_F(JoinCacheTest, requiredTermsRegisterNoUsesScoredOrCounted) {
  indexRangeOwners();
  auto req = localReq(helper.getSearchEngine());
  auto term = qb::match(req->mr, "tag_w", "common");
  auto* terms = parseQuery(*req, qb::boolean(req->mr, {term, qb::match(req->mr, "id", "r1")}));
  auto* context = Query::Context::create(&req->arena, *req->createPlanningContext(*req->reader));
  auto epoch = cache->currentEpoch();
  auto* scored = terms->createWeight(*context, Query::NEED_SCORES | Query::ALLOW_PRUNING);
  EXPECT_NE(nullptr, scored->scorerSupplier(req->requestPool, req->reader->segments()[0]));
  EXPECT_NE(nullptr, terms->createWeight(*context, 0)->scorerSupplier(req->requestPool, req->reader->segments()[0]));
  EXPECT_EQ(epoch, cache->currentEpoch());
  EXPECT_TRUE(cache->dump(100, nullptr, true).empty());
  // A counted clause that would score a constant is the one that registers.
  auto registered = [&](bool disabled) {
    SearchOverridesGuard mandatory(BooleanQuery::disableMandatoryCacheForTests);
    BooleanQuery::disableMandatoryCacheForTests = disabled;
    auto counted = localReq(helper.getSearchEngine());
    auto* query = parseQuery(*counted, qb::boolean(counted->mr, {qb::match(counted->mr, "tag_w", "common"),
        qb::toParent(counted->mr, "rows_nested", qb::range(counted->mr, "value_i",
            qb::valI64(counted->mr, 1), nullptr, qb::valI64(counted->mr, 2), nullptr), "max")}));
    auto before = cache->currentEpoch();
    query->createWeight(*Query::Context::create(&counted->arena, *counted->createPlanningContext(*counted->reader)), 0);
    return cache->currentEpoch() - before;
  };
  auto without = registered(true);
  EXPECT_GT(registered(false), without);
}

TEST_F(JoinCacheTest, requiredConstantsShareOwnerMembershipAndKeepTheirScores) {
  SearchOverridesGuard whole(QueryPrep::disableWholeMembershipPlanForTests);
  QueryPrep::disableWholeMembershipPlanForTests = true;
  indexRangeOwners();
  for (int variant = 0; variant < 4; variant++) for (bool requiredFirst : {false, true}) {
    SCOPED_TRACE(variant);
    SCOPED_TRACE(requiredFirst);
    auto wire = localReq(helper.getSearchEngine());
    auto join = qb::toParent(wire->mr, "rows_nested",
        qb::range(wire->mr, "value_i", qb::valI64(wire->mr, 1), nullptr,
            qb::valI64(wire->mr, 2), nullptr), variant == 3 ? "none" : "max");
    if (variant == 1) join = qb::boost(wire->mr, join, 2.5f);
    if (variant == 2) join = qb::constantScore(wire->mr, join, 3.25f);
    auto run = [&](bool required) {
      auto req = localReq(helper.getSearchEngine());
      auto term = qb::match(req->mr, "tag_w", "common");
      auto& top = req->topDocs().getScores().fields({"id"}).limit(10).countLimit();
      top.rawQuery() = required ? qb::boolean(req->mr, {term, join})
          : qb::boolean(req->mr, {term}, {}, {}, {join});
      req->execute(1); EXPECT_OK(req);
      EXPECT_EQ(64, req->getMatchCount());
      std::vector<std::pair<std::string, float>> result;
      auto* docs = req->docList();
      auto ids = std::get<api::ColStr>(docs->columns.at("id").kind).v;
      auto scores = std::get<api::ColFloat>(docs->columns.at("_score_").kind).v;
      for (size_t i = 0; i < ids.size(); i++) result.emplace_back(ids[i], scores[i]);
      return result;
    };
    cache->resize(0);
    auto requiredOff = run(true), filteredOff = run(false);
    cache->resize(1024 * 1024); cache->resetAdmission();
    fundSource("", join);
    fundSource("", join);
    auto builds = cache->counters().builds;
    EXPECT_EQ(requiredFirst ? requiredOff : filteredOff, run(requiredFirst));
    EXPECT_GT(cache->counters().builds, builds);
    SkipStatsScope stats;
    EXPECT_EQ(requiredFirst ? filteredOff : requiredOff, run(!requiredFirst));
    EXPECT_EQ(0, SkipStats::joinChildSuppliers);
    for (const auto& entry : cache->dump(100, nullptr, true)) for (const auto& value : entry.values) {
      if (value.space != SpaceOrdinal::ROOT) {
        EXPECT_EQ(0u, value.absentCount); EXPECT_EQ(0u, value.servedCount);
      }
    }
  }
}

TEST_F(JoinCacheTest, exactLocalMembershipKeepsScoresAndResidentOwnerSources) {
  cache = std::make_shared<FilterCache>(FilterCacheConfig{.maxBytes = 1024 * 1024,
      .minSegmentDocs = 0, .admissionThreshold = 2, .maxMetadataBytes = 256 * 1024});
  helper.getIndexWriter()->snapshots.readers.filterCache = cache;
  indexRangeOwners();
  SearchOverridesGuard local(QueryPrep::disableWholeMembershipBypassReuseForTests);
  for (bool deleted : {false, true}) {
    if (deleted) { ASSERT_TRUE(helper.deleteById("r0", UpdateMessage::COMMIT).success); }
    for (bool required : {false, true}) for (int variant = 0; variant < 5; variant++) {
      SCOPED_TRACE(required);
      SCOPED_TRACE(variant);
      auto wire = localReq(helper.getSearchEngine());
      auto join = qb::toParent(wire->mr, "rows_nested",
          qb::range(wire->mr, "value_i", qb::valI64(wire->mr, 1), nullptr,
              qb::valI64(wire->mr, 2), nullptr), variant == 3 ? "none" : "max");
      if (variant == 1) join = qb::boost(wire->mr, join, 2.5f);
      if (variant == 2) join = qb::constantScore(wire->mr, join, 3.25f);
      auto run = [&]() {
        auto req = localReq(helper.getSearchEngine());
        auto term = qb::match(req->mr, "tag_w", "common");
        auto& top = req->topDocs().countLimit().getScores().fields({"id"}).limit(10);
        if (variant == 4) {
          top.rawQuery() = required ? qb::boolean(req->mr, {term, join, join})
              : qb::boolean(req->mr, {term}, {}, {}, {join, join});
        } else {
          top.rawQuery() = required ? qb::boolean(req->mr, {term, join})
              : qb::boolean(req->mr, {term}, {}, {}, {join});
        }
        req->execute(1); EXPECT_OK(req);
        EXPECT_EQ(64, req->getMatchCount());
        std::vector<std::pair<std::string, uint32_t>> result;
        auto* docs = req->docList();
        auto ids = std::get<api::ColStr>(docs->columns.at("id").kind).v;
        auto scores = std::get<api::ColFloat>(docs->columns.at("_score_").kind).v;
        for (size_t i = 0; i < ids.size(); i++) result.emplace_back(ids[i], std::bit_cast<uint32_t>(scores[i]));
        return result;
      };
      cache->resize(0);
      QueryPrep::disableWholeMembershipBypassReuseForTests = true;
      auto off = run();
      QueryPrep::disableWholeMembershipBypassReuseForTests = false;
      {
        SkipStatsScope stats;
        EXPECT_EQ(off, run());
        EXPECT_EQ(0, SkipStats::wholeTopKCountLocalCounts);
      }
      cache->resize(1024 * 1024); cache->resetAdmission();
      publishSource(join);
      {
        SkipStatsScope stats;
        auto before = cache->counters();
        EXPECT_EQ(off, run());
        EXPECT_EQ(0, SkipStats::wholeTopKCountLocalCounts);
        EXPECT_EQ(1, SkipStats::wholeTopKCountFallbackSuppliers);
        EXPECT_EQ(0, SkipStats::joinChildSuppliers);
        EXPECT_EQ(before.builds, cache->counters().builds);
      }
      bool ownerSample = false;
      for (const auto& entry : cache->dump(100, nullptr, true)) for (const auto& value : entry.values) {
        ownerSample |= value.resident && value.space == SpaceOrdinal::ROOT && value.servedCount > 0;
      }
      EXPECT_TRUE(ownerSample);
      {
        SkipStatsScope stats;
        EXPECT_EQ(off, run());
        EXPECT_EQ(1, SkipStats::wholeTopKCountBuilds);
        EXPECT_EQ(0, SkipStats::wholeTopKCountLocalCounts);
        EXPECT_EQ(0, SkipStats::joinChildSuppliers);
      }
      cache->resize(1); cache->sweep(); cache->resize(1024 * 1024);
      publishSource(join);
      {
        SkipStatsScope stats;
        auto before = cache->counters();
        EXPECT_EQ(off, run());
        EXPECT_EQ(deleted ? 1 : 0, SkipStats::wholeTopKCountLocalCounts);
        EXPECT_EQ(deleted ? 0 : 1, SkipStats::wholeMembershipCardinalityCounts);
        EXPECT_EQ(0, SkipStats::joinChildSuppliers);
        EXPECT_EQ(before.builds, cache->counters().builds);
      }
      {
        SkipStatsScope stats;
        EXPECT_EQ(off, run());
        EXPECT_EQ(1, SkipStats::wholeTopKCountBuilds);
      }
      {
        SkipStatsScope stats;
        EXPECT_EQ(off, run());
        EXPECT_EQ(1, SkipStats::wholeTopKCountHits);
        EXPECT_EQ(0, SkipStats::joinChildSuppliers);
      }
    }
  }
}

TEST_F(JoinCacheTest, localWholeCountScreensBeforeConstructingTheSupplier) {
  cache = std::make_shared<FilterCache>(FilterCacheConfig{.maxBytes = 1024 * 1024,
      .minSegmentDocs = 0, .admissionThreshold = 2, .maxMetadataBytes = 256 * 1024});
  helper.getIndexWriter()->snapshots.readers.filterCache = cache;
  indexRangeOwners();
  ASSERT_TRUE(helper.deleteById("r0", UpdateMessage::COMMIT).success);
  auto wireReq = localReq(helper.getSearchEngine());
  auto join = qb::toParent(wireReq->mr, "rows_nested", qb::range(wireReq->mr, "value_i",
      qb::valI64(wireReq->mr, 1), nullptr, qb::valI64(wireReq->mr, 2), nullptr), "none");
  auto wire = qb::boolean(wireReq->mr, {qb::match(wireReq->mr, "tag_w", "common"), join});
  publishSource(wire);
  cache->resize(1); cache->sweep(); cache->resize(1024 * 1024);
  auto req = localReq(helper.getSearchEngine());
  auto* query = parseQuery(*req, wire);
  auto* context = Query::Context::create(&req->arena, *req->createPlanningContext(*req->reader));
  auto* weight = query->createWeight(*context, 0);
  auto* use = context->getFilterUse(*query, FilterCache::AdmissionLane::WHOLE);
  QueryPrep::WholeMembershipPlan plan(*weight, use, context->filterUses,
      PreparedDomainDependence::QUERY_CANONICAL, QueryPrep::WholeMembershipConsumer::TOP_K_COUNT);
  SkipStatsScope stats;
  EXPECT_FALSE(resolveWholeMembershipForTests(plan, *req->reader, req->reader->segments()[0], nullptr, nullptr, 1).available);
  EXPECT_FALSE(resolveWholeMembershipForTests(plan, *req->reader, req->reader->segments()[0], nullptr, nullptr, 128, 1).available);
  EXPECT_EQ(0, SkipStats::joinChildSuppliers);
  auto result = resolveWholeMembershipForTests(plan, *req->reader, req->reader->segments()[0], nullptr, nullptr, 128);
  ASSERT_TRUE(result.available);
  EXPECT_EQ(64, result.count);
  EXPECT_EQ(nullptr, result.docs.get());
  EXPECT_EQ(nullptr, use->rawDocSet(0));
  EXPECT_EQ(0u, use->ownedBytesForTest());
  EXPECT_EQ(1, SkipStats::joinChildSuppliers);
  EXPECT_EQ(1, SkipStats::wholeTopKCountLocalCounts);
}

TEST_F(JoinCacheTest, residentConstantKeepsBoundsWithSeveralScoringClauses) {
  SearchOverridesGuard whole(QueryPrep::disableWholeMembershipPlanForTests);
  QueryPrep::disableWholeMembershipPlanForTests = true;
  indexRangeOwners();
  auto wire = localReq(helper.getSearchEngine());
  auto join = qb::boost(wire->mr, qb::toParent(wire->mr, "rows_nested",
      qb::range(wire->mr, "value_i", qb::valI64(wire->mr, 1), nullptr,
          qb::valI64(wire->mr, 2), nullptr), "max"), 1.3f);
  auto run = [&](bool pruned) {
    auto req = localReq(helper.getSearchEngine());
    req->topDocs().getScores().fields({"id"}).limit(1).countLimit(pruned ? 1 : -1).rawQuery() =
        qb::boolean(req->mr, {qb::match(req->mr, "tag_w", "common"),
            qb::match(req->mr, "band_w", "at"), join});
    req->execute(1); EXPECT_OK(req);
    auto* docs = req->docList();
    auto ids = std::get<api::ColStr>(docs->columns.at("id").kind).v;
    auto scores = std::get<api::ColFloat>(docs->columns.at("_score_").kind).v;
    EXPECT_EQ(1u, ids.size());
    return std::pair(std::string(ids[0]), scores[0]);
  };
  cache->resize(0); auto ordinary = run(false);
  cache->resize(1024 * 1024); publishSource(join);
  for (bool pruned : {false, true}) {
    SkipStatsScope stats;
    auto resident = run(pruned);
    EXPECT_EQ(ordinary.first, resident.first);
    EXPECT_FLOAT_EQ(ordinary.second, resident.second);
    EXPECT_EQ(0, SkipStats::joinChildSuppliers);
  }
}

TEST_F(JoinCacheTest, scoredSumJoinKeepsItsOrdinarySourceWithResidentOwners) {
  SearchOverridesGuard whole(QueryPrep::disableWholeMembershipPlanForTests);
  QueryPrep::disableWholeMembershipPlanForTests = true;
  indexRangeOwners();
  auto run = [&] {
    auto req = localReq(helper.getSearchEngine());
    auto join = qb::toParent(req->mr, "rows_nested",
        qb::range(req->mr, "value_i", qb::valI64(req->mr, 1), nullptr,
            qb::valI64(req->mr, 2), nullptr), "sum");
    req->topDocs().getScores().limit(10).countLimit().rawQuery() =
        qb::boolean(req->mr, {qb::match(req->mr, "tag_w", "common"), join});
    req->execute(1); EXPECT_OK(req);
    EXPECT_EQ(64, req->getMatchCount());
    return req->getDocs();
  };
  cache->resize(0); auto ordinary = run();
  cache->resize(1024 * 1024);
  auto wire = localReq(helper.getSearchEngine());
  publishSource(qb::toParent(wire->mr, "rows_nested",
      qb::range(wire->mr, "value_i", qb::valI64(wire->mr, 1), nullptr,
          qb::valI64(wire->mr, 2), nullptr), "none"));
  SkipStatsScope stats;
  auto resident = run();
  EXPECT_GT(SkipStats::joinChildSuppliers, 0);
  ASSERT_EQ(ordinary.size(), resident.size());
  for (size_t i = 0; i < ordinary.size(); i++) EXPECT_TRUE(docEquals(ordinary[i], resident[i]));
}

TEST_F(JoinCacheTest, mandatoryResidentSourceServesPullBulkCountAndSingleton) {
  indexRangeOwners(1024);
  auto wire = localReq(helper.getSearchEngine());
  for (auto word : {"below", "at"}) {
    auto constant = qb::constantScore(wire->mr, qb::match(wire->mr, "band_w", word), 3.25f);
    publishSource(constant);
    for (int mode = 0; mode < 4; mode++) {
      SCOPED_TRACE(word);
      SCOPED_TRACE(mode);
      auto req = localReq(helper.getSearchEngine());
      auto term = qb::match(req->mr, "tag_w", "common");
      auto queryWire = mode == 3 ? qb::boolean(req->mr, {constant}, {term})
          : qb::boolean(req->mr, {term, constant});
      auto* query = parseQuery(*req, queryWire);
      auto* context = Query::Context::create(&req->arena, *req->createPlanningContext(*req->reader));
      int32_t flags = mode >= 2 ? 0 : Query::NEED_SCORES | (mode == 0 ? Query::ALLOW_PRUNING : 0);
      auto* weight = query->createWeight(*context, flags);
      auto before = cache->counters();
      auto& segment = req->reader->segments()[0];
      auto* supplier = weight->scorerSupplier(req->requestPool, segment);
      bool accepted = std::string_view(word) == "at" || mode != 0;
      EXPECT_EQ(accepted ? 1u : 0u, cache->counters().hits - before.hits);
      int32_t expected = std::string_view(word) == "at" ? 4 : 3;
      if (mode == 0) {
        auto* scorer = supplier->resolve(req->requestPool, supplier->makePlanContext(
            Query::Demand::fromLeadCost(segment.maxDoc())))->build(req->requestPool);
        int32_t count = 0;
        while (scorer->next() != PostingsReader::END) { count++; EXPECT_GT(scorer->score(), 3.25f); }
        EXPECT_EQ(expected, count);
      } else {
        auto use = mode == 1 ? Query::ExecutionUse::SCORED_WINDOWS : Query::ExecutionUse::COUNT_WINDOWS;
        auto plan = supplier->planBulk(use, {});
        ASSERT_EQ(Query::ScorerSupplier::BulkAnswer::YES, plan.available);
        if (mode == 3) { EXPECT_NE(nullptr, supplier->exactDocSet()); }
        if (plan.hasConstantCount()) { EXPECT_EQ(expected, plan.constantCount); continue; }
        auto* bulk = supplier->buildBulk(req->requestPool, plan);
        int64_t count = 0;
        for (int32_t cursor = 0; cursor != PostingsReader::END && cursor < segment.maxDoc();) {
          if (mode == 1) {
            ScoreWindow window;
            cursor = bulk->scoreNextWindow(window, nullptr, cursor, segment.maxDoc(), 0);
            count += window.size;
            for (int32_t i = 0; i < window.size; i++) EXPECT_GT(window.scores[i], 3.25f);
          } else cursor = bulk->countNextWindow(count, nullptr, nullptr, cursor, segment.maxDoc());
        }
        EXPECT_EQ(expected, count);
      }
    }
  }
}

TEST_F(JoinCacheTest, untimedRequiredClausesDoNotFundTheirMembership) {
  indexRangeOwners();
  auto wire = localReq(helper.getSearchEngine());
  auto join = qb::toParent(wire->mr, "rows_nested",
      qb::range(wire->mr, "value_i", qb::valI64(wire->mr, 1), nullptr,
          qb::valI64(wire->mr, 2), nullptr), "max");
  auto run = [&] {
    auto req = localReq(helper.getSearchEngine());
    auto* query = parseQuery(*req, qb::boolean(req->mr, {qb::match(req->mr, "tag_w", "common"), join}));
    auto* context = Query::Context::create(&req->arena, *req->createPlanningContext(*req->reader));
    auto* weight = query->createWeight(*context, Query::NEED_SCORES | Query::ALLOW_PRUNING);
    auto docs = QueryPrep::materialize(weight->scorerSupplier(req->requestPool, req->reader->segments()[0]),
        req->requestPool, req->reader->segments()[0], nullptr);
    EXPECT_EQ(64, docs->card());
  };
  for (int i = 0; i < 10; i++) run();
  EXPECT_EQ(0u, cache->counters().assumedCharges);
  EXPECT_EQ(0u, cache->counters().observedCharges);
  EXPECT_EQ(0u, cache->counters().buildAttempts);
  publishSource(join);
  auto before = cache->counters();
  SkipStatsScope stats;
  run();
  EXPECT_EQ(0, SkipStats::joinChildSuppliers);
  EXPECT_EQ(before.buildAttempts, cache->counters().buildAttempts);
  EXPECT_EQ(before.hits + 1, cache->counters().hits);
}

TEST_F(JoinCacheTest, residentRangeOwnerAvoidsChildSupplier) {
  CollectionHelper::UpdateBuilder input;
  for (int i = 0; i < 256; i++) {
    auto json = "{\"id\":\"r" + std::to_string(i) + "\",\"tag_w\":\"common\","
        "\"rows_nested\":{\"value_i\":" + std::to_string(i % 4) + "}}";
    input.addRaw([&](auto& doc, auto& arena) { ASSERT_TRUE(api::read_json(doc, json, arena)); });
  }
  input.commit(true, 1);
  ASSERT_TRUE(helper.submit(input).success);
  for (bool folded : {false, true}) for (bool exact : {false, true}) {
    SCOPED_TRACE(folded);
    SCOPED_TRACE(exact);
    cache->clear();
    cache->resetAdmission();
    auto run = [&](bool warm) {
      auto req = localReq(helper.getSearchEngine());
      auto& mr = req->mr;
      auto join = qb::toParent(mr, "rows_nested",
          qb::range(mr, "value_i", qb::valI64(mr, 1), nullptr, qb::valI64(mr, 2), nullptr), "max");
      auto& op = req->topDocs().getScores().limit(warm ? 0 : 10).countLimit(exact ? -1 : 20);
      if (warm) op.rawQuery() = join;
      else if (folded) { op.rawQuery() = qb::match(mr, "tag_w", "common"); op.filter(join); }
      else op.rawQuery() = qb::boolean(mr, {qb::match(mr, "tag_w", "common")}, {}, {}, {join});
      req->execute(1);
      EXPECT_OK(req);
      EXPECT_GE(req->getMatchCount(), 20);
      if (!warm) { EXPECT_EQ(10u, req->getDocs().size()); }
    };
    run(true);
    run(true);
    auto before = cache->counters();
    SkipStatsScope stats;
    run(false);
    auto after = cache->counters();
    EXPECT_EQ(0, SkipStats::joinChildSuppliers);
    for (const auto& entry : cache->dump(20, nullptr, true)) {
      for (const auto& value : entry.values) {
        if (value.space != SpaceOrdinal::ROOT) {
          EXPECT_EQ(0u, value.absentCount);
          EXPECT_EQ(0u, value.servedCount);
        }
      }
    }
    EXPECT_EQ(1u, after.hits - before.hits);
    EXPECT_EQ(exact ? 1u : 0u, after.misses - before.misses);
    EXPECT_EQ(before.buildAttempts, after.buildAttempts);
  }
}

TEST_F(JoinCacheTest, booleanOwnerFilterRecordsOrdinaryTimeBeforeBuying) {
  CollectionHelper::UpdateBuilder input;
  for (int i = 0; i < 128; i++) {
    auto json = "{\"id\":\"t" + std::to_string(i) + "\",\"tag_w\":\"common\","
        "\"rows_nested\":{\"value_i\":" + std::to_string(i % 4) + "}}";
    input.addRaw([&](auto& doc, auto& arena) { ASSERT_TRUE(api::read_json(doc, json, arena)); });
  }
  input.commit(true, 1);
  ASSERT_TRUE(helper.submit(input).success);
  for (bool folded : {false, true}) {
    cache->clear();
    cache->resetAdmission();
    for (int i = 0; i < 2; i++) {
      auto req = localReq(helper.getSearchEngine());
      auto& mr = req->mr;
      auto join = qb::toParent(mr, "rows_nested",
          qb::range(mr, "value_i", qb::valI64(mr, 1), nullptr, qb::valI64(mr, 2), nullptr), "max");
      auto& op = req->topDocs().getScores().limit(10).countLimit(20);
      if (folded) { op.rawQuery() = qb::match(mr, "tag_w", "common"); op.filter(join); }
      else op.rawQuery() = qb::boolean(mr, {qb::match(mr, "tag_w", "common")}, {}, {}, {join});
      req->execute(1);
      ASSERT_OK(req);
    }
    EXPECT_EQ(0u, cache->counters().buildAttempts);
    uint64_t owners = 0, children = 0;
    for (const auto& entry : cache->dump(20, nullptr, true)) {
      for (const auto& value : entry.values) {
        (value.space == SpaceOrdinal::ROOT ? owners : children) += value.absentCount;
      }
    }
    EXPECT_EQ(1u, owners);
    EXPECT_EQ(1u, children);
  }
}

TEST_F(JoinCacheTest, exactDomainAttemptObservesOwnerBeforeStreamingSubOperations) {
  CollectionHelper::UpdateBuilder input;
  for (int i = 0; i < 128; i++) {
    auto json = "{\"id\":\"d" + std::to_string(i) + "\",\"tag_w\":\"common\","
        "\"group_s\":\"g\",\"rows_nested\":{\"value_i\":" + std::to_string(i % 4) + "}}";
    input.addRaw([&](auto& doc, auto& arena) { ASSERT_TRUE(api::read_json(doc, json, arena)); });
  }
  input.commit(true, 1);
  ASSERT_TRUE(helper.submit(input).success);
  auto run = [&](bool subOperations) {
    auto req = localReq(helper.getSearchEngine());
    auto& mr = req->mr;
    auto join = qb::toParent(mr, "rows_nested",
        qb::range(mr, "value_i", qb::valI64(mr, 1), nullptr, qb::valI64(mr, 2), nullptr), "max");
    auto& top = req->topDocs().getScores().limit(10).countLimit(20);
    top.rawQuery() = qb::boolean(mr, {qb::match(mr, "tag_w", "common")}, {}, {}, {join});
    if (subOperations) {
      top.limit(0);
      top.facet("groups", "group_s").limit(-1);
    }
    req->execute(1);
    ASSERT_OK(req);
    EXPECT_GE(req->getMatchCount(), 20);
  };
  run(false);
  SkipStatsScope stats;
  run(true);
  EXPECT_GT(SkipStats::exactDomainStreamFallbacks, 0);
  EXPECT_EQ(0u, cache->counters().buildAttempts);
  uint64_t samples = 0;
  for (const auto& entry : cache->dump(20, nullptr, true)) {
    for (const auto& value : entry.values) {
      if (value.space == SpaceOrdinal::ROOT) samples += value.absentCount;
    }
  }
  EXPECT_EQ(1u, samples);
}

TEST_F(JoinCacheTest, requiredUniformRangeEarnsChildMembershipAndPreservesScores) {
  SearchOverridesGuard whole(QueryPrep::disableWholeMembershipPlanForTests,
      BooleanQuery::disableMandatoryCacheForTests);
  QueryPrep::disableWholeMembershipPlanForTests = true;
  BooleanQuery::disableMandatoryCacheForTests = true;
  CollectionHelper::UpdateBuilder input;
  for (int i = 0; i < 128; i++) {
    auto json = "{\"id\":\"s" + std::to_string(i) + "\",\"tag_w\":\"common\","
        "\"rows_nested\":{\"value_i\":" + std::to_string(i % 4) + "}}";
    input.addRaw([&](auto& doc, auto& arena) { ASSERT_TRUE(api::read_json(doc, json, arena)); });
  }
  input.commit(true, 1); ASSERT_TRUE(helper.submit(input).success);
  auto run = [&] {
    auto req = localReq(helper.getSearchEngine());
    auto join = qb::toParent(req->mr, "rows_nested",
        qb::range(req->mr, "value_i", qb::valI64(req->mr, 1), nullptr, qb::valI64(req->mr, 2), nullptr), "max");
    req->topDocs().getScores().fields({"id"}).limit(10).countLimit(20).rawQuery() =
        qb::boolean(req->mr, {qb::match(req->mr, "tag_w", "common"), join});
    req->execute(1); EXPECT_OK(req);
    std::vector<std::pair<std::string, float>> result;
    auto* docs = req->docList();
    auto ids = std::get<api::ColStr>(docs->columns.at("id").kind).v;
    auto scores = std::get<api::ColFloat>(docs->columns.at("_score_").kind).v;
    for (size_t i = 0; i < ids.size(); i++) result.emplace_back(ids[i], scores[i]);
    EXPECT_EQ(20, req->getMatchCount());
    return result;
  };
  auto ordinary = run();
  auto wire = localReq(helper.getSearchEngine());
  fundSource("rows_nested", qb::range(wire->mr, "value_i", qb::valI64(wire->mr, 1), nullptr,
      qb::valI64(wire->mr, 2), nullptr));
  auto builds = cache->counters().builds;
  EXPECT_EQ(ordinary, run());
  EXPECT_EQ(builds + 1, cache->counters().builds);
  auto hits = cache->counters().hits;
  EXPECT_EQ(ordinary, run());
  EXPECT_GT(cache->counters().hits, hits);
  cache->resize(0);
  EXPECT_EQ(ordinary, run());
}

TEST_F(JoinCacheTest, untimedJoinOnlyServesItsChildAndNeverFundsIt) {
  auto run = [&] {
    auto req = localReq(helper.getSearchEngine());
    req->reader = helper.getIndexWriter()->snapshots.readers.getReader();
    req->schema = req->reader->schema();
    req->filterUses = std::make_shared<FilterCache::UseRegistry>(cache.get(), *req->reader);
    ParseView view(*req->schema, nullptr, &req->reader, req.get());
    ParseContext parseContext(req->requestPool, *req->schema, req->arena,
        CoerceContext{}, "q", nullptr, &view);
    auto wire = qb::toParent(req->mr, "items_nested", qb::match(req->mr, "color_s", "red"), "none");
    auto* query = ProtobufQueryParser(parseContext).parse(wire);
    auto* context = Query::Context::create(&req->arena, *req->createPlanningContext(*req->reader));
    auto* weight = query->createWeight(*context, 0);
    auto& segment = req->reader->segments()[0];
    auto* supplier = weight->scorerSupplier(req->requestPool, segment);
    EXPECT_NE(nullptr, supplier);
    auto docs = QueryPrep::materialize(supplier, req->requestPool, segment, nullptr);
    EXPECT_EQ(12, docs->card());
  };
  for (int i = 0; i < 20; i++) run();
  EXPECT_EQ(0u, cache->counters().assumedCharges);
  EXPECT_EQ(0u, cache->counters().buildAttempts);
  auto wire = localReq(helper.getSearchEngine());
  fundSource("items_nested", qb::match(wire->mr, "color_s", "red"));
  EXPECT_EQ(12, count());
  auto before = cache->counters();
  for (int i = 0; i < 20; i++) run();
  EXPECT_EQ(before.buildAttempts, cache->counters().buildAttempts);
  EXPECT_EQ(before.assumedCharges, cache->counters().assumedCharges);
  EXPECT_EQ(before.observedCharges, cache->counters().observedCharges);
  EXPECT_GE(cache->counters().hits, before.hits + 20);
}

TEST_F(JoinCacheTest, nestedTopDocsFundsAnExactSourceBeyondItsSmallParentDomain) {
  CollectionHelper::UpdateBuilder input;
  for (int i = 0; i < 16; i++) {
    auto json = "{\"id\":\"n" + std::to_string(i)
        + "\",\"tag_w\":\"common\",\"value_i\":" + std::to_string(i) + "}";
    input.addRaw([&](auto& doc, auto& arena) { ASSERT_TRUE(api::read_json(doc, json, arena)); });
  }
  input.commit(true, 1); ASSERT_TRUE(helper.submit(input).success);
  auto run = [&](std::string_view id, bool source) {
    auto req = localReq(helper.getSearchEngine());
    auto& root = req->topDocs("root").matchQuery("id", id).limit(0);
    auto& nested = root.topDocs("inside").countLimit().limit(0);
    if (source) nested.matchQuery("tag_w", "common");
    else nested.allQuery();
    nested.avg("mean", "value_i");
    req->execute(1); ASSERT_OK(req);
    auto* inner = req->docList("root")->ops.at("inside")->docList();
    ASSERT_NE(nullptr, inner);
    EXPECT_EQ(1, *inner->found);
  };
  run("n0", false); run("n0", false);
  run("n0", true);
  auto wire = localReq(helper.getSearchEngine());
  fundSource("", qb::match(wire->mr, "tag_w", "common"));
  auto before = cache->counters();
  run("n0", true);
  EXPECT_EQ(before.builds + 1, cache->counters().builds);
  auto hits = cache->counters().hits;
  run("n1", true);
  EXPECT_GT(cache->counters().hits, hits);
}

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

TEST_F(JoinCacheTest, mainAndProhibitedPositionsAdmitOnSecondSighting) {
  for (int32_t position : {0, 2}) {
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
  {
    SearchOverridesGuard guard(QueryPrep::disableWholeMembershipPlanForTests);
    QueryPrep::disableWholeMembershipPlanForTests = true;
    for (int i = 0; i < 2; i++) EXPECT_EQ(1, count(1, false, 0, true));
    auto wire = localReq(helper.getSearchEngine());
    fundSource("", qb::toParent(wire->mr, "items_nested",
        qb::match(wire->mr, "color_s", "red"), "none"));
    EXPECT_EQ(1, count(1, false, 0, true));
  }
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

TEST_F(JoinCacheTest, fundedRawChildIsReusedAcrossDirectAndComposedJoins) {
  EXPECT_EQ(6, count(0, false, 2));
  auto wire = localReq(helper.getSearchEngine());
  fundSource("items_nested.deep_nested", qb::phraseText(wire->mr, "text_w", "deep common"));
  auto before = cache->counters();
  EXPECT_EQ(6, count(0, false, 3));
  EXPECT_GT(cache->counters().builds, before.builds);
  auto hits = cache->counters().hits;
  EXPECT_EQ(6, count(0, false, 2));
  EXPECT_GT(cache->counters().hits, hits);
  cache->resize(0);
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
  // Owner production censors child observations; the resident owner skips it.
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
