// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include <gtest/gtest.h>

#include "luxir/query/QueryBuilder.h"
#include "luxir/server/JsonRequest.h"
#include "luxir/search/SortField.h"
#include "luxir/value/ValueExprParser.h"
#include "luxir/index/PostingsWriter.h"
#include "luxir/reader/FieldReader.h"
#include "luxir/search/IndexReader.h"
#include "test/CollectionHelper.h"
#include "test/LocalReq.h"
#include "test/LuxirTest.h"
#include "test/SchemaBuilder.h"
#include "test/TestUtils.h"

namespace luxir::test {
namespace {

// Re-declaring a type on a live field is rejected, so drop it first. Segments written
// before the drop keep the old type, which is the mismatch these tests need.
void setValueType(CollectionHelper& helper, api::FieldDef::FieldClass type) {
  SchemaBuilder removed;
  removed.replaceAll(helper.collection());
  SchemaBuilder schema;
  schema.field("value").type = type;
  schema.set(helper.collection());
}

IndexResult forceMerge(CollectionHelper& helper) {
  CollectionHelper::UpdateBuilder request;
  request.commit(true, 1);
  return helper.submit(request);
}

} // namespace

class FieldTypeContractTest : public LuxirTest {};

TEST_F(FieldTypeContractTest, operationCapabilities) {
  using F = FieldType;
  const uint32_t terms = F::MATCH | F::ANY_OF | F::PREFIX | F::PATTERN | F::FUZZY | F::FREE_TEXT
      | F::TERM_FILTER | F::RANGE | F::SORT | F::FIELD_FACET | F::COLLAPSE | F::VALUE_EXPR;
  const uint32_t text = F::MATCH | F::ANY_OF | F::PREFIX | F::PATTERN | F::FUZZY | F::FREE_TEXT | F::RANGE | F::FIELD_FACET;
  const uint32_t numeric = F::MATCH | F::ANY_OF | F::RANGE | F::SORT | F::FIELD_FACET | F::RANGE_FACET | F::VALUE_EXPR;
  struct Case { std::shared_ptr<FieldType> type; uint32_t expected; std::string_view literal = "7"; };
  const Case cases[] = {
      {std::make_shared<FieldType>("none", F::NONE, 0), 0},
      {std::make_shared<StrFieldType>("string"), terms},
      {std::make_shared<IpFieldType>("ip"), terms & ~(F::PREFIX | F::PATTERN | F::FUZZY | F::FREE_TEXT | F::TERM_FILTER), "10.0.0.1"},
      {std::make_shared<TextFieldType>("text"), text},
      {std::make_shared<StoredFieldType>("bin"), 0},
      {std::make_shared<IntFieldType>("int"), numeric | F::COLLAPSE},
      {std::make_shared<FloatFieldType>("float"), numeric},
      {std::make_shared<DoubleFieldType>("double"), numeric},
      {std::make_shared<IdFieldType>("id"), terms},
      {std::make_shared<VectorFieldType>("vector", 3), 0},
      {std::make_shared<DateFieldType>("date"), numeric | F::COLLAPSE},
      {std::make_shared<DecimalFieldType>("decimal", 2), numeric | F::COLLAPSE},
      {std::make_shared<GeoPointFieldType>("geo"), F::VALUE_EXPR}};
  CollectionHelper helper;
  auto schema = Schema::createDefaultSchema();
  for (const auto& c : cases) schema->fieldTypeMap[c.type->name_] = c.type;
  helper.collection().setSchema(schema);
  for (const auto& c : cases) {
    SCOPED_TRACE(c.type->name_);
    EXPECT_EQ(c.expected, c.type->capabilities());
    auto flags = c.type->flags_;
    c.type->flags_ ^= F::INDEX_DOCS | F::COLUMN_STORED;
    EXPECT_EQ(c.expected, c.type->capabilities());
    c.type->flags_ = flags;
    auto catalog = schema->searchableFields();
    auto storage = FieldType::storage(c.type->type());
    EXPECT_EQ((c.expected & F::MATCH) != 0 && (storage == FieldType::Storage::TERMS || storage == FieldType::Storage::TEXT),
              std::ranges::contains(catalog, c.type->name_));
    for (auto capability : {F::MATCH, F::ANY_OF, F::PREFIX, F::PATTERN, F::FUZZY, F::RANGE,
                           F::SORT, F::FIELD_FACET, F::RANGE_FACET, F::COLLAPSE, F::VALUE_EXPR, F::TERM_FILTER}) {
      SCOPED_TRACE(capability);
      bool expected = (c.expected & capability) != 0;
      EXPECT_EQ(expected, c.type->supports(capability));
      try {
        MemPool pool;
        QueryBuilder builder(pool, *schema, CoerceContext{});
        auto name = c.type->name_;
        api::Val value = coerce::scalarVal(c.literal);
        std::string op;
        switch (capability) {
          case F::MATCH: builder.createMatchQuery(name, value); break;
          case F::ANY_OF: {
            api::Val values;
            values.kind.emplace<api::ArrVal>().v = {&value, 1};
            builder.createAnyOfQuery(builder.canonicalizeFieldValues(name, ValueSequence(values, "values")));
            break;
          }
          case F::PREFIX: builder.createPrefixQuery(name, "7"); break;
          case F::PATTERN:
            builder.createWildcardQuery(name, "7*");
            builder.createRegexQuery(name, "7.*");
            break;
          case F::FUZZY: builder.createFuzzyQuery(name, "7"); break;
          case F::RANGE: builder.createRangeQuery(name, &value, nullptr, nullptr, nullptr); break;
          case F::SORT: { SortField field(name, *c.type); break; }
          case F::VALUE_EXPR: {
            google::protobuf::Arena arena;
            ValueExprParser(ValueExprOptions{schema.get(), {}}, arena).parse("count(" + name + ")");
            break;
          }
          case F::FIELD_FACET: op = R"({"field_facet":{"field":")" + name + R"("}})"; break;
          case F::RANGE_FACET: op = R"({"range_facet":{"field":")" + name + R"(","start":0,"end":10,"gap":1}})"; break;
          case F::TERM_FILTER: op = R"({"field_facet":{"field":")" + name + R"(","term_filter":{"prefix":"7"}}})"; break;
          case F::COLLAPSE: op = R"({"top_docs":{"query":"*:*","limit":0,"collapse":{"field":[")" + name + R"("]}}})"; break;
          default: FAIL() << "unhandled capability";
        }
        if (!op.empty()) {
          auto req = localReq(helper.getSearchEngine());
          parseQueryRequest(R"({"ops":{"q":)" + op + "}}", req->rawRequest(), req->mr);
          req->execute(false);
          EXPECT_EQ(expected, req->ok()) << req->errorMsg();
        } else EXPECT_TRUE(expected);
      } catch (const std::exception& error) {
        EXPECT_FALSE(expected) << error.what();
      }
    }
  }
}

TEST_F(FieldTypeContractTest, textCapabilityRemedies) {
  TextFieldType text("text");
  for (auto capability : {FieldType::SORT, FieldType::VALUE_EXPR, FieldType::COLLAPSE, FieldType::RANGE_FACET}) {
    SCOPED_TRACE(capability);
    try {
      text.requireCapability(capability, "operation", "title");
      FAIL() << "expected unsupported operation";
    } catch (const RequestError& error) {
      EXPECT_EQ("operation is not supported on text field 'title'; use a string variant", std::string(error.what()));
    }
  }
  EXPECT_TRUE(text.capabilityRemedy(FieldType::TERM_FILTER).empty());
  EXPECT_TRUE(IntFieldType("int").capabilityRemedy(FieldType::PREFIX).empty());
}

TEST_F(FieldTypeContractTest, typeFlagsSurviveFlushMergeAndReopen) {
  CollectionHelper helper;
  SchemaBuilder schema;
  for (auto name : {"nano", "nanos"}) {
    auto& field = schema.field(name);
    field.type = api::FieldDef::FieldClass::DATE;
    field.unit = api::FieldDef::DateUnit::NS;
    field.multi = name == std::string_view("nanos") ? api::FieldDef::MultiMode::DEFAULT : api::FieldDef::MultiMode::SINGLE;
  }
  schema.set(helper.collection());
  auto writer = helper.getIndexWriter();
  writer->mergePolicy->setMergeFactor(100);
  for (auto id : {"one", "two"}) {
    ASSERT_TRUE(helper.index(flatdoc("id", id, "n_i", 7, "when_dt", (int64_t)123,
        "nano", 7, "nanos", vec_i(1, 2), "text_w", "words", "many_is", vec_i(1, 2)), UpdateMessage::COMMIT).success);
  }
  auto check = [&] {
    auto reopened = readDurableIndexInfo(writer->dir);
    for (const auto& segment : reopened->segments) {
      PostingsReader postings(writer->dir, segment.seg_id, segment.creator_epoch);
      FieldReader fields(postings);
      while (fields.readNextField()) {
        SegFieldInfo info{};
        fields.readFieldInfo(info);
        auto name = (std::string_view)info.fieldname;
        uint32_t expected = name == "nano" || name == "nanos" ? 1 : 0;
        EXPECT_EQ(expected, info.typeFlags) << name;
        EXPECT_EQ(expected, FieldType::typeParameters(info.type, info.typeFlags));
      }
      EXPECT_EQ(FieldType::ID, postings.fieldInfo("id")->type);
      EXPECT_EQ(FieldType::DATE, postings.fieldInfo("when_dt")->type);
      EXPECT_EQ(0u, postings.fieldInfo("when_dt")->typeFlags);
    }
  };
  check();
  ASSERT_TRUE(forceMerge(helper).success);
  ASSERT_EQ(1u, helper.durableSegmentCount());
  check();
}

TEST_F(FieldTypeContractTest, unknownTypeFlagsAreRejectedWithoutSchema) {
  for (int type = FieldType::NONE; type <= FieldType::IP; type++) {
    SCOPED_TRACE(FieldType::typeName((FieldType::Type)type));
    RAMDir dir;
    PostingsWriter writer(dir, 1, 1, 0);
    auto& info = writer.addField("value");
    info.type = (FieldType::Type)type;
    info.typeFlags = type == FieldType::DECIMAL ? 19 : type == FieldType::DATE ? 2 : 1; // deliberately invalid persisted metadata
    writeUnversionedRuns(writer);
    writer.finish();
    PostingsReader reader(dir, 1, 1);
    FieldReader fields(reader);
    ASSERT_TRUE(fields.readNextField());
    SegFieldInfo decoded{};
    EXPECT_THROW(fields.readFieldInfo(decoded), std::runtime_error);
    EXPECT_THROW(reader.fieldInfo("value"), std::runtime_error);
  }
}

TEST_F(FieldTypeContractTest, changedDateUnitFailsOnlyFieldUsers) {
  CollectionHelper helper;
  SchemaBuilder first;
  auto& ns = first.field("value");
  ns.type = api::FieldDef::FieldClass::DATE;
  ns.unit = api::FieldDef::DateUnit::NS;
  first.set(helper.collection());
  ASSERT_TRUE(helper.index(flatdoc("id", "one", "value", 7), UpdateMessage::COMMIT).success);
  auto writer = helper.getIndexWriter();
  auto before = writer->snapshots.readers.getReader();
  EXPECT_EQ(1u, before->field("value").at(0)->typeFlags);

  SchemaBuilder removed;
  removed.replaceAll(helper.collection());
  setValueType(helper, api::FieldDef::FieldClass::DATE);
  IndexReader reopened(writer->dir);
  EXPECT_EQ((std::vector<std::string>{"one"}), allIds(helper));
  EXPECT_EQ(1u, before->field("value").at(0)->typeFlags);
  EXPECT_THROW(reopened.field("value"), RequestError);
  for (bool retrieve : {true, false}) {
    auto request = localReq(helper.getSearchEngine());
    auto& op = request->collection("main").topDocs("q");
    if (retrieve) op.allQuery().fields({"value"});
    else op.matchQuery("value", "7").fields({"id"});
    request->execute();
    ASSERT_FALSE(request->ok());
    EXPECT_NE(std::string::npos, request->errorMsg().find("schema type 'date' unit 'ms'"));
    EXPECT_NE(std::string::npos, request->errorMsg().find("persisted type 'date' unit 'ns'"));
  }
  EXPECT_EQ((std::vector<std::string>{"one"}), allIds(helper));
}

TEST_F(FieldTypeContractTest, mixedDateUnitsRefuseMerge) {
  CollectionHelper helper;
  auto writer = helper.getIndexWriter();
  writer->mergePolicy->setMergeFactor(100);
  setValueType(helper, api::FieldDef::FieldClass::DATE);
  ASSERT_TRUE(helper.index(flatdoc("id", "old", "value", 1), UpdateMessage::COMMIT).success);
  SchemaBuilder removed;
  removed.replaceAll(helper.collection());
  SchemaBuilder schema;
  auto& field = schema.field("value");
  field.type = api::FieldDef::FieldClass::DATE;
  field.unit = api::FieldDef::DateUnit::NS;
  schema.set(helper.collection());
  ASSERT_TRUE(helper.index(flatdoc("id", "new", "value", 2), UpdateMessage::COMMIT).success);
  ExpectLog quiet("Incompatible persisted types for field 'value'");
  auto result = forceMerge(helper);
  EXPECT_FALSE(result.success);
  EXPECT_NE(std::string::npos, result.error_message.find("type 'date' unit 'ms'"));
  EXPECT_NE(std::string::npos, result.error_message.find("type 'date' unit 'ns'"));
  EXPECT_FALSE(writer->testMergeRunning());
  EXPECT_EQ(2u, helper.durableSegmentCount());
  EXPECT_EQ(2u, allIds(helper).size());
}

TEST_F(FieldTypeContractTest, schemaMismatchFailsOnlyFieldUsers) {
  CollectionHelper helper;
  setValueType(helper, api::FieldDef::FieldClass::INT);
  ASSERT_TRUE(helper.index(flatdoc("id", "one", "value", 7), UpdateMessage::COMMIT).success);
  auto writer = helper.getIndexWriter();
  auto before = writer->snapshots.readers.getReader();
  EXPECT_EQ(FieldType::INT, before->field("value").at(0)->type);

  setValueType(helper, api::FieldDef::FieldClass::DATE);
  auto after = writer->snapshots.readers.getReader();
  EXPECT_EQ(before->coreGen(), after->coreGen());
  EXPECT_EQ(FieldType::INT, before->field("value").at(0)->type);
  EXPECT_THROW(after->field("value"), RequestError);
  EXPECT_EQ((std::vector<std::string>{"one"}), allIds(helper));

  for (bool retrieve : {true, false}) {
    auto request = localReq(helper.getSearchEngine());
    auto& op = request->collection("main").topDocs("q");
    if (retrieve) op.allQuery().fields({"value"});
    else op.matchQuery("value", "7").fields({"id"});
    request->execute();
    ASSERT_FALSE(request->ok());
    EXPECT_NE(std::string::npos, request->errorMsg().find("schema type 'date'"));
    EXPECT_NE(std::string::npos, request->errorMsg().find("persisted type 'int'"));
  }
  EXPECT_EQ((std::vector<std::string>{"one"}), allIds(helper));
}

TEST_F(FieldTypeContractTest, refusedMergeReleasesPolicyAndOtherMergesProceed) {
  CollectionHelper helper;
  auto writer = helper.getIndexWriter();
  writer->mergePolicy->setMergeFactor(100);
  setValueType(helper, api::FieldDef::FieldClass::INT);
  ASSERT_TRUE(helper.index(flatdoc("id", "old", "value", 1), UpdateMessage::COMMIT).success);
  setValueType(helper, api::FieldDef::FieldClass::DATE);
  ASSERT_TRUE(helper.index(flatdoc("id", "new", "value", 2), UpdateMessage::COMMIT).success);

  ExpectLog quiet("Incompatible persisted types for field 'value'");
  auto refused = forceMerge(helper);
  EXPECT_FALSE(refused.success);
  EXPECT_NE(std::string::npos, refused.error_message.find("Incompatible persisted types"));
  EXPECT_NE(std::string::npos, refused.error_message.find("'int'"));
  EXPECT_NE(std::string::npos, refused.error_message.find("'date'"));
  ASSERT_EQ(2u, helper.durableSegmentCount());
  EXPECT_FALSE(writer->testMergeRunning());
  auto failure = writer->testLastMergeFailure();
  ASSERT_TRUE(failure.has_value());
  EXPECT_FALSE(failure->outputPublished);
  EXPECT_EQ("segment_merge", failure->phase);

  // Two larger compatible segments can merge at another level even while
  // the original incompatible pair remains present at level zero.
  writer->mergePolicy->setMergeFactor(2);
  for (int segment = 0; segment < 2; segment++) {
    CollectionHelper::UpdateBuilder request;
    for (int i = 0; i < 2; i++) request.add(flatdoc(
        "id", std::format("large{}-{}", segment, i), "value", 3));
    request.commit(true);
    ASSERT_TRUE(helper.submit(request).success);
  }
  EXPECT_FALSE(writer->testMergeRunning());
  EXPECT_EQ(3u, helper.durableSegmentCount());
  EXPECT_EQ(6u, allIds(helper).size());

  ASSERT_TRUE(helper.deleteById("old", UpdateMessage::COMMIT).success);
  ASSERT_TRUE(forceMerge(helper).success);
  EXPECT_FALSE(writer->testMergeRunning());
  EXPECT_EQ(1u, helper.durableSegmentCount());
  EXPECT_EQ(5u, allIds(helper).size());
}

} // namespace luxir::test
