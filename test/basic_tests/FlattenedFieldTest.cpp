// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include <gtest/gtest.h>

#include <map>
#include <string>

#include "test/CollectionHelper.h"
#include "test/LocalReq.h"
#include "test/LuxirTest.h"
#include "test/SchemaBuilder.h"
#include "test/TestUtils.h"

namespace luxir::test {

using FieldClass = api::FieldDef::FieldClass;

class FlattenedFieldTest : public LuxirTest {
protected:
  // labels: flattened, rebuilt from its column. attrs: flattened with stored
  // source, a depth limit of 2 and ignore_above 5.
  static void setSchema(CollectionHelper& helper) {
    SchemaBuilder b;
    b.field("labels").type = FieldClass::FLATTENED;
    auto& attrs = b.field("attrs");
    attrs.type = FieldClass::FLATTENED;
    attrs.stored = true;
    attrs.depth_limit = 2;
    attrs.ignore_above = 5;
    b.set(helper.collection());
  }

  static IndexResult indexJson(CollectionHelper& helper, std::initializer_list<std::string_view> docs) {
    CollectionHelper::UpdateBuilder builder;
    for (std::string_view json : docs) {
      builder.addRaw([&](api::Map& doc, std::pmr::memory_resource& mr) {
        EXPECT_TRUE(api::read_json(doc, json, mr)) << json;
      });
    }
    builder.commit();
    return helper.submit(builder);
  }

  static void indexLabels(CollectionHelper& helper) {
    setSchema(helper);
    auto result = indexJson(helper, {
        R"({"id":"d1","labels":{"app":"web","env":"prod","tier":{"zone":"a"}}})",
        R"({"id":"d2","labels":{"app":"db","env":"prod","port":5432,"tags":["x","y"]}})",
        R"({"id":"d3","labels":{"app":"web","env":"dev"},"attrs":{"n":1,"s":"abcdefgh"}})"});
    ASSERT_TRUE(result.success) << result.error_message;
    ASSERT_EQ(0, result.total_errors);
  }

  static auto run(CollectionHelper& helper, std::string_view ops) {
    auto request = localReq(helper.getSearchEngine());
    std::string json = R"({"collection":"main","document_format":"rows","ops":)" + std::string(ops) + "}";
    EXPECT_TRUE(api::read_json(request->rawRequest(), json, request->mr)) << json;
    request->execute();
    return request;
  }

  static std::vector<std::string> ids(CollectionHelper& helper, std::string_view expr) {
    auto request = run(helper, R"({"q":{"top_docs":{"query":")" + std::string(expr)
                                   + R"(","fields":["id"],"limit":-1,"sort":"id"}}})");
    EXPECT_TRUE(request->ok()) << expr << ": " << request->errorMsg();
    std::vector<std::string> out;
    for (const auto& doc : request->getDocs()) out.push_back(std::get<std::string>(doc[0].val));
    return out;
  }

  // The JSON of one document's returned field.
  static std::string retrieved(CollectionHelper& helper, std::string_view id, std::string_view field) {
    auto request = run(helper, R"({"q":{"top_docs":{"query":"id:)" + std::string(id)
                                   + R"(","fields":[")" + std::string(field) + R"("]}}})");
    EXPECT_TRUE(request->ok()) << request->errorMsg();
    const auto* docs = request->docList();
    if (docs == nullptr || docs->docs.size() != 1) return "<no doc>";
    const auto* val = docs->docs[0].fields.find(field);
    std::string json;
    if (val == nullptr || !api::write_json(**val, json)) return "<missing>";
    return json;
  }
};

TEST_F(FlattenedFieldTest, keyPathQueries) {
  CollectionHelper helper;
  indexLabels(helper);
  using V = std::vector<std::string>;
  EXPECT_EQ((V{"d1", "d3"}), ids(helper, "labels.app:web"));
  EXPECT_EQ((V{"d1"}), ids(helper, "labels.tier.zone:a"));
  EXPECT_EQ((V{"d2"}), ids(helper, "labels.port:5432"));
  EXPECT_EQ((V{"d2"}), ids(helper, "labels.tags:y"));
  // Bare values match under any key.
  EXPECT_EQ((V{"d1", "d2"}), ids(helper, "labels:prod"));
  EXPECT_EQ((V{"d1", "d2"}), ids(helper, "labels.env:pr*"));
  // Ranges compare as strings and stay inside the key path.
  EXPECT_EQ((V{"d2"}), ids(helper, "labels.app:[a TO e]"));
  EXPECT_EQ((V{"d1", "d3"}), ids(helper, "labels.app:>=w"));
  EXPECT_EQ((V{"d2"}), ids(helper, "labels.app:<e"));
  EXPECT_EQ((V{"d2"}), ids(helper, "labels.port:*"));
  EXPECT_EQ((V{"d1", "d2", "d3"}), ids(helper, "labels:*"));
  EXPECT_EQ((V{}), ids(helper, "labels.env:web"));
  // attrs.s is longer than ignore_above, so it was not indexed.
  EXPECT_EQ((V{"d3"}), ids(helper, "attrs.n:1"));
  EXPECT_EQ((V{}), ids(helper, "attrs.s:abcdefgh"));
}

TEST_F(FlattenedFieldTest, keyPathFacets) {
  CollectionHelper helper;
  indexLabels(helper);
  auto request = run(helper, R"({"app":{"field_facet":{"field":"labels.app"}},)"
                             R"("env":{"field_facet":{"field":"labels.env","mincount":0,"limit":-1}},)"
                             R"("sel":{"field_facet":{"field":"labels.env","selected":["dev"],"limit":0}}})");
  ASSERT_TRUE(request->ok()) << request->errorMsg();
  auto buckets = [&](std::string_view name) {
    const auto& result = *request->responses[0]->proto.ops.at(name)->facetResult();
    std::map<std::string, int64_t> out;
    const auto& keys = std::get<api::ColStr>(result.bucket_ids->kind).v;
    for (size_t i = 0; i < keys.size(); i++) out[std::string(keys[i])] = result.counts[i];
    return out;
  };
  using M = std::map<std::string, int64_t>;
  EXPECT_EQ((M{{"web", 2}, {"db", 1}}), buckets("app"));
  EXPECT_EQ((M{{"prod", 2}, {"dev", 1}}), buckets("env"));
  EXPECT_EQ((M{{"dev", 1}}), buckets("sel"));
}

TEST_F(FlattenedFieldTest, retrievalReturnsTheObject) {
  CollectionHelper helper;
  indexLabels(helper);
  // Without stored source the object is rebuilt from the terms, leaves as strings.
  EXPECT_EQ(R"({"app":"db","env":"prod","port":"5432","tags":["x","y"]})", retrieved(helper, "d2", "labels"));
  EXPECT_EQ(R"({"app":"web","env":"prod","tier":{"zone":"a"}})", retrieved(helper, "d1", "labels"));
  // Stored source returns the object as indexed, including ignored leaves.
  EXPECT_EQ(R"({"n":1,"s":"abcdefgh"})", retrieved(helper, "d3", "attrs"));
}

TEST_F(FlattenedFieldTest, rejectsInvalidValuesAndOperations) {
  CollectionHelper helper;
  setSchema(helper);
  auto result = indexJson(helper, {R"({"id":"a","labels":"web"})",
                                   R"({"id":"b","attrs":{"x":{"y":{"z":1}}}})",
                                   R"({"id":"c","labels":{"":1}})"});
  EXPECT_EQ(3, result.total_errors);
  for (std::string_view ops : {R"({"q":{"top_docs":{"query":{"wildcard":{"field":"labels.app","pattern":"w*b"}}}}})",
                               R"({"q":{"top_docs":{"sort":"labels"}}})",
                               R"({"q":{"top_docs":{"fields":["labels.app"]}}})",
                               R"({"f":{"field_facet":{"field":"labels.app","missing":true}}})",
                               R"({"q":{"top_docs":{"query":"id.x:1"}}})"}) {
    EXPECT_FALSE(run(helper, ops)->ok()) << ops;
  }
  SchemaBuilder b;
  auto& bad = b.field("bad");
  bad.type = FieldClass::FLATTENED;
  bad.index = api::FieldDef::IndexMode::NONE;
  EXPECT_THROW(b.build(), SchemaError);
}

} // namespace luxir::test
