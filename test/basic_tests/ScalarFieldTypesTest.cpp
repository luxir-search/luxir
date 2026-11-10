// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include "test/LuxirTest.h"
#include "test/CollectionHelper.h"
#include "test/LocalReq.h"
#include "test/SchemaBuilder.h"
#include "test/TestUtils.h"
#include "luxir/schema/ValCoerce.h"
#include "luxir/search/IndexReader.h"
#include "luxir/server/JsonRequest.h"
#include "luxir/server/JsonResponse.h"
#include "luxir/util/Decimal.h"
#include "luxir/util/IpAddress.h"
#include "luxir/value/ValueExprParser.h"
#include "luxir/api/RequestFlags.h"
#include "luxir/schema/FieldSignature.h"

using namespace luxir;
using namespace luxir::test;
using namespace std::literals;
using FieldClass = api::FieldDef::FieldClass;

class ScalarFieldTypesTest : public LuxirTest {
protected:
  CollectionHelper helper{"main"};

  void SetUp() override {
    LuxirTest::SetUp();
    SchemaBuilder b;
    b.field("flag").type = FieldClass::BOOLEAN;
    b.set(helper.collection());
    ASSERT_TRUE(helper.indexAll({
      flatdoc("id", "a", "flag", true, "tags_bs", std::vector<bool>{true, false, true}),
      flatdoc("id", "b", "flag", false, "tags_bs", vecs("false")),
      flatdoc("id", "c", "flag", "true"), flatdoc("id", "d"),
    }, UpdateMessage::COMMIT).success);
  }

  LocalReqHandle run(std::string_view json) {
    auto req = localReq(helper.getSearchEngine());
    parseQueryRequest(api::build::arenaStr(req->mr, json), req->rawRequest(), req->mr);
    req->collection("main").execute(false);
    return req;
  }

  IndexResult indexJson(std::string_view json) {
    CollectionHelper::UpdateBuilder update;
    update.addRaw([json](api::Map& doc, std::pmr::memory_resource& mr) {
      if (!api::read_json(doc, json, mr)) throw std::runtime_error("bad test document");
    });
    update.commit();
    return helper.submit(update);
  }

  std::vector<std::string> hits(std::string_view query) {
    auto req = run(std::format(R"({{"query":{},"fields":["id"],"limit":-1}})", query));
    EXPECT_TRUE(req->ok()) << req->errorMsg();
    std::vector<std::string> out;
    for (const auto& doc : req->getDocs()) out.push_back(std::get<std::string>(*find(doc, "id")));
    std::sort(out.begin(), out.end());
    return out;
  }
};

TEST_F(ScalarFieldTypesTest, booleanQueries) {
  EXPECT_EQ(vecs("a", "c"), hits(R"({"match":{"flag":true}})"));
  EXPECT_EQ(vecs("b"), hits(R"({"match":{"flag":"false"}})"));
  EXPECT_EQ(vecs("a", "c"), hits(R"("flag:true")"));
  EXPECT_EQ(vecs("b"), hits(R"({"expr":"flag:false"})"));
  EXPECT_EQ(vecs("a", "c"), hits(R"({"simple_query":{"q":"flag:true","fields":["id"]}})"));
  EXPECT_EQ(vecs("a", "b", "c"), hits(R"({"any_of":{"field":"flag","values":[true,"false"]}})"));
  EXPECT_EQ(vecs("a", "b", "c"), hits(R"({"range":{"field":"flag","gte":false,"lte":true}})"));
  EXPECT_EQ(vecs("a", "c"), hits(R"({"range":{"field":"flag","gt":false}})"));
  EXPECT_EQ(vecs("b"), hits(R"({"range":{"field":"flag","lt":true}})"));
  EXPECT_EQ(vecs(), hits(R"({"range":{"field":"flag","gt":true}})"));
  EXPECT_EQ(vecs(), hits(R"({"range":{"field":"flag","lt":false}})"));
  EXPECT_EQ(vecs("a"), hits(R"({"match":{"tags_bs":true}})"));
  EXPECT_EQ(vecs("a", "b"), hits(R"({"match":{"tags_bs":false}})"));
  EXPECT_EQ(vecs("d"), hits(R"("NOT flag:*")"));
}

TEST_F(ScalarFieldTypesTest, booleanInvalidValues) {
  BoolFieldType type("boolean");
  for (const auto& value : {coerce::scalarVal(0), coerce::scalarVal(1), coerce::scalarVal(1.0),
                           coerce::scalarVal("yes"sv), coerce::scalarVal("TRUE"sv)}) {
    EXPECT_FALSE(type.tryCoerceColInt64(value, "flag").has_value());
    EXPECT_THROW(type.coerceIntWindow(value, FieldType::LiteralUse::MATCH, "flag"), RequestError);
  }
  for (auto value : {"0", "1", "0.0", "\"yes\"", "\"TRUE\""}) {
    SCOPED_TRACE(value);
    EXPECT_FALSE(indexJson(std::format(R"({{"id":"bad","flag":{}}})", value)).success);
    for (auto query : {std::format(R"({{"match":{{"flag":{}}}}})", value),
                       std::format(R"({{"any_of":{{"field":"tags_bs","values":[true,{}]}}}})", value),
                       std::format(R"({{"range":{{"field":"flag","gt":true,"lt":{}}}}})", value)}) {
      EXPECT_FALSE(run(std::format(R"({{"query":{}}})", query))->ok());
    }
  }
  EXPECT_FALSE(helper.index(flatdoc("id", "bad", "tags_bs", vec_i(0, 1)), UpdateMessage::COMMIT).success);
  EXPECT_FALSE(indexJson(R"({"id":"bad","tags_bs":[true,"yes"]})").success);
  EXPECT_TRUE(helper.index(flatdoc("id", "good", "tags_bs", false), UpdateMessage::COMMIT).success);
  EXPECT_EQ(vecs("a"), hits(R"({"match":{"tags_bs":true}})"));
  EXPECT_TRUE(run(R"({"query":{"simple_query":{"q":"flag:dune","fields":["id"]}}})")->ok());
}

TEST_F(ScalarFieldTypesTest, booleanOutputAndFacets) {
  auto req = run(R"({"sort":"id","fields":["id","flag","tags_bs"],"limit":-1,
    "ops":{"flags":{"field_facet":{"field":"flag"}},
           "tags":{"field_facet":{"field":"tags_bs"}}}})");
  ASSERT_OK(req);
  EXPECT_CONTAINS_DOC(req->getDocs(), flatdoc("id", "a", "flag", true, "tags_bs", std::vector<bool>{false, true}));
  EXPECT_CONTAINS_DOC(req->getDocs(), flatdoc("id", "b", "flag", false, "tags_bs", std::vector<bool>{false}));
  const auto& flags = *req->docList()->ops.at("flags")->facetResult();
  const auto& ids = std::get<api::ColVal>(flags.bucket_ids->kind).v;
  ASSERT_EQ(2u, ids.size());
  EXPECT_TRUE(std::get<bool>(ids[0].kind));
  EXPECT_FALSE(std::get<bool>(ids[1].kind));
  EXPECT_EQ(2, flags.counts[0]);
  EXPECT_EQ(1, flags.counts[1]);
  const auto& tagCounts = req->docList()->ops.at("tags")->facetResult()->counts;
  EXPECT_EQ((std::vector<int64_t>{2, 1}), std::vector<int64_t>(tagCounts.begin(), tagCounts.end()));
  auto body = renderSearchResponseBody(req->responses[0]->proto);
  EXPECT_NE(std::string::npos, body.find(R"("flag":false)")) << body;
  EXPECT_NE(std::string::npos, body.find(R"("tags_bs":[false,true])")) << body;
  EXPECT_NE(std::string::npos, body.find(R"("val":false,"count":1)")) << body;
  auto runs = renderDocRuns(req->responses[0]->proto);
  ASSERT_EQ(1u, runs.size());
  EXPECT_NE(std::string::npos, runs[0].body.find(R"("flag":false)"));
  EXPECT_NE(std::string::npos, runs[0].body.find(R"("tags_bs":[false,true])"));
  EXPECT_TRUE(std::get<api::ColVal>(req->docList()->columns.at("flag").kind).v[3].isNull());
  auto selected = run(R"({"limit":0,"ops":{"f":{"field_facet":{"field":"flag","selected":[false],"limit":0}}}})");
  ASSERT_OK(selected);
  const auto& facet = *selected->docList()->ops.at("f")->facetResult();
  ASSERT_EQ(1u, facet.counts.size());
  EXPECT_FALSE(std::get<bool>(std::get<api::ColVal>(facet.bucket_ids->kind).v[0].kind));
  EXPECT_EQ(1, facet.counts[0]);
  auto bad = run(R"({"ops":{"r":{"range_facet":{"field":"flag","start":false,"end":true,"gap":1}}}})");
  EXPECT_FALSE(bad->ok());
  EXPECT_NE(std::string::npos, bad->errorMsg().find("range_facet is not supported on boolean field 'flag'"));
}

TEST_F(ScalarFieldTypesTest, booleanSortCollapseAndWire) {
  ASSERT_TRUE(helper.index(flatdoc("id", "e", "flag", false), UpdateMessage::COMMIT).success);
  auto req = run(R"({"sort":["flag asc","id asc"],"fields":["id","flag"],"limit":-1,"get_scores":true,
    "collapse":{"field":"flag","missing":"group","head_sort":"id desc"}})");
  ASSERT_OK(req);
  EXPECT_CONTAINS_DOC(req->getDocs(), flatdoc("id", "e", "flag", false));
  ASSERT_EQ(3, req->docList()->row_count);
  const auto& keys = req->docList()->groups->keys;
  EXPECT_EQ(api::GroupComponent_::Type::BOOLEAN, keys[0].component[0].type);
  EXPECT_FALSE(std::get<bool>(keys[0].component[0].value));
  EXPECT_TRUE(std::get<bool>(keys[1].component[0].value));
  EXPECT_TRUE(std::holds_alternative<google::protobuf::NullValue>(keys[2].component[0].value));
  const auto& ranks = req->docList()->groups->ranks;
  ASSERT_EQ(3u, ranks.size());
  EXPECT_FALSE(std::get<bool>(ranks[0].v[0].kind));
  EXPECT_TRUE(std::get<bool>(ranks[1].v[0].kind));
  EXPECT_NE(std::string::npos, renderSearchResponseBody(req->responses[0]->proto).find(R"("_group_":{"val":false)"));
  std::vector<std::byte> binary;
  ASSERT_TRUE(api::encode(req->responses[0]->proto, binary));
  auto size = binary.size();
  binary.resize(size + 16);
  std::pmr::monotonic_buffer_resource arena;
  api::SearchResponse decoded;
  ASSERT_TRUE(api::decode(decoded, std::span(binary).first(size), arena));
  const auto* list = decoded.ops.at("q")->docList();
  EXPECT_FALSE(std::get<bool>(list->groups->keys[0].component[0].value));
  EXPECT_FALSE(std::get<bool>(std::get<api::ColVal>(list->columns.at("flag").kind).v[0].kind));
}

TEST_F(ScalarFieldTypesTest, booleanValueNatures) {
  auto req = run(R"json({"ops":{
    "lo":{"expr_op":{"expr":"min(flag)"}}, "hi":{"expr_op":{"expr":"max(flag)"}},
    "sum":{"expr_op":{"expr":"sum(flag)"}}, "avg":{"expr_op":{"expr":"avg(flag)"}},
    "arithmetic":{"expr_op":{"expr":"max(flag + flag)"}},
    "choice":{"expr_op":{"expr":"max(if(flag, flag, flag))"}},
    "default":{"expr_op":{"expr":"max(def(flag, flag))"}},
    "reduce":{"expr_op":{"expr":"max(min(tags_bs))"}},
    "array_avg":{"expr_op":{"expr":"max(avg(tags_bs))"}},
    "array_sum":{"expr_op":{"expr":"sum(sum(tags_bs))"}},
    "mixed":{"expr_op":{"expr":"max(if(flag, flag, 7))"}}
  }})json");
  ASSERT_OK(req);
  const auto& ops = req->responses[0]->proto.ops;
  EXPECT_FALSE(std::get<bool>(ops.at("lo")->kind));
  for (auto name : {"hi", "choice", "default"}) EXPECT_TRUE(std::get<bool>(ops.at(name)->kind));
  EXPECT_FALSE(std::get<bool>(ops.at("reduce")->kind));
  EXPECT_EQ(2, std::get<int64_t>(ops.at("sum")->kind));
  EXPECT_DOUBLE_EQ(2.0 / 3, std::get<double>(ops.at("avg")->kind));
  EXPECT_EQ(2, std::get<int64_t>(ops.at("arithmetic")->kind));
  EXPECT_DOUBLE_EQ(0.5, std::get<double>(ops.at("array_avg")->kind));
  EXPECT_EQ(1, std::get<int64_t>(ops.at("array_sum")->kind));
  EXPECT_EQ(7, std::get<int64_t>(ops.at("mixed")->kind));
}

TEST_F(ScalarFieldTypesTest, booleanModesAcrossFlushDeleteAndMerge) {
  for (auto multi : {api::FieldDef::MultiMode::SINGLE, api::FieldDef::MultiMode::SET, api::FieldDef::MultiMode::ARRAY}) {
    SCOPED_TRACE((int)multi);
    helper.clear();
    SchemaBuilder b;
    for (auto [name, mode] : {std::pair{"n", api::FieldDef::IndexMode::NONE},
                             {"r", api::FieldDef::IndexMode::RANGE}, {"m", api::FieldDef::IndexMode::MATCH}}) {
      auto& f = b.field(name);
      f.type = FieldClass::BOOLEAN;
      f.index = mode;
      f.multi = multi;
    }
    b.set(helper.collection());
    auto writer = helper.getIndexWriter();
    writer->mergePolicy->setMergeFactor(100);
    for (auto id : {"a", "b", "c", "d"}) {
      Doc doc = flatdoc("id", id);
      if (std::string_view(id) != "d") for (auto name : {"n", "r", "m"}) {
        if (multi == api::FieldDef::MultiMode::SINGLE) doc.push_back({name, std::string_view(id) != "b"});
        else doc.push_back({name, std::vector<bool>{std::string_view(id) != "b", false, std::string_view(id) != "b"}});
      }
      ASSERT_TRUE(helper.index(doc, UpdateMessage::COMMIT).success);
    }
    auto check = [&](bool deleted) {
      for (auto name : {"n", "r", "m"}) {
        for (auto query : {std::format(R"({{"match":{{"{}":true}}}})", name),
                           std::format(R"({{"any_of":{{"field":"{}","values":[true]}}}})", name),
                           std::format(R"({{"range":{{"field":"{}","gt":false,"lte":true}}}})", name)}) {
          EXPECT_EQ(deleted ? vecs("a") : vecs("a", "c"), hits(query));
        }
        EXPECT_EQ(multi == api::FieldDef::MultiMode::SINGLE ? vecs("b") : (deleted ? vecs("a", "b") : vecs("a", "b", "c")),
                  hits(std::format(R"({{"match":{{"{}":false}}}})", name)));
      }
      auto output = run(R"({"query":{"match":{"id":"a"}},"fields":["n"],"limit":-1})");
      ASSERT_OK(output);
      if (multi == api::FieldDef::MultiMode::SINGLE) {
        EXPECT_CONTAINS_DOC(output->getDocs(), flatdoc("n", true));
      } else {
        EXPECT_CONTAINS_DOC(output->getDocs(), flatdoc("n", multi == api::FieldDef::MultiMode::SET
            ? std::vector<bool>{false, true} : std::vector<bool>{true, false, true}));
      }
    };
    check(false);
    ASSERT_TRUE(helper.deleteById("c", UpdateMessage::COMMIT).success);
    check(true);
    writer->mergeSegments();
    ASSERT_EQ(1u, writer->snapshots.readers.getReader()->segments().size());
    check(true);
  }
}

TEST_F(ScalarFieldTypesTest, booleanPersistenceAndTypeChanges) {
  std::pmr::monotonic_buffer_resource arena;
  api::SchemaDef definition;
  ASSERT_TRUE(api::read_json(definition, R"({"fields":{"flag":"boolean"}})", arena));
  auto schema = Schema::fromProto(definition, helper.collection().getSchema().get());
  EXPECT_EQ(FieldType::BOOLEAN, schema->getFieldTypeEx("flag")->type());
  auto writer = helper.getIndexWriter();
  IndexReader reopened(writer->dir);
  EXPECT_EQ(FieldType::BOOLEAN, reopened.field("flag").at(0)->type);
  EXPECT_EQ(FieldType::BOOLEAN, reopened.schema()->getFieldTypeEx("flag")->type());
  writer->mergePolicy->setMergeFactor(100);
  SchemaBuilder removed;
  removed.replaceAll(helper.collection());
  SchemaBuilder changed;
  changed.field("flag").type = FieldClass::INT;
  changed.set(helper.collection());
  IndexReader mismatch(writer->dir);
  EXPECT_THROW(mismatch.field("flag"), RequestError);
  ASSERT_TRUE(helper.index(flatdoc("id", "new", "flag", 1), UpdateMessage::COMMIT).success);
  ExpectLog quiet("Incompatible persisted types for field 'flag'");
  CollectionHelper::UpdateBuilder merge;
  merge.commit(true, 1);
  auto result = helper.submit(merge);
  EXPECT_FALSE(result.success);
  EXPECT_NE(std::string::npos, result.error_message.find("type 'boolean'"));
  EXPECT_NE(std::string::npos, result.error_message.find("type 'int'"));
}

TEST(DecimalCoercionTest, rounding) {
  using R = coerce::DecimalRounding;
  struct Case { const char* text; int scale; int64_t nearest, floor, ceil; };
  for (const auto& c : {Case{"1.005", 2, 101, 100, 101}, {"-1.005", 2, -101, -101, -100},
      {"9.5", 0, 10, 9, 10}, {"-9.5", 0, -10, -10, -9}, {"1.2345e2", 2, 12345, 12345, 12345},
      {"5e-19", 18, 1, 0, 1}, {"-5E-19", 18, -1, -1, 0},
      {"9.223372036854775807", 18, INT64_MAX, INT64_MAX, INT64_MAX},
      {"-9.223372036854775808", 18, INT64_MIN, INT64_MIN, INT64_MIN}}) {
    SCOPED_TRACE(c.text);
    coerce::DecimalError error;
    EXPECT_EQ(c.nearest, coerce::parseScaledDecimal(c.text, c.scale, R::NEAREST_AWAY, error));
    EXPECT_EQ(c.floor, coerce::parseScaledDecimal(c.text, c.scale, R::FLOOR, error));
    EXPECT_EQ(c.ceil, coerce::parseScaledDecimal(c.text, c.scale, R::CEIL, error));
  }
  coerce::DecimalError error;
  for (auto text : {"9223372036854775807.5", "-9223372036854775808.5", "1e9999", "1e", ".", "nan", "1.2.3"}) {
    EXPECT_FALSE(coerce::parseScaledDecimal(text, 0, R::NEAREST_AWAY, error).has_value()) << text;
  }
  EXPECT_EQ(INT64_MAX, coerce::parseScaledDecimal("9223372036854775807.4", 0, R::NEAREST_AWAY, error));
  EXPECT_EQ(INT64_MIN, coerce::parseScaledDecimal("-9223372036854775808.4", 0, R::NEAREST_AWAY, error));
  EXPECT_FALSE(coerce::parseScaledDecimal("9223372036854775807.1", 0, R::CEIL, error));
  EXPECT_FALSE(coerce::parseScaledDecimal("-9223372036854775808.1", 0, R::FLOOR, error));
  std::string longFraction = "1.005" + std::string(10000, '0') + "1";
  EXPECT_EQ(101, coerce::parseScaledDecimal(longFraction, 2, R::NEAREST_AWAY, error));
  EXPECT_EQ(100, coerce::parseScaledDecimal(longFraction, 2, R::FLOOR, error));
  EXPECT_FALSE(coerce::parseScaledDecimal(longFraction, 2, R::EXACT, error));
  EXPECT_EQ(1, coerce::parseScaledDecimal("1" + std::string(20000, '0') + "e-20000", 0, R::EXACT, error));
  EXPECT_EQ(0, coerce::parseScaledDecimal("1e-99999", 18, R::NEAREST_AWAY, error));
  EXPECT_EQ(1, coerce::parseScaledDecimal("1e-99999", 18, R::CEIL, error));
  for (auto [text, expected] : {std::pair{"1e30", coerce::DecimalError::ABOVE_RANGE},
      {"-1e30", coerce::DecimalError::BELOW_RANGE}, {"9223372036854775807.5", coerce::DecimalError::ABOVE_RANGE},
      {"-9223372036854775808.5", coerce::DecimalError::BELOW_RANGE}, {"1e30oops", coerce::DecimalError::INVALID}}) {
    EXPECT_FALSE(coerce::parseScaledDecimal(text, 0, R::NEAREST_AWAY, error));
    EXPECT_EQ(expected, error);
  }
}

TEST(DecimalCoercionTest, decodeMatchesDivision) {
  uint64_t bits = 99817;
  for (int scale = 0; scale <= 18; scale++) {
    const auto& decode = DECIMAL_SCALES[scale];
    for (int64_t raw : {INT64_MIN, INT64_MAX, (int64_t)-3, (int64_t)0, (int64_t)3}) {
      EXPECT_DOUBLE_EQ((double)raw / decode.factor, decode(raw));
    }
    for (int i = 0; i < 10000; i++) {
      bits ^= bits << 13; bits ^= bits >> 7; bits ^= bits << 17;
      int64_t raw = (int64_t)bits;
      ASSERT_EQ((double)raw / decode.factor, decode(raw)) << scale << ": " << raw;
    }
  }
}

class DecimalFieldTypesTest : public ScalarFieldTypesTest {
protected:
  void SetUp() override {
    LuxirTest::SetUp();
    SchemaBuilder b;
    for (auto name : {"price", "prices", "other"}) {
      auto& f = b.field(name);
      f.type = FieldClass::DECIMAL;
      f.scale = name == "other"sv ? 3 : 2;
      if (name == "prices"sv) f.multi = api::FieldDef::MultiMode::ARRAY;
    }
    b.set(helper.collection());
    ASSERT_TRUE(helper.indexAll({
      flatdoc("id", "a", "price", 1.005, "other", "1.005", "prices", vecs("1.005", "-1.005", "2")),
      flatdoc("id", "b", "price", 10), flatdoc("id", "c", "price", "10.006"),
      flatdoc("id", "d", "price", "-10.005"), flatdoc("id", "e")}, UpdateMessage::COMMIT).success);
  }
};

TEST_F(DecimalFieldTypesTest, literalsAndIntegerBounds) {
  EXPECT_EQ(vecs("a"), hits(R"({"match":{"price":"1.005"}})"));
  EXPECT_EQ(vecs("a", "b"), hits(R"({"any_of":{"field":"price","values":[1.005,10.004]}})"));
  EXPECT_EQ(vecs("c"), hits(R"({"range":{"field":"price","gte":10.005}})"));
  EXPECT_EQ(vecs("a", "b", "d"), hits(R"({"range":{"field":"price","lt":10.005}})"));
  EXPECT_EQ(vecs("c"), hits(R"({"expr":"price:>10"})"));
  EXPECT_EQ(vecs("a"), hits(R"({"simple_query":{"q":"price:1.005","fields":["id"]}})"));
  EXPECT_EQ(vecs("d"), hits(R"({"range":{"field":"price","lt":-10.004}})"));
  EXPECT_TRUE(helper.index(flatdoc("id", "int", "n_i", 10, "wide_i", INT64_C(9007199254740993)), UpdateMessage::COMMIT).success);
  EXPECT_EQ(vecs("int"), hits(R"({"range":{"field":"n_i","gte":9.5,"lt":10.5}})"));
  EXPECT_EQ(vecs("int"), hits(R"({"range":{"field":"wide_i","gt":"9007199254740992.5"}})"));
  EXPECT_FALSE(helper.index(flatdoc("n_i", 9.5), UpdateMessage::COMMIT).success);
  EXPECT_FALSE(run(R"({"query":{"match":{"n_i":9.5}}})")->ok());
  EXPECT_FALSE(run(R"({"query":{"match":{"wide_i":"9007199254740993.1"}}})")->ok());
  DecimalFieldType type("decimal", 2);
  for (auto use : {FieldType::LiteralUse::MATCH, FieldType::LiteralUse::MEMBER, FieldType::LiteralUse::GTE,
                   FieldType::LiteralUse::GT, FieldType::LiteralUse::LTE, FieldType::LiteralUse::LT}) {
    for (auto literal : {coerce::scalarVal("-1.005"sv), coerce::scalarVal(1.005)}) {
      auto window = type.tryCoerceIntWindow(literal, use, "price");
      ASSERT_TRUE(window);
      auto required = type.coerceIntWindow(literal, use, "price");
      EXPECT_EQ(window->lo, required.lo);
      EXPECT_EQ(window->hi, required.hi);
    }
  }
  EXPECT_FALSE(type.tryCoerceIntWindow(coerce::scalarVal("dune"sv), FieldType::LiteralUse::MATCH, "price"));
}

TEST_F(DecimalFieldTypesTest, outputFlagsFacetsAndCollapse) {
  for (bool scaled : {false, true}) {
    auto req = run(std::format(R"json({{"flags":{},"sort":"id","fields":["id","price","prices"],"limit":-1,
      "ops":{{"f":{{"field_facet":{{"field":"price","limit":-1}}}},
        "r":{{"range_facet":{{"field":"price","start":{},"end":{},"gap":{}}}}}}}}})json",
        scaled ? R"(["decimal_scaled"])" : "[]", scaled ? "-1100" : "-11", scaled ? "1100" : "11", scaled ? "100" : "1"));
    ASSERT_OK(req);
    const auto& col = req->docList()->columns.at("price");
    if (scaled) {
      EXPECT_EQ(101, std::get<api::ColInt>(col.kind).v[0]);
      EXPECT_CONTAINS_DOC(req->getDocs(), flatdoc("id", "a", "price", 101, "prices", vec_i(101, -101, 200)));
    } else {
      EXPECT_DOUBLE_EQ(1.01, std::get<api::ColDouble>(col.kind).v[0]);
      EXPECT_CONTAINS_DOC(req->getDocs(), flatdoc("id", "a", "price", 1.01, "prices", vec(1.01, -1.01, 2.0)));
    }
    const auto& bucket = *req->docList()->ops.at("f")->facetResult()->bucket_ids;
    EXPECT_EQ(scaled, std::holds_alternative<api::ColInt>(bucket.kind));
    const auto& range = *req->docList()->ops.at("r")->facetResult()->bucket_ids;
    EXPECT_EQ(scaled, std::holds_alternative<api::ArrArrInt>(range.kind));
    auto collapse = run(std::format(R"({{"flags":{},"sort":"price asc","fields":["id","price"],"collapse":"price"}})",
        scaled ? R"(["decimal_scaled"])" : "[]"));
    ASSERT_OK(collapse);
    const auto& component = collapse->docList()->groups->keys[0].component[0];
    EXPECT_EQ(api::GroupComponent_::Type::DECIMAL, component.type);
    if (scaled) EXPECT_EQ(-1001, std::get<int64_t>(component.value));
    else EXPECT_DOUBLE_EQ(-10.01, std::get<double>(component.value));
    std::vector<std::byte> bytes;
    ASSERT_TRUE(api::encode(collapse->responses[0]->proto, bytes));
    size_t size = bytes.size();
    bytes.resize(size + 16);
    std::pmr::monotonic_buffer_resource arena;
    api::SearchResponse decoded;
    ASSERT_TRUE(api::decode(decoded, std::span(bytes).first(size), arena));
    const auto& wire = decoded.ops.at("q")->docList()->groups->keys[0].component[0];
    EXPECT_EQ(api::GroupComponent_::Type::DECIMAL, wire.type);
    EXPECT_EQ(scaled, std::holds_alternative<int64_t>(wire.value));
    EXPECT_NE(std::string::npos, renderSearchResponseBody(collapse->responses[0]->proto).find(
        scaled ? R"("_group_":{"val":-1001)" : R"("_group_":{"val":-10.01)"));
  }
  auto selected = run(R"({"limit":0,"ops":{"f":{"field_facet":{"field":"price","selected":[1.005],"limit":0}}}})");
  ASSERT_OK(selected);
  EXPECT_EQ(1, selected->docList()->ops.at("f")->facetResult()->counts[0]);
  EXPECT_FALSE(run(R"({"ops":{"r":{"range_facet":{"field":"price","start":0,"end":2,"gap":0.001}}}})")->ok());
  auto echo = run(R"({"flags":["decimal_scaled"],"ops":{"q":{"flags":["decimal_scaled"],"top_docs":{"limit":0}}}})");
  ASSERT_OK(echo);
  std::string canonical;
  ASSERT_TRUE(api::write_json(echo->rawRequest(), canonical));
  EXPECT_NE(std::string::npos, canonical.find("decimal_scaled"));
  auto op = run(R"({"ops":{"q":{"flags":["decimal_scaled"],"top_docs":{"query":{"match":{"price":"101.0"}},"fields":["id"]}}}})");
  ASSERT_OK(op);
  EXPECT_CONTAINS_DOC(op->getDocs(), flatdoc("id", "a"));
  auto raw = run(R"({"flags":["decimal_scaled"],"limit":0,"ops":{"f":{"field_facet":{"field":"price","selected":[101],"limit":0}}}})");
  ASSERT_OK(raw);
  EXPECT_EQ(1, raw->docList()->ops.at("f")->facetResult()->counts[0]);
  auto gridSelection = run(R"({"limit":0,"count_limit":-1,"ops":{"r":{"range_facet":{
    "field":"price","start":0,"end":11,"gap":1,"selected":[10.004]}}}})");
  ASSERT_OK(gridSelection);
  EXPECT_EQ(2, gridSelection->getMatchCount());
}

TEST_F(DecimalFieldTypesTest, outOfRangeWindows) {
  using Use = FieldType::LiteralUse;
  IntFieldType integer("integer");
  DecimalFieldType decimal("fine", 18);
  for (FieldType* type : std::array<FieldType*, 2>{&integer, &decimal}) {
    for (auto text : {"1e30"sv, "-1e30"sv}) {
      for (auto use : {Use::MATCH, Use::MEMBER, Use::GTE, Use::GT, Use::LTE, Use::LT}) {
        SCOPED_TRACE(std::format("{} {} {}", FieldType::typeName(type->type()), text, (int)use));
        auto literal = coerce::scalarVal(text);
        auto probe = type->tryCoerceIntWindow(literal, use, "fine");
        if (type == &integer && (use == Use::MATCH || use == Use::MEMBER)) {
          EXPECT_FALSE(probe);
          EXPECT_THROW(type->coerceIntWindow(literal, use, "fine"), RequestError);
          continue;
        }
        ASSERT_TRUE(probe);
        auto required = type->coerceIntWindow(literal, use, "fine");
        EXPECT_EQ(required.lo, probe->lo);
        EXPECT_EQ(required.hi, probe->hi);
        bool all = text.front() == '-' ? use == Use::GTE || use == Use::GT : use == Use::LTE || use == Use::LT;
        EXPECT_EQ(!all, required.empty());
        if (all) { EXPECT_EQ(INT64_MIN, required.lo); EXPECT_EQ(INT64_MAX, required.hi); }
      }
    }
    EXPECT_THROW(type->coerceColInt64(coerce::scalarVal("1e30"sv), "fine"), RequestError);
  }
  SchemaBuilder b;
  auto& fine = b.field("fine");
  fine.type = FieldClass::DECIMAL;
  fine.scale = 18;
  b.set(helper.collection());
  ASSERT_TRUE(helper.indexAll({flatdoc("id", "hi", "fine", 1, "n_i", 20),
      flatdoc("id", "lo", "fine", -1, "n_i", 10)}, UpdateMessage::COMMIT).success);
  for (auto query : {R"({"range":{"field":"fine","lte":100}})",
      R"({"range":{"field":"fine","gt":-100}})",
      R"({"range":{"field":"n_i","lte":"99999999999999999999"}})"}) {
    EXPECT_EQ(vecs("hi", "lo"), hits(query));
  }
  for (auto query : {R"({"range":{"field":"fine","gte":100}})",
      R"({"range":{"field":"n_i","gte":1e30}})", R"({"match":{"fine":100}})"}) {
    EXPECT_TRUE(hits(query).empty());
  }
  EXPECT_EQ(vecs("hi"), hits(R"({"any_of":{"field":"fine","values":[100,1]}})"));
  auto facet = run(R"({"ops":{"r":{"range_facet":{"field":"fine","start":100,"end":200,"gap":1}}}})");
  EXPECT_FALSE(facet->ok());
  EXPECT_NE(std::string::npos, facet->errorMsg().find("range start must be less than end"));
}

TEST_F(DecimalFieldTypesTest, statsAndExpressionExecution) {
  for (bool scaled : {false, true}) {
    auto req = run(std::format(R"json({{"flags":{},"ops":{{
      "sum":{{"expr_op":{{"expr":"sum(price)"}}}}, "avg":{{"expr_op":{{"expr":"avg(price)"}}}},
      "percentile":{{"expr_op":{{"expr":"percentile(price,100)"}}}},
      "rank":{{"expr_op":{{"expr":"percentile_rank(price,10)"}}}},
      "lo":{{"expr_op":{{"expr":"min(price)"}}}}, "hi":{{"expr_op":{{"expr":"max(price)"}}}},
      "reduced":{{"expr_op":{{"expr":"sum(max(prices))"}}}},
      "arithmetic":{{"expr_op":{{"expr":"sum(price + 1)"}}}},
      "bucket":{{"expr_op":{{"expr":"sum(price) + 1"}}}},
      "comparison":{{"expr_op":{{"expr":"sum(price) > 100"}}}},
      "negative_comparison":{{"expr_op":{{"expr":"min(price) > -100"}}}},
      "logical_gt":{{"expr_op":{{"expr":"sum(price > 10)"}}}},
      "decoded_gt":{{"expr_op":{{"expr":"sum(price + 0 > 10)"}}}},
      "off_grid_eq":{{"expr_op":{{"expr":"sum(price == 10.004)"}}}},
      "off_grid_ne":{{"expr_op":{{"expr":"sum(price != 10.004)"}}}},
      "wide_bound":{{"expr_op":{{"expr":"sum(price < 1e30)"}}}},
      "choice":{{"expr_op":{{"expr":"max(if(price > 10, price, price))"}}}}
    }}}})json", scaled ? R"(["decimal_scaled"])" : "[]"));
    ASSERT_OK(req);
    const auto& ops = req->responses[0]->proto.ops;
    EXPECT_TRUE(std::holds_alternative<double>(ops.at("avg")->kind));
    EXPECT_TRUE(std::holds_alternative<double>(ops.at("percentile")->kind));
    auto number = [&](const char* name) {
      const auto& v = ops.at(name)->kind;
      return std::holds_alternative<double>(v) ? std::get<double>(v) : (double)std::get<int64_t>(v);
    };
    EXPECT_DOUBLE_EQ(scaled ? 1101 : 11.01, number("sum"));
    EXPECT_DOUBLE_EQ(scaled ? -1001 : -10.01, number("lo"));
    EXPECT_DOUBLE_EQ(scaled ? 1001 : 10.01, number("hi"));
    EXPECT_DOUBLE_EQ(scaled ? 275.25 : 2.7525, number("avg"));
    EXPECT_DOUBLE_EQ(scaled ? 1001 : 10.01, number("percentile"));
    EXPECT_DOUBLE_EQ(75, number("rank"));
    EXPECT_DOUBLE_EQ(scaled ? 200 : 2, number("reduced"));
    EXPECT_DOUBLE_EQ(15.01, number("arithmetic"));
    EXPECT_DOUBLE_EQ(12.01, number("bucket"));
    EXPECT_DOUBLE_EQ(0, number("comparison"));
    EXPECT_DOUBLE_EQ(1, number("negative_comparison"));
    EXPECT_DOUBLE_EQ(1, number("logical_gt"));
    EXPECT_DOUBLE_EQ(1, number("decoded_gt"));
    EXPECT_DOUBLE_EQ(0, number("off_grid_eq"));
    EXPECT_DOUBLE_EQ(4, number("off_grid_ne"));
    EXPECT_DOUBLE_EQ(4, number("wide_bound"));
    EXPECT_DOUBLE_EQ(scaled ? 1001 : 10.01, number("choice"));
  }
  auto reader = helper.getIndexWriter()->snapshots.readers.getReader();
  auto schema = helper.collection().getSchema();
  auto* arena = createArena();
  auto cleanup = scope_guard([&] { releaseArena(arena); });
  MemPool pool;
  for (auto [text, expected] : {std::pair{"price + 1", 2.01}, {"price >= 1.005", 1.0},
      {"price < 1.005", 0.0}, {"1.005 <= price", 1.0}, {"max(price,other)", 1.01},
      {"price == 1.014", 0.0}, {"price != 1.014", 1.0}, {"price == 1.01", 1.0},
      {"price > 1e30", 0.0}, {"1e30 > price", 1.0}, {"min(prices) == -1.014", 0.0},
      {"min(prices + 1)", -0.01}, {"avg(prices) + 1", 1.6666666666666665}}) {
    SCOPED_TRACE(text);
    auto* program = ValueExprParser(ValueExprOptions{schema.get(), {}}, *arena).parse(text);
    auto* bound = program->bind(pool, reader->segments()[0], program->resolveFields(pool, *reader));
    auto point = bound->evalPoint(0, 0);
    ASSERT_TRUE(point.valid);
    EXPECT_NEAR(expected, point.type == ValueType::DOUBLE ? point.doubleValue : (double)point.intValue, 1e-14);
    std::array<int32_t, 1> docs{0};
    std::array<ValueResult, 1> batch;
    bound->evalBatch(docs, {}, batch);
    ASSERT_TRUE(batch[0].valid);
    EXPECT_NEAR(expected, batch[0].type == ValueType::DOUBLE ? batch[0].doubleValue : (double)batch[0].intValue, 1e-14);
    std::array<ScalarValueResult, 1> scalar;
    bound->evalScalarBatch(docs, scalar);
    ASSERT_TRUE(scalar[0].valid);
    EXPECT_NEAR(expected, valueDouble(program->root().type) ? scalar[0].doubleValue : (double)scalar[0].intValue, 1e-14);
  }
  auto* raw = ValueExprParser(ValueExprOptions{schema.get(), {}}, *arena).parse("price > 10");
  EXPECT_EQ(ValueType::INT64, raw->nodes[raw->root().children[0]].type);
  EXPECT_EQ(1001, raw->nodes[raw->root().children[1]].intValue);
  auto* array = ValueExprParser(ValueExprOptions{schema.get(), {}}, *arena).parse("prices + 1");
  auto* bound = array->bind(pool, reader->segments()[0], array->resolveFields(pool, *reader));
  auto values = bound->evalPoint(0, 0);
  ASSERT_EQ(3, values.array.size);
  EXPECT_NEAR(-0.01, bound->evalArrayElement(values.array, 1).doubleValue, 1e-14);
  auto* bare = ValueExprParser(ValueExprOptions{schema.get(), {}}, *arena).parse("price");
  EXPECT_EQ(ValueNodeKind::COLUMN, bare->root().kind);
  EXPECT_EQ(ValueNature::DECIMAL_2, bare->root().nature);
}

TEST_F(DecimalFieldTypesTest, scaledStringsAndWideSum) {
  helper.clear();
  SchemaBuilder b;
  auto& price = b.field("price");
  price.type = FieldClass::DECIMAL;
  price.scale = 2;
  b.set(helper.collection());
  ASSERT_TRUE(helper.indexAll({flatdoc("id", "a", "price", "92233720368547758.07"),
      flatdoc("id", "b", "price", "92233720368547758.07")}, UpdateMessage::COMMIT).success);
  auto raw = run(R"({"flags":["decimal_scaled","int64_str"],"query":{"match":{"id":"a"}},"fields":["price"]})");
  ASSERT_OK(raw);
  auto body = renderSearchResponseBody(raw->responses[0]->proto, false, {.int64Str = true});
  EXPECT_NE(std::string::npos, body.find(R"("price":"9223372036854775807")")) << body;
  auto echo = run(R"({"flags":["decimal_scaled"],"query":{"match":{"price":"9223372036854775807"}},"fields":["id"],"count_limit":-1})");
  ASSERT_OK(echo);
  EXPECT_EQ(2, echo->getMatchCount());
  EXPECT_TRUE(run(R"({"flags":["decimal_scaled"],"query":{"match":{"price":"9223372036854775807.0"}},"limit":0})")->ok());
  EXPECT_FALSE(run(R"({"flags":["decimal_scaled"],"query":{"match":{"price":"1.5"}},"limit":0})")->ok());
  auto sum = run(R"json({"ops":{"s":{"expr_op":{"expr":"sum(price)"}}}})json");
  ASSERT_OK(sum);
  EXPECT_DOUBLE_EQ((double)((__int128)INT64_MAX * 2) / 100, std::get<double>(sum->responses[0]->proto.ops.at("s")->kind));
  auto comparison = run(R"json({"ops":{"s":{"expr_op":{"expr":"sum(price) > 92233720368547758.06"}}}})json");
  ASSERT_OK(comparison);
  EXPECT_EQ(1, std::get<int64_t>(comparison->responses[0]->proto.ops.at("s")->kind));
  for (auto flags : {"[]", R"(["decimal_scaled"])"}) {
    auto wide = run(std::format(R"json({{"flags":{},"ops":{{"s":{{"expr_op":{{"expr":"sum(price) > 1e17"}}}}}}}})json", flags));
    ASSERT_OK(wide);
    EXPECT_EQ(1, std::get<int64_t>(wide->responses[0]->proto.ops.at("s")->kind));
  }
  auto scaled = run(R"json({"flags":["decimal_scaled"],"ops":{"s":{"expr_op":{"expr":"sum(price)"}}}})json");
  ASSERT_OK(scaled);
  EXPECT_TRUE(scaled->responses[0]->proto.ops.at("s")->isNull());
  ASSERT_FALSE(scaled->responses[0]->proto.warnings.empty());
  EXPECT_EQ("aggregate_eval_failed", scaled->responses[0]->proto.warnings[0].code);
  auto reader = helper.getIndexWriter()->snapshots.readers.getReader();
  auto schema = helper.collection().getSchema();
  auto* arena = createArena();
  auto cleanup = scope_guard([&] { releaseArena(arena); });
  auto* program = ValueExprParser(ValueExprOptions{schema.get(), {}}, *arena).parse("price > 92233720368547758.06");
  MemPool pool;
  auto* bound = program->bind(pool, reader->segments()[0], program->resolveFields(pool, *reader));
  EXPECT_EQ(1, bound->evalPoint(0, 0).intValue);
  auto* negative = ValueExprParser(ValueExprOptions{schema.get(), {}}, *arena).parse("price > -92233720368547758.06");
  auto* negativeBound = negative->bind(pool, reader->segments()[0], negative->resolveFields(pool, *reader));
  EXPECT_EQ(1, negativeBound->evalPoint(0, 0).intValue);
}

TEST_F(DecimalFieldTypesTest, scalePersistenceAndSchemaValidation) {
  SchemaBuilder inherited;
  auto& base = inherited.field("base");
  base.type = FieldClass::DECIMAL;
  base.scale = 3;
  inherited.field("child").parent = "base";
  auto& changedType = inherited.field("integer");
  changedType.parent = "base";
  changedType.type = FieldClass::INT;
  auto schema = inherited.build(helper.collection().getSchema().get());
  EXPECT_EQ(3u, schema->getFieldTypeEx("child")->segmentTypeFlags());
  EXPECT_EQ(FieldType::INT, schema->getFieldTypeEx("integer")->type());
  FieldSignature signature("child", *schema->getFieldTypeEx("child"));
  EXPECT_EQ("3", signature.properties.at("scale"));
  std::pmr::monotonic_buffer_resource arena;
  api::SchemaDef definition;
  ASSERT_TRUE(api::read_json(definition, R"({"fields":{"money":{"type":"decimal","scale":2}}})", arena));
  auto jsonSchema = Schema::fromProto(definition, schema.get());
  EXPECT_EQ(2u, jsonSchema->getFieldTypeEx("money")->segmentTypeFlags());
  for (int scale : {-1, 19}) {
    SchemaBuilder bad;
    auto& field = bad.field("bad");
    field.type = FieldClass::DECIMAL;
    field.scale = scale;
    EXPECT_THROW(bad.build(), SchemaError);
  }
  SchemaBuilder missing;
  missing.field("missing").type = FieldClass::DECIMAL;
  EXPECT_THROW(missing.build(), SchemaError);
  SchemaBuilder wrong;
  auto& wrongField = wrong.field("wrong");
  wrongField.type = FieldClass::INT;
  wrongField.scale = 2;
  EXPECT_THROW(wrong.build(), SchemaError);
  auto writer = helper.getIndexWriter();
  writer->mergePolicy->setMergeFactor(100);
  IndexReader reopened(writer->dir);
  EXPECT_EQ(2u, reopened.field("price").at(0)->typeFlags);
  EXPECT_EQ(FieldType::DECIMAL, reopened.field("price").at(0)->type);
  SchemaBuilder removed;
  removed.replaceAll(helper.collection());
  SchemaBuilder changed;
  auto& changedField = changed.field("price");
  changedField.type = FieldClass::DECIMAL;
  changedField.scale = 3;
  changed.set(helper.collection());
  IndexReader mismatch(writer->dir);
  EXPECT_THROW(mismatch.field("price"), RequestError);
  ASSERT_TRUE(helper.index(flatdoc("id", "new", "price", 1), UpdateMessage::COMMIT).success);
  ExpectLog quiet("Incompatible persisted types for field 'price'");
  CollectionHelper::UpdateBuilder merge;
  merge.commit(true, 1);
  auto result = helper.submit(merge);
  EXPECT_FALSE(result.success);
  EXPECT_NE(std::string::npos, result.error_message.find("scale 2"));
  EXPECT_NE(std::string::npos, result.error_message.find("scale 3"));
}

TEST_F(DecimalFieldTypesTest, typedMembershipAndIndexes) {
  SchemaBuilder b;
  for (auto [name, index] : {std::pair{"r", api::FieldDef::IndexMode::RANGE}, {"m", api::FieldDef::IndexMode::MATCH}}) {
    auto& f = b.field(name);
    f.type = FieldClass::DECIMAL;
    f.scale = 2;
    f.multi = api::FieldDef::MultiMode::ARRAY;
    f.index = index;
  }
  b.set(helper.collection());
  ASSERT_TRUE(helper.index(flatdoc("id", "typed", "r", vec(1.005, 10.0), "m", vec(1.005, 10.0)), UpdateMessage::COMMIT).success);
  for (auto field : {"r", "m"}) {
    for (bool integers : {false, true}) {
      auto req = localReq(helper.getSearchEngine());
      auto& top = req->collection("main").topDocs("q").fields({"id"});
      auto& query = top.rawQuery().kind.emplace<api::AnyOfQuery>();
      query.field = field;
      auto* values = api::build::allocMessage<api::Val>(req->mr);
      if (integers) {
        *api::build::allocArray(values->kind.emplace<api::ArrInt>().v, 1, req->mr) = 10;
      } else {
        *api::build::allocArray(values->kind.emplace<api::ArrDouble>().v, 1, req->mr) = 1.005;
      }
      query.values = values;
      req->execute(false);
      ASSERT_OK(req);
      EXPECT_CONTAINS_DOC(req->getDocs(), flatdoc("id", "typed"));
    }
  }
  helper.getIndexWriter()->mergeSegments();
  EXPECT_EQ(vecs("typed"), hits(R"({"match":{"r":1.005}})"));
  EXPECT_EQ(vecs("typed"), hits(R"({"range":{"field":"m","gte":1.004,"lt":1.014}})"));
}

TEST_F(DecimalFieldTypesTest, typeChangeRefusesReadAndMerge) {
  auto writer = helper.getIndexWriter();
  writer->mergePolicy->setMergeFactor(100);
  SchemaBuilder removed;
  removed.replaceAll(helper.collection());
  SchemaBuilder changed;
  changed.field("price").type = FieldClass::INT;
  changed.set(helper.collection());
  IndexReader reader(writer->dir);
  EXPECT_THROW(reader.field("price"), RequestError);
  ASSERT_TRUE(helper.index(flatdoc("id", "new", "price", 1), UpdateMessage::COMMIT).success);
  CollectionHelper::UpdateBuilder merge;
  merge.commit(true, 1);
  ExpectLog quiet("Incompatible persisted types for field 'price'");
  auto result = helper.submit(merge);
  EXPECT_FALSE(result.success);
  EXPECT_NE(std::string::npos, result.error_message.find("type 'decimal' scale 2"));
  EXPECT_NE(std::string::npos, result.error_message.find("type 'int'"));
}

TEST_F(DecimalFieldTypesTest, wideMetricRanksStayRawUntilRendering) {
  helper.clear();
  SchemaBuilder b;
  auto& price = b.field("price");
  price.type = FieldClass::DECIMAL;
  price.scale = 2;
  b.set(helper.collection());
  ASSERT_TRUE(helper.indexAll({
    flatdoc("id", "a", "key_s", "x", "price", "92233720368547758.07"),
    flatdoc("id", "b", "key_s", "x", "price", "92233720368547758.07"),
    flatdoc("id", "c", "key_s", "y", "price", "92233720368547758.07")}, UpdateMessage::COMMIT).success);
  for (bool scaled : {false, true}) {
    auto request = std::string(scaled ? R"({"flags":["decimal_scaled"],)" : "{") + R"json(
      "fields":["id"],"get_scores":true,"limit":-1,"sort":"id",
      "collapse":{"field":"key_s","sort":"total desc",
        "ops":{"total":{"expr_op":{"expr":"sum(price)"}}}}})json";
    auto req = run(request);
    ASSERT_OK(req);
    const auto& groups = *req->docList()->groups;
    ASSERT_EQ(2u, groups.ranks.size());
    EXPECT_EQ("x", std::get<std::string_view>(groups.keys[0].component[0].value));
    if (scaled) {
      EXPECT_TRUE(groups.ranks[0].v[0].isNull());
      EXPECT_EQ(INT64_MAX, std::get<int64_t>(groups.ranks[1].v[0].kind));
    } else {
      EXPECT_DOUBLE_EQ((double)((__int128)INT64_MAX * 2) / 100, std::get<double>(groups.ranks[0].v[0].kind));
      EXPECT_DOUBLE_EQ((double)INT64_MAX / 100, std::get<double>(groups.ranks[1].v[0].kind));
    }
  }
}

TEST(IpAddressTest, parsingAndFormatting) {
  EXPECT_EQ(ip::parse("10.0.0.1"), ip::parse("::ffff:10.0.0.1"));
  for (auto text : {"10.0.0.256", "1.2.3", "fe80::1%eth0", "1.2.3.4/32", ""}) EXPECT_FALSE(ip::parse(text));
  EXPECT_FALSE(ip::parse("10.0.0.1\0bad"sv));
  char buffer[ip::TEXT_BUF_SIZE];
  auto address = ip::parse("2001:0DB8:0000::1");
  ASSERT_TRUE(address);
  EXPECT_EQ("2001:db8::1", ip::format(ip::bytes(*address), buffer));
  EXPECT_THROW(ip::format("bad", buffer), std::invalid_argument);
  for (auto text : {"1.2.3.4/33", "::/129", "::/-1", "::/0junk", "1.2.3.4/"}) EXPECT_FALSE(ip::parseCidr(text));
  auto block = ip::parseCidr("10.1.2.3/15");
  ASSERT_TRUE(block);
  EXPECT_EQ("10.0.0.0", ip::format(ip::bytes(block->lo), buffer));
  EXPECT_EQ("10.1.255.255", ip::format(ip::bytes(block->hi), buffer));
  block = ip::parseCidr("fe80::1/10");
  ASSERT_TRUE(block);
  EXPECT_EQ("fe80::", ip::format(ip::bytes(block->lo), buffer));
  EXPECT_EQ("febf:ffff:ffff:ffff:ffff:ffff:ffff:ffff", ip::format(ip::bytes(block->hi), buffer));
  IpFieldType type("ip");
  char scratch[coerce::TEXT_BUF_SIZE];
  EXPECT_FALSE(type.tryCoerceTermWindow(coerce::scalarVal("dune"sv), "ip", scratch));
  EXPECT_TRUE(type.tryCoerceTermWindow(coerce::scalarVal("10.0.0.0/8"sv), "ip", scratch));
}

class IpFieldTypesTest : public ScalarFieldTypesTest {
protected:
  void SetUp() override {
    LuxirTest::SetUp();
    SchemaBuilder b;
    b.field("addr").type = FieldClass::IP;
    auto& array = b.field("array_ip");
    array.type = FieldClass::IP;
    array.multi = api::FieldDef::MultiMode::ARRAY;
    b.set(helper.collection());
    ASSERT_TRUE(helper.indexAll({
      flatdoc("id", "a", "addr", "10.0.0.2", "label_s", "hello", "many_ips", vecs("2001:0db8::1", "10.0.0.1", "::ffff:10.0.0.1")),
      flatdoc("id", "b", "addr", "::ffff:10.0.0.10", "array_ip", vecs("10.0.0.2", "::ffff:10.0.0.2", "::1")),
      flatdoc("id", "c", "addr", "2001:0DB8:0000::1"), flatdoc("id", "d", "addr", "2001:db9::1"),
      flatdoc("id", "e")}, UpdateMessage::COMMIT).success);
  }
};

TEST_F(IpFieldTypesTest, literalsAndCidr) {
  EXPECT_EQ(vecs("a"), hits(R"({"match":{"addr":"::ffff:10.0.0.2"}})"));
  EXPECT_EQ(vecs("c"), hits(R"({"match":{"addr":"2001:db8::1"}})"));
  for (auto cidr : {"10.1.2.3/8", "0.0.0.0/0", "::ffff:0:0/96"}) {
    EXPECT_EQ(vecs("a", "b"), hits(std::format(R"({{"match":{{"addr":"{}"}}}})", cidr)));
  }
  EXPECT_EQ(vecs("a", "b", "c", "d"), hits(R"({"match":{"addr":"::/0"}})"));
  EXPECT_EQ(vecs("a"), hits(R"({"match":{"addr":"10.0.0.2/32"}})"));
  EXPECT_EQ(vecs("c"), hits(R"({"match":{"addr":"2001:db8:abcd::/32"}})"));
  EXPECT_EQ(vecs("c"), hits(R"({"match":{"addr":"2001:db8::1/128"}})"));
  EXPECT_EQ(vecs("b"), hits(R"({"range":{"field":"addr","gt":"10.0.0.2","lte":"10.0.0.10"}})"));
  EXPECT_EQ(vecs("a"), hits(R"({"range":{"field":"addr","gte":"10.0.0.2","lt":"10.0.0.10"}})"));
  EXPECT_EQ(vecs("a", "c"), hits(R"({"any_of":{"field":"addr","values":["10.0.0.2","2001:db8::1"]}})"));
  EXPECT_EQ(vecs("a"), hits(R"({"multi_match":{"text":"hello","fields":["*"]}})"));
  EXPECT_EQ(vecs("b"), hits(R"({"multi_match":{"text":"10.0.0.10","fields":["*"]}})"));
  EXPECT_EQ(vecs("a"), hits(R"({"simple_query":{"q":"addr:10.0.0.2","fields":["label_s"]}})"));
  EXPECT_TRUE(run(R"({"query":{"simple_query":{"q":"addr:dune","fields":["label_s"]}}})")->ok());
  EXPECT_EQ(vecs("c"), hits(R"({"expr":"addr:'2001:db8::1'"})"));
  auto typed = localReq(helper.getSearchEngine());
  auto& top = typed->collection("main").topDocs("q").fields({"id"});
  auto& query = top.rawQuery().kind.emplace<api::AnyOfQuery>();
  query.field = "addr";
  auto* values = api::build::allocMessage<api::Val>(typed->mr);
  auto* strings = api::build::allocArray(values->kind.emplace<api::ArrStr>().v, 2, typed->mr);
  strings[0] = "::ffff:10.0.0.2"; strings[1] = "2001:db8::1";
  query.values = values;
  typed->execute(false);
  ASSERT_OK(typed);
  EXPECT_EQ(2u, typed->getDocs().size());
}

TEST_F(IpFieldTypesTest, outputFacetsSortAndCollapse) {
  for (auto format : {"rows", "columns"}) {
    auto req = run(std::format(R"({{"fields":["id","addr","many_ips","array_ip"],"sort":"id","limit":-1,"document_format":"{}",
      "ops":{{"f":{{"field_facet":{{"field":"addr","limit":-1}}}}}}}})", format));
    ASSERT_OK(req);
    auto docs = req->getDocs();
    EXPECT_CONTAINS_DOC(docs, flatdoc("id", "b", "addr", "10.0.0.10", "array_ip", vecs("10.0.0.2", "10.0.0.2", "::1")));
    EXPECT_CONTAINS_DOC(docs, flatdoc("id", "a", "addr", "10.0.0.2", "many_ips", vecs("10.0.0.1", "2001:db8::1")));
    EXPECT_CONTAINS_DOC(docs, flatdoc("id", "c", "addr", "2001:db8::1"));
    const auto& ids = std::get<api::ColStr>(req->docList()->ops.at("f")->facetResult()->bucket_ids->kind).v;
    EXPECT_EQ(vecs("10.0.0.2", "10.0.0.10", "2001:db8::1", "2001:db9::1"), std::vector<std::string>(ids.begin(), ids.end()));
    auto body = renderSearchResponseBody(req->responses[0]->proto);
    EXPECT_NE(std::string::npos, body.find("2001:db8::1"));
  }
  auto selected = run(R"({"limit":0,"ops":{"f":{"field_facet":{"field":"addr","selected":["::ffff:10.0.0.2"],"limit":0}}}})");
  ASSERT_OK(selected);
  EXPECT_EQ(1, selected->docList()->ops.at("f")->facetResult()->counts[0]);
  ASSERT_TRUE(helper.index(flatdoc("id", "z", "addr", "10.0.0.2"), UpdateMessage::COMMIT).success);
  auto req = run(R"({"query":{"match":{"addr":"::/0"}},"sort":"addr asc","fields":["id","addr"],"get_scores":true,
    "collapse":{"field":"addr","head_sort":"id desc"}})");
  ASSERT_OK(req);
  const auto& groups = *req->docList()->groups;
  ASSERT_EQ(4u, groups.keys.size());
  EXPECT_EQ(api::GroupComponent_::Type::IP, groups.keys[0].component[0].type);
  EXPECT_EQ("10.0.0.2", std::get<std::string_view>(groups.keys[0].component[0].value));
  EXPECT_EQ("10.0.0.10", std::get<std::string_view>(groups.keys[1].component[0].value));
  EXPECT_EQ("10.0.0.2", std::get<std::string_view>(groups.ranks[0].v[0].kind));
  std::vector<std::byte> bytes;
  ASSERT_TRUE(api::encode(req->responses[0]->proto, bytes));
  bytes.resize(bytes.size() + 16);
  std::pmr::monotonic_buffer_resource arena;
  api::SearchResponse decoded;
  ASSERT_TRUE(api::decode(decoded, std::span(bytes).first(bytes.size() - 16), arena));
  EXPECT_EQ(api::GroupComponent_::Type::IP, decoded.ops.at("q")->docList()->groups->keys[0].component[0].type);
  auto nested = run(R"({"limit":0,"ops":{"outer":{"field_facet":{"field":"addr","limit":-1,
    "ops":{"inner":{"field_facet":{"field":"many_ips","limit":-1}}}}}}})");
  ASSERT_OK(nested);
  EXPECT_NE(std::string::npos, renderSearchResponseBody(nested->responses[0]->proto).find("10.0.0.1"));
  auto count = run(R"json({"ops":{"n":{"expr_op":{"expr":"sum(count(many_ips))"}}}})json");
  ASSERT_OK(count);
  EXPECT_EQ(2, std::get<int64_t>(count->responses[0]->proto.ops.at("n")->kind));
}

TEST_F(IpFieldTypesTest, validationIsAtomic) {
  for (auto value : {R"(123)", R"(true)", R"("bad")", R"("10.0.0.1\u0000bad")"}) {
    EXPECT_FALSE(indexJson(std::format(R"({{"addr":{}}})", value)).success);
    EXPECT_FALSE(run(std::format(R"({{"query":{{"match":{{"addr":{}}}}}}})", value))->ok());
  }
  EXPECT_FALSE(helper.index(flatdoc("id", "a", "many_ips", vecs("192.0.2.1", "bad")), UpdateMessage::COMMIT).success);
  EXPECT_FALSE(indexJson(R"({"id":"a","many_ips":["192.0.2.1",42]})").success);
  ASSERT_TRUE(helper.index(flatdoc("id", "next", "many_ips", vecs("192.0.2.2", "192.0.2.3")), UpdateMessage::COMMIT).success);
  EXPECT_EQ(vecs("a"), hits(R"({"match":{"addr":"10.0.0.2"}})"));
  EXPECT_TRUE(hits(R"({"match":{"many_ips":"192.0.2.1"}})").empty());
  EXPECT_EQ(vecs("next"), hits(R"({"match":{"many_ips":"192.0.2.3"}})"));
  for (auto query : {R"({"prefix":{"field":"addr","prefix":"10"}})",
      R"({"wildcard":{"field":"addr","pattern":"10.*"}})",
      R"({"regex":{"field":"addr","pattern":".*"}})",
      R"({"fuzzy":{"field":"addr","term":"10.0.0.2"}})",
      R"({"any_of":{"field":"addr","values":["10.0.0.0/8"]}})",
      R"({"match":{"field":"addr","val":"10.0.0.2","case_insensitive":true}})",
      R"({"range":{"field":"addr","gte":"10.0.0.0/8"}})"}) {
    EXPECT_FALSE(run(std::format(R"({{"query":{}}})", query))->ok()) << query;
  }
  EXPECT_FALSE(run(R"({"ops":{"f":{"field_facet":{"field":"addr","term_filter":{"prefix":"10"}}}}})")->ok());
  EXPECT_FALSE(run(R"({"query":{"simple_query":{"q":"10.0.0.2","fields":["addr"]}}})")->ok());
  for (auto property : {R"("stored":true)", R"("index":"none")", R"("index":"range")"}) {
    std::pmr::monotonic_buffer_resource arena;
    api::SchemaDef definition;
    ASSERT_TRUE(api::read_json(definition, std::format(R"({{"fields":{{"bad":{{"type":"ip",{}}}}}}})", property), arena));
    EXPECT_THROW(Schema::fromProto(definition), SchemaError);
  }
}

TEST_F(IpFieldTypesTest, persistenceMergeAndTypeCheck) {
  auto writer = helper.getIndexWriter();
  IndexReader reader(writer->dir);
  EXPECT_EQ(FieldType::IP, reader.field("addr").at(0)->type);
  ASSERT_TRUE(helper.index(flatdoc("id", "f", "addr", "10.0.0.3"), UpdateMessage::COMMIT).success);
  writer->mergeSegments();
  EXPECT_EQ(vecs("a", "b", "f"), hits(R"({"match":{"addr":"10.0.0.0/8"}})"));
  ASSERT_TRUE(helper.deleteById("a", UpdateMessage::COMMIT).success);
  writer->mergeSegments();
  EXPECT_EQ(vecs("b", "f"), hits(R"({"match":{"addr":"10.0.0.0/8"}})"));
  SchemaBuilder removed;
  removed.replaceAll(helper.collection());
  SchemaBuilder changed;
  changed.field("addr").type = FieldClass::STRING;
  changed.set(helper.collection());
  IndexReader mismatch(writer->dir);
  EXPECT_THROW(mismatch.field("addr"), RequestError);
  ASSERT_TRUE(helper.index(flatdoc("id", "new", "addr", "hello"), UpdateMessage::COMMIT).success);
  CollectionHelper::UpdateBuilder merge;
  merge.commit(true, 1);
  ExpectLog quiet("Incompatible persisted types for field 'addr'");
  auto result = helper.submit(merge);
  EXPECT_FALSE(result.success);
  EXPECT_NE(std::string::npos, result.error_message.find("type 'ip'"));
}

TEST_F(IpFieldTypesTest, storedStringSourceAndIpVariant) {
  std::pmr::monotonic_buffer_resource arena;
  api::SchemaDef definition;
  ASSERT_TRUE(api::read_json(definition, R"({"fields":{"source":{"type":"string","stored":true,
    "variants":{"ip":{"type":"ip"}}}}})", arena));
  helper.collection().setSchema(Schema::fromProto(definition, helper.collection().getSchema().get()));
  ASSERT_TRUE(helper.index(flatdoc("id", "variant", "source", "::ffff:192.0.2.1"), UpdateMessage::COMMIT).success);
  auto req = run(R"({"query":{"match":{"source__ip":"192.0.2.1"}},"fields":["source","source__ip"]})");
  ASSERT_OK(req);
  EXPECT_CONTAINS_DOC(req->getDocs(), flatdoc("source", "::ffff:192.0.2.1", "source__ip", "192.0.2.1"));
}
