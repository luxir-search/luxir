// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include <gtest/gtest.h>

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

void setValueType(CollectionHelper& helper, api::FieldDef::FieldClass type) {
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

TEST_F(FieldTypeContractTest, typeFlagsSurviveFlushMergeAndReopen) {
  CollectionHelper helper;
  auto writer = helper.getIndexWriter();
  writer->mergePolicy->setMergeFactor(100);
  for (auto id : {"one", "two"}) {
    ASSERT_TRUE(helper.index(flatdoc("id", id, "n_i", 7, "when_dt", (int64_t)123,
        "text_w", "words", "many_is", vec_i(1, 2)), UpdateMessage::COMMIT).success);
  }
  auto check = [&] {
    IndexReader reopened(writer->dir);
    for (auto& segment : reopened.segments()) {
      PostingsReader postings(writer->dir, segment.segInfo.seg_id);
      FieldReader fields(postings);
      while (fields.readNextField()) {
        SegFieldInfo info{};
        fields.readFieldInfo(info);
        EXPECT_EQ(0u, info.typeFlags) << (std::string_view)info.fieldname;
        EXPECT_EQ(0u, FieldType::typeParameters(info.type, info.typeFlags));
      }
      EXPECT_EQ(FieldType::STRING, postings.fieldInfo("id")->type);
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
  for (int type = FieldType::NONE; type <= FieldType::GEO_POINT; type++) {
    SCOPED_TRACE(FieldType::typeName((FieldType::Type)type));
    RAMDir dir;
    PostingsWriter writer(dir, 1, 0);
    auto& info = writer.addField("value");
    info.type = (FieldType::Type)type;
    info.typeFlags = 1; // deliberately invalid persisted metadata
    writeUnversionedRuns(writer);
    writer.finish();
    PostingsReader reader(dir, 1);
    FieldReader fields(reader);
    ASSERT_TRUE(fields.readNextField());
    SegFieldInfo decoded{};
    EXPECT_THROW(fields.readFieldInfo(decoded), std::runtime_error);
    EXPECT_THROW(reader.fieldInfo("value"), std::runtime_error);
  }
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
