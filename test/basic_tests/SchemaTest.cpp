
#include <gtest/gtest.h>

#include "solux/schema/Schema.h"
#include "solux/schema/FieldType.h"
#include "protos/solux_types.pb.h"
#include "test/SoluxTest.h"
#include "test/CollectionHelper.h"
#include "test/LocalReq.h"
#include "test/TestUtils.h"

using namespace solux;
using namespace solux::test;

class SchemaTest : public SoluxTest {};


TEST_F(SchemaTest, defaultSchema) {
  auto schema = Schema::createDefaultSchema();

  // Concrete fields should be found by exact name
  ASSERT_NE(nullptr, schema->getFieldTypePtr("id"));
  ASSERT_NE(nullptr, schema->getFieldTypePtr("_version_"));
  EXPECT_EQ(FieldType::STRING, schema->getFieldTypePtr("id")->type());
  EXPECT_EQ(FieldType::INT, schema->getFieldTypePtr("_version_")->type());
  EXPECT_FALSE(schema->getFieldTypePtr("id")->isAbstract());
  EXPECT_FALSE(schema->getFieldTypePtr("_version_")->isAbstract());

  // Abstract dynamic fields should NOT be found by exact name
  ASSERT_EQ(nullptr, schema->getFieldTypePtr("_s"));
  ASSERT_EQ(nullptr, schema->getFieldTypePtr("_w"));
  ASSERT_EQ(nullptr, schema->getFieldTypePtr("_wl"));
  ASSERT_EQ(nullptr, schema->getFieldTypePtr("_i"));

  // But suffix matching should work
  ASSERT_NE(nullptr, schema->getFieldTypePtr("title_s"));
  ASSERT_NE(nullptr, schema->getFieldTypePtr("body_w"));
  ASSERT_NE(nullptr, schema->getFieldTypePtr("body_wl"));
  ASSERT_NE(nullptr, schema->getFieldTypePtr("count_i"));
  EXPECT_EQ(FieldType::STRING, schema->getFieldTypePtr("title_s")->type());
  EXPECT_EQ(FieldType::TEXT, schema->getFieldTypePtr("body_w")->type());
  EXPECT_EQ(FieldType::INT, schema->getFieldTypePtr("count_i")->type());
}


TEST_F(SchemaTest, fromProtoBasic) {
  proto::SchemaDef def;

  auto* f = def.add_fields();
  f->set_name("title");
  f->set_field_class(proto::FieldDef::TEXT);
  f->set_indexed(true);
  f->mutable_analyzer()->set_tokenizer("whitespace");
  f->mutable_analyzer()->add_filters("lowercase");

  auto* f2 = def.add_fields();
  f2->set_name("price");
  f2->set_field_class(proto::FieldDef::INT);
  f2->set_column_stored(true);

  auto schema = Schema::fromProto(def);

  auto* title = schema->getFieldTypePtr("title");
  ASSERT_NE(nullptr, title);
  EXPECT_EQ(FieldType::TEXT, title->type());
  EXPECT_TRUE(title->indexed());
  EXPECT_FALSE(title->isAbstract());

  auto* price = schema->getFieldTypePtr("price");
  ASSERT_NE(nullptr, price);
  EXPECT_EQ(FieldType::INT, price->type());
  EXPECT_TRUE(price->hasColumn());
}


TEST_F(SchemaTest, inheritance) {
  proto::SchemaDef def;

  // Parent: analyzed text with whitespace+lowercase
  auto* parent = def.add_fields();
  parent->set_name("_wl");
  parent->set_field_class(proto::FieldDef::TEXT);
  parent->set_indexed(true);
  parent->set_abstract(true);
  parent->mutable_analyzer()->set_tokenizer("whitespace");
  parent->mutable_analyzer()->add_filters("lowercase");

  // Child inherits from _wl
  auto* child = def.add_fields();
  child->set_name("title");
  child->set_parent("_wl");

  auto schema = Schema::fromProto(def);

  // title should inherit TEXT type and analyzer from _wl
  auto* title = schema->getFieldTypePtr("title");
  ASSERT_NE(nullptr, title);
  EXPECT_EQ(FieldType::TEXT, title->type());
  EXPECT_TRUE(title->indexed());
  EXPECT_FALSE(title->isAbstract());

  // _wl is abstract, should not be found by exact name
  EXPECT_EQ(nullptr, schema->getFieldTypePtr("_wl"));
  // But suffix matching should work
  EXPECT_NE(nullptr, schema->getFieldTypePtr("body_wl"));
}


TEST_F(SchemaTest, inheritanceOverride) {
  proto::SchemaDef def;

  auto* parent = def.add_fields();
  parent->set_name("_wl");
  parent->set_field_class(proto::FieldDef::TEXT);
  parent->set_indexed(true);
  parent->set_abstract(true);
  parent->mutable_analyzer()->set_tokenizer("whitespace");
  parent->mutable_analyzer()->add_filters("lowercase");

  // Child overrides the analyzer
  auto* child = def.add_fields();
  child->set_name("title");
  child->set_parent("_wl");
  child->mutable_analyzer()->set_tokenizer("keyword");

  auto schema = Schema::fromProto(def);

  auto* title = schema->getFieldTypePtr("title");
  ASSERT_NE(nullptr, title);
  EXPECT_EQ(FieldType::TEXT, title->type());

  // Check that the analyzer was overridden
  auto* textFt = (TextFieldType*)(title);
  EXPECT_EQ("keyword", textFt->tokenizer_);
  EXPECT_TRUE(textFt->filters_.empty());  // override is atomic, so parent's filters are not kept
}


TEST_F(SchemaTest, circularInheritance) {
  proto::SchemaDef def;

  auto* a = def.add_fields();
  a->set_name("a");
  a->set_parent("b");
  a->set_field_class(proto::FieldDef::STRING);

  auto* b = def.add_fields();
  b->set_name("b");
  b->set_parent("a");
  b->set_field_class(proto::FieldDef::STRING);

  EXPECT_THROW(Schema::fromProto(def), std::runtime_error);
}


TEST_F(SchemaTest, mergeMode) {
  // Start with a base schema
  proto::SchemaDef baseDef;
  auto* f1 = baseDef.add_fields();
  f1->set_name("title");
  f1->set_field_class(proto::FieldDef::TEXT);
  f1->set_indexed(true);
  f1->mutable_analyzer()->set_tokenizer("whitespace");

  auto* f2 = baseDef.add_fields();
  f2->set_name("author");
  f2->set_field_class(proto::FieldDef::STRING);

  auto baseSchema = Schema::fromProto(baseDef);

  // Merge: add a new field, existing "author" should survive
  proto::SchemaDef mergeDef;
  auto* f3 = mergeDef.add_fields();
  f3->set_name("price");
  f3->set_field_class(proto::FieldDef::INT);
  f3->set_column_stored(true);

  auto mergedSchema = Schema::fromProto(mergeDef, baseSchema.get());

  // All three fields should exist
  EXPECT_NE(nullptr, mergedSchema->getFieldTypePtr("title"));
  EXPECT_NE(nullptr, mergedSchema->getFieldTypePtr("author"));
  EXPECT_NE(nullptr, mergedSchema->getFieldTypePtr("price"));
}


TEST_F(SchemaTest, mergeWithParentFromBase) {
  auto baseSchema = Schema::createDefaultSchema();

  // Merge: add "title" that inherits from "_wl" in the base schema
  proto::SchemaDef mergeDef;
  auto* f = mergeDef.add_fields();
  f->set_name("title");
  f->set_parent("_wl");

  auto merged = Schema::fromProto(mergeDef, baseSchema.get());

  // "title" should inherit TEXT type and analyzer from base's _wl
  auto* title = merged->getFieldTypePtr("title");
  ASSERT_NE(nullptr, title);
  EXPECT_EQ(FieldType::TEXT, title->type());
  EXPECT_TRUE(title->indexed());
  EXPECT_FALSE(title->isAbstract());

  auto* textFt = (TextFieldType*)(title);
  EXPECT_EQ("whitespace", textFt->tokenizer_);
  ASSERT_EQ(1, textFt->filters_.size());
  EXPECT_EQ("lowercase", textFt->filters_[0]);

  // Base fields should still be present
  ASSERT_NE(nullptr, merged->getFieldTypePtr("id"));
  ASSERT_NE(nullptr, merged->getFieldTypePtr("title_s"));
}


TEST_F(SchemaTest, replaceMode) {
  // Base schema with "title" and "author"
  proto::SchemaDef baseDef;
  auto* f1 = baseDef.add_fields();
  f1->set_name("title");
  f1->set_field_class(proto::FieldDef::TEXT);
  f1->set_indexed(true);
  f1->mutable_analyzer()->set_tokenizer("whitespace");

  auto* f2 = baseDef.add_fields();
  f2->set_name("author");
  f2->set_field_class(proto::FieldDef::STRING);

  auto baseSchema = Schema::fromProto(baseDef);

  // Replace: only "price" remains
  proto::SchemaDef replaceDef;
  auto* f3 = replaceDef.add_fields();
  f3->set_name("price");
  f3->set_field_class(proto::FieldDef::INT);
  f3->set_column_stored(true);

  auto replacedSchema = Schema::fromProto(replaceDef);  // no base = replace

  EXPECT_EQ(nullptr, replacedSchema->getFieldTypePtr("title"));
  EXPECT_EQ(nullptr, replacedSchema->getFieldTypePtr("author"));
  EXPECT_NE(nullptr, replacedSchema->getFieldTypePtr("price"));
}


TEST_F(SchemaTest, roundtrip) {
  auto original = Schema::createDefaultSchema();

  // Serialize to proto
  proto::SchemaDef def;
  original->toProto(&def);

  // Deserialize back
  auto roundtripped = Schema::fromProto(def);

  // All fields from the original should be present with the same types and flags
  for (auto& [name, ft] : original->fieldTypeMap) {
    auto it = roundtripped->fieldTypeMap.find(name);
    ASSERT_NE(it, roundtripped->fieldTypeMap.end()) << "Missing field: " << name;
    EXPECT_EQ(ft->type(), it->second->type()) << "Type mismatch for field: " << name;
    EXPECT_EQ(ft->flags_, it->second->flags_) << "Flags mismatch for field: " << name;
    EXPECT_EQ(ft->isAbstract(), it->second->isAbstract()) << "Abstract mismatch for field: " << name;
  }

  // Same number of fields
  EXPECT_EQ(original->fieldTypeMap.size(), roundtripped->fieldTypeMap.size());
}


TEST_F(SchemaTest, indexAndSearchWithExplicitField) {
  CollectionHelper ch;
  ch.clear();

  // Set a custom schema on the collection: "title" as TEXT with whitespace+lowercase
  auto defaultSchema = Schema::createDefaultSchema();

  proto::SchemaDef customDef;
  defaultSchema->toProto(&customDef);

  // Add an explicit "title" field as TEXT with whitespace+lowercase
  auto* titleField = customDef.add_fields();
  titleField->set_name("title");
  titleField->set_field_class(proto::FieldDef::TEXT);
  titleField->set_indexed(true);
  titleField->mutable_analyzer()->set_tokenizer("whitespace");
  titleField->mutable_analyzer()->add_filters("lowercase");

  auto newSchema = Schema::fromProto(customDef);

  // Verify "title" exists in the new schema
  ASSERT_NE(nullptr, newSchema->getFieldTypePtr("title"));
  ASSERT_EQ(FieldType::TEXT, newSchema->getFieldTypePtr("title")->type());

  ch.collection().setSchema(newSchema);

  // Verify the collection has the new schema
  auto collSchema = ch.collection().getSchema();
  ASSERT_NE(nullptr, collSchema->getFieldTypePtr("title"));

  // Index a doc with the explicit "title" field
  Doc doc1 = {{"id", std::string("1")}, {"title", std::string("hello world")}};
  ch.index(doc1);
  ch.commit();

  // Search for "hello" should match
  auto& engine = ch.getSearchEngine();
  auto* req = LocalReq::create(engine);
  req->collection("main")
     .matchQuery("title", "hello")
     .limit(10)
     .withStats()
     .execute();

  EXPECT_EQ(1, req->getMatchCount());
  req->done();

  // Clean up
  ch.clear();
  ch.collection().setSchema(Schema::createDefaultSchema());
}


TEST_F(SchemaTest, fieldClassDefaults) {
  proto::SchemaDef def;

  // STRING field with no explicit flags — should get default indexed=true, column_stored=true
  auto* f1 = def.add_fields();
  f1->set_name("str_field");
  f1->set_field_class(proto::FieldDef::STRING);

  // INT field with no explicit flags — should get default indexed=false, column_stored=true
  auto* f2 = def.add_fields();
  f2->set_name("int_field");
  f2->set_field_class(proto::FieldDef::INT);

  // TEXT field with no explicit flags — should get default indexed=true, column_stored=false
  auto* f3 = def.add_fields();
  f3->set_name("text_field");
  f3->set_field_class(proto::FieldDef::TEXT);

  auto schema = Schema::fromProto(def);

  auto* str = schema->getFieldTypePtr("str_field");
  ASSERT_NE(nullptr, str);
  EXPECT_TRUE(str->indexed());
  EXPECT_TRUE(str->hasColumn());

  auto* intF = schema->getFieldTypePtr("int_field");
  ASSERT_NE(nullptr, intF);
  EXPECT_FALSE(intF->indexed());
  EXPECT_TRUE(intF->hasColumn());

  auto* text = schema->getFieldTypePtr("text_field");
  ASSERT_NE(nullptr, text);
  EXPECT_TRUE(text->indexed());
  EXPECT_FALSE(text->hasColumn());
}


TEST_F(SchemaTest, missingFieldClass) {
  proto::SchemaDef def;

  // Field with no field_class and no parent — should error
  auto* f = def.add_fields();
  f->set_name("broken");

  EXPECT_THROW(Schema::fromProto(def), std::runtime_error);
}
