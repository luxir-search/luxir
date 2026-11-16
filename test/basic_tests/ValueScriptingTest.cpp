// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include <gtest/gtest.h>

#include <xxhash.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <numbers>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <vector>

#include "luxir/api/RequestFlags.h"
#include "luxir/util/geo.h"
#include "luxir/util/luxir_util.h"
#include "luxir/value/ValueExprParser.h"
#include "test/CollectionHelper.h"
#include "test/LocalReq.h"
#include "test/QueryBuild.h"
#include "test/LuxirTest.h"
#include "test/SchemaBuilder.h"
#include "test/TestUtils.h"

using namespace luxir;
using namespace luxir::test;

// The math, bitwise, date, geo and hash functions of the value language, its
// local variables and type-defined field components.
class ValueScriptingTest : public LuxirTest {
protected:
  using Values = std::vector<std::optional<double>>;
  static constexpr size_t DOCS = 5;

  google::protobuf::Arena* arena = createArena();
  CollectionHelper helper;
  std::shared_ptr<IndexReader> reader;
  MemPool pool;
  std::vector<api::Val> varValues;
  std::vector<std::pair<std::string_view, ::hpp_proto::indirect_view<api::Val>>> varPairs;

  // 2024-03-15T10:20:30.456Z, 1970-01-01, 2020-12-31T23:59:59.999Z, one
  // millisecond before the epoch.
  static constexpr int64_t T_A = 1710498030456;
  static constexpr int64_t T_B = 0;
  static constexpr int64_t T_D = 1609459199999;
  static constexpr int64_t T_E = -1;

  ~ValueScriptingTest() override { releaseArena(arena); }

  // Docs a, b, c, d, e:
  //   a: x_i=2  y_i=5 v_d=10  vals_ds=[10,40] when=T_A loc=(40.7128,-74.006)
  //   b: x_i=-1       v_d=130 vals_ds=[70]    when=T_B loc=(-33.8688,151.2093)
  //   c: no values
  //   d: x_i=7        v_d=0.5                 when=T_D loc=(-33,175)
  //   e: x_i=0                                when=T_E
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
    auto& stamp = b.field("stamp");
    stamp.type = api::FieldDef::FieldClass::DATE;
    stamp.unit = api::FieldDef::DateUnit::NS;
    b.set(helper.collection());
    helper.index(flatdoc("id_s", "a", "body_w", "alpha", "x_i", 2, "y_i", 5, "v_d", 10.0,
                         "vals_ds", vec(10.0, 40.0), "when_dt", T_A, "when_dts", vec_i(T_A, T_A + 1), "stamp", T_A * 1000000,
                         "loc_p", "40.7128,-74.006", "locs_p", vecs("40.7,-74", "51.5,0")),
                 UpdateMessage::NO_COMMIT);
    helper.index(flatdoc("id_s", "b", "body_w", "alpha", "x_i", -1, "v_d", 130.0,
                         "vals_ds", vec(70.0), "when_dt", T_B, "loc_p", "-33.8688,151.2093"),
                 UpdateMessage::NO_COMMIT);
    helper.index(flatdoc("id_s", "c", "body_w", "alpha"), UpdateMessage::NO_COMMIT);
    helper.index(flatdoc("id_s", "d", "body_w", "alpha", "x_i", 7, "v_d", 0.5,
                         "when_dt", T_D, "loc_p", "-33,175"),
                 UpdateMessage::NO_COMMIT);
    helper.index(flatdoc("id_s", "e", "body_w", "alpha", "x_i", 0, "when_dt", T_E, "when_dts", vec_i(-2, -1)),
                 UpdateMessage::COMMIT);
    reader = helper.getIndexWriter()->snapshots.readers.getReader();
  }

  void bindVar(std::string_view name, api::Val value) {
    varValues.reserve(8);
    varValues.push_back(value);
    varPairs.push_back({name, ::hpp_proto::indirect_view<api::Val>{&varValues.back()}});
  }

  ValueProgram* parse(std::string_view expression) {
    ValueExprOptions options{helper.collection().getSchema().get(), {}};
    options.vars = api::map_view<std::string_view, ::hpp_proto::indirect_view<api::Val>>(
        std::span(varPairs.data(), varPairs.size()));
    return ValueExprParser(options, *arena).parse(expression);
  }

  // One value per doc; nullopt for missing.
  Values eval(std::string_view expression, float score = 0.0f) {
    auto program = parse(expression);
    BoundValueProgram* bound = program->bind(pool, reader->segments()[0],
                                             program->resolveFields(pool, *reader));
    Values out;
    for (int32_t doc = 0; doc < (int32_t)DOCS; doc++) {
      ValueResult r = bound->evalPoint(doc, score);
      if (!r.valid) {
        out.push_back(std::nullopt);
      } else {
        out.push_back(r.type == ValueType::DOUBLE ? r.doubleValue : (double)r.intValue);
      }
    }
    return out;
  }

  // The expression's value for a doc, as an integer.
  int64_t evalInt(std::string_view expression, int32_t doc = 0) {
    auto program = parse(expression);
    BoundValueProgram* bound = program->bind(pool, reader->segments()[0],
                                             program->resolveFields(pool, *reader));
    ValueResult r = bound->evalPoint(doc, 0.0f);
    EXPECT_TRUE(r.valid) << expression;
    EXPECT_EQ(ValueType::INT64, r.type) << expression;
    return r.intValue;
  }

  void expectValues(std::string_view expression, const Values& expected, double tolerance = 1e-12) {
    auto actual = eval(expression);
    ASSERT_EQ(expected.size(), actual.size());
    for (size_t i = 0; i < expected.size(); i++) {
      ASSERT_EQ(expected[i].has_value(), actual[i].has_value()) << expression << " doc " << i;
      if (expected[i]) {
        EXPECT_NEAR(*expected[i], *actual[i], tolerance) << expression << " doc " << i;
      }
    }
  }

  // The same value for every doc.
  void expectConstant(std::string_view expression, double expected, double tolerance = 1e-12) {
    expectValues(expression, Values(DOCS, expected), tolerance);
  }

  // f applied to the doc values of `column` (docs a, b, d have v_d; c, e have none).
  void expectUnary(std::string_view expression, double (*f)(double), double tolerance = 1e-12) {
    expectValues(expression, {f(10.0), f(130.0), std::nullopt, f(0.5), std::nullopt}, tolerance);
  }

  void expectError(std::string_view expression, std::string_view expected) {
    try {
      eval(expression);
      FAIL() << "expected a failure for " << expression;
    } catch (const std::exception& error) {
      EXPECT_NE(std::string(error.what()).find(expected), std::string::npos)
          << expression << ": " << error.what();
    }
  }
};

namespace {

// ---------------------------------------------------------------------------
// Reference implementations: the Painless scripts, ported line by line.

constexpr std::string_view BASE32 = "0123456789bcdefghjkmnpqrstuvwxyz";

std::string painlessGeohash(double lat, double lon, size_t length) {
  double minLat = -90, maxLat = 90, minLon = -180, maxLon = 180;
  bool even = true;
  int bit = 0, ch = 0;
  std::string hash;
  while (hash.size() < length) {
    double mid;
    if (even) {
      mid = (minLon + maxLon) / 2;
      if (lon >= mid) { ch |= (16 >> bit); minLon = mid; } else { maxLon = mid; }
    } else {
      mid = (minLat + maxLat) / 2;
      if (lat >= mid) { ch |= (16 >> bit); minLat = mid; } else { maxLat = mid; }
    }
    even = !even;
    if (bit < 4) { bit++; } else { hash += BASE32[(size_t)ch]; bit = 0; ch = 0; }
  }
  return hash;
}

int64_t geohashKey(std::string_view hash) {
  int64_t key = 0;
  for (char c : hash) key = key * 32 + (int64_t)BASE32.find(c);
  return key;
}

struct Tile {
  int zoom, x, y;
};

Tile painlessGeotile(double lat, double lon, int zoom) {
  if (lat > 85.0511287798066) lat = 85.0511287798066;
  if (lat < -85.0511287798066) lat = -85.0511287798066;
  int tiles = 1 << zoom;
  int x = (int)std::floor((lon + 180.0) / 360.0 * tiles);
  if (x < 0) x = 0; else if (x >= tiles) x = tiles - 1;
  double latSin = std::sin(lat * 0.017453292519943295);
  double yTile = (0.5 - (std::log((1.0 + latSin) / (1.0 - latSin)) / (4.0 * std::numbers::pi))) * tiles;
  int y = (int)std::floor(yTile);
  if (y < 0) y = 0; else if (y >= tiles) y = tiles - 1;
  return {zoom, x, y};
}

double javaRound(double x) { return std::floor(x + 0.5); }

std::pair<int, int> painlessGeohex(double lat, double lon) {
  int res = 5;
  double size = 180.0 / std::pow(std::sqrt(7.0), res);
  double q = (2.0 / 3.0 * lon) / size;
  double r = (-1.0 / 3.0 * lon + std::sqrt(3.0) / 3.0 * lat) / size;
  double x = q, zc = r, y = -x - zc;
  int rx = (int)javaRound(x), ry = (int)javaRound(y), rz = (int)javaRound(zc);
  double xd = std::abs(rx - x), yd = std::abs(ry - y), zd = std::abs(rz - zc);
  if (xd > yd && xd > zd) { rx = -ry - rz; } else if (yd > zd) { ry = -rx - rz; } else { rz = -rx - ry; }
  return {rx, rz};
}

// The three scripts as Luxir expressions. Painless emits strings; these return
// one int64 per cell (a geohash is 5 bits per character, a geotile is
// Elasticsearch's (zoom << 58) | (x << 29) | y, a hex cell is rx << 32 | rz).
constexpr std::string_view GEOHASH_SCRIPT = "geohash(loc_p.lat, loc_p.lon, 5)";

constexpr std::string_view GEOTILE_SCRIPT =
    "let(lat, clamp(loc_p.lat, -85.0511287798066, 85.0511287798066), "
    "let(lon, loc_p.lon, let(tiles, 1 << 5, "
    "let(x, clamp(to_int(floor((lon + 180.0) / 360.0 * tiles)), 0, tiles - 1), "
    "let(s, sin(to_radians(lat)), "
    "let(y, clamp(to_int(floor((0.5 - log((1.0 + s) / (1.0 - s)) / (4.0 * pi())) * tiles)), "
    "0, tiles - 1), (5 << 58) | (x << 29) | y))))))";

constexpr std::string_view GEOHEX_SCRIPT =
    "let(lon, loc_p.lon, let(lat, loc_p.lat, "
    "let(size, 180.0 / pow(sqrt(7), 5), "
    "let(q, (2.0 / 3.0 * lon) / size, "
    "let(r, (-1.0 / 3.0 * lon + sqrt(3.0) / 3.0 * lat) / size, "
    "let(s, -q - r, let(rx, round(q), let(ry, round(s), let(rz, round(r), "
    "let(xd, abs(rx - q), let(yd, abs(ry - s), let(zd, abs(rz - r), "
    "let(first, xd > yd && xd > zd, "
    "let(fx, first ? -ry - rz : rx, "
    "let(fz, first || yd > zd ? rz : -rx - ry, "
    "(to_int(fx) << 32) | (to_int(fz) & 4294967295))))))))))))))))";

} // namespace

// ---------------------------------------------------------------------------
// Math

TEST_F(ValueScriptingTest, trigonometryAndLogarithms) {
  expectUnary("sin(v_d)", [](double x) { return std::sin(x); });
  expectUnary("cos(v_d)", [](double x) { return std::cos(x); });
  expectUnary("tan(v_d)", [](double x) { return std::tan(x); });
  expectUnary("atan(v_d)", [](double x) { return std::atan(x); });
  expectUnary("sinh(v_d / 100)", [](double x) { return std::sinh(x / 100); });
  expectUnary("cosh(v_d / 100)", [](double x) { return std::cosh(x / 100); });
  expectUnary("tanh(v_d)", [](double x) { return std::tanh(x); });
  expectUnary("asinh(v_d)", [](double x) { return std::asinh(x); });
  expectUnary("acosh(v_d + 1)", [](double x) { return std::acosh(x + 1); });
  expectUnary("asin(v_d / 130)", [](double x) { return std::asin(x / 130); });
  expectUnary("acos(v_d / 130)", [](double x) { return std::acos(x / 130); });
  expectUnary("atanh(v_d / 131)", [](double x) { return std::atanh(x / 131); });
  expectUnary("log10(v_d)", [](double x) { return std::log10(x); });
  expectUnary("log2(v_d)", [](double x) { return std::log2(x); });
  expectUnary("ln(v_d)", [](double x) { return std::log(x); });
  expectUnary("expm1(v_d / 100)", [](double x) { return std::expm1(x / 100); });
  expectUnary("cbrt(v_d)", [](double x) { return std::cbrt(x); });
  expectUnary("to_radians(v_d)", [](double x) { return x * 0.017453292519943295; });
  expectUnary("to_degrees(v_d)", [](double x) { return x * 57.29577951308232; });
  expectValues("max(sin(vals_ds))", {std::max(std::sin(10.0), std::sin(40.0)), std::sin(70.0),
                                     std::nullopt, std::nullopt, std::nullopt});
  expectConstant("atan2(1, 1)", std::numbers::pi / 4);
  expectConstant("atan2(-1, -1)", -3 * std::numbers::pi / 4);
  expectConstant("hypot(3, 4)", 5);
  expectConstant("pi()", std::numbers::pi);
  expectConstant("e()", std::numbers::e);
  expectConstant("ln(e())", 1);
  expectConstant("log2(8)", 3);
  expectConstant("log10(1000)", 3);
  expectConstant("cbrt(-27)", -3);
  expectValues("hypot(x_i, 4)", {std::hypot(2.0, 4.0), std::hypot(-1.0, 4.0), std::nullopt,
                                 std::hypot(7.0, 4.0), 4.0});
}

TEST_F(ValueScriptingTest, roundingFollowsJava) {
  // Math.round rounds half up: -2.5 -> -2, and 0.49999999999999994 -> 0.
  expectConstant("round(2.5)", 3);
  expectConstant("round(-2.5)", -2);
  expectConstant("round(-0.5)", 0);
  expectConstant("round(0.49999999999999994)", 0);
  expectConstant("round(4503599627370497)", 4503599627370497.0);
  expectConstant("rint(2.5)", 2);
  expectConstant("rint(3.5)", 4);
  expectConstant("ceil(-1.5)", -1);
  expectConstant("ceil(1.2)", 2);
  expectConstant("trunc(-1.9)", -1);
  expectConstant("signum(-3.5)", -1);
  expectConstant("signum(0)", 0);
  expectConstant("signum(8)", 1);
  expectConstant("to_int(3.9)", 3);
  expectConstant("to_int(-3.9)", -3);
  expectConstant("to_double(7) / 2", 3.5);
  EXPECT_EQ(ValueType::INT64, parse("to_int(2.5)")->root().type);
  EXPECT_EQ(ValueType::DOUBLE, parse("round(2)")->root().type);
  EXPECT_EQ(ValueType::INT64, parse("clamp(x_i, 0, 5)")->root().type);
  EXPECT_EQ(ValueType::DOUBLE, parse("clamp(x_i, 0, 5.5)")->root().type);
  expectValues("clamp(x_i, 0, 5)", {2, 0, std::nullopt, 5, 0});
  expectValues("clamp(v_d, 1.5, 20)", {10, 20, std::nullopt, 1.5, std::nullopt});
  expectError("to_int(1e30)", "int64");
}

TEST_F(ValueScriptingTest, mathDomainErrorsFailLikeLog) {
  expectError("asin(2)", "provably produces NaN or infinity");
  expectError("acosh(0.5)", "NaN");
  expectError("atanh(1)", "NaN");
  expectError("asin(v_d)", "NaN");
  expectError("log10(0 * x_i)", "NaN");
  // A missing value is missing, not an error.
  expectValues("asin(v_d / 130)", {std::asin(10.0 / 130), 1.5707963267948966, std::nullopt,
                                   std::asin(0.5 / 130), std::nullopt});
}

// ---------------------------------------------------------------------------
// Bitwise operators and modulo

TEST_F(ValueScriptingTest, bitwiseOperators) {
  expectValues("x_i & 3", {2, 3, std::nullopt, 3, 0});
  expectValues("x_i | 4", {6, -1, std::nullopt, 7, 4});
  expectValues("x_i ^ 3", {1, -4, std::nullopt, 4, 3});
  expectValues("x_i << 3", {16, -8, std::nullopt, 56, 0});
  expectValues("~x_i", {-3, 0, std::nullopt, -8, -1});
  expectConstant("-8 >> 1", -4);
  expectConstant("-8 >>> 60", 15);
  expectConstant("1 << 62", 4611686018427387904.0);
  // The distance is masked to six bits, as in Java: 1 << 65 is 1 << 1.
  expectConstant("1 << 65", 2);
  expectConstant("bit_and(12, 10)", 8);
  expectConstant("bit_or(12, 10)", 14);
  expectConstant("bit_xor(12, 10)", 6);
  expectConstant("bit_not(0)", -1);
  expectConstant("shl(1, 4)", 16);
  expectConstant("shr(-16, 2)", -4);
  expectConstant("ushr(-1, 63)", 1);
}

TEST_F(ValueScriptingTest, operatorPrecedenceFollowsJava) {
  // & binds tighter than ^, which binds tighter than |.
  expectConstant("1 | 2 & 3 ^ 1", 3);
  expectConstant("(1 | 2) & 3 ^ 1", 2);
  // + binds tighter than <<, which binds tighter than a comparison.
  expectConstant("1 + 2 << 1", 6);
  expectConstant("1 << 2 < 5", 1);
  expectConstant("8 >> 1 == 4", 1);
  // Bitwise operators sit below comparisons, as in Java: 2 & (1 == 0).
  expectConstant("2 & 1 == 0", 0);
  expectConstant("(2 & 1) == 0", 1);
  // And above the logical operators; a doubled operator stays logical.
  expectConstant("1 || 0 & 0", 1);
  expectConstant("0 && 1 | 1", 0);
  expectConstant("1 | 0 || 0", 1);
  expectConstant("3 & 1 && 2 & 2", 1);
  expectValues("-x_i + 1 - -1", {0, 3, std::nullopt, -5, 2});
}

TEST_F(ValueScriptingTest, moduloIsJavaRemainder) {
  expectConstant("7 % 3", 1);
  expectConstant("-7 % 3", -1);
  expectConstant("7 % -3", 1);
  expectConstant("7.5 % 2", 1.5);
  expectConstant("-7.5 % 2", -1.5);
  expectConstant("mod(9, 4)", 1);
  expectConstant("10 + 7 % 4 * 2", 16);
  expectValues("x_i % 2", {0, -1, std::nullopt, 1, 0});
  EXPECT_EQ(ValueType::INT64, parse("x_i % 2")->root().type);
  EXPECT_EQ(ValueType::DOUBLE, parse("x_i % 2.0")->root().type);
  // A zero divisor is missing, as for /.
  expectValues("7 % x_i", {1, 0, std::nullopt, 0, std::nullopt});
  expectConstant("-9223372036854775807 % -1", 0);
  EXPECT_EQ(0, evalInt("(0 - 9223372036854775807 - 1) % -1"));
}

TEST_F(ValueScriptingTest, bitwiseOperatorsNeedIntegers) {
  expectError("v_d & 1", "integers");
  expectError("1.5 << 2", "integers");
  expectError("~v_d", "integers");
  expectError("vals_ds | 1", "scalars");
  EXPECT_EQ(ValueType::INT64, parse("x_i & 1")->root().type);
}

// ---------------------------------------------------------------------------
// Column and parameter spellings

TEST_F(ValueScriptingTest, scoreHasTwoSpellings) {
  EXPECT_EQ(eval("score * 2", 1.5f), eval("_score * 2", 1.5f));
  EXPECT_EQ(Values(DOCS, 3.5), eval("_score + 1", 2.5f));
  EXPECT_TRUE(parse("_score")->needsScore);
  EXPECT_EQ(ValueNodeKind::SCORE, parse("_score")->root().kind);
}

TEST_F(ValueScriptingTest, variablesUseDollarNames) {
  api::Val three;
  three.kind = int64_t{3};
  bindVar("w", three);
  expectValues("x_i * $w", {6, -3, std::nullopt, 21, 0});
}

TEST_F(ValueScriptingTest, fieldComponentsFollowTheColumnType) {
  for (const ValueFunction& function : ValueFunctionRegistry::entries()) {
    if (function.component.empty()) continue;
    std::string_view field = function.operands[0].kind == ValueFunction::Kind::DATE ? "when_dt" : "loc_p";
    auto expected = eval(fmt::format("{}({})", function.name, field));
    EXPECT_EQ(expected, eval(fmt::format("{}.{}", field, function.component)));
    EXPECT_EQ(expected, eval(fmt::format("col(\"{}\").{}", field, function.component)));
  }
  EXPECT_EQ(eval("max(geo_lat(locs_p))"), eval("max(locs_p.lat)"));
  for (auto expression : {"x_i.lat", "x_i.year", "x_i.value", "x_i.min", "x_i.size()"}) {
    expectError(expression, "field 'x_i' of type");
    expectError(expression, "has no components");
  }
  for (auto expression : {"loc_p.year", "loc_p.getLat()"}) {
    expectError(expression, "field 'loc_p' of type geo_point");
    expectError(expression, "valid components: lat, lon");
  }
  for (auto expression : {"loc_p.lat.x", "col(\"loc_p\").lat.x"}) {
    expectError(expression, "only one component may follow");
  }
  expectError("when_dt .year", "trailing input");
  expectError("when_dt. year", "immediately follow");
  expectError("col(\"when_dt\") .year", "trailing input");
  for (auto expression : {"when_dt.lat", "when_dt.getYear()", "when_dt.monthOfYear"}) {
    expectError(expression, "field 'when_dt' of type date");
    expectError(expression, "valid components: year, month, day_of_month");
  }
  expectError("let(t, when_dt, t.year)", "expected");
  expectError("max(when_dt, when_dt).year", "trailing input");
  // Parameter accessors are rejected by both expression parsers.
  for (auto spelling : {"params.w", "params['w']"}) {
    expectError(spelling, "Field not found");
    auto req = localReq(helper.getSearchEngine());
    req->collection("main").expr("bad", std::string("sum(x_i) * ") + spelling, "w", int64_t{3});
    req->execute(false);
    EXPECT_FALSE(req->ok());
  }
}

// ---------------------------------------------------------------------------
// Dates

TEST_F(ValueScriptingTest, dateParts) {
  expectValues("year(when_dt)", {2024, 1970, std::nullopt, 2020, 1969});
  expectValues("month(when_dt)", {3, 1, std::nullopt, 12, 12});
  expectValues("day_of_month(when_dt)", {15, 1, std::nullopt, 31, 31});
  expectValues("quarter(when_dt)", {1, 1, std::nullopt, 4, 4});
  // 2024-03-15 is a Friday; the epoch a Thursday; 2020-12-31 a Thursday; 1969-12-31 a Wednesday.
  expectValues("day_of_week(when_dt)", {5, 4, std::nullopt, 4, 3});
  expectValues("day_of_year(when_dt)", {75, 1, std::nullopt, 366, 365});
  // ISO weeks: 2020-12-31 is in week 53, and 1969-12-31 already in week 1 of 1970.
  expectValues("week_of_year(when_dt)", {11, 1, std::nullopt, 53, 1});
  expectValues("hour(when_dt)", {10, 0, std::nullopt, 23, 23});
  expectValues("minute(when_dt)", {20, 0, std::nullopt, 59, 59});
  expectValues("second(when_dt)", {30, 0, std::nullopt, 59, 59});
  expectValues("millis(when_dt)", {456, 0, std::nullopt, 999, 999});
  expectValues("millis_of_day(when_dt)", {37230456, 0, std::nullopt, 86399999, 86399999});
  expectValues("epoch_millis(when_dt)", {(double)T_A, 0, std::nullopt, (double)T_D, -1});
  // A nanosecond field has the same parts, and its epoch milliseconds.
  expectValues("year(stamp)", {2024, std::nullopt, std::nullopt, std::nullopt, std::nullopt});
  expectValues("epoch_millis(stamp)", {(double)T_A, std::nullopt, std::nullopt, std::nullopt, std::nullopt});
  expectValues("millis(stamp)", {456, std::nullopt, std::nullopt, std::nullopt, std::nullopt});
}

TEST_F(ValueScriptingTest, datePartsFloorFractionalTicks) {
  expectValues("year(when_dt + 0.5)", {2024, 1970, std::nullopt, 2020, 1969});
  expectValues("epoch_millis(when_dt + 0.5)", {(double)T_A, 0, std::nullopt, (double)T_D, -1});
  expectValues("epoch_millis(avg(when_dts))", {(double)T_A, std::nullopt, std::nullopt, std::nullopt, -2});
  expectValues("max(year(when_dts + 0.5))", {2024, std::nullopt, std::nullopt, std::nullopt, 1969});
  expectValues("year(def(stamp, -0.5))", {2024, 1969, 1969, 1969, 1969});
  expectValues("epoch_millis(def(stamp, -0.5))", {(double)T_A, -1, -1, -1, -1});
  expectError("year(when_dt + 1e308)", "outside the int64 date range");
  expectError("epoch_millis(when_dt - 1e308)", "outside the int64 date range");
}

TEST_F(ValueScriptingTest, dateOffsetsShiftTheCalendar) {
  expectValues("hour(when_dt, 120)", {12, 2, std::nullopt, 1, 1});
  expectValues("hour(when_dt, -60)", {9, 23, std::nullopt, 22, 22});
  expectValues("day_of_month(when_dt, 120)", {15, 1, std::nullopt, 1, 1});
  expectValues("year(when_dt, 120)", {2024, 1970, std::nullopt, 2021, 1970});
  expectValues("day_of_week(when_dt, -1439)", {4, 3, std::nullopt, 4, 3});
  expectValues("minute(when_dt, 90)", {50, 30, std::nullopt, 29, 29});
  expectError("hour(when_dt, x_i)", "argument 2 must be a constant numeric scalar");
  expectError("hour(when_dt, 1.5)", "whole number of minutes");
  expectError("hour(when_dt, 1440)", "whole number of minutes");
  expectError("hour(x_i)", "not a DATE value");
  expectError("year(v_d)", "not a DATE value");
  expectError("epoch_millis(when_dt, 60)", "expects exactly 1 argument");
}

// ---------------------------------------------------------------------------
// Geo

TEST_F(ValueScriptingTest, geoCoordinates) {
  auto lat = [](double degrees) { return geo::decodeLatitude(geo::encodeLatitude(degrees)); };
  auto lon = [](double degrees) { return geo::decodeLongitude(geo::encodeLongitude(degrees)); };
  expectValues("geo_lat(loc_p)", {lat(40.7128), lat(-33.8688), std::nullopt, lat(-33), std::nullopt});
  expectValues("geo_lon(loc_p)", {lon(-74.006), lon(151.2093), std::nullopt, lon(175), std::nullopt});
  EXPECT_EQ(eval("geo_lat(loc_p)"), eval("loc_p.lat"));
  EXPECT_EQ(eval("geo_lon(loc_p)"), eval("loc_p.lon"));
  EXPECT_EQ(eval("geo_lon(loc_p)"), eval("col(\"loc_p\").lon"));
  // A multi-valued point gives arrays.
  expectValues("max(geo_lat(locs_p))", {lat(51.5), std::nullopt, std::nullopt, std::nullopt, std::nullopt});
  expectValues("min(locs_p.lon)", {lon(-74), std::nullopt, std::nullopt, std::nullopt, std::nullopt});
  expectError("geo_lat(x_i)", "not a GEO_POINT value");
  expectError("loc_p", "GEO_POINT is not numeric");
  expectError("x_i.lat", "has no components");
}

TEST_F(ValueScriptingTest, geoDistancesBetweenPoints) {
  auto lat = [](double degrees) { return geo::decodeLatitude(geo::encodeLatitude(degrees)); };
  auto lon = [](double degrees) { return geo::decodeLongitude(geo::encodeLongitude(degrees)); };
  double toLondon = geo::haversinMeters(lat(40.7128), lon(-74.006), 51.5074, -0.1278);
  auto arcValues = eval("arc_distance(loc_p.lat, loc_p.lon, 51.5074, -0.1278)");
  ASSERT_TRUE(arcValues[0].has_value());
  EXPECT_NEAR(toLondon, *arcValues[0], 1e-6);
  EXPECT_FALSE(arcValues[2].has_value());
  // Lucene expressions' haversin() is in kilometers.
  auto kilometers = eval("haversin(loc_p.lat, loc_p.lon, 51.5074, -0.1278)");
  ASSERT_TRUE(kilometers[0].has_value());
  EXPECT_NEAR(toLondon / 1000.0, *kilometers[0], 1e-9);
  // It agrees with geo_distance, which measures to a constant point.
  auto distances = eval("geo_distance(loc_p, 51.5074, -0.1278)");
  for (size_t i = 0; i < DOCS; i++) {
    auto arc = eval("arc_distance(loc_p.lat, loc_p.lon, 51.5074, -0.1278)")[i];
    ASSERT_EQ(distances[i].has_value(), arc.has_value());
    if (arc) {
      EXPECT_NEAR(*distances[i], *arc, 1e-6);
    }
  }
  // The plane distance is close to the arc over a short distance.
  auto plane = eval("plane_distance(10, 20, 10.01, 20.01)");
  auto arc = eval("arc_distance(10, 20, 10.01, 20.01)");
  EXPECT_NEAR(*arc[0], *plane[0], *arc[0] * 1e-3);
  expectConstant("arc_distance(10, 20, 10, 20)", 0);
  expectConstant("arc_distance(0, 0, 0, 180)", std::numbers::pi * geo::EARTH_MEAN_RADIUS_METERS, 1e-3);
  // Bearings: east, north, south, west.
  expectConstant("geo_bearing(0, 0, 0, 90)", 90);
  expectConstant("geo_bearing(0, 0, 10, 0)", 0);
  expectConstant("geo_bearing(0, 0, -10, 0)", 180);
  expectConstant("geo_bearing(0, 0, 0, -90)", 270);
  auto bearing = eval("geo_bearing(40.7128, -74.006, 51.5074, -0.1278)");
  EXPECT_NEAR(51.2, *bearing[0], 1.5);
}

TEST_F(ValueScriptingTest, geoBoxTestsEncodedCoordinatesAndWrapsTheDateline) {
  expectValues("geo_in_box(loc_p, 40, -75, 41, -73)", {1, 0, std::nullopt, 0, std::nullopt});
  expectValues("geo_in_box(loc_p, -40, 170, -30, -170)", {0, 0, std::nullopt, 1, std::nullopt});
  expectValues("geo_in_box(loc_p, -40, 100, -30, 180)", {0, 1, std::nullopt, 1, std::nullopt});
  // Any value of a multi-valued point.
  expectValues("geo_in_box(locs_p, 50, -1, 52, 1)", {1, std::nullopt, std::nullopt, std::nullopt, std::nullopt});
  expectValues("geo_in_box(locs_p, 0, 0, 1, 1)", {0, std::nullopt, std::nullopt, std::nullopt, std::nullopt});
  expectValues("geo_in_box(loc_p, 40, -75, 41, -73) ? 2.5 : 1", {2.5, 1, std::nullopt, 1, std::nullopt});
  expectError("geo_in_box(loc_p, 41, -75, 40, -73)", "minimum latitude");
  expectError("geo_in_box(loc_p, 0, 0, 91, 0)", "latitude");
  expectError("geo_in_box(loc_p, 0, -181, 1, 0)", "longitude");
  expectError("geo_in_box(loc_p, x_i, 0, 1, 0)", "constant");
}

TEST_F(ValueScriptingTest, geohashMatchesTheStandardAlgorithm) {
  EXPECT_EQ(geohashKey("u4pruydqqvj"), evalInt("geohash(57.64911, 10.40744, 11)"));
  EXPECT_EQ(geohashKey("ezs42"), evalInt("geohash(42.6, -5.6, 5)"));
  EXPECT_EQ(geohashKey(painlessGeohash(0, 0, 12)), evalInt("geohash(0, 0, 12)"));
  std::mt19937_64 random(7);
  std::uniform_real_distribution<double> latitudes(-90, 90), longitudes(-180, 180);
  std::vector<std::pair<std::string, int64_t>> cells;
  for (int i = 0; i < 300; i++) {
    // The literals in the expression carry 12 decimals, so use those values.
    double lat = std::stod(fmt::format("{:.12f}", latitudes(random)));
    double lon = std::stod(fmt::format("{:.12f}", longitudes(random)));
    for (size_t length : {1u, 5u, 9u, 12u}) {
      std::string expression = fmt::format("geohash({:.12f}, {:.12f}, {})", lat, lon, length);
      EXPECT_EQ(geohashKey(painlessGeohash(lat, lon, length)), evalInt(expression)) << expression;
    }
    cells.push_back({painlessGeohash(lat, lon, 5), evalInt(fmt::format("geohash({:.12f}, {:.12f}, 5)", lat, lon))});
  }
  // Keys of one precision sort as the strings do.
  std::vector<std::pair<std::string, int64_t>> byString = cells, byKey = cells;
  std::ranges::sort(byString);
  std::ranges::sort(byKey, {}, &std::pair<std::string, int64_t>::second);
  for (size_t i = 0; i < cells.size(); i++) EXPECT_EQ(byString[i].second, byKey[i].second);
  expectError("geohash(91, 0, 5)", "coordinate");
  expectError("geohash(0, 181, 5)", "coordinate");
  expectError("geohash(0, 0, 13)", "precision must be an integer from 1 to 12");
  expectError("geohash(0, 0, 0)", "precision");
  expectError("geohash(0, 0, x_i)", "constant");
}

TEST_F(ValueScriptingTest, geotileMatchesTheStandardAlgorithm) {
  std::mt19937_64 random(11);
  std::uniform_real_distribution<double> latitudes(-89, 89), longitudes(-180, 180);
  for (int i = 0; i < 300; i++) {
    double lat = std::stod(fmt::format("{:.12f}", latitudes(random)));
    double lon = std::stod(fmt::format("{:.12f}", longitudes(random)));
    for (int zoom : {0, 1, 5, 12, 29}) {
      Tile tile = painlessGeotile(lat, lon, zoom);
      int64_t expected = ((int64_t)tile.zoom << 58) | ((int64_t)tile.x << 29) | tile.y;
      std::string expression = fmt::format("geotile({:.12f}, {:.12f}, {})", lat, lon, zoom);
      EXPECT_EQ(expected, evalInt(expression)) << expression;
    }
  }
  // The edges clamp into the grid.
  for (auto [lat, lon] : {std::pair{90.0, 180.0}, {-90.0, -180.0}, {90.0, -180.0}, {-90.0, 180.0}}) {
    Tile corner = painlessGeotile(lat, lon, 5);
    EXPECT_EQ(((int64_t)5 << 58) | ((int64_t)corner.x << 29) | corner.y,
              evalInt(fmt::format("geotile({}, {}, 5)", lat, lon)));
  }
  EXPECT_EQ(31, painlessGeotile(90, 180, 5).x);
  EXPECT_EQ(31, painlessGeotile(-90, 180, 5).y);
  expectError("geotile(0, 0, 30)", "zoom must be an integer from 0 to 29");
}

// The three Painless grid calculations rewritten as Luxir expressions over loc_p.
TEST_F(ValueScriptingTest, gridScriptsRewrittenAsLuxirExpressions) {
  auto lat = [](double degrees) { return geo::decodeLatitude(geo::encodeLatitude(degrees)); };
  auto lon = [](double degrees) { return geo::decodeLongitude(geo::encodeLongitude(degrees)); };
  struct Point { int32_t doc; double lat, lon; };
  for (Point p : {Point{0, 40.7128, -74.006}, {1, -33.8688, 151.2093}, {3, -33.0, 175.0}}) {
    SCOPED_TRACE(p.doc);
    double la = lat(p.lat), lo = lon(p.lon);
    // The built-in geohash interleaves longitude and latitude bits.
    EXPECT_EQ(geohashKey(painlessGeohash(la, lo, 5)), evalInt(GEOHASH_SCRIPT, p.doc));
    EXPECT_EQ(evalInt("geohash(loc_p.lat, loc_p.lon, 5)", p.doc),
              evalInt(GEOHASH_SCRIPT, p.doc));
    // geotile at zoom 5.
    Tile tile = painlessGeotile(la, lo, 5);
    int64_t expectedTile = ((int64_t)5 << 58) | ((int64_t)tile.x << 29) | tile.y;
    EXPECT_EQ(expectedTile, evalInt(GEOTILE_SCRIPT, p.doc));
    EXPECT_EQ(expectedTile, evalInt("geotile(loc_p.lat, loc_p.lon, 5)", p.doc));
    // geohex at resolution 5.
    auto [rx, rz] = painlessGeohex(la, lo);
    int64_t key = evalInt(GEOHEX_SCRIPT, p.doc);
    EXPECT_EQ(rx, (int32_t)(key >> 32));
    EXPECT_EQ(rz, (int32_t)(key & 0xFFFFFFFF));
  }
  // A document without a point has no cell.
  for (auto script : {GEOHASH_SCRIPT, GEOTILE_SCRIPT, GEOHEX_SCRIPT}) {
    auto values = eval(script);
    EXPECT_TRUE(values[0].has_value());
    EXPECT_FALSE(values[2].has_value());
    EXPECT_FALSE(values[4].has_value());
  }
}

// ---------------------------------------------------------------------------
// Hashing

TEST_F(ValueScriptingTest, hashFunctions) {
  auto bitsOf = [](int64_t v) { return (uint64_t)v; };
  uint64_t plain = 123456;
  EXPECT_EQ((int64_t)XXH64(&plain, 8, 0), evalInt("xxhash64(123456)"));
  uint64_t seeded = bitsOf(-42);
  EXPECT_EQ((int64_t)XXH64(&seeded, 8, 99), evalInt("xxhash64(-42, 99)"));
  EXPECT_EQ((int64_t)Hash::MurmurHash64A(&seeded, 8, 0), evalInt("murmur64(-42)"));
  EXPECT_EQ((int64_t)Hash::MurmurHash64A(&seeded, 8, 5), evalInt("murmur64(-42, 5)"));
  EXPECT_NE(evalInt("xxhash64(1)"), evalInt("xxhash64(2)"));
  EXPECT_NE(evalInt("xxhash64(1)"), evalInt("xxhash64(1, 1)"));
  EXPECT_NE(evalInt("xxhash64(1)"), evalInt("murmur64(1)"));
  // A double hashes by its bits, a column by its value.
  uint64_t bits = std::bit_cast<uint64_t>(10.0);
  EXPECT_EQ((int64_t)XXH64(&bits, 8, 0), evalInt("xxhash64(v_d)", 0));
  EXPECT_EQ(evalInt("xxhash64(2)", 0), evalInt("xxhash64(x_i)", 0));
  EXPECT_FALSE(eval("xxhash64(x_i)")[2].has_value());
  // hash_combine is order dependent.
  uint64_t a = 17, b = 99;
  uint64_t expected = a ^ (b + UINT64_C(0x9e3779b97f4a7c15) + (a << 6) + (a >> 2));
  EXPECT_EQ((int64_t)expected, evalInt("hash_combine(17, 99)"));
  EXPECT_NE(evalInt("hash_combine(17, 99)"), evalInt("hash_combine(99, 17)"));
  expectError("xxhash64(1, x_i)", "constant");
  expectError("xxhash64(1, 1.5)", "integer constant");
  expectError("hash_hash(1)", "unknown value function");
  expectError("hash_combine(v_d, 1)", "integers");
}

TEST_F(ValueScriptingTest, hashBucketsAreStableAndEven) {
  std::vector<int> counts(10);
  for (int i = 0; i < 1000; i++) {
    int64_t bucket = evalInt(fmt::format("hash_bucket({}, 10)", i));
    ASSERT_GE(bucket, 0);
    ASSERT_LT(bucket, 10);
    counts[(size_t)bucket]++;
    EXPECT_EQ(bucket, evalInt(fmt::format("hash_bucket({}, 10)", i)));
  }
  for (int count : counts) {
    EXPECT_GT(count, 60);
    EXPECT_LT(count, 140);
  }
  EXPECT_NE(evalInt("hash_bucket(1, 1000000, 1)"), evalInt("hash_bucket(1, 1000000, 2)"));
  EXPECT_EQ(0, evalInt("hash_bucket(5, 1)"));
  expectValues("hash_bucket(x_i, 4) < 4", {1, 1, std::nullopt, 1, 1});
  auto bounds = parse("hash_bucket(x_i, 4)");
  auto* bound = bounds->bind(pool, reader->segments()[0], bounds->resolveFields(pool, *reader));
  EXPECT_EQ(0, bound->bounds(bounds->rootNode).intMin);
  EXPECT_EQ(3, bound->bounds(bounds->rootNode).intMax);
  expectError("hash_bucket(1, 0)", "positive");
  expectError("hash_bucket(1, x_i)", "constant");
}

// ---------------------------------------------------------------------------
// let

TEST_F(ValueScriptingTest, letBindsAValueOnce) {
  expectValues("let(a, x_i + 1, a * a)", {9, 0, std::nullopt, 64, 1});
  expectValues("let(a, x_i, let(b, a + 1, a * b))", {6, 0, std::nullopt, 56, 0});
  // A variable keeps its type.
  EXPECT_EQ(ValueType::INT64, parse("let(a, 3, a + 1)")->root().type);
  EXPECT_EQ(ValueType::DOUBLE, parse("let(a, 3.5, a + 1)")->root().type);
  EXPECT_EQ(ValueType::DOUBLE, parse("let(a, 3, a / 2)")->root().type);
  // A missing value flows through, and def() recovers it.
  expectValues("let(a, x_i, def(a, 100) + 1)", {3, 0, 101, 8, 1});
  expectValues("let(a, v_d, a * 2)", {20, 260, std::nullopt, 1, std::nullopt});
  // The variable has the bounds of its value, so provable errors still surface.
  expectError("let(a, 0, log(a))", "log()");
  // Arrays are consumed inside the body, where the variable is in scope.
  expectValues("let(k, 2, max(vals_ds * k))", {80, 140, std::nullopt, std::nullopt, std::nullopt});
  expectValues("let(k, x_i, sum(vals_ds) * k)", {100, -70, std::nullopt, std::nullopt, std::nullopt});
  // Dates and points keep their nature.
  expectValues("let(t, when_dt, year(t))", {2024, 1970, std::nullopt, 2020, 1969});
  expectValues("let(p, loc_p, geo_lat(p))", eval("geo_lat(loc_p)"));
  expectValues("let(a, 1, let(a2, 2, a + a2)) + let(a, 10, a)", Values(DOCS, 13));
}

// Every dependency is used twice, so a tree walk would expand exponentially.
static std::string sharedLetChain(std::string_view seed, int count, bool dates = false) {
  std::string expression = fmt::format("let(v0, {}, ", seed);
  for (int i = 1; i <= count; i++) {
    expression += dates ? fmt::format("let(v{}, max(v{}, v{}), ", i, i - 1, i - 1)
        : fmt::format("let(v{}, v{} + v{}, ", i, i - 1, i - 1);
  }
  expression += fmt::format("v{}", count);
  expression.append(count + 1, ')');
  return expression;
}

TEST_F(ValueScriptingTest, sharedLetChainsStayLinear) {
  auto expression = sharedLetChain("def(x_i, 0)", 40);
  expectValues(expression, {2.0 * (1LL << 40), -(double)(1LL << 40), 0,
                            7.0 * (1LL << 40), 0});
  auto program = parse(expression);
  EXPECT_TRUE(program->structurallyEqual(*parse(expression)));
  EXPECT_FALSE(program->structurallyEqual(*parse(sharedLetChain("def(y_i, 0)", 40))));
  auto* bound = program->bind(pool, reader->segments()[0], program->resolveFields(pool, *reader));
  EXPECT_TRUE(bound->firstUnboundedNode().empty());
  EXPECT_NO_THROW(bound->boundsForScore(ValueBounds::floatingRange(0, 10)));
  auto scored = parse(sharedLetChain("score", 40));
  auto* scoredBound = scored->bind(pool, reader->segments()[0], scored->resolveFields(pool, *reader));
  EXPECT_TRUE(scored->needsScore);
  EXPECT_EQ("score", scoredBound->firstUnboundedNode());
  EXPECT_EQ(1.5 * (1LL << 40), scoredBound->evalPoint(0, 1.5f).doubleValue);

  auto dates = sharedLetChain("when_dt", 70, true);
  auto dateProgram = parse(dates);
  EXPECT_EQ("'when_dt' (DATE unit 'ms')", dateProgram->describeDateSources(dateProgram->rootNode));
  auto req = localReq(helper.getSearchEngine());
  auto flags = RequestFlags::fromNames(std::array<std::string_view, 1>{"date_iso"});
  req->rawRequest().flags = flags.flags;
  req->rawRequest().flags_set = flags.flagsSet;
  req->collection("main").expr("date", "min(" + dates + ")");
  req->execute(-1);
  ASSERT_TRUE(req->ok()) << req->errorMsg();
  EXPECT_EQ("1969-12-31T23:59:59.999Z", req->scalar<std::string_view>("date"));
}

TEST_F(ValueScriptingTest, letDepthIncludesSharedDependencies) {
  // The grammar is only 50 calls deep, but the dependency path is over 128.
  std::string expression = "let(v0, x_i, ";
  for (int i = 1; i <= 50; i++) {
    expression += fmt::format("let(v{}, max(max(max(v{}, v{}), v{}), v{}), ",
                              i, i - 1, i - 1, i - 1, i - 1);
  }
  expression += "v50";
  expression.append(51, ')');
  expectError(expression, "nesting exceeds");
  int budget = 5;
  ValueExprOptions options{helper.collection().getSchema().get(), {}, &budget};
  ValueExprParser parser(options, *arena);
  EXPECT_THROW(parser.parse(sharedLetChain("x_i", 6)), std::runtime_error);
  EXPECT_EQ(5, budget);
  EXPECT_NO_THROW(parser.parse(sharedLetChain("x_i", 3)));
  EXPECT_EQ(5, budget);
  budget = 4;
  EXPECT_NO_THROW(parser.parse("let(z, 1+1+1, hour(when_dt, z)+z)"));
  EXPECT_EQ(4, budget);
}

TEST_F(ValueScriptingTest, letConstantsAndLazyValues) {
  auto constant = parse("let(z, 5, z + z)");
  ASSERT_TRUE(constant->constantScalar);
  EXPECT_EQ(10, constant->constantScalar->intValue);
  expectValues("let(z, 2 + 3, geotile(loc_p.lat, loc_p.lon, z))",
               eval("geotile(loc_p.lat, loc_p.lon, 5)"));
  expectConstant("let(unused, log(x_i), 7)", 7);
  expectConstant("let(unused, log(-1), 7)", 7);
  expectConstant("let(a, log(x_i), def(7, a))", 7);
  expectValues("let(a, acos(x_i), if(x_i > 1, 7, a + a))",
               {7, 2 * std::numbers::pi, std::nullopt, 7, std::numbers::pi});
  auto unused = parse("let(a, score, 7)");
  EXPECT_FALSE(unused->needsScore);
  EXPECT_EQ(1u, unused->nodes.size());
}

TEST_F(ValueScriptingTest, sharedValuesUseDocumentAndScoreAcrossEvaluationPaths) {
  auto program = parse("let(s, score + 1, sum(vals_ds * s) + s)");
  auto* bound = program->bind(pool, reader->segments()[0], program->resolveFields(pool, *reader));
  EXPECT_EQ(102, bound->evalPoint(0, 1).doubleValue);
  EXPECT_EQ(153, bound->evalSequentialPoint(0, 2).doubleValue);
  std::array<int32_t, 4> docs{0, 0, 1, 0};
  std::array<float, 4> scores{1, 2, 3, 1};
  std::array<ValueResult, 4> results;
  bound->evalBatch(docs, scores, results);
  for (size_t i = 0; i < results.size(); i++) {
    ASSERT_TRUE(results[i].valid);
    EXPECT_EQ((std::array<double, 4>{102, 153, 284, 102})[i], results[i].doubleValue);
  }
  std::array<ScalarValueResult, 4> scalars;
  bound->evalScalarBatch(docs, scalars);
  for (size_t i = 0; i < scalars.size(); i++) {
    ASSERT_TRUE(scalars[i].valid);
    EXPECT_EQ(docs[i] == 1 ? 71 : 51, scalars[i].doubleValue);
  }
  // This scalar has just one incoming edge, but lazy array elements reuse it.
  expectValues("let(k, x_i + 1, sum(vals_ds * k))", {150, 0, std::nullopt, std::nullopt, std::nullopt});
  auto arrays = parse("vals_ds * let(s, score + 1, s + s)");
  auto* arrayBound = arrays->bind(pool, reader->segments()[0], arrays->resolveFields(pool, *reader));
  arrayBound->evalBatch(docs, scores, results);
  for (size_t i = 0; i < results.size(); i++) {
    EXPECT_EQ((std::array<double, 4>{40, 60, 560, 40})[i],
              arrayBound->evalArrayElement(results[i].array, 0).doubleValue);
  }
  auto zero = parse("let(s, score + score, s + s)");
  auto* zeroBound = zero->bind(pool, reader->segments()[0], zero->resolveFields(pool, *reader));
  EXPECT_FALSE(std::signbit(zeroBound->evalPoint(0, 0.0f).doubleValue));
  EXPECT_TRUE(std::signbit(zeroBound->evalPoint(0, -0.0f).doubleValue));
  EXPECT_TRUE(parse("let(a, x_i, a + a)")->structurallyEqual(*parse("x_i + x_i")));
}

TEST_F(ValueScriptingTest, letScopeAndNameRules) {
  expectError("let(a, 1, a) + a", "Field not found: a");
  expectError("let(a, 1, let(a, 2, a))", "already defined");
  expectError("let(score, 1, score)", "cannot be used as a variable name");
  expectConstant("let(doc, 1, let(params, 2, doc + params))", 3);
  expectError("let(1, 1, 1)", "expected a variable name");
  expectError("let(a, vals_ds, a)", "must be a scalar");
  expectError("let(a, 1, vals_ds)", "must be a scalar");
  expectError("let(a, loc_p, a)", "GEO_POINT is not numeric");
  expectError("let(a, 1)", "expects exactly 3 arguments");
  // A variable shadows a column of the same name.
  expectValues("let(x_i, 5, x_i + 1)", Values(DOCS, 6));
}

TEST_F(ValueScriptingTest, variablesAreStructurallyComparable) {
  auto equal = [&](std::string_view a, std::string_view b) {
    return parse(a)->structurallyEqual(*parse(b));
  };
  EXPECT_TRUE(equal("let(a, x_i, a + 1)", "let(a, x_i, a + 1)"));
  EXPECT_FALSE(equal("let(a, x_i, a + 1)", "let(a, x_i, a + 2)"));
}

// ---------------------------------------------------------------------------
// End to end: rescore, sort and bucket expressions

TEST_F(ValueScriptingTest, rescoreUsesTheNewLanguage) {
  auto ids = [&](std::string_view query) {
    auto req = localReq(helper.getSearchEngine());
    req->collection("main").topDocs("q").exprQuery(query).fields({"id_s"}).getScores().limit(-1);
    req->execute(false);
    EXPECT_TRUE(req->ok()) << req->errorMsg();
    std::vector<std::string> out;
    for (auto id : std::get<api::ColStr>(req->docList()->columns.at("id_s").kind).v) {
      out.emplace_back(id);
    }
    return out;
  };
  using Ids = std::vector<std::string>;
  // Bitwise and remainder operators inside a query function argument:
  // a=8, d=7, c=e=1, b=0.
  auto bits = ids("rescore(body_w:alpha, expr=if(def(x_i, 0) > 1, 10 - (def(x_i, 0) & 3), "
                  "1 + def(x_i, 0) % 2))");
  ASSERT_EQ(5u, bits.size());
  EXPECT_EQ((Ids{"a", "d"}), (Ids{bits[0], bits[1]}));
  EXPECT_EQ("b", bits[4]);
  // _score: d=17, a=7, c=e=3, b=1.
  auto spelled = ids("rescore(body_w:alpha, expr=def(x_i, 0) * 2 + (_score > 0 ? 3 : 0))");
  ASSERT_EQ(5u, spelled.size());
  EXPECT_EQ((Ids{"d", "a"}), (Ids{spelled[0], spelled[1]}));
  EXPECT_EQ("b", spelled[4]);
  // Date parts and a geo box in one score: a=6.012, d=1.01, b=0.985, e=0.9845, c=0.
  EXPECT_EQ((Ids{"a", "d", "b", "e", "c"}),
            ids("rescore(body_w:alpha, expr=def(year(when_dt), 0) / 2000 + "
                "def(geo_in_box(loc_p, 40, -75, 41, -73) ? 5 : 0, 0))"));
}

// The grid scripts are sort keys: a sort clause is a value expression.
TEST_F(ValueScriptingTest, sortsByTheNewFunctions) {
  using Ids = std::vector<std::string>;
  auto asc = api::SortSpec::SortDir::ASC;
  auto desc = api::SortSpec::SortDir::DESC;
  auto sorted = [&](std::string_view expression, api::SortSpec::SortDir direction) {
    auto req = localReq(helper.getSearchEngine());
    auto& cursor = req->collection("main").topDocs("q").exprQuery("body_w:alpha")
        .fields({"id_s"}).limit(-1);
    auto& topDocs = std::get<api::TopDocs>(cursor.rawOp().kind);
    api::SortSpec* spec = api::build::allocArray(topDocs.sort, 1, req->mr);
    spec[0].expr = api::build::arenaStr(req->mr, expression);
    spec[0].dir = direction;
    req->execute(false);
    EXPECT_TRUE(req->ok()) << req->errorMsg();
    Ids out;
    for (auto id : std::get<api::ColStr>(req->docList()->columns.at("id_s").kind).v) {
      out.emplace_back(id);
    }
    return out;
  };
  // The expected order, from reference keys for docs a, b and d (the docs with
  // a point); the others have no key and sort last in either direction.
  auto lat = [](double degrees) { return geo::decodeLatitude(geo::encodeLatitude(degrees)); };
  auto lon = [](double degrees) { return geo::decodeLongitude(geo::encodeLongitude(degrees)); };
  struct Point { std::string id; double lat, lon; };
  std::vector<Point> points{{"a", lat(40.7128), lon(-74.006)}, {"b", lat(-33.8688), lon(151.2093)},
                            {"d", lat(-33.0), lon(175.0)}};
  auto order = [&](auto key, bool descending) {
    std::vector<Point> byKey = points;
    std::ranges::stable_sort(byKey, [&](const Point& x, const Point& y) {
      return descending ? key(x) > key(y) : key(x) < key(y);
    });
    Ids ids;
    for (const Point& p : byKey) ids.push_back(p.id);
    ids.push_back("c");
    ids.push_back("e");
    return ids;
  };
  auto cell = [](const Point& p) { return geohashKey(painlessGeohash(p.lat, p.lon, 5)); };
  EXPECT_EQ(order(cell, false), sorted("geohash(loc_p.lat, loc_p.lon, 5)", asc));
  EXPECT_EQ(order(cell, true), sorted("geohash(loc_p.lat, loc_p.lon, 5)", desc));
  EXPECT_EQ(order(cell, false), sorted(GEOHASH_SCRIPT, asc));
  auto tile = [](const Point& p) {
    Tile t = painlessGeotile(p.lat, p.lon, 5);
    return ((int64_t)t.x << 29) | t.y;
  };
  EXPECT_EQ(order(tile, false), sorted(GEOTILE_SCRIPT, asc));
  auto hex = [](const Point& p) {
    auto [rx, rz] = painlessGeohex(p.lat, p.lon);
    return ((int64_t)rx << 32) | (int64_t)(uint32_t)rz;
  };
  EXPECT_EQ(order(hex, false), sorted(GEOHEX_SCRIPT, asc));
  // Distance from London, nearest first.
  auto distance = [](const Point& p) { return geo::haversinMeters(p.lat, p.lon, 51.5074, -0.1278); };
  EXPECT_EQ(order(distance, false),
            sorted("arc_distance(loc_p.lat, loc_p.lon, 51.5074, -0.1278)", asc));
  EXPECT_EQ(order(distance, false), sorted("geo_distance(loc_p, 51.5074, -0.1278)", asc));
  // Ties keep document order; a missing value sorts last.
  EXPECT_EQ((Ids{"a", "b", "d", "e", "c"}), sorted("day_of_week(when_dt)", desc));
  EXPECT_EQ((Ids{"d", "e", "a", "b", "c"}), sorted("let(h, hour(when_dt), h * h)", desc));
}

TEST_F(ValueScriptingTest, bucketExpressionsTakeTheMathAndBitFunctions) {
  api::Val three;
  three.kind = int64_t{3};
  auto req = localReq(helper.getSearchEngine());
  auto& root = req->collection("main");
  root.expr("mod", "sum(x_i) % 4");
  root.expr("and", "sum(x_i) & 7");
  root.expr("shifted", "(sum(x_i) << 2) | 1");
  root.expr("sine", "sin(sum(x_i))");
  root.expr("hyp", "hypot(sum(x_i), 4)");
  root.expr("rounded", "round(sum(v_d) / 3)");
  root.expr("clamped", "clamp(sum(x_i), 0, 5)");
  root.expr("int", "to_int(sum(v_d))");
  root.expr("tau", "pi() * 2 + sum(x_i) * 0");
  root.expr("weighted", "sum(x_i) * $w", "w", int64_t{3});
  req->execute(-1);
  ASSERT_TRUE(req->ok()) << req->errorMsg();
  // sum(x_i) = 2 - 1 + 7 + 0 = 8; sum(v_d) = 140.5.
  EXPECT_EQ(0, req->scalar<int64_t>("mod"));
  EXPECT_EQ(0, req->scalar<int64_t>("and"));
  EXPECT_EQ(33, req->scalar<int64_t>("shifted"));
  EXPECT_NEAR(std::sin(8.0), req->scalar<double>("sine"), 1e-12);
  EXPECT_NEAR(std::hypot(8.0, 4.0), req->scalar<double>("hyp"), 1e-12);
  EXPECT_NEAR(47, req->scalar<double>("rounded"), 1e-12);
  EXPECT_EQ(5, req->scalar<int64_t>("clamped"));
  EXPECT_EQ(140, req->scalar<int64_t>("int"));
  EXPECT_NEAR(2 * std::numbers::pi, req->scalar<double>("tau"), 1e-12);
  EXPECT_EQ(24, req->scalar<int64_t>("weighted"));
}

TEST_F(ValueScriptingTest, perDocumentOnlyFunctionsAreRejectedOnBuckets) {
  // Inside an aggregate they are ordinary per-document functions.
  auto req = localReq(helper.getSearchEngine());
  auto& root = req->collection("main");
  root.expr("years", "sum(year(when_dt))");
  root.expr("cells", "sum(geohash(1, 2, 3) % 7)");
  root.expr("hashes", "sum(hash_bucket(x_i, 1))");
  req->execute(-1);
  ASSERT_TRUE(req->ok()) << req->errorMsg();
  EXPECT_EQ(2024 + 1970 + 2020 + 1969, req->scalar<int64_t>("years"));
  EXPECT_EQ(0, req->scalar<int64_t>("hashes"));
  // Applied to a bucket value they are rejected.
  for (auto expression : {"year(sum(x_i))", "geohash(sum(x_i), 0, 3)", "xxhash64(sum(x_i))"}) {
    SCOPED_TRACE(expression);
    auto bad = localReq(helper.getSearchEngine());
    bad->collection("main").expr("bad", expression);
    bad->execute(false);
    EXPECT_FALSE(bad->ok());
    EXPECT_NE(std::string(bad->errorMsg()).find("per-document value function"), std::string::npos)
        << bad->errorMsg();
  }
}

TEST_F(ValueScriptingTest, geoBoxesMatchTheActualQueryAtEncodedEdges) {
  CollectionHelper boxes("box_function_test");
  SchemaBuilder schema;
  auto& loc = schema.field("loc_p");
  loc.type = api::FieldDef::FieldClass::GEO_POINT;
  loc.index = api::FieldDef::IndexMode::RANGE;
  loc.column = true;
  loc.multi = api::FieldDef::MultiMode::DEFAULT;
  schema.set(boxes.collection());
  std::vector<std::string> points{"40.7128,-74.006", "-33,175", "-90,-180", "90,180",
                                 "0,-180", "0,179.99", "0,-179.99", "0,0", "0,0.00000001"};
  for (size_t i = 0; i < points.size(); i++) {
    boxes.index(flatdoc("id_s", std::to_string(i), "loc_p", points[i]), UpdateMessage::NO_COMMIT);
  }
  boxes.index(flatdoc("id_s", "9", "loc_p", vecs("0,175", "0,-175")), UpdateMessage::COMMIT);
  auto r = boxes.getIndexWriter()->snapshots.readers.getReader();
  for (auto box : {geo::BoundingBox{40, 41, -74.006, -74.006}, {-1, 1, 1e-8, 2e-8},
                   {90, 90, -180, 180}, {-90, 90, 180, 180}, {-90, 90, 180, -170},
                   {-90, 90, 170, -170}, {-90, 90, -180, 180}, {1e-8, 2e-8, -180, 180},
                   {0, 0, 0, 0}, {-90, 90, 179.999999999, 180}}) {
    auto req = localReq(boxes.getSearchEngine());
    auto& top = req->collection("box_function_test").topDocs("q").fields({"id_s"}).limit(-1);
    top.rawQuery() = qb::geoBox(top.mr(), "loc_p", box.minLat, box.maxLat, box.minLon, box.maxLon);
    req->execute(false);
    ASSERT_TRUE(req->ok()) << req->errorMsg();
    std::array<bool, 10> expected{};
    for (const auto& doc : req->getDocs()) {
      expected[(size_t)std::stoi(std::get<std::string>(*find(doc, "id_s")))] = true;
    }
    std::string expression = fmt::format("geo_in_box(loc_p, {:.17g}, {:.17g}, {:.17g}, {:.17g})",
                                         box.minLat, box.minLon, box.maxLat, box.maxLon);
    ValueExprOptions options{boxes.collection().getSchema().get(), {}};
    auto* program = ValueExprParser(options, *arena).parse(expression);
    auto* bound = program->bind(pool, r->segments()[0], program->resolveFields(pool, *r));
    for (int32_t doc = 0; doc < 10; doc++) {
      EXPECT_EQ(expected[doc], bound->evalPoint(doc, 0).intValue != 0) << expression << " doc " << doc;
    }
  }
}

TEST_F(ValueScriptingTest, possibleGeoBoundsDoNotProveDomainErrors) {
  for (auto [expression, doc] : {std::pair{"sqrt(geo_lat(loc_p))", 0},
                                 {"log(geo_lon(loc_p))", 1}, {"log1p(geo_lat(loc_p))", 0},
                                 {"log(geo_in_box(loc_p, 40, -75, 41, -73))", 0}}) {
    auto* program = parse(expression);
    auto* bound = program->bind(pool, reader->segments()[0], program->resolveFields(pool, *reader));
    EXPECT_TRUE(bound->evalPoint(doc, 0).valid) << expression;
  }
}

TEST_F(ValueScriptingTest, distanceFunctionsValidateConstantCoordinates) {
  for (auto function : {"arc_distance", "haversin", "plane_distance", "geo_bearing"}) {
    for (auto args : {"91, 0, 0, 0", "0, 181, 0, 0", "0, 0, -91, 0", "0, 0, 0, -181"}) {
      EXPECT_THROW(parse(fmt::format("{}({})", function, args)), std::runtime_error);
    }
  }
}

TEST_F(ValueScriptingTest, hashIntegerConstantsKeepAll64Bits) {
  uint64_t bits = 1;
  for (int64_t seed : {INT64_C(9007199254740993), INT64_MAX, INT64_MIN}) {
    EXPECT_EQ((int64_t)XXH64(&bits, sizeof bits, (uint64_t)seed),
              evalInt(fmt::format("xxhash64(1, {})", seed)));
    EXPECT_EQ((int64_t)Hash::MurmurHash64A(&bits, sizeof bits, (uint64_t)seed),
              evalInt(fmt::format("murmur64(1, {})", seed)));
  }
  EXPECT_EQ((int64_t)(XXH64(&bits, sizeof bits, 0) % (uint64_t)INT64_MAX),
            evalInt("hash_bucket(1, 9223372036854775807)"));
  EXPECT_EQ((int64_t)(XXH64(&bits, sizeof bits, 0) % UINT64_C(9007199254740993)),
            evalInt("hash_bucket(1, 9007199254740993)"));
  expectError("xxhash64(1, 9223372036854775808.0)", "integer constant");
  expectConstant("1 << 9007199254740993", 2);
}

TEST_F(ValueScriptingTest, successfulPureSubtreesBecomeLiterals) {
  std::array<double, 2> weights{2, 3};
  bindVar("weights", api::Val{api::ArrDouble{weights}});
  for (auto expression : {"pi()/180", "1 << 5", "pow(sqrt(7), 5)", "if(0, log(-1), 7)",
                          "def(1/0, 7)", "if(0, score, 7)", "def(7, score)", "0 && score", "1 || score",
                          "sum($weights)", "count($weights)"}) {
    auto* program = parse(expression);
    EXPECT_EQ(ValueNodeKind::CONSTANT, program->root().kind) << expression;
    EXPECT_EQ(1, program->nodes.size()) << expression;
  }
  EXPECT_EQ(ValueNodeKind::CONSTANT, parse("sqrt($weights)")->root().kind);
  expectConstant("count($weights/0)", 0);
  expectConstant("sum(sqrt($weights))", std::sqrt(2) + std::sqrt(3));
  for (auto expression : {"log(-1)", "random()", "floor(random())", "if(x_i, log(-1), 7)"}) {
    EXPECT_EQ(ValueNodeKind::FUNCTION, parse(expression)->root().kind) << expression;
  }
  expectValues("let(unused, log(-1), x_i)", {2, -1, std::nullopt, 7, 0});
  expectConstant("if(0, log(-1), 7)", 7);
  expectConstant("def(7, log(-1))", 7);
  auto* date = parse("when_dt + (pi()/pi()+0.5)");
  EXPECT_EQ(ValueNature::DATE_MS, date->root().nature);
  EXPECT_EQ(ValueNodeKind::CONSTANT, date->nodes[date->root().children[1]].kind);
  expectValues("epoch_millis(when_dt + (pi()/pi()+0.5))",
               {(double)T_A + 1, 1, std::nullopt, (double)T_D + 1, 0});
  auto* missing = parse("1/0");
  EXPECT_EQ(ValueNodeKind::MISSING, missing->root().kind);
  EXPECT_FALSE(missing->constantScalar);
  EXPECT_TRUE(parse("pow(sqrt(7), 5)")->structurallyEqual(*parse("pow(sqrt(7), 5)")));
}

TEST_F(ValueScriptingTest, constantArraysKeepOnlyTheMaterializedResult) {
  std::array<double, 32> weights;
  weights.fill(0.5);
  bindVar("weights", api::Val{api::ArrDouble{weights}});
  auto nested = [](std::string expression) {
    for (int i = 0; i < 40; i++) expression = "sin(" + expression + ")";
    return expression;
  };
  auto* folded = parse(nested("$weights"));
  EXPECT_EQ(ValueNodeKind::CONSTANT, folded->root().kind);
  EXPECT_EQ(2 * weights.size(), folded->doubleArrays.size());
  weights.back() = -1;
  auto* failed = parse(nested("log($weights)"));
  EXPECT_EQ(ValueNodeKind::FUNCTION, failed->root().kind);
  EXPECT_EQ(weights.size(), failed->doubleArrays.size());
}

TEST_F(ValueScriptingTest, failedConstantDependenciesStayLazyThroughLetChains) {
  std::string expression = "let(v0, log(-1), ";
  for (int i = 1; i <= 70; i++) expression += fmt::format("let(v{}, v{}+v{}, ", i, i - 1, i - 1);
  expression += "if(0, v70, 7)";
  expression.append(71, ')');
  auto* program = parse(expression);
  ASSERT_TRUE(program->constantScalar);
  EXPECT_DOUBLE_EQ(7, program->constantScalar->doubleValue);
  EXPECT_EQ(1, program->nodes.size());
}
