// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <optional>
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
// Elasticsearch gauss/exp/linear decay functions of the value language.
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
    b.set(helper.collection());
    helper.index(flatdoc("id_s", "a", "body_w", "alpha", "x_i", 2, "v_d", 10.0,
                         "vals_ds", vec(10.0, 40.0), "when_dt", int64_t{1000},
                         "loc_p", std::vector<double>{0.0, 0.5}),
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
    BoundValueProgram* bound = parse(expression)->bind(pool, reader->segments()[0]);
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
  expectParseError("exp(v_d, 1)", "3 to 5");
}

TEST_F(FunctionScoreTest, randomIsDeterministicPerSeedAndValue) {
  auto first = eval("random(42, x_i)");
  EXPECT_EQ(first, eval("random(42, x_i)"));
  EXPECT_NE(first, eval("random(43, x_i)"));
  EXPECT_FALSE(first[2].has_value());
  for (std::string_view expression : {"random()", "random(7)", "random(42, x_i)"}) {
    for (auto value : eval(expression)) {
      if (!value) continue;
      EXPECT_GE(*value, 0.0) << expression;
      EXPECT_LT(*value, 1.0) << expression;
      EXPECT_EQ(*value, (double)(float)*value) << expression;
    }
  }
  EXPECT_EQ(eval("random(5)"), eval("random(5)"));
  expectParseError("random(1.5)", "must be an integer");
}

TEST_F(FunctionScoreTest, decayFunctionsMatchElasticsearch) {
  struct Case {
    std::string expression;
    double (*formula)(double, double, double);
    std::vector<std::optional<double>> values;  // doc a, b; c is missing
    double origin, scale, offset, decay;
  };
  std::vector<Case> cases{
      {"gauss(v_d, 100, 50)", esGauss, {10.0, 130.0}, 100, 50, 0, 0.5},
      {"exp(v_d, 100, 50, 10, 0.3)", esExp, {10.0, 130.0}, 100, 50, 10, 0.3},
      {"linear(v_d, 100, 50, 25, 0.2)", esLinear, {10.0, 130.0}, 100, 50, 25, 0.2},
      // Multi-valued fields use the closest value (multi_value_mode MIN).
      {"gauss(vals_ds, 35, 20)", esGauss, {40.0, 70.0}, 35, 20, 0, 0.5},
      // DATE fields decay over milliseconds from an epoch-millis origin.
      {"exp(when_dt, 2000, 1000, 500, 0.5)", esExp, {1000.0, 5000.0}, 2000, 1000, 500, 0.5},
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
  expectValues("gauss(geo_distance(loc_p, 0, 0), 0, 100000, 1000)",
               {esGauss(esDistance(meters, 0, 1000), 100000, 0.5), 1.0, 1.0});
  expectValues("geo_distance(loc_p, 0, 0)", {meters, std::nullopt, std::nullopt});

  // Decays are provably within (0, 1] for pruning.
  BoundValueProgram* bound = parse("gauss(v_d, 100, 50)")->bind(pool, reader->segments()[0]);
  const ValueBounds& bounds = bound->bounds(bound->program.rootNode);
  EXPECT_EQ(BoundsCertainty::BOUNDED, bounds.certainty);
  EXPECT_GE(bounds.doubleMin, 0.0);
  EXPECT_LE(bounds.doubleMax, 1.0);

  expectParseError("gauss(v_d, 0, 0)", "scale must be positive");
  expectParseError("gauss(v_d, 0, 1, -1)", "offset must be non-negative");
  expectParseError("exp(v_d, 0, 1, 0, 1)", "decay must be strictly between");
  expectParseError("linear(v_d, 0, x_i)", "scale must be a numeric literal");
  expectParseError("geo_distance(v_d, 0, 0)", "not a GEO_POINT field");
  expectParseError("geo_distance(loc_p, 91, 0)", "latitude");
}

TEST_F(FunctionScoreTest, rescoreRanksByDecay) {
  auto req = localReq(helper.getSearchEngine());
  req->collection("main").topDocs("q")
      .exprQuery("rescore(body_w:alpha, expr=gauss(v_d, 120, 50) * 2)")
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
