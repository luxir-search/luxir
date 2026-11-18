// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include <random>
#include <glaze/glaze.hpp>
#include "luxir/query/ParseContext.h"
#include "luxir/query/ProtobufQueryParser.h"
#include "luxir/reader/SkipStats.h"
#include "luxir/server/JsonResponse.h"
#include "test/CollectionHelper.h"
#include "test/KnnTestUtils.h"
#include "test/LocalReq.h"
#include "test/QueryBuild.h"
#include "test/SchemaBuilder.h"

namespace luxir::test {
namespace {
class OwnerKnnTest : public LuxirTest {
protected:
  CollectionHelper helper;
  struct Row {
    std::string label;
    int root, item, eligible;
    std::vector<float> values;
  };
  std::vector<Row> roots, items, parts;
  using Hits = std::vector<std::pair<std::string, float>>;

  void schema(bool reverse, api::VectorAnnDef ann = rabitqAnn(4)) {
    SchemaBuilder schema;
    for (auto path : {"items", "items.parts"}) {
      auto& field = schema.field(path);
      field.type = api::FieldDef::FieldClass::NESTED;
      field.reverse_map = reverse;
    }
    for (auto path : {"items.v", "items.parts.v"}) {
      auto& field = schema.field(path);
      field.type = api::FieldDef::FieldClass::VECTOR;
      field.column = true; field.multi = api::FieldDef::MultiMode::ARRAY;
      field.metric = api::VectorMetric::L2; field.dims = 4; field.ann = ann;
    }
    schema.set(helper.collection());
    helper.getIndexWriter()->setMergeFactor(100);
  }

  void add(std::string_view json, bool commit = false) {
    CollectionHelper::UpdateBuilder input;
    input.addRaw([&](auto& doc, auto& arena) { EXPECT_TRUE(api::read_json(doc, json, arena)); });
    if (commit) input.commit();
    ASSERT_TRUE(helper.submit(input).success);
  }

  LocalReqHandle request(std::string_view body) {
    auto req = localReq(helper.getSearchEngine());
    EXPECT_TRUE(api::read_json(req->rawRequest(), body, req->mr)) << body;
    req->execute(1);
    EXPECT_OK(req);
    return req;
  }

  static Hits hits(const LocalReq& req) {
    Hits result;
    if (!req.ok()) return result;
    for (auto* response : req.responses) {
      const auto* entry = response->proto.ops.find("q");
      if (!entry) continue;
      const auto* list = (*entry)->docList();
      if (!list || list->row_count == 0) continue;
      auto labels = std::get<api::ColStr>(list->columns.at("label_s").kind).v;
      auto scores = std::get<api::ColFloat>(list->columns.at("_score_").kind).v;
      for (size_t i = 0; i < labels.size(); i++) result.emplace_back(labels[i], scores[i]);
    }
    return result;
  }

  static std::string query(std::string_view field, int k, bool exact, std::string_view extra = {}) {
    return R"({"knn":{"field":")" + std::string(field) + R"(","query":[0,0,0,0],"k":)"
        + std::to_string(k) + R"(,"exact":)" + (exact ? "true" : "false") + std::string(extra) + "}}";
  }

  LocalReqHandle search(std::string_view view, std::string_view field, int k,
      bool exact = true, int eligibility = -1, std::string_view extra = {}) {
    auto body = R"({"limit":-1,"count_limit":-1,"fields":["label_s"],"get_scores":true,)"
        + (eligibility < 0 ? std::string() : R"("filter":["eligible_i:)" + std::to_string(eligibility) + R"("],)")
        + R"("query":)" + query(field, k, exact, extra) + '}';
    if (view.empty()) return request(body);
    return request(R"({"ops":{"q":{"domain":{"to":")" + std::string(view) + R"("},"top_docs":)" + body + "}}}");
  }

  static float score(float value) { return 1.0f / (1.0f + value * value); }

  Hits model(std::string_view view, bool deep, int k, int eligibility, bool deleted, float floor = -1) {
    const auto& units = view.empty() ? roots : view == "items" ? items : parts;
    const auto& source = deep ? parts : items;
    Hits result;
    for (const auto& unit : units) {
      if ((deleted && unit.root % 7 == 0) || (eligibility >= 0 && unit.eligible != eligibility)) continue;
      float best = -1;
      for (const auto& row : source) {
        if (view.empty() ? row.root != unit.root : view == "items" ? row.item != unit.item : row.label != unit.label) continue;
        for (float value : row.values) best = std::max(best, score(value));
      }
      if (best >= 0) result.emplace_back(unit.label, best);
    }
    std::stable_sort(result.begin(), result.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
    if (result.size() > (size_t)k) result.resize((size_t)k);
    std::erase_if(result, [&](const auto& hit) { return hit.second < floor; });
    return result;
  }

  void generate(uint32_t seed, api::VectorAnnDef ann = rabitqAnn(4), int mapMode = -1) {
    std::mt19937 rng(seed);
    auto vector = [&](Row& row) {
      std::string json;
      for (int n = (int)(rng() % 4); n; n--) {
        float value = (float)(rng() % 17);
        row.values.push_back(value);
        json += (json.empty() ? "" : ",") + std::string("[") + std::to_string(value) + ",0,0,0]";
      }
      return json.empty() ? std::string() : ",\"v\":[" + json + ']';
    };
    for (int root = 0; root < 30; root++) {
      if (root % 10 == 0) schema(mapMode < 0 ? root % 20 == 0 : mapMode == 1, ann);
      roots.push_back({"r" + std::to_string(root), root, -1, (int)(rng() % 4), {}});
      std::string json = R"({"id":")" + roots.back().label + R"(","label_s":")" + roots.back().label
          + R"(","key_s":")" + std::to_string(root % 4) + R"(","eligible_i":)" + std::to_string(roots.back().eligible) + R"(,"items":[)";
      int count = root == 1 ? 30 : (int)(rng() % 7);
      for (int i = 0; i < count; i++) {
        int item = (int)items.size();
        Row row{"i" + std::to_string(item), root, item, (int)(rng() % 4), {}};
        json += (i ? "," : "") + std::string(R"({"label_s":")") + row.label + R"(","eligible_i":)" + std::to_string(row.eligible)
            + vector(row) + R"(,"parts":[)";
        items.push_back(row);
        for (int p = 0, n = (int)(rng() % 5); p < n; p++) {
          Row part{"p" + std::to_string(parts.size()), root, item, (int)(rng() % 4), {}};
          json += (p ? "," : "") + std::string(R"({"label_s":")") + part.label + R"(","eligible_i":)" + std::to_string(part.eligible)
              + vector(part) + '}';
          parts.push_back(std::move(part));
        }
        json += "]}";
      }
      add(json + "]}", root % 10 == 9);
    }
  }
};

TEST_F(OwnerKnnTest, exactMatchesDistinctOwnerModelAtEveryView) {
  for (uint32_t seed : {17, 83, 991}) for (int maps : {-1, 0, 1}) {
    SCOPED_TRACE(::testing::Message() << seed << "/maps=" << maps);
    helper.clear(); roots.clear(); items.clear(); parts.clear(); generate(seed, rabitqAnn(4), maps);
    for (int phase = 0; phase < 3; phase++) {
      if (phase == 1) {
        for (int root = 0; root < 30; root += 7) ASSERT_TRUE(helper.deleteById("r" + std::to_string(root), UpdateMessage::COMMIT).success);
      }
      if (phase == 2) {
        CollectionHelper::UpdateBuilder input; input.commit(true, 1);
        ASSERT_TRUE(helper.submit(input).success);
      }
      for (auto view : {"", "items", "items.parts"}) for (bool deep : {false, true}) {
        if (!deep && std::string_view(view) == "items.parts") continue;
        std::string field = !deep ? (std::string_view(view).empty() ? "items.v" : "v")
            : std::string_view(view).empty() ? "items.parts.v" : std::string_view(view) == "items" ? "parts.v" : "v";
        for (int eligibility : {-1, 0, 3}) for (int k : {1, 7, 1000}) {
          SCOPED_TRACE(::testing::Message() << phase << '/' << view << '/' << field << '/' << eligibility << '/' << k);
          auto actual = search(view, field, k, true, eligibility);
          ASSERT_TRUE(actual->ok());
          EXPECT_EQ(model(view, deep, k, eligibility, phase > 0), hits(*actual));
          EXPECT_EQ((int64_t)hits(*actual).size(), actual->getMatchCount());
        }
      }
      auto floor = search("", "items.parts.v", 10, true, -1, R"(,"similarity":2)");
      EXPECT_EQ(model("", true, 10, -1, phase > 0, score(2)), hits(*floor));
    }
  }
}

TEST_F(OwnerKnnTest, approximateChannelsAndMixedReverseMapsUseOwnerUnits) {
  ForceIvfGuard force;
  IvfTestGuard ivf(8);
  for (auto ann : {pqAnn(2, 2, 1), rabitqAnn(4), rabitqAnn(4, 4)}) for (int maps : {-1, 0, 1}) {
    helper.clear(); roots.clear(); items.clear(); parts.clear();
    generate(17, ann, maps); schema(true, ann);
    // Rewriting follows the current codec; the sources retain mixed ownership maps.
    helper.commit({"vec.items.v", "vec.items.parts.v"});
    ASSERT_TRUE(helper.deleteById("r0", UpdateMessage::COMMIT).success);
    auto reader = helper.getIndexWriter()->snapshots.readers.getReader();
    ASSERT_EQ(3u, reader->segments().size());
    EXPECT_EQ(maps != 0, reader->segments()[0].segmentCore().ownership("items")->hasReverseMap());
    EXPECT_EQ(maps == 1, reader->segments()[1].segmentCore().ownership("items")->hasReverseMap());
    for (int eligibility : {-1, 0}) for (auto view : {"", "items"}) {
      auto field = std::string_view(view).empty() ? "items.parts.v" : "parts.v";
      auto expected = hits(*search(view, field, 7, true, eligibility));
      auto ordinary = hits(*search(view, field, 7, false, eligibility));
      EXPECT_EQ(expected.size(), ordinary.size());
      int recalled = 0;
      for (const auto& hit : ordinary) recalled += std::ranges::find(expected, hit) != expected.end();
      EXPECT_GE(recalled, (int)std::ceil(expected.size() * 0.8));
      for (auto extra : {R"(,"ivf":{"min_scan_fraction":1})", R"(,"refine_candidates":100000,"ivf":{"min_scan_fraction":1})"}) {
        KnnQuery::Stats stats;
        SearchOverridesGuard guard(KnnQuery::statsForTests); KnnQuery::statsForTests = &stats;
        auto actual = search(view, field, 7, false, eligibility, extra);
        EXPECT_EQ(expected, hits(*actual));
        EXPECT_GT(stats.codesVisited.load(), 0);
        if (eligibility < 0 && std::string_view(view).empty()) {
          EXPECT_LE(stats.unitRowsVisited.load(), 1);
        }
      }
      auto expectedFloor = hits(*search(view, field, 1000, true, eligibility, R"(,"similarity":2)"));
      EXPECT_EQ(expectedFloor, hits(*search(view, field, 1000, false, eligibility,
          R"(,"similarity":2,"refine_candidates":100000,"ivf":{"min_scan_fraction":1})")));
    }
    // All owners but one: a dense owner domain reaches IVF as runs of families.
    auto dense = [&](bool exact, std::string_view extra) {
      return hits(*request(R"({"limit":-1,"fields":["label_s"],"get_scores":true,"filter":[{"boolean":{
          "required":[{"all":true}],"prohibited":[{"match":{"field":"id","val":"r5"}}]}}],"query":)"
          + query("items.parts.v", 7, exact, extra) + '}'));
    };
    EXPECT_EQ(7u, dense(true, "").size());
    EXPECT_EQ(dense(true, ""), dense(false, R"(,"refine_candidates":100000,"ivf":{"min_scan_fraction":1})"));
  }
}

TEST_F(OwnerKnnTest, giantNearFamilyUsesExistingBoundedWideningAndIntervalWitnesses) {
  ForceIvfGuard force; IvfTestGuard ivf(8);
  for (auto ann : {pqAnn(2, 2, 2), rabitqAnn(2)}) {
    helper.clear(); schema(true, ann);
    std::string json = R"({"id":"hog","label_s":"hog","items":[)";
    for (int n = 0; n < 2048; n++) json += (n ? "," : "") + std::string(R"({"v":[[0,0,0,0]]})");
    add(json + "]}");
    for (int root = 1; root < 16; root++) add(R"({"id":")" + std::to_string(root) + R"(","label_s":")" + std::to_string(root)
        + R"(","items":{"v":[[)" + std::to_string(root) + ",0,0,0]]}}");
    helper.commit({"vec.items.v"});
    auto exact = hits(*search("", "items.v", 5));
    ASSERT_EQ(5u, exact.size());
    KnnQuery::Stats stats;
    SearchOverridesGuard guard(KnnQuery::statsForTests, KnnQuery::maxKnnCandidates);
    KnnQuery::statsForTests = &stats; KnnQuery::maxKnnCandidates = 64;
    // A bounded pool is cut in vectors: at a tight work cap the giant family may crowd it.
    search("", "items.v", 5, false, -1, R"(,"refine_candidates":5,"ivf":{"min_scan_fraction":1})");
    EXPECT_LE(stats.terminalReads.load(), 64);
    if (ann.codec == api::VectorAnnDef::Codec::RABITQ) {
      auto interval = search("", "items.v", 5, false, -1, R"(,"ivf":{"min_scan_fraction":1})");
      EXPECT_EQ(exact, hits(*interval));
    }
    KnnQuery::maxKnnCandidates = 10000;
    auto widened = search("", "items.v", 5, false, -1, R"(,"refine_candidates":5,"ivf":{"min_scan_fraction":1})");
    EXPECT_EQ(exact, hits(*widened));
  }
}

TEST_F(OwnerKnnTest, explicitChildKnnStillCountsChildrenAndAllJoinModesReduceThem) {
  schema(true);
  add(R"({"id":"a","label_s":"a","items":[{"label_s":"a0","v":[[0,0,0,0]]},{"label_s":"a1","v":[[1,0,0,0]]}]})");
  add(R"({"id":"b","label_s":"b","items":{"label_s":"b0","v":[[2,0,0,0]]}})", true);
  EXPECT_EQ(2u, hits(*search("", "items.v", 2)).size());
  EXPECT_EQ((Hits{{"a0", 1}, {"a1", 0.5f}}), hits(*search("items", "v", 2)));
  for (auto [mode, expected] : {std::pair{"max", 1.0f}, {"min", 0.5f}, {"sum", 1.5f}, {"avg", 0.75f}, {"none", 0.0f}}) {
    auto req = request(R"({"limit":10,"fields":["label_s"],"get_scores":true,"query":{"to_parent":{"path":"items","score":")"
        + std::string(mode) + R"(","query":)" + query("v", 2, true) + "}}}");
    EXPECT_EQ((Hits{{"a", expected}}), hits(*req));
  }
}

TEST_F(OwnerKnnTest, groupsAreGroupsOfOwnersAndFusionAndDiversifySeeOwners) {
  ForceIvfGuard force; IvfTestGuard ivf(8);
  schema(true);
  for (int i = 0; i < 20; i++) add(R"({"id":")" + std::to_string(i) + R"(","label_s":")" + std::to_string(i)
      + R"(","key_s":")" + std::to_string(i % 3) + R"(","items":[{"v":[[)" + std::to_string(i) + ",0,0,0]]},{\"v\":[[30,0,0,0]]}]}");
  helper.commit();
  helper.commit({"vec.items.v"});
  auto groups = request(R"({"limit":10,"fields":["label_s"],"get_scores":true,"collapse":"key_s","query":)"
      + query("items.v", 3, true, R"(,"unit":"groups")") + '}');
  EXPECT_EQ(3u, hits(*groups).size());
  EXPECT_EQ((Hits{{"0",1}, {"1",0.5f}, {"2",0.2f}}), hits(*groups));
  auto approximate = request(R"({"limit":10,"fields":["label_s"],"get_scores":true,"collapse":"key_s","query":)"
      + query("items.v", 3, false, R"(,"unit":"groups","refine_candidates":100,"ivf":{"min_scan_fraction":1})") + '}');
  EXPECT_EQ(hits(*groups), hits(*approximate));
  auto fusion = request(R"({"ops":{"q":{"fusion":{"rrf":{},"limit":3,"fields":["label_s"],"sources":{"v":{"limit":3,"query":)"
      + query("items.v", 3, true) + "}}}}}}");
  EXPECT_TRUE(fusion->ok());
  auto diverse = request(R"({"limit":10,"fields":["label_s"],"get_scores":true,"diversify":{"field":"key_s","max_per_key":1},"query":)"
      + query("items.v", 10, true) + '}');
  EXPECT_EQ((Hits{{"0",1}, {"1",0.5f}, {"2",0.2f}}), hits(*diverse));
}

TEST_F(OwnerKnnTest, resultViewIsIdentityAndNamedKnnKeepsItsPreciseError) {
  schema(true);
  add(R"({"id":"a","label_s":"a","items":{"v":[[0,0,0,0]],"parts":{"v":[[0,0,0,0]]}}})", true);
  auto req = localReq(helper.getSearchEngine());
  req->schema = helper.collection().getSchema();
  req->reader = helper.getIndexWriter()->snapshots.readers.getReader();
  ParseView view(*req->schema, nullptr, &req->reader, req.get());
  ParseContext context(req->requestPool, *req->schema, req->arena, CoerceContext{}, "q", nullptr, &view);
  auto parse = [&](std::string_view field) {
    auto wire = qb::knn(req->mr, field, {0,0,0,0}, 2, 0, true);
    return ProtobufQueryParser(context).parse(wire);
  };
  auto* root = parse("items.parts.v");
  auto scope = view.scoped("items");
  auto* child = parse("parts.v");
  EXPECT_EQ(QueryKind::KNN, root->getKind()); EXPECT_EQ(QueryKind::KNN, child->getKind());
  EXPECT_FALSE(root->equals(*child)); EXPECT_NE(root->hash(), child->hash());
  auto key = [](Query& query) {
    FilterKeyBuilder out; FilterKeyContext context;
    auto scope = query.appendFilterKey(out, context);
    return std::move(out).finishStructural(scope, context);
  };
  EXPECT_NE(key(*root), key(*child));
  auto bad = localReq(helper.getSearchEngine());
  ASSERT_TRUE(api::read_json(bad->rawRequest(), R"({"query":{"name":"v","knn":{"field":"items.v","query":[0,0,0,0],"k":2}},"row_ops":{"x":{"domain":{"from_match":"v"},"top_docs":{"limit":2}}}})", bad->mr));
  bad->execute(1); EXPECT_FALSE(bad->ok());
  EXPECT_NE(std::string::npos, bad->errorMsg().find("kNN and other domain-consuming clauses cannot be replayed per row"));
}
TEST_F(OwnerKnnTest, singleValuedAndSparseChildColumnsAndMissingPathsUseTheSameUnit) {
  schema(true);
  SchemaBuilder fields;
  auto& vector = fields.field("items.one");
  vector.type = api::FieldDef::FieldClass::VECTOR;
  vector.column = true; vector.dims = 4; vector.metric = api::VectorMetric::L2;
  fields.set(helper.collection());
  add(R"({"id":"old","label_s":"old"})", true);
  add(R"({"id":"a","label_s":"a","items":[{"one":[0,0,0,0]},{"one":[1,0,0,0]},{}]})");
  add(R"({"id":"b","label_s":"b","items":{"one":[2,0,0,0]}})", true);
  EXPECT_EQ((Hits{{"a",1},{"b",0.2f}}), hits(*search("", "items.one", 10)));
  EXPECT_TRUE(hits(*search("", "items.parts.v", 10)).empty());
}

TEST_F(OwnerKnnTest, ownerEligibilityIsPreparedBeforeSelectionAndRoutedVariantsRemainOwnerRows) {
  auto cached = std::make_shared<FilterCache>(FilterCacheConfig{
      .maxBytes = 1024 * 1024, .minSegmentDocs = 0, .admissionThreshold = 1});
  helper.getIndexWriter()->snapshots.readers.filterCache = cached;
  schema(true);
  add(R"({"id":"a","label_s":"a","items":[{"v":[[0,0,0,0]]},{"v":[[1,0,0,0]]}]})");
  add(R"({"id":"b","label_s":"b","items":{"v":[[2,0,0,0]]}})", true);
  auto req = request(R"({"limit":10,"fields":["label_s"],"get_scores":true,"query":{"boolean":{"required":[)"
      + query("items.v", 1, true) + R"(],"prohibited":[{"match":{"field":"id","val":"a"}}]}}})");
  EXPECT_EQ((Hits{{"b",0.2f}}), hits(*req));
  auto routed = request(R"({"ops":{"q":{"top_docs":{"limit":10,"fields":["label_s"],"get_scores":true,
      "filter":[{"query":"id:b","except_ops":["all"]}],"query":)" + query("items.v", 1, true)
      + R"(,"ops":{"all":{"field_facet":{"field":"id"}}}}}}})");
  EXPECT_EQ((Hits{{"b",0.2f}}), hits(*routed));
  glz::generic_i64 output;
  ASSERT_FALSE(glz::read_json(output, renderSearchResponseBody(routed->responses.back()->proto)));
  auto& buckets = output["ops"]["q"]["ops"]["all"]["buckets"].get_array();
  ASSERT_EQ(1u, buckets.size());
  EXPECT_EQ("a", buckets[0]["val"].get_string());
  for (int n = 0; n < 3; n++) {
    auto all = search("", "items.v", 1);
    EXPECT_EQ((Hits{{"a",1}}), hits(*all));
    KnnQuery::Stats work;
    SearchOverridesGuard counter(KnnQuery::statsForTests); KnnQuery::statsForTests = &work;
    auto filtered = request(R"({"limit":10,"get_scores":true,"fields":["label_s"],"filter":["id:b"],"query":)"
        + query("items.v", 1, true) + '}');
    EXPECT_EQ((Hits{{"b",0.2f}}), hits(*filtered));
    EXPECT_LE(work.unitRowsVisited.load(), 1);
  }
  for (int n = 0; n < 3; n++) {
    auto membership = request(R"({"limit":0,"count_limit":-1,"query":{"all":true},"filter":[)"
        + query("items.v", 1, true) + "]}");
    EXPECT_EQ(1, membership->getMatchCount());
  }
  EXPECT_GT(cached->counters().readerStableHits, 0);
}

}
}
