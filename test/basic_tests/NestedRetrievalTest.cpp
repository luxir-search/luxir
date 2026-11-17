// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include <glaze/glaze.hpp>
#include "luxir/server/JsonResponse.h"
#include "luxir/search/SearchOverrides.h"
#include "test/LuxirTest.h"
#include "test/NestedTestData.h"
#include "test/QueryBuild.h"

namespace luxir::test {
namespace {

std::string canonical(const glz::generic_i64& value) {
  if (value.is_object()) {
    std::map<std::string, std::string> fields;
    for (const auto& [key, child] : value.get_object()) fields[key] = canonical(child);
    return glz::write_json(fields).value();
  }
  if (value.is_array()) {
    std::vector<std::string> rows;
    for (const auto& child : value.get_array()) rows.push_back(canonical(child));
    return glz::write_json(rows).value();
  }
  return glz::write_json(value).value();
}

class NestedRetrievalTest : public LuxirTest {
protected:
  CollectionHelper helper;

  void setup() {
    nested_test::setSchema(helper.collection());
    helper.getIndexWriter()->mergePolicy->setMergeFactor(1000);
    for (int batch = 0; batch < 2; batch++) {
      CollectionHelper::UpdateBuilder input;
      for (int i = batch * 2; i < batch * 2 + 2; i++) nested_test::add(input, nested_test::DOCS[i]);
      if (!batch) nested_test::add(input, R"({"id":"dead","items":{"color":"discard"}})");
      else nested_test::add(input, R"({"id":"holes","items":[{"color":"green","parts":[{},{"material":"tin"}]},{},{"color":"gold"}]})");
      input.commit(); ASSERT_TRUE(helper.submit(input).success);
    }
    ASSERT_TRUE(helper.deleteById("dead", UpdateMessage::COMMIT).success);
    SchemaBuilder later;
    later.field("later_nested").type = api::FieldDef::FieldClass::NESTED;
    later.set(helper.collection());
  }

  glz::generic_i64 query(std::initializer_list<std::string> fields, std::string_view expr = "id:a",
                         std::string_view path = {}, api::DocFormat format = api::DocFormat::ROWS) {
    auto req = localReq(helper.getSearchEngine());
    auto& op = req->topDocs().exprQuery(expr).fields(fields).limit(-1).countLimit().documentFormat(format);
    if (!path.empty()) op.rawOp().domain.emplace().to = path;
    req->execute(1); EXPECT_OK(req);
    glz::generic_i64 parsed;
    auto body = renderSearchResponseBody(req->responses.back()->proto);
    EXPECT_FALSE(glz::read_json(parsed, body)) << body;
    return parsed["ops"]["q"]["docs"];
  }
};

TEST_F(NestedRetrievalTest, selectionsStayAlignedAcrossDeletesAndMergeInEveryFormat) {
  setup();
  auto source = helper.getIndexWriter()->snapshots.readers.getReader();
  auto initialBounds = source->view("items")->segments()[0].liveCountBounds();
  query({"items"});
  EXPECT_EQ(initialBounds, source->view("items")->segments()[0].liveCountBounds());
  for (bool merged : {false, true}) {
    if (merged) helper.getIndexWriter()->mergeSegments();
    for (auto format : {api::DocFormat::ROWS, api::DocFormat::COLUMNS, api::DocFormat::ADAPTIVE}) {
      SCOPED_TRACE(::testing::Message() << merged << '/' << (int)format);
      auto full = query({"id", "items"}, "id:a", {}, format);
      auto& items = full[0]["items"];
      ASSERT_EQ(2u, items.get_array().size());
      EXPECT_EQ("red", items[0]["color"].get_string());
      EXPECT_EQ("red child", items[0]["note_w"].get_string());
      EXPECT_EQ("steel", items[0]["parts"][0]["material"].get_string());
      EXPECT_EQ("wood", items[1]["parts"][0]["material"].get_string());
      auto fields = query({"items.color", "items.size", "items.color"}, "id:a", {}, format);
      EXPECT_EQ("S", fields[0]["items"][0]["size"].get_string());
      EXPECT_EQ("L", fields[0]["items"][1]["size"].get_string());
      EXPECT_FALSE(fields[0]["items"][0].contains("parts"));
      auto empty = query({"items.color"}, "id:d", {}, format);
      ASSERT_EQ(1u, empty[0]["items"].get_array().size());
      EXPECT_TRUE(empty[0]["items"][0].get_object().empty());
      for (const auto& [field, id] : std::vector<std::pair<std::string, std::string>>{{"items", "c"}, {"later_nested", "a"}}) {
        auto missing = query({field}, "id:" + id, {}, format);
        if (format == api::DocFormat::COLUMNS) { EXPECT_TRUE(missing[0][field].is_null()); }
        else { EXPECT_FALSE(missing[0].contains(field)); }
      }
      EXPECT_FALSE(query({"*"}, "id:a", {}, format)[0].contains("items"));
      auto own = query({"items.*"}, "id:a", {}, format);
      EXPECT_EQ("red", own[0]["items"][0]["color"].get_string());
      EXPECT_FALSE(own[0]["items"][0].contains("parts"));
      auto deep = query({"items.parts.material"}, "id:a", {}, format);
      EXPECT_EQ("wood", deep[0]["items"][1]["parts"][0]["material"].get_string());
      EXPECT_FALSE(deep[0]["items"][0].contains("color"));
      auto aligned = query({"items.color", "items.parts.material"}, "id:holes", {}, format);
      const auto& holes = aligned[0]["items"];
      ASSERT_EQ(3u, holes.get_array().size());
      EXPECT_TRUE(holes[1].get_object().empty());
      EXPECT_EQ("gold", holes[2]["color"].get_string());
      EXPECT_TRUE(holes[0]["parts"][0].get_object().empty());
      EXPECT_EQ("tin", holes[0]["parts"][1]["material"].get_string());
      auto unioned = query({"id", "items.color", "items", "items.parts.material"}, "id:a", {}, format);
      EXPECT_EQ(canonical(full), canonical(unioned));
    }
  }
}

TEST_F(NestedRetrievalTest, siblingEmittersPinTheirViewsTogether) {
  setup();
  auto req = localReq(helper.getSearchEngine());
  for (int i = 0; i < 8; i++) req->topDocs("q" + std::to_string(i)).exprQuery("id:a").fields({"items"});
  req->execute(-1); ASSERT_OK(req);
  EXPECT_EQ(2u, req->scopedViewCountForTests());
  for (int i = 0; i < 8; i++) EXPECT_EQ(1, req->docList("q" + std::to_string(i))->row_count);
}

TEST_F(NestedRetrievalTest, ancestorsAndLocalSubtreesUseTheirOwnViews) {
  setup();
  for (auto format : {api::DocFormat::ROWS, api::DocFormat::COLUMNS, api::DocFormat::ADAPTIVE}) {
    auto items = query({"color", "../id", "/id", "../*", "parts"}, "color:red", "items", format);
    ASSERT_EQ(2u, items.get_array().size());
    for (auto& item : items.get_array()) {
      EXPECT_EQ(item["../id"].get_string(), item["/id"].get_string());
      EXPECT_EQ("steel", item["parts"][0]["material"].get_string());
    }
    auto repeated = query({"/id"}, "color:(red OR blue)", "items", format);
    std::vector<std::string> owners;
    for (const auto& row : repeated.get_array()) owners.push_back(row["/id"].get_string());
    std::sort(owners.begin(), owners.end());
    EXPECT_EQ((std::vector<std::string>{"a", "a", "b"}), owners);
    auto parts = query({"/id", "../../id", "../color", "/*", "_version_"}, "material:steel", "items.parts", format);
    ASSERT_EQ(2u, parts.get_array().size());
    for (auto& part : parts.get_array()) {
      EXPECT_EQ(part["/id"].get_string(), part["../../id"].get_string());
      EXPECT_EQ("red", part["../color"].get_string());
      EXPECT_GT(part["_version_"].get<int64_t>(), 0);
    }
    auto versions = query({"_version_", "items._version_"}, "id:a", {}, format);
    for (auto& child : versions[0]["items"].get_array()) EXPECT_EQ(versions[0]["_version_"].get<int64_t>(), child["_version_"].get<int64_t>());
  }
}

TEST_F(NestedRetrievalTest, repeatedOwnersUseNativeColumnsAndStoredReaders) {
  nested_test::setSchema(helper.collection());
  SchemaBuilder schema;
  for (auto name : {"raw", "raw_multi"}) {
    auto& field = schema.field(name);
    field.type = api::FieldDef::FieldClass::STRING;
    field.index = api::FieldDef::IndexMode::NONE;
    field.multi = name == std::string_view("raw") ? api::FieldDef::MultiMode::SINGLE : api::FieldDef::MultiMode::ARRAY;
  }
  auto& notes = schema.field("notes_w");
  notes.type = api::FieldDef::FieldClass::TEXT;
  notes.stored = true;
  notes.multi = api::FieldDef::MultiMode::ARRAY;
  schema.set(helper.collection());
  CollectionHelper::UpdateBuilder input;
  nested_test::add(input, R"({"id":"a","raw":"raw","raw_multi":["z","x"],"n_i":7,"nums_is":[3,8],"tags_ss":["x","z"],"note_w":"owner text","notes_w":["first text","second text"],"items":[{"note_w":"child one"},{"note_w":"child two"}]})");
  nested_test::add(input, R"({"id":"b","items":{"note_w":"child three"}})");
  input.commit(); ASSERT_TRUE(helper.submit(input).success);
  for (auto format : {api::DocFormat::ROWS, api::DocFormat::COLUMNS, api::DocFormat::ADAPTIVE}) {
    auto run = [&](bool child) {
      auto req = localReq(helper.getSearchEngine());
      auto& op = req->topDocs().allQuery().limit(-1).documentFormat(format).fields({
          "/id", "/raw", "/raw_multi", "/n_i", "/nums_is", "/tags_ss", "/note_w", "/notes_w", "/_version_"});
      if (child) {
        op.rawOp().domain.emplace().to = "items";
      }
      req->execute(1); EXPECT_OK(req);
      return req;
    };
    auto roots = run(false), children = run(true);
    ASSERT_EQ(3, children->docList()->row_count);
    if (format == api::DocFormat::COLUMNS) {
      for (const auto& [name, column] : roots->docList()->columns) {
        EXPECT_EQ(column.kind.index(), children->docList()->columns.at(name).kind.index()) << name;
      }
      EXPECT_TRUE(std::holds_alternative<api::ColStr>(children->docList()->columns.at("/id").kind));
      EXPECT_TRUE(std::holds_alternative<api::ArrArrInt>(children->docList()->columns.at("/nums_is").kind));
    }
    auto body = [&](auto& req) {
      glz::generic_i64 result;
      EXPECT_FALSE(glz::read_json(result, renderSearchResponseBody(req->responses.back()->proto)));
      return result["ops"]["q"]["docs"];
    };
    auto expected = body(roots), actual = body(children);
    EXPECT_EQ(canonical(expected[0]), canonical(actual[0]));
    EXPECT_EQ(canonical(expected[0]), canonical(actual[1]));
    EXPECT_EQ(canonical(expected[1]), canonical(actual[2]));
    auto mixed = query({"note_w", "/note_w", "/notes_w"}, "*:*", "items", format);
    EXPECT_EQ("child one", mixed[0]["note_w"].get_string());
    EXPECT_EQ("child two", mixed[1]["note_w"].get_string());
    EXPECT_EQ("owner text", mixed[1]["/note_w"].get_string());
    EXPECT_EQ(2u, mixed[1]["/notes_w"].get_array().size());
  }
}

TEST_F(NestedRetrievalTest, collapsedHeadsAndMemberDocumentsUseTheSameProjection) {
  setup();
  auto req = localReq(helper.getSearchEngine());
  ASSERT_TRUE(api::read_json(req->rawRequest(), R"({"ops":{"q":{"top_docs":{
    "query":"id:a","fields":["items"],"collapse":{"field":["id"],"get_count":true,
    "ops":{"members":{"top_docs":{"fields":["items.parts.material"]}}}}}}}})", req->mr));
  req->execute(1); ASSERT_OK(req);
  glz::generic_i64 body;
  ASSERT_FALSE(glz::read_json(body, renderSearchResponseBody(req->responses.back()->proto)));
  auto& head = body["ops"]["q"]["docs"][0];
  EXPECT_EQ("red", head["items"][0]["color"].get_string());
  auto& member = head["_group_"]["members"]["docs"][0];
  EXPECT_EQ("steel", member["items"][0]["parts"][0]["material"].get_string());
}

TEST_F(NestedRetrievalTest, upwardNamesRemainInvalidForOtherConsumers) {
  setup();
  for (int kind = 0; kind < 6; kind++) {
    auto req = localReq(helper.getSearchEngine());
    auto& op = req->topDocs().allQuery();
    op.rawOp().domain.emplace().to = "items";
    if (kind == 0) qb::sort(op, "../id", qb::ASC);
    if (kind == 1) op.facet("f", "../id");
    if (kind == 2) op.expr("e", "sum(col(\"../id\"))");
    if (kind == 3) op.rawQuery() = qb::rescore(req->mr, qb::all(), "col(\"../id\")");
    if (kind == 4) op.rawQuery() = qb::match(req->mr, "../id", "a");
    if (kind == 5) {
      ASSERT_TRUE(api::read_json(req->rawRequest(), R"({"ops":{"q":{"domain":{"to":"items"},"top_docs":{"collapse":{"field":["../id"]}}}}})", req->mr));
    }
    req->execute(1); EXPECT_FALSE(req->ok()) << kind;
    EXPECT_FALSE(req->errorMsg().empty());
  }
}

TEST_F(NestedRetrievalTest, templatesValuesAndRenderingUseTheOrdinaryLoaders) {
  SchemaBuilder schema;
  auto& code = schema.field("lines_nested.code_s");
  code.type = api::FieldDef::FieldClass::STRING;
  schema.variant(code, "other").type = api::FieldDef::FieldClass::STRING;
  auto& price = schema.field("lines_nested.price");
  price.type = api::FieldDef::FieldClass::DECIMAL; price.scale = 2;
  schema.set(helper.collection());
  CollectionHelper::UpdateBuilder input;
  nested_test::add(input, R"({"id":"a","lines_nested":[{"code_s":"raw","tags_ss":["x","y"],"price":"1.25","date_dt":"2026-01-01T00:00:00Z","geo_p":[12,45]},{}]})");
  input.commit(); ASSERT_TRUE(helper.submit(input).success);
  auto values = query({"lines_nested.code_s__other", "lines_nested.tags_ss", "lines_nested.price", "lines_nested.geo_p"});
  auto& first = values[0]["lines_nested"][0];
  EXPECT_EQ("raw", first["code_s__other"].get_string());
  EXPECT_EQ(2u, first["tags_ss"].get_array().size());
  EXPECT_DOUBLE_EQ(1.25, first["price"].get<double>());
  EXPECT_EQ(2u, first["geo_p"].get_array().size());
  auto req = localReq(helper.getSearchEngine());
  ASSERT_TRUE(api::read_json(req->rawRequest(), R"({"flags":["date_iso","decimal_scaled"],"ops":{"q":{"top_docs":{"fields":["lines_nested.date_dt","lines_nested.price"]}}}})", req->mr));
  req->execute(1); ASSERT_OK(req);
  glz::generic_i64 rendered;
  ASSERT_FALSE(glz::read_json(rendered, renderSearchResponseBody(req->responses.back()->proto)));
  auto& row = rendered["ops"]["q"]["docs"][0]["lines_nested"][0];
  EXPECT_EQ("2026-01-01T00:00:00.000Z", row["date_dt"].get_string());
  EXPECT_EQ(125, row["price"].get<int64_t>());
}

TEST_F(NestedRetrievalTest, accountingResourceRetainsArenaChargesUntilDestruction) {
  RequestMemTracker tracker(0);
  {
    RequestMemResource heap(tracker, "heap");
    auto* memory = heap.allocate(64);
    EXPECT_EQ(64u, tracker.bytes());
    heap.deallocate(memory, 64);
    EXPECT_EQ(0u, tracker.bytes());
  }
  std::pmr::monotonic_buffer_resource arena;
  {
    RequestMemResource retained(tracker, "arena", "returned fields", &arena,
                                RequestMemResource::Lifetime::RESOURCE);
    auto* memory = retained.allocate(128);
    retained.deallocate(memory, 128);
    EXPECT_EQ(128u, tracker.bytes());
  }
  EXPECT_EQ(0u, tracker.bytes());
}

TEST_F(NestedRetrievalTest, upwardGrammarAndResponseMemoryAreChecked) {
  setup();
  for (auto [path, selector, message] : std::vector<std::tuple<std::string, std::string, std::string>>{
      {"", "../id", "above the root"}, {"items", "../../id", "above the root"},
      {"items", "../items.color", "up then down"}, {"items.parts", "/items", "up then down"},
      {"items", "../", "expected"}, {"items", "../id/x", "expected"}}) {
    auto req = localReq(helper.getSearchEngine());
    auto& op = req->topDocs().allQuery().fields({selector});
    if (!path.empty()) op.rawOp().domain.emplace().to = path;
    req->execute(1); ASSERT_FALSE(req->ok());
    EXPECT_NE(std::string::npos, req->toString().find(message)) << req->toString();
  }
  for (auto name : {"/id", "../id", "items.1bad"}) {
    SchemaBuilder invalid;
    invalid.field(name).type = api::FieldDef::FieldClass::STRING;
    EXPECT_THROW(invalid.set(helper.collection()), SchemaError);
  }
  CollectionHelper::UpdateBuilder input;
  std::string doc = R"({"id":"many","items":[)";
  for (int i = 0; i < 2000; i++) doc += i ? ",{}" : "{}";
  nested_test::add(input, doc + "]}");
  input.commit(); ASSERT_TRUE(helper.submit(input).success);
  SearchOverridesGuard ceiling(forcedRequestMemoryMaxBytes);
  forcedRequestMemoryMaxBytes = 65536;
  auto req = localReq(helper.getSearchEngine());
  req->topDocs().exprQuery("id:many").fields({"items"});
  req->execute(1); ASSERT_FALSE(req->ok());
  EXPECT_NE(std::string::npos, req->toString().find("returned fields")) << req->toString();
  auto local = localReq(helper.getSearchEngine());
  local->topDocs().exprQuery("id:many").fields({"id"});
  local->execute(1); EXPECT_OK(local);
  EXPECT_EQ(0u, local->scopedViewCountForTests());
  forcedRequestMemoryMaxBytes = 0;
  auto complete = localReq(helper.getSearchEngine());
  complete->topDocs().exprQuery("id:many").fields({"items"}).documentFormat(api::DocFormat::ROWS);
  complete->execute(1); ASSERT_OK(complete);
  const auto& rows = std::get<api::ArrVal>(complete->docList()->docs[0].fields.at("items")->kind).v;
  EXPECT_EQ(2000u, rows.size());
  EXPECT_GE(complete->memoryTracker.peak(), rows.size() * sizeof(api::Val));
}

} // namespace
} // namespace luxir::test
