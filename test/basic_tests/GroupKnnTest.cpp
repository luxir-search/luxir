// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include "test/LuxirTest.h"
#include "test/CollectionHelper.h"
#include "test/LocalReq.h"
#include "test/TestUtils.h"
#include "test/SchemaBuilder.h"
#include "luxir/query/KnnQuery.h"
#include "luxir/index/VectorIndexBuilder.h"
#include "luxir/search/SearchOverrides.h"

#include <random>
#include <set>

using namespace luxir;
using namespace luxir::test;

namespace {
using Part = std::variant<std::monostate, std::string, int64_t>;
using Key = std::vector<Part>;
using Row = std::pair<std::string, float>;

std::vector<Row> rows(const LocalReq& req) {
  std::vector<Row> result;
  for (auto* response : req.responses) {
    auto* value = response->proto.ops.find("q");
    if (value == nullptr) continue;
    for (const auto& doc : (*value)->docList()->docs) {
      result.emplace_back(std::string(doc.fields.at("id")->asString()),
          std::get<float>(doc.fields.at("_score_")->kind));
    }
  }
  return result;
}

std::vector<Row> oracle(const LocalReq& full, std::span<const std::string> fields,
                        std::string_view missing, size_t k) {
  std::set<Key> seen;
  std::vector<Row> result;
  for (auto* response : full.responses) for (const auto& doc : response->proto.ops.at("q")->docList()->docs) {
    Key key;
    bool absent = false;
    for (const auto& field : fields) {
      auto* part = doc.fields.find(field);
      if (part == nullptr || (*part)->isNull()) { absent = true; key.emplace_back(); }
      else if (auto* text = std::get_if<std::string_view>(&(*part)->kind)) key.emplace_back(std::string(*text));
      else key.emplace_back((*part)->asInt());
    }
    if (absent && missing == "drop") continue;
    if (absent && missing == "expand") key = {std::string("singleton"), std::string(doc.fields.at("id")->asString())};
    if (!seen.insert(key).second) continue;
    result.emplace_back(std::string(doc.fields.at("id")->asString()),
        std::get<float>(doc.fields.at("_score_")->kind));
    if (result.size() == k) return result;
  }
  return result;
}

std::string keySpec(std::span<const std::string> fields, std::string_view policy) {
  std::string s = R"({"field":[)";
  for (const auto& field : fields) { if (s.back() != '[') s += ','; s += '"' + field + '"'; }
  return s + R"(],"missing":")" + std::string(policy) + R"("})";
}

std::string vectorJson(std::span<const float> vector) {
  std::string json = "[";
  for (float x : vector) {
    if (json.size() > 1) json += ',';
    json += std::to_string(x);
  }
  return json + ']';
}

void schema(CollectionHelper& helper, api::VectorMetric metric = api::VectorMetric::L2,
            std::optional<api::VectorAnnDef> ann = std::nullopt, int32_t dims = 4) {
  SchemaBuilder b;
  auto& v = b.templ("_vs");
  v.type = api::FieldDef::FieldClass::VECTOR;
  v.column = true;
  v.multi = true;
  v.dims = dims;
  v.metric = metric;
  v.ann = ann;
  b.set(helper.collection());
}
}

class GroupKnnTest : public LuxirTest {
protected:
  auto run(std::string_view collection, std::string_view body, int32_t parallel = 1, bool ok = true) {
    auto request = localReq(luxirNode->getSearchEngine());
    std::string json = R"({"collection":")" + std::string(collection)
        + R"(","get_scores":true,"get_number":true,"document_format":"rows","batch_size":10000,)"
        + std::string(body) + '}';
    if (!api::read_json(request->rawRequest(), json, request->mr)) throw std::runtime_error(json);
    request->execute(parallel);
    EXPECT_EQ(ok, request->ok()) << request->errorMsg() << json;
    return request;
  }
  static std::string knn(int k, bool groups, bool exact = true, std::string_view extra = {},
                          std::string_view vector = "[0,0,0,0]") {
    return R"({"knn":{"field":"vec_vs","query":)" + std::string(vector) + R"(,"k":)" + std::to_string(k)
        + (groups ? R"(,"unit":"groups")" : "")
        + (exact ? R"(,"exact":true)" : "") + std::string(extra) + "}}";
  }
};

TEST_F(GroupKnnTest, exactMatchesExhaustiveDocumentOracle) {
  std::mt19937 rng(418);
  std::vector<Doc> docs;
  for (int64_t i = 0; i < 168; i++) {
    float x = (float)(rng() % 13), y = (float)(rng() % 11);
    auto doc = flatdoc("id", std::to_string(i), "eligible_s", i % 7 ? "yes" : "no");
    if (i % 17) doc.emplace_back("vec_vs", std::vector<std::vector<float>>{
        {x, y, 0, 0}, {y * 0.5f, x * 0.25f, 0, 0}});
    if (i % 5) doc.emplace_back("key_s", "g" + std::to_string(rng() % 13));
    if (i % 3) doc.emplace_back("key_i", i % 2 ? INT64_MIN + (int64_t)(rng() % 11) : INT64_MAX - (int64_t)(rng() % 11));
    if (i % 4) doc.emplace_back("when_dt", (int64_t)(rng() % 5) * 1000);
    docs.push_back(std::move(doc));
  }
  for (int segments : {1, 3, 7}) {
    std::string name = "knn_groups_" + std::to_string(segments);
    CollectionHelper helper(name);
    schema(helper);
    for (int segment = 0; segment < segments; segment++) {
      ASSERT_TRUE(helper.indexAll(std::span<const Doc>(docs).subspan(
          (size_t)(segment * 168 / segments), (size_t)(168 / segments)), UpdateMessage::COMMIT).success);
    }
    ASSERT_TRUE(helper.deleteByIds({"13", "100"}, UpdateMessage::COMMIT).success);
    for (bool filtered : {false, true}) {
      std::string filter = filtered ? R"(,"filter":["eligible_s:yes"] )" : "";
      auto full = run(name, R"("limit":-1,"query":)" + knn(1000, false) + filter);
      for (const auto& fields : std::vector<std::vector<std::string>>{
          {"key_s"}, {"key_i"}, {"when_dt"}, {"key_s", "key_i"}, {"key_i", "when_dt"}}) {
        for (auto policy : {"expand", "group", "drop"}) for (int k : {1, 9, 99, 1000}) {
          auto expected = oracle(*full, fields, policy, k);
          for (int parallel : {1, -1}) {
            auto actual = run(name, R"("limit":-1,"query":)" + knn(k, true)
                + R"(,"collapse":)" + keySpec(fields, policy) + filter, parallel);
            EXPECT_EQ(expected, rows(*actual)) << name << policy << k;
            EXPECT_EQ((int64_t)expected.size(), actual->docList()->found.value_or(-1));
          }
        }
      }
    }
  }
}

TEST_F(GroupKnnTest, adversarialGroupsPagingWrappersAndWitnessDomains) {
  CollectionHelper helper;
  schema(helper);
  for (int segment = 0; segment < 3; segment++) {
    std::vector<Doc> docs;
    for (int i = 0; i < 80; i++) docs.push_back(flatdoc("id", std::to_string(segment * 80 + i),
        "key_s", i < 70 ? "near" : "g" + std::to_string(i),
        "vec_vs", std::vector<std::vector<float>>{{i < 70 ? 0.0f : (float)i, 0, 0, 0}}));
    helper.indexAll(docs, UpdateMessage::COMMIT);
  }
  auto docs = run("main", R"("limit":-1,"collapse":"key_s","query":)" + knn(7, false));
  EXPECT_EQ(1u, rows(*docs).size());
  std::string group = knn(7, true);
  for (std::string query : {group, R"({"boolean":{"required":[)" + group + "]}}",
      R"({"rescore":{"query":)" + group + R"(,"expr":"score"}})"}) {
    auto actual = run("main", R"("limit":-1,"collapse":"key_s","query":)" + query
        + R"(,"ops":{"w":{"query_facet":{"count_unit":"docs","buckets":{"all":"*:*"}}}})", -1);
    EXPECT_EQ(7u, rows(*actual).size());
    EXPECT_EQ(7, actual->docList()->ops.at("w")->facetResult()->counts[0]);
    auto page = run("main", R"("limit":100,"offset":5,"collapse":"key_s","query":)" + query);
    auto expected = rows(*actual);
    ASSERT_GE(expected.size(), 5u);
    expected.erase(expected.begin(), expected.begin() + 5);
    EXPECT_EQ(expected, rows(*page)); // k is never enlarged by the page.
  }
}

TEST_F(GroupKnnTest, reportsExhaustionAndCapsAndRejectsMissingScope) {
  CollectionHelper helper;
  schema(helper);
  for (int i = 0; i < 8; i++) helper.index(flatdoc("id", std::to_string(i),
      "key_s", "g" + std::to_string(i), "vec_vs", std::vector<std::vector<float>>{{(float)i, 0, 0, 0}}));
  helper.commit();
  auto empty = run("main", R"("filter":["id:absent"],"collapse":"key_s","query":)" + knn(5, true));
  EXPECT_TRUE(rows(*empty).empty());
  ASSERT_EQ(1u, empty->responses.back()->proto.warnings.size());
  EXPECT_NE(std::string_view::npos, empty->responses.back()->proto.warnings[0].message.find("candidates_exhausted=true"));
  auto bad = run("main", R"("query":)" + knn(3, true), 1, false);
  EXPECT_NE(std::string::npos, bad->errorMsg().find("requires an enclosing collapse"));
  auto invalid = knn(3, false);
  invalid.insert(invalid.size() - 2, R"(,"unit":"bogus")");
  auto invalidUnit = run("main", R"("collapse":"key_s","query":)" + invalid, 1, false);
  EXPECT_NE(std::string::npos, invalidUnit->errorMsg().find("knn.unit must be docs or groups"));
  auto explicitDocs = knn(3, false);
  explicitDocs.insert(explicitDocs.size() - 2, R"(,"unit":"docs")");
  auto ordinary = run("main", R"("limit":-1,"query":)" + explicitDocs);
  EXPECT_EQ(3u, rows(*ordinary).size());
  auto exhausted = run("main", R"("limit":-1,"collapse":"key_s","query":)" + knn(20, true));
  ASSERT_EQ(1u, exhausted->responses.back()->proto.warnings.size());
  EXPECT_EQ("knn_group_underfill", exhausted->responses.back()->proto.warnings[0].code);
  EXPECT_NE(std::string_view::npos, exhausted->responses.back()->proto.warnings[0].message.find("candidates_exhausted=true"));
  SearchOverridesGuard guard(KnnQuery::maxKnnCandidates);
  KnnQuery::maxKnnCandidates = 2;
  auto capped = run("main", R"("limit":-1,"collapse":"key_s","query":)" + knn(5, true, false));
  EXPECT_EQ(2u, rows(*capped).size());
  ASSERT_EQ(1u, capped->responses.back()->proto.warnings.size());
  EXPECT_NE(std::string_view::npos, capped->responses.back()->proto.warnings[0].message.find("candidates_exhausted=false"));
  auto exact = run("main", R"("limit":-1,"collapse":"key_s","query":)" + knn(5, true));
  EXPECT_EQ(5u, rows(*exact).size()); // exact overrides the heuristic cap.
}

TEST_F(GroupKnnTest, actualIvfAndFlatSegmentsStillFindDistinctGroups) {
  CollectionHelper helper;
  for (const auto& ann : {pqAnn(4, 2, 2), rabitqAnn(4)}) {
    helper.clear();
    schema(helper, api::VectorMetric::L2, ann);
    for (int segment = 0; segment < 2; segment++) {
      std::vector<Doc> docs;
      for (int i = 0; i < 160; i++) {
        auto doc = flatdoc("id", std::to_string(segment * 160 + i),
            "vec_vs", std::vector<std::vector<float>>{{i < 128 ? 0.0f : (float)i, 0, 0, 0}});
        if (i % 7) doc.emplace_back("key_s", i < 128 ? "near" : "g" + std::to_string(i));
        if (i % 11) doc.emplace_back("key_i", (int64_t)(i < 128 ? 0 : i));
        docs.push_back(std::move(doc));
      }
      helper.indexAll(docs, UpdateMessage::COMMIT);
    }
    IvfTestGuard ivf(/*minTraining=*/16);
    helper.commit({"vec.vec_vs"});
    // A later segment has no aux, and the same near group straddles all three.
    helper.indexAll({flatdoc("id", "flat", "key_s", "flat", "key_i", (int64_t)1000,
        "vec_vs", std::vector<std::vector<float>>{{1, 0, 0, 0}})}, UpdateMessage::COMMIT);
    helper.deleteByIds({"0", "160"}, UpdateMessage::COMMIT);
    auto full = run("main", R"("limit":-1,"query":)" + knn(1000, false));
    for (auto fields : std::vector<std::vector<std::string>>{{"key_s"}, {"key_i"}, {"key_s", "key_i"}}) {
    for (auto policy : {"expand", "group", "drop"}) for (int parallel : {1, -1}) {
      KnnQuery::Stats stats;
      SearchOverridesGuard guard(KnnQuery::statsForTests);
      KnnQuery::statsForTests = &stats;
      auto actual = run("main", R"("limit":-1,"collapse":)" + keySpec(fields, policy)
          + R"(,"query":)" + knn(8, true, false, R"(,"refine_candidates":1000,"ivf":{"min_scan_fraction":1})"), parallel);
      EXPECT_EQ(oracle(*full, fields, policy, 8), rows(*actual));
      EXPECT_EQ(8u, rows(*actual).size());
      EXPECT_GT(stats.lists.load(), 0);
      EXPECT_GT(stats.bytes.load(), 0);
    }
    }
    auto pinned = run("main", R"("limit":-1,"collapse":"key_s","filter":["key_s:near"],"query":)"
        + knn(8, true, false, R"(,"ivf":{"nprobe":1})"));
    ASSERT_LT(rows(*pinned).size(), 8u);
    ASSERT_EQ(1u, pinned->responses.back()->proto.warnings.size());
    EXPECT_NE(std::string_view::npos, pinned->responses.back()->proto.warnings[0].message.find("candidates_exhausted=false"));

  }
}

TEST_F(GroupKnnTest, rabitqPreparedQueryFindsGroupsAndRepresentatives) {
  IvfTestGuard ivf;
  CollectionHelper helper;
  constexpr int dims = 16;
  schema(helper, api::VectorMetric::IP, rabitqAnn(4, 4), dims);
  std::mt19937 rng(418);
  std::vector<float> query(dims);
  for (auto& x : query) x = (float)(rng() % 2001) / 1000.0f - 1.0f;
  std::string vector = vectorJson(query);
  std::vector<std::vector<float>> centers(40, std::vector<float>(dims));
  for (auto& center : centers) for (auto& x : center) x = (float)(rng() % 2001) / 100.0f - 10.0f;
  for (int segment = 0; segment < 2; segment++) {
    for (int group = 0; group < 40; group++) for (int member = 0; member < 4; member++) {
      auto value = centers[(size_t)group];
      for (int d = 0; d < dims; d++) value[(size_t)d] += (float)(member + segment * 4) * query[(size_t)d];
      helper.index(flatdoc("id", std::to_string(segment * 160 + group * 4 + member),
          "key_s", "g" + std::to_string(group), "eligible_s", group % 3 ? "yes" : "no",
          "vec_vs", std::vector<std::vector<float>>{value}));
    }
    helper.commit({"vec.vec_vs"});
  }
  SearchOverridesGuard grains(KnnQuery::scanTaskGrainVectors, KnnQuery::rescoreTaskGrainCandidates);
  KnnQuery::scanTaskGrainVectors = KnnQuery::rescoreTaskGrainCandidates = 1;
  for (bool filtered : {false, true}) {
    std::string prefix = R"("limit":-1,"collapse":"key_s",)"
        + std::string(filtered ? R"("filter":["eligible_s:yes"],)" : "") + R"("query":)";
    auto expected = rows(*run("main", prefix + knn(6, true, true, {}, vector)));
    ASSERT_EQ(6u, expected.size());
    for (int parallel : {1, -1}) {
      KnnQuery::Stats stats;
      SearchOverridesGuard saved(KnnQuery::statsForTests);
      KnnQuery::statsForTests = &stats;
      auto actual = run("main", prefix + knn(6, true, false,
          R"(,"refine_candidates":24,"ivf":{"min_scan_fraction":1})", vector), parallel);
      EXPECT_EQ(expected, rows(*actual));
      EXPECT_GT(stats.lists.load(), 0);
      EXPECT_GT(stats.bytes.load(), 0);
    }
  }
}

TEST_F(GroupKnnTest, rabitqQueryBitsChangeBoundedGroupedSelection) {
  IvfTestGuard ivf;
  CollectionHelper helper;
  constexpr int dims = 16;
  schema(helper, api::VectorMetric::IP, rabitqAnn(4), dims);
  std::mt19937 rng(831);
  std::vector<float> query(dims);
  for (auto& x : query) x = (float)(rng() % 2001) / 1000.0f - 1.0f;
  std::string vector = vectorJson(query);
  for (int i = 0; i < 320; i++) {
    std::vector<float> value(dims);
    for (auto& x : value) x = (float)(rng() % 2001) / 1000.0f - 1.0f;
    helper.index(flatdoc("id", std::to_string(i), "key_s", std::to_string(i),
        "vec_vs", std::vector<std::vector<float>>{value}));
  }
  helper.commit({"vec.vec_vs"});
  auto reader = helper.getIndexWriter()->snapshots.readers.getReader();
  auto aux = reader->segments()[0].getAuxReader("vec.vec_vs");
  ASSERT_NE(nullptr, aux);
  std::vector<std::vector<Row>> selected;
  for (int bits : {0, 1}) {
    schema(helper, api::VectorMetric::IP, rabitqAnn(4, 1, bits), dims);
    auto current = helper.getIndexWriter()->snapshots.readers.getReader();
    EXPECT_EQ(aux.get(), current->segments()[0].getAuxReader("vec.vec_vs").get());
    auto extra = R"(,"refine_candidates":3,"ivf":{"min_scan_fraction":1})";
    auto docs = rows(*run("main", R"("limit":-1,"query":)" + knn(3, false, false, extra, vector)));
    ASSERT_EQ(3u, docs.size());
    std::vector<Row> groups;
    for (int parallel : {1, -1}) {
      groups = rows(*run("main", R"("limit":-1,"collapse":"key_s","query":)"
          + knn(3, true, false, extra, vector), parallel));
      EXPECT_EQ(docs, groups) << bits;
    }
    selected.push_back(std::move(groups));
  }
  EXPECT_NE(selected[0], selected[1]);
}

TEST_F(GroupKnnTest, identityIncludesResolvedKeyPolicyAndUnit) {
  CollectionHelper helper;
  schema(helper);
  helper.indexAll({flatdoc("id", "0", "key_s", "a", "other_s", "a", "vec_vs",
      std::vector<std::vector<float>>{{0, 0, 0, 0}})}, UpdateMessage::COMMIT);
  auto reader = helper.getIndexWriter()->snapshots.readers.getReader();
  const auto& type = (const VectorFieldType&)*reader->schema()->getFieldTypePtr("vec_vs");
  using Missing = api::Collapse_::MissingPolicy;
  CollapsePlan first(*reader, Missing::GROUP, false), equal(*reader, Missing::GROUP, false),
      policy(*reader, Missing::EXPAND, false), field(*reader, Missing::GROUP, false);
  first.fields.push_back({"key_s", api::GroupComponent_::Type::STRING, reader->getOrdMap("key_s"),
      reader->field("key_s")});
  equal.fields = policy.fields = first.fields;
  field.fields.push_back({"other_s", api::GroupComponent_::Type::STRING, reader->getOrdMap("other_s"),
      reader->field("other_s")});
  std::array<float, 4> vector{0, 0, 0, 0};
  KnnQuery a("vec_vs", type, vector, 2, 0, 0, 0, false, &first),
      b("vec_vs", type, vector, 2, 0, 0, 0, false, &equal),
      c("vec_vs", type, vector, 2, 0, 0, 0, false, &policy),
      d("vec_vs", type, vector, 2, 0, 0, 0, false, &field),
      docs("vec_vs", type, vector, 2);
  EXPECT_TRUE(a.equals(b));
  EXPECT_EQ(a.hash(), b.hash());
  for (const auto* other : {&c, &d, &docs}) {
    EXPECT_FALSE(a.equals(*other));
    EXPECT_NE(a.hash(), other->hash());
  }
  auto key = [](const KnnQuery& q) {
    FilterKeyBuilder builder;
    FilterKeyContext context;
    auto scope = q.appendFilterKey(builder, context);
    return std::move(builder).finishStructural(scope, context);
  };
  EXPECT_EQ(key(a), key(b));
  EXPECT_NE(key(a), key(c));
  EXPECT_NE(key(a), key(d));
  EXPECT_NE(key(a), key(docs));
}

TEST_F(GroupKnnTest, annAdmissionPrecedesTheVectorCutForIpAndCosine) {
  IvfTestGuard ivf(/*minTraining=*/16);
  for (auto metric : {api::VectorMetric::IP, api::VectorMetric::COSINE}) {
    std::string name = "group_metric_" + std::to_string((int)metric);
    CollectionHelper helper(name);
    schema(helper, metric, pqAnn(4, 2, 2));
    for (int group = 20; group > 0; group--) for (int member = 0; member < 8; member++) {
      helper.index(flatdoc("id", std::to_string(group * 8 + member), "key_s", "g" + std::to_string(group),
          "vec_vs", std::vector<std::vector<float>>{{(float)group, 1, 0, 0}, {(float)group * 0.75f, 0.5f, 0, 0}}));
    }
    helper.commit({"vec.vec_vs"});
    auto full = run(name, R"("limit":-1,"query":)" + knn(1000, false, true, {}, "[1,0,0,0]"));
    auto known = rows(*full);
    for (int parallel : {1, -1}) {
      KnnQuery::Stats stats;
      SearchOverridesGuard statsGuard(KnnQuery::statsForTests);
      KnnQuery::statsForTests = &stats;
      auto actual = run(name, R"("limit":-1,"collapse":"key_s","query":)"
          + knn(3, true, false, R"(,"refine_candidates":3,"ivf":{"min_scan_fraction":1})", "[1,0,0,0]"), parallel);
      EXPECT_GT(stats.lists.load(), 0);
      ASSERT_EQ(3u, rows(*actual).size());
      for (const auto& row : rows(*actual)) EXPECT_NE(known.end(), std::find(known.begin(), known.end(), row));
    }
  }
}

TEST_F(GroupKnnTest, cappedAdmissionMatchesGreedyAcrossPartitions) {
  CollectionHelper helper;
  helper.indexAll({flatdoc("id", "0", "key_s", "key")}, UpdateMessage::COMMIT);
  auto reader = helper.getIndexWriter()->snapshots.readers.getReader();
  CollapsePlan plan(*reader, api::Collapse_::MissingPolicy::GROUP, false);
  struct Entry { int score, doc, key; size_t slot = 0; };
  struct Better {
    bool operator()(const Entry& a, const Entry& b) const {
      return a.score != b.score ? a.score > b.score : a.doc < b.doc;
    }
  };
  std::mt19937 rng(903);
  for (int cap = 1; cap <= 4; cap++) for (int n : {0, 1, 7, 23, 150}) {
    std::vector<Entry> all;
    for (int i = 0; i < 150; i++) all.push_back({(int)(rng() % 13), i, (int)(rng() % 17)});
    auto select = [&](std::vector<Entry> input) {
      GroupAdmission<false> admission(plan, (size_t)cap);
      ExpandingPQ<Entry, Better, true> heap((size_t)n);
      for (const auto& row : input) {
        CollapseKey key; key.components.push_back({true, row.key});
        admission.admit(key, heap, row, [&](const Entry& old) { return Better{}(row, old); }, [](size_t, bool) {});
      }
      std::vector<Entry> result(heap.span().begin(), heap.span().end());
      std::sort(result.begin(), result.end(), Better{});
      return result;
    };
    auto expected = all;
    std::sort(expected.begin(), expected.end(), Better{});
    std::vector<int> counts(17);
    std::erase_if(expected, [&](const Entry& row) { return counts[(size_t)row.key]++ >= cap; });
    expected.resize(std::min(expected.size(), (size_t)n));
    auto verify = [&](const std::vector<Entry>& actual) {
      ASSERT_EQ(expected.size(), actual.size());
      for (size_t i = 0; i < expected.size(); i++) EXPECT_EQ(expected[i].doc, actual[i].doc) << cap << '/' << n;
    };
    std::shuffle(all.begin(), all.end(), rng);
    verify(select(all));
    std::vector<std::vector<Entry>> parts(7);
    for (auto row : all) parts[rng() % parts.size()].push_back(row);
    for (auto& part : parts) part = select(std::move(part));
    std::shuffle(parts.begin(), parts.end(), rng);
    std::vector<Entry> merged;
    for (const auto& part : parts) {
      merged.insert(merged.end(), part.begin(), part.end());
      merged = select(std::move(merged));
    }
    verify(merged);
  }
}

TEST_F(GroupKnnTest, approximateMemberCapRecoversExactRepresentative) {
  CollectionHelper helper;
  schema(helper, api::VectorMetric::L2, pqAnn(2, 0, 1));
  for (int group = 0; group < 48; group++) for (int member = 0; member < 2; member++) {
    helper.index(flatdoc("id", std::to_string(group * 2 + member), "key_s", "g" + std::to_string(group),
        "vec_vs", std::vector<std::vector<float>>{{(float)group + (float)member * 0.1f, 0, 0, 0}}));
  }
  IvfTestGuard ivf(/*minTraining=*/8);
  helper.commit({"vec.vec_vs"});
  auto truth = rows(*run("main", R"("limit":-1,"collapse":"key_s","query":)" + knn(3, true)));
  auto reader = helper.getIndexWriter()->snapshots.readers.getReader();
  CollapsePlan plan(*reader, api::Collapse_::MissingPolicy::GROUP, false);
  plan.fields.push_back({"key_s", api::GroupComponent_::Type::STRING, reader->getOrdMap("key_s"),
      reader->field("key_s")});
  // Like QuantizingEngine in KnnQueryTest: transform a real exhaustive result,
  // retaining its document identities but deliberately reversing sibling scores.
  auto exhaustive = rows(*run("main", R"("limit":-1,"query":)" + knn(1000, false)));
  std::vector<VectorEngineHit> vectors;
  for (const auto& [id, score] : exhaustive) vectors.push_back({score, 0, std::stoll(id)});
  class ReversedSiblings final : public VectorEngine {
    const CollapsePlan& plan;
    std::vector<VectorEngineHit> vectors;
  public:
    ReversedSiblings(const CollapsePlan& plan, std::vector<VectorEngineHit> vectors) : plan(plan), vectors(std::move(vectors)) {}
    VectorSearchResult search(const VectorSearchRequest& request) override {
      struct Entry : VectorEngineHit { size_t slot = 0; };
      struct Better {
        bool operator()(const Entry& a, const Entry& b) const {
          return a.score != b.score ? a.score > b.score : a.valueRank < b.valueRank;
        }
      };
      ExpandingPQ<Entry, Better, true> heap((size_t)request.candidates);
      GroupAdmission<false> admission(plan, (size_t)request.perKey);
      admission.reader.setSegment(0);
      for (const auto& vector : vectors) {
        Entry row; static_cast<VectorEngineHit&>(row) = vector;
        row.score = 1.0f / (1.0f + (float)(row.valueRank / 2)) + (float)(row.valueRank % 2) * 0.001f;
        if (heap.size() == heap.capacity() && !Better{}(row, heap.top())) continue;
        const auto* key = admission.reader.read(0, (int32_t)row.valueRank);
        admission.admit(*key, heap, row, [&](const Entry& old) { return Better{}(row, old); }, [](size_t, bool) {});
      }
      VectorSearchResult result;
      for (const auto& row : heap.span()) result.hits.push_back(row);
      result.scoresAreExact = false;
      result.poolExhausted = true;
      return result;
    }
  };
  SearchOverridesGuard wrapper(KnnQuery::engineWrapperForTests);
  KnnQuery::engineWrapperForTests = [&](VectorEngine&, int64_t) { return std::make_unique<ReversedSiblings>(plan, vectors); };
  SearchOverridesGuard memberCap(KnnQuery::membersPerGroupForTests);
  for (int parallel : {1, -1}) {
    KnnQuery::membersPerGroupForTests = 1;
    auto lost = rows(*run("main", R"("limit":-1,"collapse":"key_s","query":)"
        + knn(3, true, false, R"(,"refine_candidates":12)"), parallel));
    EXPECT_NE(truth, lost);
    KnnQuery::membersPerGroupForTests = 0;
    auto recovered = rows(*run("main", R"("limit":-1,"collapse":"key_s","query":)"
        + knn(3, true, false, R"(,"refine_candidates":12)"), parallel));
    EXPECT_EQ(truth, recovered);
  }
}

TEST_F(GroupKnnTest, enrichmentReplaysOnlyAdmittedMembers) {
  CollectionHelper helper;
  schema(helper);
  ASSERT_TRUE(helper.indexAll({
      flatdoc("id", "a", "key_s", "x", "price_i", (int64_t)4,
          "vec_vs", std::vector<std::vector<float>>{{0.0f, 0, 0, 0}}),
      flatdoc("id", "b", "key_s", "x", "price_i", (int64_t)2,
          "vec_vs", std::vector<std::vector<float>>{{1.0f, 0, 0, 0}}),
      flatdoc("id", "c", "key_s", "x", "price_i", (int64_t)0,
          "vec_vs", std::vector<std::vector<float>>{{50.0f, 0, 0, 0}}),
      flatdoc("id", "d", "key_s", "y", "price_i", (int64_t)1, "vec_vs", std::vector<std::vector<float>>{{2.0f, 0, 0, 0}})},
      UpdateMessage::COMMIT).success);
  for (bool groups : {false, true}) for (int parallel : {1, -1}) {
    auto actual = run("main", R"("limit":-1,"fields":["id"],"query":)" + knn(3, groups)
        + R"(,"collapse":{"field":"key_s","get_number":true,"head_sort":"price_i asc","ops":{"members":{"top_docs":{"sort":"price_i asc","limit":-1,"fields":["id"],"document_format":"rows"}}}})", parallel);
    ASSERT_TRUE(actual->ok()) << actual->errorMsg();
    auto result = rows(*actual);
    ASSERT_EQ(2u, result.size());
    EXPECT_EQ(groups ? "a" : "b", result[0].first);
    const auto& table = *actual->docList()->groups;
    EXPECT_EQ(groups ? 1 : 2, table.counts[0]);
    auto& members = *std::get<api::ArrVal>(table.ops.at("members")->kind).v[0].docList();
    ASSERT_EQ(groups ? 1 : 2, members.row_count);
    EXPECT_EQ(groups ? "a" : "b", members.docs[0].fields.at("id")->asString());
    for (auto& doc : members.docs) EXPECT_NE("c", doc.fields.at("id")->asString());
    if (!groups) {
      ASSERT_EQ(1u, table.ranks[0].v.size());
      EXPECT_GT(std::get<float>(table.ranks[0].v[0].kind), result[0].second);
    }
  }
}

TEST_F(GroupKnnTest, groupChildUsesNearestCollapseAndFacetsDefaultToDocs) {
  CollectionHelper helper;
  schema(helper);
  ASSERT_TRUE(helper.indexAll({flatdoc("id", "a", "key_s", "x", "vec_vs",
      std::vector<std::vector<float>>{{0, 0, 0, 0}}),
      flatdoc("id", "b", "key_s", "x", "vec_vs", std::vector<std::vector<float>>{{1, 0, 0, 0}})},
      UpdateMessage::COMMIT).success);
  for (bool own : {false, true}) for (int parallel : {1, -1}) {
    auto actual = run("main", R"("collapse":{"field":"key_s","ops":{
      "docs":{"field_facet":{"field":"key_s"}},
      "groups":{"field_facet":{"field":"key_s","count_unit":"groups"}},
      "child":{"top_docs":{)" + std::string(own ? R"("collapse":"key_s",)" : "")
        + R"("query":)" + knn(1, true) + "}}}}", parallel);
    ASSERT_TRUE(actual->ok()) << actual->errorMsg();
    const auto& table = *actual->docList()->groups;
    EXPECT_EQ(1, std::get<api::ArrVal>(table.ops.at("child")->kind).v[0].docList()->row_count);
    EXPECT_EQ(2, std::get<api::ArrVal>(table.ops.at("docs")->kind).v[0].facetResult()->counts[0]);
    EXPECT_EQ(1, std::get<api::ArrVal>(table.ops.at("groups")->kind).v[0].facetResult()->counts[0]);
  }
}

TEST_F(GroupKnnTest, selectionBucketsReevaluateAndResetQueriesUseTheirScope) {
  CollectionHelper helper;
  schema(helper);
  std::vector<Doc> documents{
      flatdoc("id", "a", "key_s", "x", "price_i", (int64_t)4,
          "vec_vs", std::vector<std::vector<float>>{{0, 0, 0, 0}}),
      flatdoc("id", "b", "key_s", "x", "price_i", (int64_t)2,
          "vec_vs", std::vector<std::vector<float>>{{1, 0, 0, 0}}),
      flatdoc("id", "c", "key_s", "x", "price_i", (int64_t)0,
          "vec_vs", std::vector<std::vector<float>>{{50, 0, 0, 0}})};
  for (const auto& doc : documents) {
    ASSERT_TRUE(helper.indexAll(std::span(&doc, 1), UpdateMessage::COMMIT).success);
  }
  auto selection = knn(1, false);
  for (auto query : {selection, R"({"rescore":{"query":)" + selection + R"(,"expr":"score"}})",
      R"({"boolean":{"required":[)" + selection + R"(,"*:*" ]}})"}) {
    for (bool throughList : {false, true}) for (bool filtered : {false, true}) for (int parallel : {1, -1}) {
      auto reset = R"("members":{"top_docs":{"limit":-1,"fields":["id"],"document_format":"rows"}
        ,"domain":{"query":"*:*")"
          + std::string(filtered ? R"(,"filter":["price_i:<3"]}})" : "}}");
      auto children = throughList ? R"("through":{"top_docs":{"query":"id:b",
        "limit":0,"ops":{)" + reset + "}}}" : reset;
      auto actual = run("main", R"("query":"id:b","limit":0,"ops":{"qf":{"query_facet":{
        "buckets":{"near":)"
          + query + R"(},"ops":{)" + children + "}}}}", parallel);
      ASSERT_TRUE(actual->ok()) << actual->errorMsg();
      const auto& facet = *actual->docList()->ops.at("qf")->facetResult();
      EXPECT_EQ(1, facet.counts[0]); // The original window contains b.
      const api::Val* members;
      if (throughList) {
        auto& list = *std::get<api::ArrVal>(facet.ops.at("through")->kind).v[0].docList();
        members = &*list.ops.at("members");
      } else members = &std::get<api::ArrVal>(facet.ops.at("members")->kind).v[0];
      ASSERT_EQ(1, members->docList()->row_count);
      EXPECT_EQ(filtered ? "b" : "a", members->docList()->docs[0].fields.at("id")->asString());
    }
  }
  for (int outer : {1, 2}) for (int parallel : {1, -1}) {
    auto actual = run("main", R"("query":"id:c","limit":0,"ops":{"outer":{"query_facet":{
        "buckets":{"near":)"
        + knn(outer, false) + R"(},"ops":{"eligible":{"query_facet":{"buckets":{
        "cheap":"price_i:<=2"},"ops":{
          "inner":{"query_facet":{"buckets":{"near":)" + knn(1, false, true, {}, "[50,0,0,0]")
        + R"(},"ops":{"members":{"top_docs":{"limit":-1,"fields":["id"],"document_format":"rows"}
        ,"domain":{"query":"*:*"}}}}}
        }}}}}}})", parallel);
    ASSERT_TRUE(actual->ok()) << actual->errorMsg();
    auto child = [](const api::FacetResult& parent, std::string_view name) -> const api::Val& {
      return std::get<api::ArrVal>(parent.ops.at(name)->kind).v[0];
    };
    auto& first = *actual->docList()->ops.at("outer")->facetResult();
    EXPECT_EQ(1, first.counts[0]);
    auto& eligible = *child(first, "eligible").facetResult();
    auto& inner = *child(eligible, "inner").facetResult();
    EXPECT_EQ(1, inner.counts[0]);
    auto& members = *child(inner, "members").docList();
    EXPECT_EQ(outer == 1 ? 0 : 1, members.row_count);
    if (members.row_count) { EXPECT_EQ("b", members.docs[0].fields.at("id")->asString()); }
  }
  for (bool groups : {false, true}) for (bool filter : {false, true}) for (int parallel : {1, -1}) {
    std::string domain = filter ? R"("query":"*:*","filter":[)" + knn(2, false) + ']'
        : R"("query":)" + knn(2, false);
    auto actual = run("main", R"("fields":["id"],"query":)" + knn(1, groups)
        + R"(,"collapse":{"field":"key_s","get_number":true,"ops":{"members":{
        "top_docs":{"sort":"price_i asc","limit":-1,"fields":["id"],"document_format":"rows"}
        ,"domain":{)"
        + domain + R"(}},"inherited":{"top_docs":{"limit":-1,"fields":["id"]}
        ,"domain":{"filter":[)"
        + knn(2, false) + "]}}}}", parallel);
    ASSERT_TRUE(actual->ok()) << actual->errorMsg();
    auto& table = *actual->docList()->groups;
    ASSERT_EQ(1u, table.counts.size());
    EXPECT_EQ(1, table.counts[0]);
    EXPECT_EQ(1, std::get<api::ArrVal>(table.ops.at("inherited")->kind).v[0].docList()->row_count);
    const auto& members = *std::get<api::ArrVal>(table.ops.at("members")->kind).v[0].docList();
    ASSERT_EQ(2, members.row_count);
    EXPECT_EQ("b", members.docs[0].fields.at("id")->asString());
    EXPECT_EQ("a", members.docs[1].fields.at("id")->asString());
  }
}

TEST_F(GroupKnnTest, resetSelectionPreparesInsideEachBucketWithCacheEnabled) {
  CollectionHelper helper;
  schema(helper);
  FilterCacheConfig config;
  config.maxBytes = 4 * 1024 * 1024;
  config.minSegmentDocs = 0;
  config.admissionThreshold = 1;
  helper.getIndexWriter()->snapshots.readers.filterCache = std::make_shared<FilterCache>(config);
  for (int segment = 0; segment < 2; segment++) {
    std::vector<Doc> docs;
    for (int i = segment * 2; i < segment * 2 + 2; i++) docs.push_back(flatdoc(
        "id", std::to_string(i), "key_s", i % 2 ? "y" : "x",
        "vec_vs", std::vector<std::vector<float>>{{(float)i, 0, 0, 0}}));
    ASSERT_TRUE(helper.indexAll(docs, UpdateMessage::COMMIT).success);
  }
  for (int parallel : {1, -1, 1}) {
    auto actual = run("main", R"("query":"*:*","sort":"key_s asc","collapse":{
        "field":"key_s","ops":{
      "nearest":{"top_docs":{"fields":["id"],"document_format":"rows","limit":-1}
        ,"domain":{"query":)"
        + knn(1, false) + "}}}}", parallel);
    ASSERT_TRUE(actual->ok()) << actual->errorMsg();
    const auto& table = *actual->docList()->groups;
    ASSERT_EQ(2u, table.keys.size());
    for (size_t group = 0; group < table.keys.size(); group++) {
      auto& nearest = *std::get<api::ArrVal>(table.ops.at("nearest")->kind).v[group].docList();
      ASSERT_EQ(1, nearest.row_count);
      EXPECT_EQ(std::to_string(group), nearest.docs[0].fields.at("id")->asString());
    }
  }
}

TEST_F(GroupKnnTest, selectionResetKeepsSharedPrefixStateAcrossBucketBlocks) {
  CollectionHelper helper;
  schema(helper);
  for (int segment = 0; segment < 3; segment++) {
    std::vector<Doc> docs;
    for (int i = segment * 8; i < (segment + 1) * 8; i++) docs.push_back(flatdoc(
        "id", std::to_string(i), "title_t", "prefix" + std::to_string(i % 5),
        "vec_vs", std::vector<std::vector<float>>{{(float)i, 0, 0, 0}}));
    ASSERT_TRUE(helper.indexAll(docs, UpdateMessage::COMMIT).success);
  }
  ASSERT_TRUE(helper.deleteByIds({"2", "11"}, UpdateMessage::COMMIT).success);
  SearchOverridesGuard guard(forcedFacetBucketBlockBuckets);
  forcedFacetBucketBlockBuckets = 1;
  std::string buckets;
  for (int bucket = 0; bucket < 5; bucket++) {
    if (bucket) buckets += ',';
    buckets += '"' + std::to_string(bucket) + R"(":{"boolean":{"required":[)"
        + knn(4, false) + R"(,"title_t:prefix*"]}})";
  }
  for (int parallel : {1, -1}) {
    auto actual = run("main", R"("query":"id:7","limit":0,"ops":{"near":{"query_facet":{
        "buckets":{)"
        + buckets + R"(},"ops":{"ordinary":{"top_docs":{"limit":-1,"fields":["id"],
        "document_format":"rows"}},
          "reset":{"top_docs":{"sort":"id asc","limit":-1,"fields":["id"],"document_format":"rows"}
        ,"domain":{"query":"*:*"}}}}}})", parallel);
    ASSERT_TRUE(actual->ok()) << actual->errorMsg();
    const auto& facet = *actual->docList()->ops.at("near")->facetResult();
    ASSERT_EQ(5u, facet.counts.size());
    for (size_t bucket = 0; bucket < facet.counts.size(); bucket++) {
      EXPECT_EQ(1, facet.counts[bucket]);
      const auto& ordinary = *std::get<api::ArrVal>(facet.ops.at("ordinary")->kind).v[bucket].docList();
      ASSERT_EQ(1, ordinary.row_count);
      EXPECT_EQ("7", ordinary.docs[0].fields.at("id")->asString());
      const auto& reset = *std::get<api::ArrVal>(facet.ops.at("reset")->kind).v[bucket].docList();
      ASSERT_EQ(4, reset.row_count);
      std::set<std::string> ids;
      for (const auto& doc : reset.docs) ids.emplace(doc.fields.at("id")->asString());
      EXPECT_EQ((std::set<std::string>{"0", "1", "3", "4"}), ids);
    }
  }
}

TEST_F(GroupKnnTest, selectionAndPredicateBucketsShareResetBlock) {
  CollectionHelper helper;
  schema(helper);
  for (int i = 0; i < 2; i++) {
    ASSERT_TRUE(helper.index(flatdoc("id", i ? "b" : "a", "vec_vs",
        std::vector<std::vector<float>>{{(float)i, 0, 0, 0}}), UpdateMessage::COMMIT).success);
  }
  SearchOverridesGuard guard(forcedFacetBucketBlockBuckets);
  forcedFacetBucketBlockBuckets = 2;
  for (int parallel : {1, -1}) {
    auto actual = run("main", R"("query":"id:b","limit":0,"ops":{"qf":{"query_facet":{
      "buckets":{"both":"id:a OR id:b","near":)" + knn(1, false) + R"(},"ops":{
        "members":{"top_docs":{"sort":"id asc","limit":-1,"fields":["id"],
          "document_format":"rows"},"domain":{"query":"*:*"}}
      }}}})", parallel);
    ASSERT_TRUE(actual->ok()) << actual->errorMsg();
    const auto& facet = *actual->docList()->ops.at("qf")->facetResult();
    auto labels = std::get<api::ColStr>(facet.bucket_ids->kind).v;
    ASSERT_EQ(2u, labels.size());
    for (size_t bucket = 0; bucket < labels.size(); bucket++) {
      EXPECT_EQ(1, facet.counts[bucket]);
      const auto& members = *std::get<api::ArrVal>(facet.ops.at("members")->kind).v[bucket].docList();
      ASSERT_EQ(labels[bucket] == "both" ? 2 : 1, members.row_count);
      EXPECT_EQ("a", members.docs[0].fields.at("id")->asString());
    }
  }
}

TEST_F(GroupKnnTest, resetAllExtraKeepsItsScopedOwner) {
  CollectionHelper helper;
  schema(helper);
  for (int i = 0; i < 2; i++) {
    ASSERT_TRUE(helper.index(flatdoc("id", std::to_string(i), "vec_vs",
        std::vector<std::vector<float>>{{(float)i, 0, 0, 0}}), UpdateMessage::COMMIT).success);
  }
  for (int parallel : {1, -1}) {
    auto actual = run("main", R"("limit":0,"ops":{"f":{"query_facet":{"buckets":{"all":"*:*"},
      "ops":{"t":{"top_docs":{"query":)" + knn(2, false) + R"(,"filter":["id:*"],"limit":0,
        "ops":{"n":{"top_docs":{"limit":0,"get_number":true},
          "domain":{"query":"*:*","filter":["*:*"],"apply_parent_filters":true}},
          "near":{"top_docs":{"limit":0,"get_number":true},"domain":{"query":)"
        + knn(1, false) + R"(}}}}}}}}})", parallel);
    ASSERT_TRUE(actual->ok()) << actual->errorMsg();
    const auto& facet = *actual->docList()->ops.at("f")->facetResult();
    const auto& top = *std::get<api::ArrVal>(facet.ops.at("t")->kind).v[0].docList();
    EXPECT_EQ(2, top.ops.at("n")->docList()->found.value_or(-1));
    EXPECT_EQ(1, top.ops.at("near")->docList()->found.value_or(-1));
  }
}
