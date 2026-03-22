
#include <gtest/gtest.h>

#include "solux/schema/Schema.h"
#include "solux/schema/FieldType.h"
#include "solux/store/InputStream.h"
#include "solux/reader/Postings.h"
#include "protos/solux_types.pb.h"
#include "test/SoluxTest.h"
#include "test/CollectionHelper.h"
#include "test/LocalReq.h"
#include "test/TestUtils.h"

using namespace solux;
using namespace solux::test;

static std::string schemaFileName(uint64_t gen) {
  return "_schema_" + Postings::getSortableString(gen);
}

class SchemaTest : public SoluxTest {};


TEST_F(SchemaTest, defaultSchema) {
  auto schema = Schema::createDefaultSchema();

  // Concrete fields should be found by exact name
  ASSERT_NE(nullptr, schema->getFieldTypePtr("id"));
  ASSERT_NE(nullptr, schema->getFieldTypePtr("_version_"));
  EXPECT_EQ(FieldType::ID, schema->getFieldTypePtr("id")->type());
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


TEST_F(SchemaTest, schemaPersistence) {
  CollectionHelper ch;
  ch.clear();

  // Set a custom schema with an explicit "title" field
  auto defaultSchema = Schema::createDefaultSchema();
  proto::SchemaDef customDef;
  auto* titleField = customDef.add_fields();
  titleField->set_name("title");
  titleField->set_field_class(proto::FieldDef::TEXT);
  titleField->set_indexed(true);
  titleField->mutable_analyzer()->set_tokenizer("whitespace");
  titleField->mutable_analyzer()->add_filters("lowercase");

  auto newSchema = Schema::fromProto(customDef, defaultSchema.get());
  ch.collection().setSchema(newSchema);

  // Verify gen was assigned
  auto schema = ch.collection().getSchema();
  EXPECT_GT(schema->gen_, (uint64_t)0);
  uint64_t gen = schema->gen_;

  // Verify schema file exists in Directory
  auto& dir = *ch.collection().getShard()->getDirectory();
  std::string fileName = schemaFileName(gen);
  auto file = dir.openFile(fileName);
  ASSERT_NE(nullptr, file.get()) << "Schema file " << fileName << " should exist";

  // Verify we can parse the persisted schema
  InputStream is = file->getInputStream();
  proto::SchemaDef persistedDef;
  ASSERT_TRUE(persistedDef.ParseFromArray(is.ptr(), (int)is.left()));

  // The persisted def should contain "title" field
  bool foundTitle = false;
  for (int i = 0; i < persistedDef.fields_size(); i++) {
    if (persistedDef.fields(i).name() == "title") {
      foundTitle = true;
      EXPECT_EQ(proto::FieldDef::TEXT, persistedDef.fields(i).field_class());
      break;
    }
  }
  EXPECT_TRUE(foundTitle) << "Persisted schema should contain 'title' field";

  ch.clear();
  ch.collection().setSchema(Schema::createDefaultSchema());
}


TEST_F(SchemaTest, schemaGenIncrements) {
  CollectionHelper ch;
  ch.clear();

  auto defaultSchema = Schema::createDefaultSchema();

  // Set schema first time
  proto::SchemaDef def1;
  auto* f1 = def1.add_fields();
  f1->set_name("field1");
  f1->set_field_class(proto::FieldDef::STRING);
  auto schema1 = Schema::fromProto(def1, defaultSchema.get());
  ch.collection().setSchema(schema1);
  uint64_t gen1 = ch.collection().getSchema()->gen_;

  // Set schema second time
  proto::SchemaDef def2;
  auto* f2 = def2.add_fields();
  f2->set_name("field2");
  f2->set_field_class(proto::FieldDef::INT);
  auto schema2 = Schema::fromProto(def2, ch.collection().getSchema().get());
  ch.collection().setSchema(schema2);
  uint64_t gen2 = ch.collection().getSchema()->gen_;

  EXPECT_GT(gen2, gen1) << "Schema gen should increment on each setSchema";

  // Old schema file should be cleaned up
  auto oldFile = ch.collection().getShard()->getDirectory()->openFile(schemaFileName(gen1));
  EXPECT_EQ(nullptr, oldFile.get()) << "Old schema file should be deleted";

  // New schema file should exist
  auto newFile = ch.collection().getShard()->getDirectory()->openFile(schemaFileName(gen2));
  EXPECT_NE(nullptr, newFile.get()) << "New schema file should exist";

  ch.clear();
  ch.collection().setSchema(Schema::createDefaultSchema());
}


TEST_F(SchemaTest, schemaGenWrittenToIndexInfo) {
  CollectionHelper ch;
  ch.clear();

  // Set a custom schema
  auto defaultSchema = Schema::createDefaultSchema();
  proto::SchemaDef customDef;
  auto* f = customDef.add_fields();
  f->set_name("title");
  f->set_field_class(proto::FieldDef::TEXT);
  f->set_indexed(true);
  f->mutable_analyzer()->set_tokenizer("whitespace");

  auto newSchema = Schema::fromProto(customDef, defaultSchema.get());
  ch.collection().setSchema(newSchema);
  uint64_t expectedGen = ch.collection().getSchema()->gen_;

  // Index a doc and commit so IndexInfo gets written
  Doc doc = {{"id", std::string("1")}, {"title_s", std::string("test")}};
  ch.index(doc);
  ch.commit();

  // Read IndexInfo directly and check schema_gen
  auto& dir = *ch.collection().getShard()->getDirectory();
  auto indexInfoFile = dir.openFile("s.olux");
  ASSERT_NE(nullptr, indexInfoFile.get());

  InputStream is = indexInfoFile->getInputStream();
  proto::IndexInfo indexInfo;
  ASSERT_TRUE(indexInfo.ParseFromArray(is.ptr(), (int)is.left()));
  EXPECT_EQ(expectedGen, indexInfo.schema_gen()) << "IndexInfo should contain the current schema_gen";

  // Check that SegmentInfo also has schema_gen
  ASSERT_GT(indexInfo.segments_size(), 0);
  EXPECT_EQ(expectedGen, indexInfo.segments(0).schema_gen()) << "SegmentInfo should have schema_gen";

  ch.clear();
  ch.collection().setSchema(Schema::createDefaultSchema());
}


TEST_F(SchemaTest, schemaLoadOnRestart) {
  CollectionHelper ch;
  ch.clear();

  // Set a custom schema with "title" field
  auto defaultSchema = Schema::createDefaultSchema();
  proto::SchemaDef customDef;
  auto* f = customDef.add_fields();
  f->set_name("title");
  f->set_field_class(proto::FieldDef::TEXT);
  f->set_indexed(true);
  f->mutable_analyzer()->set_tokenizer("whitespace");
  f->mutable_analyzer()->add_filters("lowercase");

  auto newSchema = Schema::fromProto(customDef, defaultSchema.get());
  ch.collection().setSchema(newSchema);
  uint64_t schemaGen = ch.collection().getSchema()->gen_;

  // Index and commit so IndexInfo has schema_gen
  Doc doc = {{"id", std::string("1")}, {"title_s", std::string("test")}};
  ch.index(doc);
  ch.commit();

  // Simulate restart: load latest schema from Directory
  auto loaded = ch.collection().loadSchema();
  ASSERT_TRUE(loaded) << "Should successfully load schema from Directory";

  auto loadedSchema = ch.collection().getSchema();
  EXPECT_EQ(schemaGen, loadedSchema->gen_);

  // The loaded schema should have the "title" field
  auto* titleFt = loadedSchema->getFieldTypePtr("title");
  ASSERT_NE(nullptr, titleFt) << "Loaded schema should contain 'title' field";
  EXPECT_EQ(FieldType::TEXT, titleFt->type());

  // It should also have default fields that were merged in
  EXPECT_NE(nullptr, loadedSchema->getFieldTypePtr("id"));
  EXPECT_NE(nullptr, loadedSchema->getFieldTypePtr("title_s"));

  ch.clear();
  ch.collection().setSchema(Schema::createDefaultSchema());
}


TEST_F(SchemaTest, sourceDef) {
  // Verify that sourceDef_ preserves the original SchemaDef (including parent references)
  proto::SchemaDef def;

  auto* parent = def.add_fields();
  parent->set_name("_wl");
  parent->set_field_class(proto::FieldDef::TEXT);
  parent->set_indexed(true);
  parent->set_abstract(true);
  parent->mutable_analyzer()->set_tokenizer("whitespace");
  parent->mutable_analyzer()->add_filters("lowercase");

  auto* child = def.add_fields();
  child->set_name("title");
  child->set_parent("_wl");

  auto schema = Schema::fromProto(def);
  ASSERT_FALSE(schema->sourceDef_.empty());

  // Parse back the sourceDef and verify it has parent references
  proto::SchemaDef roundtripped;
  ASSERT_TRUE(roundtripped.ParseFromString(schema->sourceDef_));

  bool foundChild = false;
  for (int i = 0; i < roundtripped.fields_size(); i++) {
    if (roundtripped.fields(i).name() == "title") {
      foundChild = true;
      EXPECT_EQ("_wl", roundtripped.fields(i).parent())
        << "sourceDef should preserve parent references";
      break;
    }
  }
  EXPECT_TRUE(foundChild);
}


TEST_F(SchemaTest, loadSchemaAfterOldFileDeleted) {
  // Simulates the race where the schema file from IndexInfo's gen was already
  // cleaned up by a newer setSchema. loadSchema() must find the newer file.
  // Without the retry logic, this test fails because the only schema file
  // is manually deleted before calling loadSchema().
  CollectionHelper ch;
  ch.clear();

  auto baseSchema = Schema::createDefaultSchema();

  // setSchema — writes _schema.<gen1>
  proto::SchemaDef def1;
  auto* f1 = def1.add_fields();
  f1->set_name("field1");
  f1->set_field_class(proto::FieldDef::STRING);
  ch.collection().setSchema(Schema::fromProto(def1, baseSchema.get()));
  uint64_t gen1 = ch.collection().getSchema()->gen_;

  // Manually delete the schema file to simulate the race
  auto& dir = *ch.collection().getShard()->getDirectory();
  ASSERT_TRUE(dir.deleteFile(schemaFileName(gen1)));

  // loadSchema should fail — no schema files remain
  EXPECT_FALSE(ch.collection().loadSchema()) << "Should fail with no schema files";

  // Now do two setSchema calls so the first gen's file gets cleaned up naturally
  proto::SchemaDef def2;
  auto* f2 = def2.add_fields();
  f2->set_name("field2");
  f2->set_field_class(proto::FieldDef::INT);
  ch.collection().setSchema(Schema::fromProto(def2, baseSchema.get()));

  proto::SchemaDef def3;
  auto* f3 = def3.add_fields();
  f3->set_name("field3");
  f3->set_field_class(proto::FieldDef::INT);
  f3->set_column_stored(true);
  ch.collection().setSchema(Schema::fromProto(def3, ch.collection().getSchema().get()));
  uint64_t gen3 = ch.collection().getSchema()->gen_;

  // loadSchema should find the latest file
  ASSERT_TRUE(ch.collection().loadSchema());
  auto loadedSchema = ch.collection().getSchema();
  EXPECT_EQ(gen3, loadedSchema->gen_) << "Should have loaded the newest schema generation";
  EXPECT_NE(nullptr, loadedSchema->getFieldTypePtr("field3"))
    << "Loaded schema should contain 'field3' from the newest schema";

  ch.clear();
  ch.collection().setSchema(Schema::createDefaultSchema());
}
