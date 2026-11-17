// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <optional>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

#include "luxir/util/geo.h"
#include "luxir/value/ValueExprParser.h"
#include "test/CollectionHelper.h"
#include "test/LocalReq.h"
#include "test/LuxirTest.h"
#include "test/SchemaBuilder.h"
#include "test/TestUtils.h"

using namespace luxir;
using namespace luxir::test;

// exp, pow, comparisons, conditionals, random, geo_distance, and the
// Elasticsearch-style decay functions of the value language.
class FunctionScoreTest : public LuxirTest {
protected:
  google::protobuf::Arena* arena = createArena();
  CollectionHelper helper;
  std::shared_ptr<IndexReader> reader;
  MemPool pool;

  ~FunctionScoreTest() override { releaseArena(arena); }

  // Docs: a(x_i=2, v_d=10, vals_ds=[10,40], when_dt=1000, loc_p=(0.5,0)),
  // b(x_i=-1, v_d=130, vals_ds=[70], when_dt=5000), c(no values).
  void SetUp() override {
    SchemaBuilder b;
    auto& loc = b.field("loc_p");
    loc.type = api::FieldDef::FieldClass::GEO_POINT;
    loc.index = api::FieldDef::IndexMode::RANGE;
    loc.column = true;
    auto& multi = b.field("locs_p");
    multi.type = api::FieldDef::FieldClass::GEO_POINT;
    multi.multi = api::FieldDef::MultiMode::DEFAULT;
    multi.column = true;
    auto& noColumn = b.field("no_column_p");
    noColumn.type = api::FieldDef::FieldClass::GEO_POINT;
    noColumn.index = api::FieldDef::IndexMode::NONE;
    noColumn.column = false;
    b.set(helper.collection());
    helper.index(flatdoc("id_s", "a", "body_w", "alpha", "x_i", 2, "v_d", 10.0,
                         "vals_ds", vec(10.0, 40.0), "when_dt", int64_t{1000},
                         "loc_p", std::vector<double>{0.0, 0.5},
                         "locs_p", vecs("0.5,0", "1,0")),
                 UpdateMessage::NO_COMMIT);
    helper.index(flatdoc("id_s", "b", "body_w", "alpha", "x_i", -1, "v_d", 130.0,
                         "vals_ds", vec(70.0), "when_dt", int64_t{5000}),
                 UpdateMessage::NO_COMMIT);
    helper.index(flatdoc("id_s", "c", "body_w", "alpha"), UpdateMessage::COMMIT);
    reader = helper.getIndexWriter()->snapshots.readers.getReader();
  }

  ValueProgram* parse(std::string_view expression) {
    ValueExprOptions options{helper.collection().getSchema().get(), {}};
    return ValueExprParser(options, *arena).parse(expression);
  }

  // One value per doc a, b, c; nullopt for missing.
  std::vector<std::optional<double>> eval(std::string_view expression) {
    auto program = parse(expression);
    BoundValueProgram* bound = program->bind(pool, reader->segments()[0],
                                             program->resolveFields(pool, *reader));
    std::vector<std::optional<double>> out;
    for (int32_t doc = 0; doc < 3; doc++) {
      ValueResult r = bound->evalPoint(doc, 0.0f);
      if (!r.valid) {
        out.push_back(std::nullopt);
      } else {
        out.push_back(r.type == ValueType::DOUBLE ? r.doubleValue : (double)r.intValue);
      }
    }
    return out;
  }

  void expectValues(std::string_view expression,
                    const std::vector<std::optional<double>>& expected) {
    auto actual = eval(expression);
    for (size_t i = 0; i < expected.size(); i++) {
      ASSERT_EQ(expected[i].has_value(), actual[i].has_value()) << expression << " doc " << i;
      if (expected[i]) {
        EXPECT_NEAR(*expected[i], *actual[i], 1e-12) << expression << " doc " << i;
      }
    }
  }

  void expectParseError(std::string_view expression, std::string_view expected) {
    try {
      parse(expression);
      FAIL() << "expected parse failure for " << expression;
    } catch (const std::runtime_error& error) {
      EXPECT_NE(std::string(error.what()).find(expected), std::string::npos) << error.what();
    }
  }
};

namespace {

// Elasticsearch DecayFunctionBuilder formulas.
double esDistance(double value, double origin, double offset) {
  return std::max(0.0, std::abs(value - origin) - offset);
}
double esGauss(double d, double scale, double decay) {
  double sigmaSquared = -scale * scale / (2.0 * std::log(decay));
  return std::exp(-d * d / (2.0 * sigmaSquared));
}
double esExp(double d, double scale, double decay) {
  return std::exp(std::log(decay) / scale * d);
}
double esLinear(double d, double scale, double decay) {
  double s = scale / (1.0 - decay);
  return std::max(0.0, (s - d) / s);
}

} // namespace

TEST_F(FunctionScoreTest, mathComparisonsAndConditionals) {
  expectValues("exp(v_d / 10)", {std::exp(1.0), std::exp(13.0), std::nullopt});
  expectValues("pow(x_i, 2) + pow(4, 0.5)", {6.0, 3.0, std::nullopt});
  expectValues("x_i > 0 ? 10 : 20", {10.0, 20.0, std::nullopt});
  expectValues("if(x_i >= 2 && !(x_i == 3), 1, 0)", {1.0, 0.0, std::nullopt});
  expectValues("x_i < 0 || v_d != 10", {0.0, 1.0, std::nullopt});
  expectValues("def(x_i, 0) <= 0 ? 1 : x_i > 1 ? 2 : 3", {2.0, 1.0, 1.0});
  // Only the taken branch runs, so log() of a non-positive column binds.
  expectValues("x_i > 0 ? log(x_i) : 0", {std::log(2.0), 0.0, std::nullopt});
  EXPECT_THROW(eval("log(x_i)"), ValueEvaluationError);

  expectParseError("1 < 2 < 3", "do not chain");
  expectParseError("x_i > 0 ? 1", "expected ':'");
  expectParseError("vals_ds > 1", "reduce an array first");
  expectParseError("if(x_i, when_dt, 1)", "both branches");
  expectParseError("exp(v_d, 1)", "expects exactly 1 argument");
}

TEST_F(FunctionScoreTest, randomIsDeterministicPerSeedAndValue) {
  auto first = eval("random(42, x_i)");
  EXPECT_EQ(first, eval("random(42, x_i)"));
  EXPECT_NE(first, eval("random(43, x_i)"));
  EXPECT_FALSE(first[2].has_value());
  for (std::string_view expression : {"random()", "random(42, x_i)"}) {
    for (auto value : eval(expression)) {
      if (!value) continue;
      EXPECT_GE(*value, 0.0) << expression;
      EXPECT_LT(*value, 1.0) << expression;
      EXPECT_EQ(*value, (double)(float)*value) << expression;
    }
  }
  EXPECT_EQ(eval("random(5, x_i)"), eval("random(5, x_i)"));
  EXPECT_EQ(eval("random(1000 * 60, x_i)"), eval("random(60000, x_i)"));
  EXPECT_EQ(eval("random(0 - 5, x_i)"), eval("random(-5, x_i)"));
  EXPECT_EQ(eval("random(if(0, 3, 7), x_i)"), eval("random(7, x_i)"));
  expectParseError("random(x_i, x_i)", "argument 1 must be a constant numeric scalar");
  expectParseError("random(log(-1), x_i)", "argument 1 must be a constant numeric scalar");
  expectParseError("random(1.5, x_i)", "must be an integer");
  expectParseError("random(5)", "docids are not stable across merges");
  for (auto expression : {"random(5, vals_ds)", "random(5, x_i + 0)", "random(5, 1)",
                          "random(5, score)", "random(5, _docid_)"}) {
    expectParseError(expression, "key must be a scalar numeric column");
  }
  expectParseError("random(5, id_s)", "non-numeric column");
  auto* unseeded = parse("random()");
  auto* bound = unseeded->bind(pool, reader->segments()[0], unseeded->resolveFields(pool, *reader));
  EXPECT_DOUBLE_EQ(bound->evalPoint(0, 0).doubleValue, bound->evalPoint(0, 0).doubleValue);
}

TEST_F(FunctionScoreTest, randomKeysUseNativeBitsWithoutOtherSalts) {
  CollectionHelper h("random_keys");
  ASSERT_TRUE(h.indexAll({
      flatdoc("id", "a", "key_i", INT64_C(9007199254740992), "copy_i", INT64_C(9007199254740992),
              "key_d", 0.0, "key_f", 1.0f),
      flatdoc("id", "b", "key_i", INT64_C(9007199254740993), "copy_i", INT64_C(9007199254740993),
              "key_d", -0.0, "key_f", 1.0f),
      flatdoc("id", "c", "key_i", INT64_C(9007199254740992), "copy_i", INT64_C(9007199254740992))},
      UpdateMessage::COMMIT).success);
  auto index = h.getIndexWriter()->snapshots.readers.getReader();
  auto schema = h.collection().getSchema();
  google::protobuf::Arena arena;
  MemPool memory;
  auto values = [&](std::string_view field) {
    ValueExprOptions options{schema.get(), {}};
    auto* program = ValueExprParser(options, arena).parse(fmt::format("random(7, {})", field));
    auto* bound = program->bind(memory, index->segments()[0], program->resolveFields(memory, *index));
    std::array<ValueResult, 3> results;
    std::array<int32_t, 3> docs{0, 1, 2};
    bound->evalBatch(docs, {}, results);
    for (int32_t doc : docs) EXPECT_EQ(results[doc].valid, bound->evalPoint(doc, 0).valid);
    return results;
  };
  auto integers = values("key_i");
  auto copies = values("copy_i");
  EXPECT_NE(integers[0].doubleValue, integers[1].doubleValue);
  EXPECT_EQ(integers[0].doubleValue, integers[2].doubleValue);
  for (int i = 0; i < 3; i++) EXPECT_EQ(integers[i].doubleValue, copies[i].doubleValue);
  auto doubles = values("key_d");
  EXPECT_NE(doubles[0].doubleValue, doubles[1].doubleValue);
  EXPECT_FALSE(doubles[2].valid);
  auto floats = values("key_f");
  EXPECT_EQ(floats[0].doubleValue, floats[1].doubleValue);
  EXPECT_FALSE(floats[2].valid);
}

TEST_F(FunctionScoreTest, integerSeedsPreserveBitsAboveDoublePrecision) {
  std::vector<Doc> docs;
  for (int i = 0; i < 32; i++) {
    docs.push_back(flatdoc("id_s", std::to_string(i), "body_w", "seeded", "x_i", i));
  }
  ASSERT_TRUE(helper.indexAll(docs, UpdateMessage::COMMIT).success);
  auto order = [&](std::string_view seed) {
    auto req = localReq(helper.getSearchEngine());
    req->collection("main").topDocs("q")
        .exprQuery(fmt::format("rescore(body_w:seeded, expr=random({}, x_i))", seed))
        .fields({"id_s"}).limit(-1);
    req->execute(false);
    EXPECT_TRUE(req->ok()) << req->errorMsg();
    std::vector<std::string> ids;
    if (!req->ok()) return ids;
    for (auto id : std::get<api::ColStr>(req->docList()->columns.at("id_s").kind).v) {
      ids.emplace_back(id);
    }
    return ids;
  };
  auto first = order("9007199254740992");
  ASSERT_EQ(32, first.size());
  EXPECT_NE(first, order("9007199254740993"));
  EXPECT_EQ(order("9007199254740993"), order("9007199254740992 + 1"));
  auto* maximum = parse("random(9223372036854775807, x_i)");
  EXPECT_EQ(INT64_MAX, maximum->root().intValue);
}

TEST_F(FunctionScoreTest, decayFunctionsMatchElasticsearch) {
  struct Case {
    std::string expression;
    double (*formula)(double, double, double);
    std::vector<std::optional<double>> values;  // doc a, b; c is missing
    double origin, scale, offset, decay;
  };
  std::vector<Case> cases{
      {"gauss_decay(v_d, 100, 50)", esGauss, {10.0, 130.0}, 100, 50, 0, 0.5},
      {"exp_decay(v_d, 100, 50, 10, 0.3)", esExp, {10.0, 130.0}, 100, 50, 10, 0.3},
      {"linear_decay(v_d, 100, 50, 25, 0.2)", esLinear, {10.0, 130.0}, 100, 50, 25, 0.2},
      // Multi-valued fields use the closest value (multi_value_mode MIN).
      {"gauss_decay(vals_ds, 35, 20)", esGauss, {40.0, 70.0}, 35, 20, 0, 0.5},
      // DATE fields decay over milliseconds from an epoch-millis origin.
      {"exp_decay(when_dt, 2000, 1000, 500, 0.5)", esExp, {1000.0, 5000.0}, 2000, 1000, 500, 0.5},
  };
  for (const Case& c : cases) {
    std::vector<std::optional<double>> expected;
    for (const auto& value : c.values) {
      expected.push_back(c.formula(esDistance(*value, c.origin, c.offset), c.scale, c.decay));
    }
    expected.push_back(1.0);  // a document without the field scores 1
    expectValues(c.expression, expected);
  }

  // Geo decay over meters from the origin point.
  double meters = geo::haversinMeters(0.0, 0.0, geo::decodeLatitude(geo::encodeLatitude(0.5)),
                                      geo::decodeLongitude(geo::encodeLongitude(0.0)));
  expectValues("gauss_decay(geo_distance(loc_p, 0, 0), 0, 100000, 1000)",
               {esGauss(esDistance(meters, 0, 1000), 100000, 0.5), 1.0, 1.0});
  expectValues("geo_distance(loc_p, 0, 0)", {meters, std::nullopt, std::nullopt});

  EXPECT_EQ(eval("gauss_decay(geo_distance(loc_p, 0, 0), 0, 100000)"),
            eval("gauss_decay(geo_distance(locs_p, 0, 0), 0, 100000)"));
  expectValues("exp_decay(10, 0, 10)", {0.5, 0.5, 0.5});
  expectParseError("gauss_decay(v_d, x_i, 10)", "argument 2 must be a constant numeric scalar");
  expectParseError("gauss(v_d, 0, 10)", "unknown value function");
  expectParseError("linear(v_d, 0, 10)", "unknown value function");
  // Decays are provably within [0, 1] for pruning.
  auto program = parse("gauss_decay(v_d, 100, 50)");
  BoundValueProgram* bound = program->bind(pool, reader->segments()[0],
                                           program->resolveFields(pool, *reader));
  EXPECT_EQ(ValueOpcode::DECAY, program->root().opcode);
  EXPECT_EQ(4, program->nodes.size());
  const ValueBounds& bounds = bound->bounds(bound->program.rootNode);
  EXPECT_EQ(BoundsCertainty::BOUNDED, bounds.certainty);
  EXPECT_GE(bounds.doubleMin, 0.0);
  EXPECT_LE(bounds.doubleMax, 1.0);

  EXPECT_EQ(eval("gauss_decay(v_d, 0, 1000 * 60)"), eval("gauss_decay(v_d, 0, 60000)"));
  EXPECT_EQ(eval("gauss_decay(v_d, 0, pow(2, 10))"), eval("gauss_decay(v_d, 0, 1024)"));
  EXPECT_EQ(eval("geo_distance(loc_p, 1 - 1, pow(2, 3) - 8)"),
            eval("geo_distance(loc_p, 0, 0)"));
  expectParseError("gauss_decay(v_d, 0, 0)", "scale must be positive");
  expectParseError("gauss_decay(v_d, 0, 1, -1)", "offset must be non-negative");
  expectParseError("exp_decay(v_d, 0, 1, 0, 1)", "decay must be strictly between");
  expectParseError("linear_decay(v_d, 0, x_i)", "argument 3 must be a constant numeric scalar");
  expectParseError("geo_distance(v_d, 0, 0)", "not a GEO_POINT value");
  expectParseError("geo_distance(loc_p, 91, 0)", "latitude");
}

TEST_F(FunctionScoreTest, nativeDecaysHandleExtremeValuesAndBounds) {
  CollectionHelper h("decay_extremes");
  SchemaBuilder builder;
  auto& stamp = builder.field("stamp");
  stamp.type = api::FieldDef::FieldClass::DATE;
  stamp.unit = api::FieldDef::DateUnit::NS;
  builder.set(h.collection());
  ASSERT_TRUE(h.indexAll({
      flatdoc("id", "min", "n_i", INT64_MIN, "v_d", -std::numeric_limits<double>::infinity(),
              "stamp", INT64_C(4700000000000000000), "tight_i", 10, "tight_ds", vec(10.0, 30.0),
              "vals_ds", vec(std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::infinity())),
      flatdoc("id", "max", "n_i", INT64_MAX, "v_d", std::numeric_limits<double>::infinity(),
              "stamp", INT64_C(-4699999999000000000), "tight_i", 20, "tight_ds", vec(20.0, 40.0),
              "vals_ds", vec(std::numeric_limits<double>::quiet_NaN(), 0.0)),
      flatdoc("id", "nan", "v_d", std::numeric_limits<double>::quiet_NaN(),
              "tight_i", 30, "tight_ds", vec(30.0, 50.0), "vals_ds", std::vector<double>{})},
      UpdateMessage::COMMIT).success);
  auto index = h.getIndexWriter()->snapshots.readers.getReader();
  auto schema = h.collection().getSchema();
  google::protobuf::Arena arena;
  MemPool memory;
  for (auto name : {"gauss_decay", "exp_decay", "linear_decay"}) {
    auto check = [&](std::string_view value, std::string_view parameters,
                     std::array<double, 3> expected) {
      SCOPED_TRACE(std::string(name) + "(" + std::string(value) + ")");
      ValueExprOptions options{schema.get(), {}};
      auto* program = ValueExprParser(options, arena).parse(fmt::format("{}({}, {})", name, value, parameters));
      auto* bound = program->bind(memory, index->segments()[0], program->resolveFields(memory, *index));
      std::array<int32_t, 3> docs{0, 1, 2};
      std::array<ValueResult, 3> batch;
      bound->evalBatch(docs, {}, batch);
      const auto& bounds = bound->bounds(program->rootNode);
      EXPECT_EQ(BoundsCertainty::BOUNDED, bounds.certainty);
      EXPECT_GE(bounds.doubleMin, 0);
      EXPECT_LE(bounds.doubleMax, 1);
      if (value == "tight_i") {
        EXPECT_DOUBLE_EQ(expected[2], bounds.doubleMin);
        EXPECT_DOUBLE_EQ(expected[0], bounds.doubleMax);
        EXPECT_TRUE(bounds.minAttained);
        EXPECT_TRUE(bounds.maxAttained);
      }
      if (value == "tight_ds") {
        EXPECT_DOUBLE_EQ(expected[0], bounds.doubleMax);
        EXPECT_FALSE(bounds.minAttained);
        EXPECT_TRUE(bounds.maxAttained);
      }
      for (int32_t doc : docs) {
        ASSERT_TRUE(batch[doc].valid);
        EXPECT_DOUBLE_EQ(expected[doc], batch[doc].doubleValue);
        EXPECT_DOUBLE_EQ(expected[doc], bound->evalPoint(doc, 0).doubleValue);
        EXPECT_LE(bounds.doubleMin, batch[doc].doubleValue);
        EXPECT_GE(bounds.doubleMax, batch[doc].doubleValue);
      }
    };
    check("n_i", "0, 1", {0, 0, 1});
    check("n_i", "9223372036854775807, 1", {0, 1, 1});
    check("v_d", "0, 1", {0, 0, 1});
    check("vals_ds", "0, 1", {0, 1, 1});
    auto formula = std::string_view(name) == "gauss_decay" ? esGauss
        : std::string_view(name) == "exp_decay" ? esExp : esLinear;
    check("tight_i", "0, 100", {formula(10, 100, 0.5), formula(20, 100, 0.5), formula(30, 100, 0.5)});
    check("tight_ds", "0, 100", {formula(10, 100, 0.5), formula(20, 100, 0.5), formula(30, 100, 0.5)});
    check("stamp", "-4700000000000000000, 1000000000", {0, 0.5, 1});
  }
}

TEST_F(FunctionScoreTest, geoOperandsUseTheRegistryAndStayNonNumeric) {
  auto* distance = parse("geo_distance(col(\"loc_p\"), 0, 0)");
  EXPECT_EQ(ValueNature::NUMBER, distance->root().nature);
  EXPECT_EQ(ValueOpcode::GEO_DISTANCE, distance->root().opcode);
  EXPECT_EQ(eval("geo_distance(loc_p, 0, 0)"), eval("geo_distance(col(\"loc_p\"), 0, 0)"));
  EXPECT_EQ(eval("geo_distance(loc_p, 0, 0)"), eval("min(geo_distance(locs_p, 0, 0))"));
  for (auto expression : {"loc_p", "loc_p + 1", "abs(loc_p)", "pow(loc_p, 2)", "loc_p == loc_p",
                          "def(loc_p, 0)", "if(1, loc_p, loc_p)",
                          "random(7, loc_p)", "min(locs_p)"}) {
    expectParseError(expression, "GEO_POINT is not numeric");
  }
  // count() takes any value and counts points.
  EXPECT_EQ(ValueType::INT64, parse("count(locs_p)")->root().type);
  expectParseError("geo_distance(no_column_p, 0, 0)", "no column values");
  expectParseError("geo_distance(loc_p, x_i, 0)", "argument 2 must be a constant numeric scalar");
  expectParseError("geo_distance(loc_p, 0, 181)", "longitude");
  auto aggregate = localReq(helper.getSearchEngine());
  aggregate->collection("main").expr("bad", "sum(loc_p)");
  aggregate->execute(false);
  EXPECT_FALSE(aggregate->ok());
  EXPECT_NE(std::string(aggregate->errorMsg()).find("use geo_distance()"), std::string::npos);
  auto rescore = localReq(helper.getSearchEngine());
  rescore->collection("main").topDocs("q").exprQuery("rescore(body_w:alpha, expr=loc_p)");
  rescore->execute(false);
  EXPECT_FALSE(rescore->ok());
  EXPECT_NE(std::string(rescore->errorMsg()).find("use geo_distance()"), std::string::npos);
}

TEST_F(FunctionScoreTest, rescoreRanksByDecay) {
  auto req = localReq(helper.getSearchEngine());
  req->collection("main").topDocs("q")
      .exprQuery("rescore(body_w:alpha, expr=gauss_decay(v_d, 120, 50) * 2)")
      .fields({"id_s"}).getScores().limit(-1);
  req->execute(false);
  ASSERT_TRUE(req->ok()) << req->errorMsg();
  const auto& ids = std::get<api::ColStr>(req->docList()->columns.at("id_s").kind).v;
  const auto& scores = std::get<api::ColFloat>(req->docList()->columns.at("_score_").kind).v;
  ASSERT_EQ(3u, ids.size());
  // c has no value (1 * 2), then b (distance 10), then a (distance 110).
  EXPECT_EQ("c", ids[0]);
  EXPECT_EQ("b", ids[1]);
  EXPECT_EQ("a", ids[2]);
  EXPECT_FLOAT_EQ(2.0f, scores[0]);
  EXPECT_FLOAT_EQ((float)(2.0 * esGauss(10.0, 50, 0.5)), scores[1]);
  EXPECT_FLOAT_EQ((float)(2.0 * esGauss(110.0, 50, 0.5)), scores[2]);
}
