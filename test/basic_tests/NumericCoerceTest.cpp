// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

// Empty strings on numeric fields at ingest (the coerce schema option).

#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <string_view>
#include <vector>

#include "test/CollectionHelper.h"
#include "test/LocalReq.h"
#include "test/LuxirTest.h"
#include "test/QueryBuild.h"
#include "test/SchemaBuilder.h"
#include "test/TestUtils.h"

using namespace luxir;
using namespace luxir::test;

using FieldClass = api::FieldDef::FieldClass;
using Ids = std::vector<std::string>;

class NumericCoerceTest : public LuxirTest {
public:
  CollectionHelper helper;

  // Submit documents read from JSON; the literals outlive the submit.
  IndexResult submit(std::initializer_list<std::string_view> docs) {
    CollectionHelper::UpdateBuilder update;
    for (auto json : docs) {
      update.addRaw([json](api::Map& doc, std::pmr::memory_resource& mr) {
        std::string error;
        if (!api::read_json(doc, json, mr, &error)) throw std::runtime_error("bad test JSON: " + error);
      });
    }
    update.commit();
    return helper.submit(update);
  }

  Ids run(auto&& setQuery) {
    auto req = localReq(helper.getSearchEngine());
    auto& cur = req->collection("main").topDocs("q");
    setQuery(cur);
    cur.fields({"id"}).limit(50);
    req->execute();
    EXPECT_OK(req);
    Ids found;
    for (const auto& doc : req->getDocs()) found.push_back(std::get<std::string>(*find(doc, "id")));
    std::sort(found.begin(), found.end());
    return found;
  }
  Ids withField(std::string_view field) {
    return run([&](auto& cur) { cur.rawQuery() = qb::exists(cur.mr(), field); });
  }
  Ids allIds() {
    return run([](auto& cur) { cur.allQuery(); });
  }

  void expectRejected(std::string_view json, std::string_view message) {
    auto result = submit({json});
    ASSERT_NE(IndexResult::Status::OK, result.status) << json;
    ASSERT_EQ(1u, result.errors.size()) << json;
    EXPECT_NE(std::string::npos, result.errors[0].error_message.find(message)) << result.errors[0].error_message;
  }
};

TEST_F(NumericCoerceTest, emptyStringIsNoValueForEachNumericType) {
  SchemaBuilder schema;
  schema.field("i").type = FieldClass::INT;
  schema.field("f").type = FieldClass::FLOAT;
  schema.field("d").type = FieldClass::DOUBLE;
  schema.set(helper.collection());
  auto result = submit({R"({"id": "empty", "i": "", "f": "", "d": ""})",
                        R"({"id": "set", "i": "5", "f": "1.5", "d": "2.5"})"});
  ASSERT_EQ(IndexResult::Status::OK, result.status);
  EXPECT_EQ((Ids{"empty", "set"}), allIds());
  for (auto field : {"i", "f", "d"}) EXPECT_EQ((Ids{"set"}), withField(field)) << field;
}

TEST_F(NumericCoerceTest, coerceFalseKeepsTheError) {
  SchemaBuilder schema;
  auto& strict = schema.field("i");
  strict.type = FieldClass::INT;
  strict.coerce = false;
  auto& strictFloat = schema.field("f");
  strictFloat.type = FieldClass::FLOAT;
  strictFloat.coerce = false;
  auto& strictDouble = schema.field("d");
  strictDouble.type = FieldClass::DOUBLE;
  strictDouble.coerce = false;
  schema.set(helper.collection());
  expectRejected(R"({"id": "a", "i": ""})", "cannot parse '' as an integer");
  expectRejected(R"({"id": "b", "f": ""})", "cannot parse '' as a number");
  expectRejected(R"({"id": "c", "d": ""})", "cannot parse '' as a number");
}

TEST_F(NumericCoerceTest, whitespaceAndGarbageStayErrors) {
  SchemaBuilder schema;
  schema.field("i").type = FieldClass::INT;
  schema.field("d").type = FieldClass::DOUBLE;
  schema.set(helper.collection());
  expectRejected(R"({"id": "a", "i": " "})", "cannot parse");
  expectRejected(R"({"id": "b", "d": "  "})", "cannot parse");
  expectRejected(R"({"id": "c", "i": "abc"})", "cannot parse 'abc'");
}

TEST_F(NumericCoerceTest, dateRejectsEmptyStringAndHasNoCoerceOption) {
  SchemaBuilder schema;
  schema.field("t").type = FieldClass::DATE;
  schema.set(helper.collection());
  auto result = submit({R"({"id": "a", "t": ""})"});
  EXPECT_NE(IndexResult::Status::OK, result.status);

  SchemaBuilder invalid;
  auto& date = invalid.field("t2");
  date.type = FieldClass::DATE;
  date.coerce = true;
  EXPECT_THROW(invalid.set(helper.collection()), std::exception);
}

TEST_F(NumericCoerceTest, multiValuedDropsEmptyElements) {
  SchemaBuilder schema;
  auto& many = schema.field("many");
  many.type = FieldClass::INT;
  many.multi = api::FieldDef::MultiMode::DEFAULT;
  schema.set(helper.collection());
  auto result = submit({R"({"id": "mixed", "many": ["", "7", ""]})",
                        R"({"id": "vals", "many": ["", 7]})",
                        R"({"id": "none", "many": ["", ""]})"});
  ASSERT_EQ(IndexResult::Status::OK, result.status);
  EXPECT_EQ((Ids{"mixed", "vals"}), withField("many"));
  EXPECT_EQ((Ids{"mixed", "vals"}), run([](auto& cur) { cur.matchQuery("many", "7"); }));
}

TEST_F(NumericCoerceTest, variantsFollowTheLogicalField) {
  SchemaBuilder schema;
  auto& number = schema.field("n");
  number.type = FieldClass::INT;
  schema.variant(number, "s").type = FieldClass::STRING;
  auto& text = schema.field("s");
  text.type = FieldClass::STRING;
  schema.variant(text, "n").type = FieldClass::INT;
  schema.set(helper.collection());
  auto result = submit({R"({"id": "a", "n": "", "s": ""})"});
  ASSERT_EQ(IndexResult::Status::OK, result.status);
  // Numeric logical field: the value is absent on every branch.
  EXPECT_TRUE(withField("n").empty());
  EXPECT_TRUE(withField("n__s").empty());
  // String logical field: the empty string is a value, the numeric branch has none.
  EXPECT_EQ((Ids{"a"}), withField("s"));
  EXPECT_TRUE(withField("s__n").empty());
}

TEST_F(NumericCoerceTest, queriesStillRejectAnEmptyString) {
  SchemaBuilder schema;
  schema.field("i").type = FieldClass::INT;
  schema.set(helper.collection());
  auto req = localReq(helper.getSearchEngine());
  req->collection("main").topDocs("q").matchQuery("i", "").fields({"id"});
  req->execute();
  EXPECT_FALSE(req->ok());
}
