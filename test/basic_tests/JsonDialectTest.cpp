// Golden-wire tests for the Solux JSON dialect (src/solux/api/json_dialect.h): literal
// JSON text in/out, pinning the surface as a contract rather than a self-round-trip.
// Val is untagged (a raw JSON value): reads dispatch on the token, writes render the arm
// bare. Map is a plain object; Vector is a bare number array.
#include <gtest/gtest.h>

#include <cstddef>
#include <memory_resource>
#include <string>
#include <variant>

#include "solux/api/solux_types.hpp"
#include "solux/api/build.h"

namespace {
using namespace std::string_view_literals;
namespace P = solux::api;
namespace B = solux::api::build;

P::Val readVal(std::string_view json, std::pmr::memory_resource& mr) {
  P::Val v;
  EXPECT_TRUE(P::read_json(v, json, mr)) << json;
  return v;
}

std::string writeVal(const P::Val& v) {
  std::string out;
  EXPECT_TRUE(P::write_json(v, out));
  return out;
}

TEST(JsonDialect, ValReadsUntagged) {
  std::pmr::monotonic_buffer_resource mr;

  EXPECT_EQ(std::get<std::string_view>(readVal(R"("x")", mr).kind), "x");
  EXPECT_EQ(std::get<std::int64_t>(readVal("3", mr).kind), 3);
  EXPECT_EQ(std::get<std::int64_t>(readVal("9223372036854775807", mr).kind), INT64_MAX);
  EXPECT_EQ(std::get<double>(readVal("3.5", mr).kind), 3.5);
  EXPECT_EQ(std::get<double>(readVal("-1e3", mr).kind), -1000.0);
  EXPECT_EQ(std::get<bool>(readVal("true", mr).kind), true);
  EXPECT_TRUE(readVal("null", mr).isNull());

  // homogeneous arrays collapse to the typed arm; ints promote among doubles
  { auto v = readVal("[1,2]", mr);   ASSERT_TRUE(std::holds_alternative<P::ArrInt>(v.kind));
    EXPECT_EQ(std::get<P::ArrInt>(v.kind).v[1], 2); }
  { auto v = readVal("[1,2.5]", mr); ASSERT_TRUE(std::holds_alternative<P::ArrDouble>(v.kind));
    EXPECT_EQ(std::get<P::ArrDouble>(v.kind).v[0], 1.0); }
  { auto v = readVal(R"(["a","b"])", mr); ASSERT_TRUE(std::holds_alternative<P::ArrStr>(v.kind));
    EXPECT_EQ(std::get<P::ArrStr>(v.kind).v[1], "b"sv); }
  // mixed, empty, and nested arrays stay generic
  EXPECT_TRUE(std::holds_alternative<P::ArrVal>(readVal(R"([1,"a"])", mr).kind));
  EXPECT_TRUE(std::holds_alternative<P::ArrVal>(readVal("[]", mr).kind));
  { auto v = readVal("[[1],[2,3]]", mr);
    const auto& a = std::get<P::ArrVal>(v.kind);
    ASSERT_EQ(a.v.size(), 2u);
    EXPECT_EQ(std::get<P::ArrInt>(a.v[1].kind).v[1], 3); }

  // objects are Map (no "fields" wrapper level)
  { auto v = readVal(R"({"a":1,"b":"x"})", mr);
    const auto& m = std::get<P::Map>(v.kind);
    ASSERT_EQ(m.fields.size(), 2u);
    EXPECT_EQ(std::get<std::int64_t>((**m.fields.find("a")).kind), 1);
    EXPECT_EQ(std::get<std::string_view>((**m.fields.find("b")).kind), "x"); }
}

TEST(JsonDialect, ValWritesUntagged) {
  std::pmr::monotonic_buffer_resource mr;
  P::Val v;

  v.kind.emplace<float>(1.5f);
  EXPECT_EQ(writeVal(v), "1.5");

  static constexpr std::byte bin[] = {std::byte{1}, std::byte{2}};
  v.kind.emplace<::hpp_proto::bytes_view>(B::arenaBytes(mr, bin));
  EXPECT_EQ(writeVal(v), R"("AQI=")");  // bytes render as base64 strings

  v.kind.emplace<std::monostate>();
  EXPECT_EQ(writeVal(v), "null");

  auto& vec = v.kind.emplace<P::Vector>();
  EXPECT_EQ(writeVal(v), "null");  // no encoding set
  float* f = B::allocArray(vec.f32.emplace().v, 2, mr);
  f[0] = 0.5f; f[1] = 1.5f;
  EXPECT_EQ(writeVal(v), "[0.5,1.5]");  // Vector is a bare number array
}

TEST(JsonDialect, VectorReadsBareArray) {
  std::pmr::monotonic_buffer_resource mr;
  P::Vector v;
  ASSERT_TRUE(P::read_json(v, "[0.5,1.5]", mr));
  ASSERT_TRUE(v.f32.has_value());
  ASSERT_EQ(v.f32->v.size(), 2u);
  EXPECT_EQ(v.f32->v[1], 1.5f);
  ASSERT_TRUE(P::read_json(v, "null", mr));
  EXPECT_FALSE(v.f32.has_value());
}

TEST(JsonDialect, ValInMessageContext) {
  std::pmr::monotonic_buffer_resource mr;
  P::Match m;
  ASSERT_TRUE(P::read_json(m, R"({"field":"title","val":"dune"})", mr));
  EXPECT_EQ(m.field, "title");
  ASSERT_TRUE(m.val.has_value());
  EXPECT_EQ(std::get<std::string_view>(m.val->kind), "dune");
}

TEST(JsonDialect, MatchSugar) {
  std::pmr::monotonic_buffer_resource mr;
  P::Match m;
  ASSERT_TRUE(P::read_json(m, R"({"title_w":"dune"})", mr));
  EXPECT_EQ(m.field, "title_w");
  EXPECT_EQ(std::get<std::string_view>(m.val->kind), "dune");

  m = {};
  ASSERT_TRUE(P::read_json(m, R"({"price_i":42,"operator":"AND"})", mr));
  EXPECT_EQ(m.field, "price_i");
  EXPECT_EQ(std::get<std::int64_t>(m.val->kind), 42);
  EXPECT_EQ(m.operator_, P::Match::Operator::AND);

  // two unknown keys / sugar mixed with explicit field or val are rejected
  EXPECT_FALSE(P::read_json(m, R"({"a":"x","b":"y"})", mr));
  EXPECT_FALSE(P::read_json(m, R"({"field":"a","b":"y"})", mr));
  std::string err;
  EXPECT_FALSE(P::read_json(m, R"({"title_w":"x","val":"y"})", mr, &err));
  EXPECT_FALSE(err.empty());
}

TEST(JsonDialect, RangeQuery) {
  std::pmr::monotonic_buffer_resource mr;
  P::Query q;
  ASSERT_TRUE(P::read_json(q, R"({"range":{"field":"year_i","gte":1960,"lt":1970}})", mr));
  const auto& r = std::get<P::RangeQuery>(q.kind);
  EXPECT_EQ(r.field, "year_i");
  ASSERT_TRUE(r.gte.has_value());
  EXPECT_EQ(std::get<std::int64_t>(r.gte->kind), 1960);
  EXPECT_FALSE(r.gt.has_value());
  ASSERT_TRUE(r.lt.has_value());
  EXPECT_EQ(std::get<std::int64_t>(r.lt->kind), 1970);
  EXPECT_FALSE(r.lte.has_value());
}

TEST(JsonDialect, DepthLimitErrorsCleanly) {
  std::pmr::monotonic_buffer_resource mr;
  std::string deep(300, '[');
  deep += "1";
  deep.append(300, ']');
  P::Val v;
  EXPECT_FALSE(P::read_json(v, deep, mr));
}

}  // namespace
