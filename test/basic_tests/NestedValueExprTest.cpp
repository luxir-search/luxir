// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include <numeric>
#include "luxir/value/ValueExprParser.h"
#include "test/CollectionHelper.h"
#include "test/LocalReq.h"
#include "test/LuxirTest.h"
#include "test/QueryBuild.h"
#include "test/SchemaBuilder.h"

using namespace luxir;
using namespace luxir::test;

class NestedValueExprTest : public LuxirTest {
protected:
  CollectionHelper helper;
  struct Model {
    std::string id;
    std::vector<int64_t> numbers, many, parts;
  };
  std::vector<Model> model{{"a", {8, -3}, {9, 2, 4, 9}, {5, 1, 7}},
      {"b", {8, 8}, {7, 1}, {2}}, {"c", {}, {}, {}}, {"d", {}, {}, {}}, {"e", {}, {}, {}}};

  void SetUp() override {
    LuxirTest::SetUp();
    SchemaBuilder schema;
    for (auto name : {"flat_numbers", "flat_many", "flat_parts", "items_nested.many"}) {
      auto& field = schema.field(name); field.type = api::FieldDef::FieldClass::INT;
      field.multi = api::FieldDef::MultiMode::ARRAY;
    }
    schema.set(helper.collection());
    helper.getIndexWriter()->mergePolicy->setMergeFactor(100);
    int i = 0;
    for (auto json : {
        R"({"id":"a","flat_numbers":[8,-3],"flat_many":[9,2,4,9],"flat_parts":[5,1,7],"items_nested":[{"n_i":8,"many":[9,2],"f_d":1.25,"label_s":"red","parts_nested":[{"p_i":5},{},{"p_i":1}]},{},{"n_i":-3,"many":[4,9],"f_d":-0.5,"label_s":"blue","parts_nested":{"p_i":7}}]})",
        R"({"id":"b","flat_numbers":[8,8],"flat_many":[7,1],"flat_parts":[2],"items_nested":[{"n_i":8,"many":[7,1],"f_d":2.5,"label_s":"red","parts_nested":{"p_i":2}},{"n_i":8}]})",
        R"({"id":"c","items_nested":[]})", R"({"id":"d","items_nested":[{}, {"label_s":"green"}]})",
        R"({"id":"e"})", R"({"id":"dead","items_nested":{"n_i":1000,"parts_nested":{"p_i":1000}}})"}) {
      CollectionHelper::UpdateBuilder input;
      input.addRaw([&](auto& doc, auto& arena) { ASSERT_TRUE(api::read_json(doc, json, arena)); });
      if (++i % 2 == 0) input.commit();
      ASSERT_TRUE(helper.submit(input).success);
    }
    ASSERT_TRUE(helper.deleteById("dead", UpdateMessage::COMMIT).success);
  }

  static std::optional<double> reduce(std::string_view op, const std::vector<int64_t>& values) {
    if (op == "count") return (double)values.size();
    if (values.empty()) return std::nullopt;
    if (op == "min") return (double)*std::min_element(values.begin(), values.end());
    if (op == "max") return (double)*std::max_element(values.begin(), values.end());
    double sum = (double)std::accumulate(values.begin(), values.end(), (int64_t)0);
    return op == "avg" ? sum / (double)values.size() : sum;
  }

  std::vector<std::string> sorted(std::string expression, qb::SortDir direction) {
    auto req = localReq(helper.getSearchEngine());
    auto& op = req->topDocs().allQuery().limit(20).fields({"id"});
    qb::sort(op, expression, direction); qb::sort(op, "id", qb::ASC);
    req->execute(1); EXPECT_OK(req);
    std::vector<std::string> ids;
    if (req->ok()) for (auto& doc : req->getDocs()) ids.push_back(std::get<std::string>(*find(doc, "id")));
    return ids;
  }
};

TEST_F(NestedValueExprTest, reducersMatchPlainModelsAndArrayTwinsAcrossMerges) {
  for (bool merge : {false, true}) {
    if (merge) {
      CollectionHelper::UpdateBuilder input; input.commit(true, 1);
      ASSERT_TRUE(helper.submit(input).success);
    }
    for (auto reducer : {"min", "max", "avg", "sum", "count"}) {
      for (int kind = 0; kind < 3; kind++) for (auto direction : {qb::ASC, qb::DESC}) {
        SCOPED_TRACE(::testing::Message() << merge << '/' << reducer << '/' << kind << '/' << (int)direction);
        auto values = [&](const Model& row) -> const auto& { return kind == 0 ? row.numbers : kind == 1 ? row.many : row.parts; };
        auto rows = model;
        std::sort(rows.begin(), rows.end(), [&](const Model& a, const Model& b) {
          auto x = reduce(reducer, values(a)), y = reduce(reducer, values(b));
          if (x.has_value() != y.has_value()) return x.has_value();
          if (!x || *x == *y) return a.id < b.id;
          return direction == qb::ASC ? *x < *y : *x > *y;
        });
        std::vector<std::string> expected;
        for (auto& row : rows) expected.push_back(row.id);
        std::string field = kind == 0 ? "items_nested.n_i" : kind == 1 ? "items_nested.many" : "items_nested.parts_nested.p_i";
        std::string twin = kind == 0 ? "flat_numbers" : kind == 1 ? "flat_many" : "flat_parts";
        EXPECT_EQ(expected, sorted(std::string(reducer) + '(' + field + ')', direction));
        EXPECT_EQ(expected, sorted(std::string(reducer) + '(' + twin + ')', direction));
      }
    }
  }
}

TEST_F(NestedValueExprTest, metricsRelativeViewsStringCountsAndRescoreUseTheSameValues) {
  auto req = localReq(helper.getSearchEngine());
  req->expr("maxima", "sum(max(items_nested.n_i))");
  req->expr("counts", "sum(count(items_nested.n_i))");
  req->expr("means", "avg(avg(items_nested.n_i))");
  req->expr("labels", "sum(count(items_nested.label_s))");
  req->expr("distinct", "cardinality(items_nested.label_s)");
  auto& rows = req->topDocs("rows").allQuery().limit(0);
  rows.rawOp().domain.emplace().to = "items_nested";
  rows.expr("parts", "sum(max(parts_nested.p_i))");
  req->expr("floats", "sum(max(items_nested.f_d))");
  req->execute(1); ASSERT_OK(req);
  EXPECT_EQ(16, req->scalar<int64_t>("maxima")); EXPECT_EQ(4, req->scalar<int64_t>("counts"));
  EXPECT_DOUBLE_EQ(5.25, req->scalar<double>("means"));
  EXPECT_EQ(4, req->scalar<int64_t>("labels")); EXPECT_EQ(3, req->scalar<int64_t>("distinct"));
  EXPECT_EQ(14, req->docList("rows")->ops.at("parts")->asInt()); EXPECT_DOUBLE_EQ(3.75, req->scalar<double>("floats"));
  auto scores = localReq(helper.getSearchEngine());
  scores->topDocs().getScores().fields({"id"}).rawQuery() = qb::rescore(scores->mr, qb::all(), "def(max(items_nested.n_i),0)");
  scores->execute(1); ASSERT_OK(scores);
  auto values = std::get<api::ColFloat>(scores->docList()->columns.at("_score_").kind).v;
  ASSERT_EQ(5u, values.size()); EXPECT_EQ(8, values[0]); EXPECT_EQ(8, values[1]); EXPECT_EQ(0, values[4]);
}

TEST_F(NestedValueExprTest, scalarConsumersRequireReducersAndUpwardNamesStayInvalid) {
  for (auto [expression, error] : {
      std::pair{"items_nested.n_i", "choose an explicit reducer"},
      {"items_nested.n_i > 1", "operands must be scalars; reduce an array first"},
      {"col('../n_i')", "Invalid field or selector root: ../n_i"},
      {"field(/n_i)", "unknown value function 'field()'"},
      {"items_nested.missing", "Field not found: items_nested.missing"}}) {
    auto req = localReq(helper.getSearchEngine());
    qb::sort(req->topDocs().allQuery(), expression, qb::ASC);
    req->execute(1); EXPECT_FALSE(req->ok()) << expression;
    EXPECT_NE(std::string::npos, req->errorMsg().find(error)) << req->errorMsg();
  }
}

TEST_F(NestedValueExprTest, longSparseFamiliesKeepTypedColumnSemantics) {
  SchemaBuilder schema;
  auto& stamp = schema.field("items_nested.stamp");
  stamp.type = api::FieldDef::FieldClass::DATE; stamp.unit = api::FieldDef::DateUnit::MS;
  auto& price = schema.field("items_nested.price");
  price.type = api::FieldDef::FieldClass::DECIMAL; price.scale = 2;
  schema.field("items_nested.point").type = api::FieldDef::FieldClass::GEO_POINT;
  schema.field("empty_nested.n_i").type = api::FieldDef::FieldClass::INT;
  schema.set(helper.collection());
  std::string json = R"({"id":"long","items_nested":[)";
  for (int i = 0; i < 300; i++) {
    if (i) json += ',';
    json += fmt::format(R"({{"n_i":{})", i);
    if (i % 2 == 0) json += fmt::format(R"(,"many":[{},{}])", i, -i);
    if (i == 0) json += R"(,"stamp":1000,"price":1.25,"point":{"lat":0,"lon":0})";
    if (i == 299) json += R"(,"stamp":2000,"price":-2.5,"point":{"lat":0,"lon":1})";
    json += '}';
  }
  json += "]}";
  CollectionHelper::UpdateBuilder input;
  input.addRaw([&](auto& doc, auto& arena) { ASSERT_TRUE(api::read_json(doc, json, arena)); });
  input.commit(); ASSERT_TRUE(helper.submit(input).success);
  auto req = localReq(helper.getSearchEngine());
  req->expr("largest", "max(max(items_nested.n_i))");
  req->expr("total", "sum(sum(items_nested.n_i))");
  req->expr("arrays", "sum(count(items_nested.many))");
  req->expr("array_sum", "sum(sum(items_nested.many))");
  req->expr("date", "max(max(items_nested.stamp)/1)");
  req->expr("price", "sum(sum(items_nested.price))");
  req->expr("distance", "max(max(geo_distance(items_nested.point,0,0)))");
  req->expr("empty", "sum(count(empty_nested.n_i))");
  req->execute(1); ASSERT_OK(req);
  EXPECT_EQ(299, req->scalar<int64_t>("largest")); EXPECT_EQ(44871, req->scalar<int64_t>("total"));
  EXPECT_EQ(306, req->scalar<int64_t>("arrays")); EXPECT_EQ(32, req->scalar<int64_t>("array_sum"));
  EXPECT_DOUBLE_EQ(2000, req->scalar<double>("date")); EXPECT_DOUBLE_EQ(-1.25, req->scalar<double>("price"));
  EXPECT_GT(req->scalar<double>("distance"), 100000); EXPECT_LT(req->scalar<double>("distance"), 120000);
  EXPECT_EQ(0, req->scalar<int64_t>("empty"));
}

TEST_F(NestedValueExprTest, componentsMapOverDescendantsAtEachDepth) {
  SchemaBuilder schema;
  schema.field("items_nested.loc").type = api::FieldDef::FieldClass::GEO_POINT;
  schema.set(helper.collection());
  CollectionHelper::UpdateBuilder input;
  input.addRaw([&](auto& doc, auto& arena) {
    ASSERT_TRUE(api::read_json(doc, R"({"id":"typed","items_nested":[
      {"when_dt":"2024-01-01T00:00:00Z","when_dts":["2020-01-01T00:00:00Z","2024-01-01T00:00:00Z"],
       "loc":{"lat":10,"lon":20},"parts_nested":[{"when_dt":"2019-01-01T00:00:00Z"},{}]},
      {},{"when_dt":"2001-01-01T00:00:00Z","when_dts":["1999-01-01T00:00:00Z"],
       "loc":{"lat":-5,"lon":40},"parts_nested":{"when_dt":"2023-01-01T00:00:00Z"}}]})", arena));
  });
  input.commit(); ASSERT_TRUE(helper.submit(input).success);
  auto req = localReq(helper.getSearchEngine());
  req->expr("latest", "max(max(items_nested.when_dt.year))");
  req->expr("earliest", "min(min(items_nested.when_dt.year))");
  req->expr("years", "sum(count(items_nested.when_dts.year))");
  req->expr("parts", "max(max(items_nested.parts_nested.when_dt.year))");
  req->expr("latitude", "sum(max(items_nested.loc.lat))");
  req->execute(1); ASSERT_OK(req);
  EXPECT_EQ(2024, req->scalar<int64_t>("latest")); EXPECT_EQ(2001, req->scalar<int64_t>("earliest"));
  EXPECT_EQ(3, req->scalar<int64_t>("years")); EXPECT_EQ(2023, req->scalar<int64_t>("parts"));
  EXPECT_NEAR(10, req->scalar<double>("latitude"), 1e-6);
  for (auto expression : {"max(items_nested.when_dt.year)", "max(year(items_nested.when_dt))",
      "max(col('items_nested.when_dt').year)", "max(col('items_nested').when_dt.year)"}) {
    EXPECT_EQ(sorted("max(items_nested.when_dt.year)", qb::DESC), sorted(expression, qb::DESC));
  }
  for (auto [expression, error] : {
      std::pair{"max(items_nested.when_dt.wrong)", "unknown component 'wrong'"},
      {"max(items_nested.when_dt.year.x)", "only one component may follow field"},
      {"max(items_nested.n_i.year)", "has no components"}}) {
    auto bad = localReq(helper.getSearchEngine());
    qb::sort(bad->topDocs().allQuery(), expression, qb::ASC);
    bad->execute(1); EXPECT_FALSE(bad->ok());
    EXPECT_NE(std::string::npos, bad->errorMsg().find(error)) << bad->errorMsg();
  }
}

TEST_F(NestedValueExprTest, letAndRepeatedDescendantsShareTheirColumnAndReduction) {
  auto req = localReq(helper.getSearchEngine());
  req->reader = helper.getIndexWriter()->snapshots.readers.getReader();
  req->schema = req->reader->schema();
  ParseView view(*req->schema, nullptr, &req->reader, req.get());
  ValueExprOptions options{req->schema.get(), {}};
  options.view = &view; options.fieldResolver = &view.fields;
  auto parse = [&](std::string_view expression) { return ValueExprParser(options, req->arena).parse(expression); };
  auto* boundArray = parse("let(v, items_nested.many, max(v)+max(v))");
  auto* repeated = parse("max(items_nested.many)+max(items_nested.many)");
  auto* boundReduction = parse("let(v, max(items_nested.many), v+v)");
  EXPECT_TRUE(boundArray->structurallyEqual(*repeated));
  EXPECT_TRUE(boundArray->structurallyEqual(*boundReduction));
  ASSERT_EQ(3u, boundArray->nodes.size());
  EXPECT_EQ(ValueNodeKind::COLUMN, boundArray->nodes[0].kind);
  EXPECT_EQ("items_nested", boundArray->nodes[0].source->path());
  EXPECT_TRUE(boundArray->nodes[1].flags & ValueNode::SHARED);
  MemPool pool;
  auto* values = boundArray->bind(pool, req->reader->segments()[0], boundArray->resolveFields(pool, *req->reader));
  ASSERT_NE(nullptr, values->nodes[1].memo);
  for (int32_t row : {0, 0, 1, 0}) {
    EXPECT_EQ(row == 0 ? 18 : 14, values->evalPoint(row, 0).intValue);
    EXPECT_EQ(row, values->nodes[0].cachedDoc);
    EXPECT_EQ(row, values->nodes[1].memo->docid);
    EXPECT_EQ(row == 0 ? 9 : 7, values->nodes[1].memo->result.intValue);
  }
  // Equal physical columns read from different owner views are different nodes.
  auto* root = parse("max(items_nested.parts_nested.p_i)");
  auto scope = view.scoped("items_nested");
  auto* child = parse("max(parts_nested.p_i)");
  EXPECT_FALSE(root->structurallyEqual(*child));
}
