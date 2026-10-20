// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <initializer_list>
#include <string>
#include <string_view>
#include <vector>

#include "luxir/api/build.h"
#include "luxir/reader/SkipStats.h"
#include "test/CollectionHelper.h"
#include "test/LocalReq.h"
#include "test/LuxirTest.h"
#include "test/SchemaBuilder.h"
#include "test/TestUtils.h"

using namespace luxir;
using namespace luxir::test;

namespace {

using FieldClass = api::FieldDef::FieldClass;
using IndexMode = api::FieldDef::IndexMode;

// 2024-01-01T00:00:00Z and 2024-02-01T00:00:00Z in epoch millis.
constexpr int64_t JAN_1 = 1704067200000;
constexpr int64_t FEB_1 = 1706745600000;

// Each doc carries the same interval in `bkd` (index=range) and `col`
// (column only), so every query checks both execution arms.
constexpr std::string_view DOCS[] = {
  R"({"id":"a","bkd":{"gte":10,"lte":20},"col":{"gte":10,"lte":20}})",
  R"({"id":"b","bkd":{"gte":15,"lte":30},"col":{"gte":15,"lte":30}})",
  R"({"id":"c","bkd":{"gte":25,"lte":40},"col":{"gte":25,"lte":40}})",
  R"({"id":"d","bkd":{"gte":50},"col":{"gte":50}})",
  R"({"id":"e","bkd":{"lte":5},"col":{"lte":5}})",
  R"({"id":"f","bkd":{"gt":20,"lt":25},"col":{"gt":20,"lt":25}})",
};

} // namespace

class RangeFieldTest : public LuxirTest {
public:
  CollectionHelper helper;
  SchemaBuilder schemaBuilder;

  void SetUp() override {
    auto& bkd = schemaBuilder.field("bkd");
    bkd.type = FieldClass::LONG_RANGE;
    bkd.index = IndexMode::RANGE;
    schemaBuilder.field("col").type = FieldClass::LONG_RANGE;
    schemaBuilder.field("price").type = FieldClass::DOUBLE_RANGE;
    auto& when = schemaBuilder.field("when");
    when.type = FieldClass::DATE_RANGE;
    when.index = IndexMode::RANGE;
    schemaBuilder.set(helper.collection());
  }

  IndexResult indexJson(std::initializer_list<std::string_view> docs, bool commit = true) {
    CollectionHelper::UpdateBuilder builder;
    for (std::string_view json : docs) {
      builder.addRaw([json](api::Map& doc, std::pmr::memory_resource& mr) {
        std::string error;
        if (!api::read_json(doc, api::build::arenaStr(mr, json), mr, &error)) {
          throw std::runtime_error("bad test doc: " + error);
        }
      });
    }
    if (commit) builder.commit();
    return helper.submit(builder);
  }

  void indexAllDocs() {
    for (std::string_view json : DOCS) ASSERT_TRUE(indexJson({json}).success);
  }

  std::vector<std::string> ids(std::string_view queryJson) {
    auto req = localReq(helper.getSearchEngine());
    auto& cursor = req->collection("main").topDocs("q");
    std::string error;
    EXPECT_TRUE(api::read_json(cursor.rawQuery(), queryJson, cursor.mr(), &error)) << error;
    cursor.fields({"id"}).limit(-1);
    req->execute();
    EXPECT_TRUE(req->ok()) << req->errorMsg();
    std::vector<std::string> out;
    for (const Doc& doc : req->getDocs()) out.push_back(std::get<std::string>(*find(doc, "id")));
    std::sort(out.begin(), out.end());
    return out;
  }

  // The same range query over both fields; the results must agree.
  std::vector<std::string> both(std::string_view bounds, std::string_view relation = "intersects") {
    auto query = [&](std::string_view field) {
      return std::string(R"({"range":{"field":")") + std::string(field) + "\"," + std::string(bounds)
          + R"(,"relation":")" + std::string(relation) + "\"}}";
    };
    auto fromBkd = ids(query("bkd"));
    EXPECT_EQ(fromBkd, ids(query("col"))) << bounds << " " << relation;
    return fromBkd;
  }
};

using Ids = std::vector<std::string>;

TEST_F(RangeFieldTest, relations) {
  indexAllDocs();
  int64_t bkdArms = SkipStats::rangeFieldBKDArms;
  // Stored: a [10,20] b [15,30] c [25,40] d [50,) e (,5] f [21,24].
  EXPECT_EQ((Ids{"a", "b", "f"}), both(R"("gte":18,"lte":22)"));
  EXPECT_EQ((Ids{"a", "b"}), both(R"("gte":16,"lte":19)", "contains"));
  EXPECT_EQ((Ids{"b", "f"}), both(R"("gte":12,"lte":35)", "within"));
  // Bounds touching an endpoint count as overlapping.
  EXPECT_EQ((Ids{"c"}), both(R"("gte":40,"lte":45)"));
  EXPECT_EQ((Ids{"b", "f"}), both(R"("gt":20,"lte":22)"));
  EXPECT_EQ((Ids{"a"}), both(R"("gte":10,"lte":20)", "within"));
  EXPECT_TRUE(both(R"("gte":30,"lte":20)").empty());
  EXPECT_GT(SkipStats::rangeFieldBKDArms, bkdArms);

  // Match and the expr language are intersects queries.
  EXPECT_EQ((Ids{"a", "b"}), ids(R"({"match":{"field":"bkd","val":17}})"));
  auto req = localReq(helper.getSearchEngine());
  req->collection("main").topDocs("q").exprQuery("col:[18 TO 22]").fields({"id"}).limit(-1);
  req->execute();
  EXPECT_TRUE(req->ok()) << req->errorMsg();
  EXPECT_EQ(3, req->getMatchCount());
}

TEST_F(RangeFieldTest, openEnds) {
  indexAllDocs();
  EXPECT_EQ((Ids{"d"}), both(R"("gte":45)"));
  EXPECT_EQ((Ids{"e"}), both(R"("lte":0)"));
  // Open stored ends are unbounded: d contains every value above 50.
  EXPECT_EQ((Ids{"d"}), both(R"("gte":1000000,"lte":2000000)", "contains"));
  EXPECT_EQ((Ids{"a", "b", "c", "d", "f"}), both(R"("gte":0)", "within"));
  EXPECT_EQ((Ids{"a", "b", "c", "d", "e", "f"}), ids(R"({"exists":{"field":"bkd"}})"));
}

TEST_F(RangeFieldTest, retrieval) {
  ASSERT_TRUE(indexJson({
    R"({"id":"a","bkd":{"gte":10,"lt":21},"price":{"gte":1.5,"lte":2.5},"when":{"gte":"2024-01-01","lt":"2024-02-01"}})",
    R"({"id":"b","bkd":{"gte":50},"when":{"gte":"2024-01-01","lte":"2024-01"}})",
  }).success);
  auto req = localReq(helper.getSearchEngine());
  req->collection("main").topDocs("q").allQuery().fields({"id", "bkd", "price", "when"}).limit(-1);
  req->execute();
  ASSERT_TRUE(req->ok()) << req->errorMsg();
  const auto* docs = req->docList();
  ASSERT_NE(nullptr, docs);
  ASSERT_EQ(2u, docs->docs.size());
  auto bound = [](const api::Map& row, std::string_view field, std::string_view key) -> const api::Val* {
    const auto* value = row.fields.find(field);
    if (value == nullptr) return nullptr;
    const auto* map = std::get_if<api::Map>(&(**value).kind);
    if (map == nullptr) return nullptr;
    const auto* found = map->fields.find(key);
    return found ? &**found : nullptr;
  };
  // Rows follow index order for an all query over one shard.
  const api::Map& a = docs->docs[0];
  const api::Map& b = docs->docs[1];
  EXPECT_EQ(10, bound(a, "bkd", "gte")->asInt());
  EXPECT_EQ(20, bound(a, "bkd", "lte")->asInt());
  EXPECT_DOUBLE_EQ(1.5, bound(a, "price", "gte")->asDouble());
  EXPECT_DOUBLE_EQ(2.5, bound(a, "price", "lte")->asDouble());
  EXPECT_EQ(JAN_1, bound(a, "when", "gte")->asInt());
  EXPECT_EQ(FEB_1 - 1, bound(a, "when", "lte")->asInt());
  // An open end is omitted; a partial date names its whole window.
  EXPECT_EQ(50, bound(b, "bkd", "gte")->asInt());
  EXPECT_EQ(nullptr, bound(b, "bkd", "lte"));
  EXPECT_EQ(nullptr, b.fields.find("price"));
  EXPECT_EQ(FEB_1 - 1, bound(b, "when", "lte")->asInt());

  // Date query bounds round the same way.
  EXPECT_EQ((Ids{"a", "b"}), ids(R"({"range":{"field":"when","gte":"2024-01","lte":"2024-01","relation":"within"}})"));
  EXPECT_EQ((Ids{"a"}), ids(R"({"range":{"field":"price","gte":2.0,"lte":2.0,"relation":"intersects"}})"));
}

TEST_F(RangeFieldTest, mergeAndDeletes) {
  indexAllDocs();  // one segment per doc
  ASSERT_TRUE(helper.deleteById("b", UpdateMessage::COMMIT).success);
  EXPECT_EQ((Ids{"a", "f"}), both(R"("gte":18,"lte":22)"));

  CollectionHelper::UpdateBuilder merge;
  merge.commit(true, 1);
  ASSERT_TRUE(helper.submit(merge).success);
  EXPECT_EQ(1u, helper.getIndexWriter()->snapshots.readers.getReader()->segments().size());
  EXPECT_EQ((Ids{"a", "f"}), both(R"("gte":18,"lte":22)"));
  EXPECT_EQ((Ids{"f"}), both(R"("gte":12,"lte":35)", "within"));
  EXPECT_EQ((Ids{"d"}), both(R"("gte":45)"));
  EXPECT_EQ((Ids{"e"}), both(R"("lte":0)"));
}

TEST_F(RangeFieldTest, invalidValuesAndSchemas) {
  auto result = indexJson({
    R"({"id":"empty","bkd":{"gte":5,"lte":1}})",
    R"({"id":"scalar","bkd":5})",
    R"({"id":"key","bkd":{"from":1}})",
    R"({"id":"both","bkd":{"gte":1,"gt":2}})",
    R"({"id":"ok","bkd":{"gte":1,"lte":2}})",
  });
  EXPECT_EQ(4, result.total_errors);
  EXPECT_EQ((Ids{"ok"}), ids(R"({"all":true})"));

  // relation only applies to range fields.
  auto req = localReq(helper.getSearchEngine());
  auto& cursor = req->collection("main").topDocs("q");
  ASSERT_TRUE(api::read_json(cursor.rawQuery(),
                             R"({"range":{"field":"id","gte":"a","relation":"within"}})", cursor.mr()));
  req->execute();
  EXPECT_FALSE(req->ok());

  SchemaBuilder multi;
  auto& field = multi.field("spans");
  field.type = FieldClass::LONG_RANGE;
  field.multi = true;
  EXPECT_ANY_THROW(multi.set(helper.collection()));
  SchemaBuilder matched;
  auto& other = matched.field("spans");
  other.type = FieldClass::DATE_RANGE;
  other.index = IndexMode::MATCH;
  EXPECT_ANY_THROW(matched.set(helper.collection()));
}
