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
