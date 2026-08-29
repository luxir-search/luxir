// Golden-wire tests for the Luxir JSON dialect (src/luxir/api/json_dialect.h): literal
// JSON text in/out, pinning the surface as a contract rather than a self-round-trip.
// Val is untagged (a raw JSON value): reads dispatch on the token, writes render the arm
// bare. Map is a plain object; Vector is a bare number array.
#include <gtest/gtest.h>

#include <cstddef>
#include <memory_resource>
#include <string>
#include <variant>

#include "luxir/api/luxir_types.hpp"
#include "luxir/api/build.h"

namespace {
using namespace std::string_view_literals;
namespace P = luxir::api;
namespace B = luxir::api::build;

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

TEST(JsonDialect, SearchRequestUnifiedRoot) {
  std::pmr::monotonic_buffer_resource mr;

  // full form: ops binds to the request
  { P::SearchRequest r;
    ASSERT_TRUE(P::read_json(r, R"({"ops":{"a":{"top_docs":{"limit":3}}},"time_zone":"UTC"})", mr));
    ASSERT_EQ(1u, r.ops.size());
    EXPECT_EQ(3, *std::get<P::TopDocs>((**r.ops.find("a")).kind).limit);
    EXPECT_EQ("UTC", r.time_zone); }

  // shorthand: root TopDocs keys register as ops["q"]; request-level keys mix in
  { P::SearchRequest r;
    ASSERT_TRUE(P::read_json(r,
        R"({"query":"title_w:dune","limit":5,"max_parallel":-1,"time_zone":"UTC","get_number":true})", mr));
    EXPECT_EQ(-1, r.max_parallel);
    EXPECT_EQ("UTC", r.time_zone);
    ASSERT_EQ(1u, r.ops.size());
    const auto& td = std::get<P::TopDocs>((**r.ops.find("q")).kind);
    EXPECT_EQ(5, *td.limit);
    EXPECT_TRUE(td.get_number);
    ASSERT_TRUE(td.query.has_value());
    EXPECT_EQ("title_w:dune", std::get<P::ExprQuery>(td.query->kind).q); }

  // shorthand + "ops": ops are the shorthand op's SUB-ops (facets under the query)
  { P::SearchRequest r;
    ASSERT_TRUE(P::read_json(r,
        R"({"ops":{"cats":{"field_facet":{"field":"cat_s"}}},"query":"title_w:dune"})", mr));
    ASSERT_EQ(1u, r.ops.size());
    const auto& td = std::get<P::TopDocs>((**r.ops.find("q")).kind);
    ASSERT_EQ(1u, td.ops.size());
    EXPECT_EQ("cat_s", std::get<P::FieldFacet>((**td.ops.find("cats")).kind).field); }

  // unknown root key stays a strict error in every form
  { P::SearchRequest r;
    EXPECT_FALSE(P::read_json(r, R"({"query":"title_w:dune","limt":10})", mr)); }
  { P::SearchRequest r;
    EXPECT_FALSE(P::read_json(r, R"({"ops":{},"bogus":1})", mr)); }
}

TEST(JsonDialect, SearchRequestSecondPassOverlay) {
  std::pmr::monotonic_buffer_resource mr;
  P::SearchRequest r;
  ASSERT_TRUE(P::read_json(r, R"({
    "request_id":"body",
    "ops":{
      "q":{"top_docs":{
        "query":"title_w:body",
        "limit":20,
        "fields":["body_a","body_b"],
        "sorts":[{"field":"old_i","dir":"asc"}]
      }},
      "cats":{"field_facet":{"field":"cat_s"}}
    }
  })", mr));

  ASSERT_TRUE(P::merge_json(r, R"({
    "request_id":"url",
    "limit":3,
    "fields":["url_a"],
    "sorts":[{"field":"new_i","dir":"desc"}]
  })", mr));

  EXPECT_EQ("url", r.request_id);
  ASSERT_EQ(2u, r.ops.size());
  EXPECT_EQ("cat_s", std::get<P::FieldFacet>((**r.ops.find("cats")).kind).field);
  const auto& td = std::get<P::TopDocs>((**r.ops.find("q")).kind);
  EXPECT_EQ(3, *td.limit);
  ASSERT_TRUE(td.query.has_value());
  EXPECT_EQ("title_w:body", std::get<P::ExprQuery>(td.query->kind).q);
  ASSERT_EQ(1u, td.fields.size());
  EXPECT_EQ("url_a", td.fields[0]);
  ASSERT_EQ(1u, td.sorts.size());
  EXPECT_EQ("new_i", td.sorts[0].expr);
  EXPECT_EQ(P::SortSpec::SortDir::DESC, td.sorts[0].dir);
}

TEST(JsonDialect, SearchRequestShorthandOpsNamedQStaysSubOp) {
  std::pmr::monotonic_buffer_resource mr;
  P::SearchRequest r;
  ASSERT_TRUE(P::read_json(r, R"({
    "ops":{"q":{"field_facet":{"field":"cat_s"}}},
    "query":"title_w:dune"
  })", mr));

  ASSERT_EQ(1u, r.ops.size());
  const auto& td = std::get<P::TopDocs>((**r.ops.find("q")).kind);
  ASSERT_EQ(1u, td.ops.size());
  EXPECT_EQ("cat_s", std::get<P::FieldFacet>((**td.ops.find("q")).kind).field);
}

TEST(JsonDialect, FacetSelectionsUseLowercaseEnumNames) {
  std::pmr::monotonic_buffer_resource mr;
  P::SearchOp field;
  ASSERT_TRUE(P::read_json(field, R"({"field_facet":{"field":"tag_ss",
    "selected":["x","y"],"selection_mode":"all"}})", mr));
  const auto& fieldFacet = std::get<P::FieldFacet>(field.kind);
  ASSERT_EQ(2u, fieldFacet.selected.size());
  EXPECT_EQ("x", fieldFacet.selected[0].asString());
  EXPECT_EQ(P::SelectionMode::ALL, fieldFacet.selection_mode);
  std::string encoded;
  ASSERT_TRUE(P::write_json(field, encoded));
  EXPECT_TRUE(encoded.contains("\"selected\":[\"x\",\"y\"]"));
  EXPECT_NE(std::string::npos, encoded.find(R"("selection_mode":"all")"));

  P::SearchOp range;
  ASSERT_TRUE(P::read_json(range, R"({"range_facet":{"field":"price_i",
    "start":0,"end":20,"gap":10,"selected":[10]}})", mr));
  const auto& rangeFacet = std::get<P::RangeFacet>(range.kind);
  ASSERT_EQ(1u, rangeFacet.selected.size());
  EXPECT_EQ(10, rangeFacet.selected[0].asInt());
  EXPECT_EQ(P::SelectionMode::ANY, rangeFacet.selection_mode);
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
  ASSERT_TRUE(P::read_json(m, R"({"price_i":42,"operator":"and"})", mr));
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

TEST(JsonDialect, InQueryCanonicalForm) {
  std::pmr::monotonic_buffer_resource mr;
  P::Query q;
  ASSERT_TRUE(P::read_json(
      q, R"({"in":{"field":"brand_s","values":["acme","globex"]}})", mr));
  const auto& in = std::get<P::InQuery>(q.kind);
  EXPECT_EQ("brand_s", in.field);
  ASSERT_EQ(2u, in.values.size());
  EXPECT_EQ("globex", std::get<std::string_view>(in.values[1].kind));

  std::string canonical;
  ASSERT_TRUE(P::write_json(q, canonical));
  EXPECT_EQ(
      R"({"in":{"field":"brand_s","values":["acme","globex"]}})",
      canonical);
}

TEST(JsonDialect, ExistsQuery) {
  std::pmr::monotonic_buffer_resource mr;
  P::Query q;
  ASSERT_TRUE(P::read_json(q, R"({"exists":{"field":"body_w"}})", mr));
  EXPECT_EQ("body_w", std::get<P::ExistsQuery>(q.kind).field);

  std::string canonical;
  ASSERT_TRUE(P::write_json(q, canonical));
  EXPECT_EQ(R"({"exists":{"field":"body_w"}})", canonical);

  // Tag 4 used to be a bare field arm. The old object spelling is not an arm.
  P::Query old;
  EXPECT_FALSE(P::read_json(old, R"({"field":"body_w"})", mr));
}

TEST(JsonDialect, QueryBareStringIsExprSugar) {
  std::pmr::monotonic_buffer_resource mr;
  P::Query q;
  ASSERT_TRUE(P::read_json(q, R"("status:active AND year_i:>=1960")", mr));
  const auto& e = std::get<P::ExprQuery>(q.kind);
  EXPECT_EQ(e.q, "status:active AND year_i:>=1960");

  // the sugar composes anywhere a query object goes, e.g. TopDocs.query
  P::TopDocs td;
  ASSERT_TRUE(P::read_json(td, R"({"query":"tag_s:scifi","limit":5})", mr));
  ASSERT_TRUE(td.query.has_value());
  EXPECT_EQ(std::get<P::ExprQuery>(td.query->kind).q, "tag_s:scifi");

  // canonical object arms still read through the same dispatch
  P::Query m;
  ASSERT_TRUE(P::read_json(m, R"({"match":{"title_w":"dune"}})", mr));
  EXPECT_EQ(std::get<P::Match>(m.kind).field, "title_w");
  P::Query bad;
  EXPECT_FALSE(P::read_json(bad, R"({"not_an_arm":1})", mr));

  // a Query object IS the oneof: a second arm is an error, not last-wins
  P::Query two;
  EXPECT_FALSE(P::read_json(two, R"({"match":{"title_w":"dune"},"all":true})", mr));
}

TEST(JsonDialect, FilterBareAndRoutedForms) {
  std::pmr::monotonic_buffer_resource mr;

  P::TopDocs td;
  ASSERT_TRUE(P::read_json(
      td,
      R"({"filter":["brand_s:acme",{"match":{"status_s":"active"}},{"query":"price_i:<1000","except_ops":["brands"]}]})",
      mr));
  ASSERT_EQ(3u, td.filter.size());
  ASSERT_TRUE(td.filter[0].query.has_value());
  EXPECT_EQ("brand_s:acme",
            std::get<P::ExprQuery>(td.filter[0].query->kind).q);
  EXPECT_TRUE(td.filter[0].except_ops.empty());
  ASSERT_TRUE(td.filter[1].query.has_value());
  EXPECT_EQ("status_s",
            std::get<P::Match>(td.filter[1].query->kind).field);
  EXPECT_TRUE(td.filter[1].except_ops.empty());
  ASSERT_TRUE(td.filter[2].query.has_value());
  ASSERT_EQ(1u, td.filter[2].except_ops.size());
  EXPECT_EQ("brands", td.filter[2].except_ops[0]);
}

TEST(JsonDialect, FilterRoutingWrapperIsStrict) {
  std::pmr::monotonic_buffer_resource mr;
  std::string err;

  P::TopDocs extra;
  EXPECT_FALSE(P::read_json(
      extra,
      R"({"filter":[{"query":"brand_s:acme","except_ops":["brands"],"extra":true}]})",
      mr, &err));
  EXPECT_NE(std::string::npos, err.find("filter[0]")) << err;
  EXPECT_NE(std::string::npos, err.find("extra")) << err;

  P::TopDocs empty;
  EXPECT_FALSE(P::read_json(
      empty,
      R"({"filter":[{"query":"brand_s:acme","except_ops":[]}]})",
      mr, &err));
  EXPECT_NE(std::string::npos, err.find("filter[0]")) << err;
  EXPECT_NE(std::string::npos, err.find("bare filter form")) << err;
}

TEST(JsonDialect, QueryBoostSiblingSugarAndArm) {
  std::pmr::monotonic_buffer_resource mr;
  P::Query flat;
  ASSERT_TRUE(P::read_json(
      flat, R"({"match":{"title_w":"dune"},"boost":2})", mr));
  ASSERT_TRUE(std::holds_alternative<P::BoostQuery>(flat.kind));
  const auto& lifted = std::get<P::BoostQuery>(flat.kind);
  ASSERT_TRUE(lifted.query.has_value());
  ASSERT_TRUE(lifted.boost.has_value());
  EXPECT_FLOAT_EQ(2.0f, *lifted.boost);
  EXPECT_EQ("title_w", std::get<P::Match>(lifted.query->kind).field);

  std::string canonical;
  ASSERT_TRUE(P::write_json(flat, canonical));
  EXPECT_EQ(canonical,
            R"({"boost":{"query":{"match":{"field":"title_w","val":"dune"}},"boost":2}})");

  P::Query siblingFirst;
  ASSERT_TRUE(P::read_json(
      siblingFirst, R"({"boost":2,"match":{"title_w":"dune"}})", mr));
  EXPECT_TRUE(std::holds_alternative<P::BoostQuery>(siblingFirst.kind));

  P::Query arm;
  ASSERT_TRUE(P::read_json(
      arm,
      R"({"boost":{"query":{"match":{"title_w":"dune"}},"boost":2}})",
      mr));
  ASSERT_TRUE(std::holds_alternative<P::BoostQuery>(arm.kind));
  const auto& explicitArm = std::get<P::BoostQuery>(arm.kind);
  ASSERT_TRUE(explicitArm.query.has_value());
  EXPECT_FLOAT_EQ(2.0f, *explicitArm.boost);

  P::Query bad;
  EXPECT_FALSE(P::read_json(bad, R"({"boost":2})", mr));
  EXPECT_FALSE(P::read_json(
      bad, R"({"matc":{"title_w":"dune"},"boost":2})", mr));
}

TEST(JsonDialect, ExprQueryStringAndObjectForms) {
  std::pmr::monotonic_buffer_resource mr;
  P::Query q;
  // bare string = q-only sugar on the arm itself
  ASSERT_TRUE(P::read_json(q, R"({"expr":"status:active"})", mr));
  EXPECT_EQ(std::get<P::ExprQuery>(q.kind).q, "status:active");

  // object form carries vars; values are untagged Vals
  P::Query qv;
  ASSERT_TRUE(P::read_json(qv, R"({"expr":{"q":"count_i:$n","vars":{"n":42}}})", mr));
  const auto& e = std::get<P::ExprQuery>(qv.kind);
  EXPECT_EQ(e.q, "count_i:$n");
  const auto* n = e.vars.find("n");
  ASSERT_NE(nullptr, n);
  EXPECT_EQ(std::get<std::int64_t>((*n)->kind), 42);

  // writes stay canonical: the structured object, not the string sugar
  std::string out;
  ASSERT_TRUE(P::write_json(q, out));
  EXPECT_EQ(out, R"({"expr":{"q":"status:active"}})");
}

TEST(JsonDialect, ExprOpStringAndObjectForms) {
  std::pmr::monotonic_buffer_resource mr;

  P::SearchOp bare;
  ASSERT_TRUE(P::read_json(bare, R"json("avg(price_i)")json", mr));
  ASSERT_TRUE(std::holds_alternative<P::ExprOp>(bare.kind));
  EXPECT_EQ("avg(price_i)", std::get<P::ExprOp>(bare.kind).expr);

  P::SearchOp arm;
  ASSERT_TRUE(P::read_json(
      arm, R"json({"expr_op":"sum(price_i) / $scale"})json", mr));
  ASSERT_TRUE(std::holds_alternative<P::ExprOp>(arm.kind));
  EXPECT_EQ("sum(price_i) / $scale", std::get<P::ExprOp>(arm.kind).expr);

  P::SearchOp object;
  ASSERT_TRUE(P::read_json(
      object,
      R"json({"expr_op":{"expr":"sum(price_i) * $scale","vars":{"scale":2}}})json",
      mr));
  const auto& expr = std::get<P::ExprOp>(object.kind);
  const auto* scale = expr.vars.find("scale");
  ASSERT_NE(nullptr, scale);
  EXPECT_EQ(2, (**scale).asInt());

  P::SearchRequest request;
  ASSERT_TRUE(P::read_json(
      request, R"json({"ops":{"metric":"avg(price_i)"}})json", mr));
  ASSERT_NE(nullptr, request.ops.find("metric"));
  EXPECT_EQ("avg(price_i)",
            std::get<P::ExprOp>((**request.ops.find("metric")).kind).expr);

  std::string out;
  ASSERT_TRUE(P::write_json(bare, out));
  EXPECT_EQ(R"json({"expr_op":{"expr":"avg(price_i)"}})json", out);
}

TEST(JsonDialect, SortSpecFieldAlias) {
  std::pmr::monotonic_buffer_resource mr;
  P::SortSpec sort;
  ASSERT_TRUE(P::read_json(sort, R"({"field":"price_i","dir":"asc"})", mr));
  EXPECT_EQ("price_i", sort.expr);
  EXPECT_EQ(P::SortSpec::SortDir::ASC, sort.dir);

  // The public output remains the expression-based canonical form.
  std::string out;
  ASSERT_TRUE(P::write_json(sort, out));
  EXPECT_EQ(R"({"expr":"price_i","dir":"asc"})", out);

  P::SortSpec longAsc, longDesc;
  EXPECT_FALSE(P::read_json(longAsc, R"({"expr":"price_i","dir":"ascending"})", mr));
  EXPECT_FALSE(P::read_json(longDesc, R"({"expr":"price_i","dir":"descending"})", mr));
}

TEST(JsonDialect, DepthLimitErrorsCleanly) {
  std::pmr::monotonic_buffer_resource mr;
  std::string deep(300, '[');
  deep += "1";
  deep.append(300, ']');
  P::Val v;
  EXPECT_FALSE(P::read_json(v, deep, mr));
}

// ---- schema (SchemaDef / FieldDef) golden wire text ----

TEST(JsonDialect, SchemaDefGoldenWire) {
  // The full friendly shape: name-keyed maps, lowercase enums, flattened
  // vector params, sparse presence. Literal text pins the public contract.
  constexpr std::string_view wire =
      R"({"fields":{"title":{"type":"text","analyzer":{"tokenizer":"unicode_word","filters":["nfkc_cf","fold"]},"stored":true},"year":{"type":"int","index":"range"},"vec":{"type":"vector","dims":4,"metric":"cosine"}},"templates":{"_x":{"type":"string","multi":true}}})";

  std::pmr::monotonic_buffer_resource mr;
  P::SchemaDef def;
  std::string err;
  ASSERT_TRUE(P::read_json(def, wire, mr, &err)) << err;

  const P::FieldDef* title = def.fields.find("title");
  ASSERT_NE(nullptr, title);
  EXPECT_EQ(P::FieldDef::FieldClass::TEXT, *title->type);
  EXPECT_EQ("unicode_word", title->analyzer->tokenizer);
  EXPECT_TRUE(*title->stored);
  const P::FieldDef* vec = def.fields.find("vec");
  ASSERT_NE(nullptr, vec);
  EXPECT_EQ(4, *vec->dims);
  EXPECT_EQ(P::VectorMetric::COSINE, *vec->metric);
  EXPECT_FALSE(vec->stored.has_value()) << "sparse presence preserved";
  const P::FieldDef* tmpl = def.templates.find("_x");
  ASSERT_NE(nullptr, tmpl);
  EXPECT_TRUE(*tmpl->multi);

  // write(read(x)) is text-identity: the canonical form round-trips exactly.
  std::string out;
  ASSERT_TRUE(P::write_json(def, out));
  EXPECT_EQ(wire, out);
}

TEST(JsonDialect, FieldDefStringShorthand) {
  // {"year": "int"} == {"year": {"type": "int"}}; writes stay canonical.
  std::pmr::monotonic_buffer_resource mr;
  P::SchemaDef def;
  ASSERT_TRUE(P::read_json(def, R"({"fields":{"year":"int","tag":"string"}})", mr));
  const P::FieldDef* year = def.fields.find("year");
  ASSERT_NE(nullptr, year);
  EXPECT_EQ(P::FieldDef::FieldClass::INT, *year->type);
  EXPECT_EQ(P::FieldDef::FieldClass::STRING, *def.fields.find("tag")->type);

  std::string out;
  ASSERT_TRUE(P::write_json(def, out));
  EXPECT_EQ(R"({"fields":{"year":{"type":"int"},"tag":{"type":"string"}}})", out);
}

TEST(JsonDialect, FieldDefStrictReads) {
  std::pmr::monotonic_buffer_resource mr;
  {  // unknown key -> error, not silent ignore
    P::SchemaDef def;
    EXPECT_FALSE(P::read_json(def, R"({"fields":{"x":{"typ":"int"}}})", mr));
  }
  {  // enum names are lowercase-exact; the old uppercase spelling is rejected
    P::SchemaDef def;
    EXPECT_FALSE(P::read_json(def, R"({"fields":{"x":{"type":"INT"}}})", mr));
  }
  {  // parent + inherited type reads fine with no type at all
    P::SchemaDef def;
    ASSERT_TRUE(P::read_json(def, R"({"fields":{"t":{"parent":"_un"}}})", mr));
    EXPECT_FALSE(def.fields.find("t")->type.has_value());
    EXPECT_EQ("_un", def.fields.find("t")->parent);
  }
}

TEST(JsonDialect, UnknownKeyIsNamed) {
  std::pmr::monotonic_buffer_resource mr;
  std::string err;
  {  // dialect reader: the error names the key and points at it, not at its value
    P::SearchRequest r;
    EXPECT_FALSE(P::read_json(r, R"({"query": {"match": {"title_w": "x"}}, "feilds": ["id"]})", mr, &err));
    EXPECT_EQ(0u, err.find(R"(1:40: unknown_key "feilds")")) << err;
  }
  {  // generated reader: same shape
    P::SearchRequest r;
    EXPECT_FALSE(P::read_json(r, R"({"ops":{"a":{"top_docs":{"quary":{}}}}})", mr, &err));
    EXPECT_EQ(0u, err.find(R"(1:26: unknown_key "quary")")) << err;
  }
  {  // a key refused by a dialect rule (val after sugar) is named too
    P::Match m;
    EXPECT_FALSE(P::read_json(m, R"({"title_w":"x","val":"y"})", mr, &err));
    EXPECT_EQ(0u, err.find(R"(1:16: unknown_key "val")")) << err;
  }
}

}  // namespace
