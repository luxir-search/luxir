
#include <gtest/gtest.h>

#include "solux/schema/Schema.h"
#include "solux/schema/FieldType.h"
#include "solux/store/InputStream.h"
#include "solux/reader/Postings.h"
#include "solux/api/solux_types.hpp"
#include "solux/api/build.h"
#include "test/SoluxTest.h"
#include "test/CollectionHelper.h"
#include "test/LocalReq.h"
#include "test/TestUtils.h"

#include <memory_resource>
#include <span>

using namespace solux;
using namespace solux::test;

namespace api = solux::api;
namespace build = solux::api::build;
using FieldClass = solux::api::FieldDef::FieldClass;

static std::string schemaFileName(uint64_t gen) {
  return "_schema_" + Postings::getSortableString(gen);
}

// Build a field's analyzer (tokenizer + optional filters) into `arena`. The concrete
// FieldDef is non-owning, so the filters span needs arena-backed storage.
static void setAnalyzer(api::FieldDef& f, std::string_view tokenizer,
                        std::initializer_list<std::string_view> filters,
                        std::pmr::memory_resource& arena) {
  auto& a = f.analyzer.emplace();
  a.tokenizer = tokenizer;
  if (filters.size() > 0) {
    std::string_view* fl = build::allocArray(a.filters, filters.size(), arena);
    std::size_t i = 0;
    for (auto x : filters) fl[i++] = x;
  }
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


TEST_F(SchemaTest, collectionHelperClearRestoresDefaultSchema) {
  CollectionHelper ch;
  ch.clear();

  std::pmr::monotonic_buffer_resource arena;
  api::SchemaDef def;
  api::FieldDef* fields = build::allocArray(def.fields, 1, arena);
  auto& f = fields[0];
  f.name = "custom_text";
  f.field_class = FieldClass::TEXT;
  auto schema = Schema::fromProto(def, ch.collection().getSchema().get());
  ch.collection().setSchema(schema);
  ASSERT_NE(nullptr, ch.collection().getSchema()->getFieldTypePtr("custom_text"));

  ch.clear();

  auto resetSchema = ch.collection().getSchema();
  EXPECT_EQ(nullptr, resetSchema->getFieldTypePtr("custom_text"));
  EXPECT_NE(nullptr, resetSchema->getFieldTypePtr("id"));
  EXPECT_NE(nullptr, resetSchema->getFieldTypePtr("body_w"));
}


TEST_F(SchemaTest, collectionHelperClearSkipsDefaultSchemaReset) {
  CollectionHelper ch;
  ch.clear();

  auto schema = ch.collection().getSchema();
  uint64_t nextGen = ch.collection().schemaGen();

  ch.clear();

  EXPECT_EQ(nextGen, ch.collection().schemaGen());
  EXPECT_EQ(schema.get(), ch.collection().getSchema().get());
}


TEST_F(SchemaTest, fromProtoBasic) {
  std::pmr::monotonic_buffer_resource arena;
  api::SchemaDef def;
  api::FieldDef* fields = build::allocArray(def.fields, 2, arena);

  auto& f = fields[0];
  f.name = "title";
  f.field_class = FieldClass::TEXT;
  f.indexed = true;
  setAnalyzer(f, "whitespace", {"lowercase"}, arena);

  auto& f2 = fields[1];
  f2.name = "price";
  f2.field_class = FieldClass::INT;
  f2.column_stored = true;

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
  std::pmr::monotonic_buffer_resource arena;
  api::SchemaDef def;
  api::FieldDef* fields = build::allocArray(def.fields, 2, arena);

  // Parent: analyzed text with whitespace+lowercase
  auto& parent = fields[0];
  parent.name = "_wl";
  parent.field_class = FieldClass::TEXT;
  parent.indexed = true;
  parent.abstract = true;
  setAnalyzer(parent, "whitespace", {"lowercase"}, arena);

  // Child inherits from _wl
  auto& child = fields[1];
  child.name = "title";
  child.parent = "_wl";

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
  std::pmr::monotonic_buffer_resource arena;
  api::SchemaDef def;
  api::FieldDef* fields = build::allocArray(def.fields, 2, arena);

  auto& parent = fields[0];
  parent.name = "_wl";
  parent.field_class = FieldClass::TEXT;
  parent.indexed = true;
  parent.abstract = true;
  setAnalyzer(parent, "whitespace", {"lowercase"}, arena);

  // Child overrides the analyzer
  auto& child = fields[1];
  child.name = "title";
  child.parent = "_wl";
  setAnalyzer(child, "keyword", {}, arena);

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
  std::pmr::monotonic_buffer_resource arena;
  api::SchemaDef def;
  api::FieldDef* fields = build::allocArray(def.fields, 2, arena);

  auto& a = fields[0];
  a.name = "a";
  a.parent = "b";
  a.field_class = FieldClass::STRING;

  auto& b = fields[1];
  b.name = "b";
  b.parent = "a";
  b.field_class = FieldClass::STRING;

  EXPECT_THROW(Schema::fromProto(def), std::runtime_error);
}


TEST_F(SchemaTest, mergeMode) {
  // Start with a base schema
  std::pmr::monotonic_buffer_resource arena;
  api::SchemaDef baseDef;
  api::FieldDef* baseFields = build::allocArray(baseDef.fields, 2, arena);
  auto& f1 = baseFields[0];
  f1.name = "title";
  f1.field_class = FieldClass::TEXT;
  f1.indexed = true;
  setAnalyzer(f1, "whitespace", {}, arena);

  auto& f2 = baseFields[1];
  f2.name = "author";
  f2.field_class = FieldClass::STRING;

  auto baseSchema = Schema::fromProto(baseDef);

  // Merge: add a new field, existing "author" should survive
  api::SchemaDef mergeDef;
  api::FieldDef* mergeFields = build::allocArray(mergeDef.fields, 1, arena);
  auto& f3 = mergeFields[0];
  f3.name = "price";
  f3.field_class = FieldClass::INT;
  f3.column_stored = true;

  auto mergedSchema = Schema::fromProto(mergeDef, baseSchema.get());

  // All three fields should exist
  EXPECT_NE(nullptr, mergedSchema->getFieldTypePtr("title"));
  EXPECT_NE(nullptr, mergedSchema->getFieldTypePtr("author"));
  EXPECT_NE(nullptr, mergedSchema->getFieldTypePtr("price"));
}


TEST_F(SchemaTest, mergeWithParentFromBase) {
  auto baseSchema = Schema::createDefaultSchema();

  // Merge: add "title" that inherits from "_wl" in the base schema
  std::pmr::monotonic_buffer_resource arena;
  api::SchemaDef mergeDef;
  api::FieldDef* fields = build::allocArray(mergeDef.fields, 1, arena);
  auto& f = fields[0];
  f.name = "title";
  f.parent = "_wl";

  auto merged = Schema::fromProto(mergeDef, baseSchema.get());

  // "title" should inherit TEXT type and analyzer from base's _wl
  auto* title = merged->getFieldTypePtr("title");
  ASSERT_NE(nullptr, title);
  EXPECT_EQ(FieldType::TEXT, title->type());
  EXPECT_TRUE(title->indexed());
  EXPECT_FALSE(title->isAbstract());

  auto* textFt = (TextFieldType*)(title);
  EXPECT_EQ("unicode_word", textFt->tokenizer_);  // inherited from _wl
  ASSERT_EQ(1, textFt->filters_.size());
  EXPECT_EQ("nfkc_cf", textFt->filters_[0]);

  // Base fields should still be present
  ASSERT_NE(nullptr, merged->getFieldTypePtr("id"));
  ASSERT_NE(nullptr, merged->getFieldTypePtr("title_s"));
}


TEST_F(SchemaTest, replaceMode) {
  // Base schema with "title" and "author"
  std::pmr::monotonic_buffer_resource arena;
  api::SchemaDef baseDef;
  api::FieldDef* baseFields = build::allocArray(baseDef.fields, 2, arena);
  auto& f1 = baseFields[0];
  f1.name = "title";
  f1.field_class = FieldClass::TEXT;
  f1.indexed = true;
  setAnalyzer(f1, "whitespace", {}, arena);

  auto& f2 = baseFields[1];
  f2.name = "author";
  f2.field_class = FieldClass::STRING;

  auto baseSchema = Schema::fromProto(baseDef);

  // Replace: only "price" remains
  api::SchemaDef replaceDef;
  api::FieldDef* replaceFields = build::allocArray(replaceDef.fields, 1, arena);
  auto& f3 = replaceFields[0];
  f3.name = "price";
  f3.field_class = FieldClass::INT;
  f3.column_stored = true;

  auto replacedSchema = Schema::fromProto(replaceDef);  // no base = replace

  EXPECT_EQ(nullptr, replacedSchema->getFieldTypePtr("title"));
  EXPECT_EQ(nullptr, replacedSchema->getFieldTypePtr("author"));
  EXPECT_NE(nullptr, replacedSchema->getFieldTypePtr("price"));
}


TEST_F(SchemaTest, inheritStoredFromParent) {
  // A child field that inherits from a STORED parent picks up the STORED
  // flag, and the resulting FieldType has STORED set.
  std::pmr::monotonic_buffer_resource arena;
  api::SchemaDef def;
  api::FieldDef* fields = build::allocArray(def.fields, 3, arena);

  auto& parent = fields[0];
  parent.name = "_body_";
  parent.field_class = FieldClass::TEXT;
  parent.indexed = true;
  parent.abstract = true;
  parent.stored = true;
  setAnalyzer(parent, "whitespace", {}, arena);

  // Child: no explicit stored flag; should inherit true.
  auto& child = fields[1];
  child.name = "title";
  child.parent = "_body_";

  // Child that explicitly disables stored (override wins).
  auto& child2 = fields[2];
  child2.name = "summary";
  child2.parent = "_body_";
  child2.stored = false;

  auto schema = Schema::fromProto(def);

  auto* title = schema->getFieldTypePtr("title");
  ASSERT_NE(nullptr, title);
  EXPECT_TRUE(title->isStored()) << "child should inherit STORED from parent";

  auto* summary = schema->getFieldTypePtr("summary");
  ASSERT_NE(nullptr, summary);
  EXPECT_FALSE(summary->isStored()) << "explicit stored=false overrides parent";
}

TEST_F(SchemaTest, defaultTSuffixIsStored) {
  // _t dynamic fields should pick up STORED from the default schema.
  auto schema = Schema::createDefaultSchema();
  auto* ft = schema->getFieldTypePtr("anything_t");
  ASSERT_NE(nullptr, ft);
  EXPECT_EQ(FieldType::TEXT, ft->type());
  EXPECT_TRUE(ft->isStored());
  EXPECT_TRUE(ft->indexed());
}

TEST_F(SchemaTest, storedRoundtripsThroughProto) {
  // Build a schema with STORED on TEXT and STRING, serialize, deserialize,
  // and verify STORED survives.
  std::pmr::monotonic_buffer_resource arena;
  api::SchemaDef def;
  api::FieldDef* fields = build::allocArray(def.fields, 3, arena);
  auto& t = fields[0];
  t.name = "body";
  t.field_class = FieldClass::TEXT;
  t.indexed = true;
  t.stored = true;
  auto& s = fields[1];
  s.name = "tag";
  s.field_class = FieldClass::STRING;
  s.indexed = true;
  s.column_stored = false;
  s.stored = true;
  // A non-stored field for contrast.
  auto& p = fields[2];
  p.name = "plain";
  p.field_class = FieldClass::TEXT;
  p.indexed = true;
  p.stored = false;

  auto original = Schema::fromProto(def);
  EXPECT_TRUE(original->getFieldTypePtr("body")->isStored());
  EXPECT_TRUE(original->getFieldTypePtr("tag")->isStored());
  EXPECT_FALSE(original->getFieldTypePtr("plain")->isStored());

  api::SchemaDef round;
  original->toProto(&round, arena);
  auto loaded = Schema::fromProto(round);
  EXPECT_TRUE(loaded->getFieldTypePtr("body")->isStored());
  EXPECT_TRUE(loaded->getFieldTypePtr("tag")->isStored());
  EXPECT_FALSE(loaded->getFieldTypePtr("plain")->isStored());
}

TEST_F(SchemaTest, storedResourceRoundtripsThroughProto) {
  // A custom stored_resource survives toProto -> fromProto; unset fields
  // keep the default "_stored_".
  std::pmr::monotonic_buffer_resource arena;
  api::SchemaDef def;
  api::FieldDef* fields = build::allocArray(def.fields, 2, arena);
  auto& custom = fields[0];
  custom.name = "paragraphs";
  custom.field_class = FieldClass::TEXT;
  custom.indexed = true;
  custom.stored = true;
  custom.stored_resource = "_stored_embeddings_";

  auto& defaulted = fields[1];
  defaulted.name = "body";
  defaulted.field_class = FieldClass::TEXT;
  defaulted.indexed = true;
  defaulted.stored = true;

  auto s1 = Schema::fromProto(def);
  EXPECT_EQ("_stored_embeddings_", s1->getFieldTypePtr("paragraphs")->storedResource_);
  EXPECT_EQ("_stored_", s1->getFieldTypePtr("body")->storedResource_);

  api::SchemaDef round;
  s1->toProto(&round, arena);
  auto s2 = Schema::fromProto(round);
  EXPECT_EQ("_stored_embeddings_", s2->getFieldTypePtr("paragraphs")->storedResource_);
  EXPECT_EQ("_stored_", s2->getFieldTypePtr("body")->storedResource_);
}

TEST_F(SchemaTest, storedResourceInheritedFromParent) {
  // A child inheriting from a parent with a non-default stored_resource
  // picks up the parent's choice.  Explicit override on the child wins.
  std::pmr::monotonic_buffer_resource arena;
  api::SchemaDef def;
  api::FieldDef* fields = build::allocArray(def.fields, 3, arena);

  auto& parent = fields[0];
  parent.name = "_emb_";
  parent.field_class = FieldClass::TEXT;
  parent.indexed = true;
  parent.abstract = true;
  parent.stored = true;
  parent.stored_resource = "_stored_embeddings_";

  auto& inherit = fields[1];
  inherit.name = "paragraphs";
  inherit.parent = "_emb_";

  auto& override_ = fields[2];
  override_.name = "captions";
  override_.parent = "_emb_";
  override_.stored_resource = "_stored_captions_";

  auto s = Schema::fromProto(def);
  EXPECT_EQ("_stored_embeddings_", s->getFieldTypePtr("paragraphs")->storedResource_);
  EXPECT_EQ("_stored_captions_", s->getFieldTypePtr("captions")->storedResource_);
}

TEST_F(SchemaTest, mergePreservesStoredResource) {
  // MERGE-mode fromProto rebuilds every FieldType from resolved data.  That
  // rebuild must carry the base schema's stored_resource forward; otherwise
  // base fields silently lose their custom column-family assignment.
  std::pmr::monotonic_buffer_resource arena;
  api::SchemaDef baseDef;
  api::FieldDef* baseFields = build::allocArray(baseDef.fields, 1, arena);
  auto& b = baseFields[0];
  b.name = "body";
  b.field_class = FieldClass::TEXT;
  b.indexed = true;
  b.stored = true;
  b.stored_resource = "_stored_embeddings_";
  auto base = Schema::fromProto(baseDef);
  ASSERT_EQ("_stored_embeddings_", base->getFieldTypePtr("body")->storedResource_);

  // Programmatically-set custom resource (no proto involvement) must also
  // survive a merge that doesn't mention the field.
  base->getFieldTypePtr("body")->storedResource_ = "_stored_programmatic_";

  api::SchemaDef mergeDef;
  api::FieldDef* mergeFields = build::allocArray(mergeDef.fields, 1, arena);
  auto& f = mergeFields[0];
  f.name = "price";
  f.field_class = FieldClass::INT;

  auto merged = Schema::fromProto(mergeDef, base.get());
  EXPECT_EQ("_stored_programmatic_", merged->getFieldTypePtr("body")->storedResource_);
  EXPECT_NE(nullptr, merged->getFieldTypePtr("price"));
}


TEST_F(SchemaTest, roundtrip) {
  auto original = Schema::createDefaultSchema();

  // Serialize to proto
  std::pmr::monotonic_buffer_resource arena;
  api::SchemaDef def;
  original->toProto(&def, arena);

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

  std::pmr::monotonic_buffer_resource arena;
  api::SchemaDef defaults;
  defaultSchema->toProto(&defaults, arena);

  // Add an explicit "title" field as TEXT with whitespace+lowercase
  std::size_t n = defaults.fields.size();
  api::SchemaDef customDef;
  api::FieldDef* fields = build::allocArray(customDef.fields, n + 1, arena);
  for (std::size_t i = 0; i < n; i++) fields[i] = defaults.fields[i];
  auto& titleField = fields[n];
  titleField.name = "title";
  titleField.field_class = FieldClass::TEXT;
  titleField.indexed = true;
  setAnalyzer(titleField, "whitespace", {"lowercase"}, arena);

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
  auto req = localReq(engine);
  req->collection("main")
     .topDocs("q")
     .matchQuery("title", "hello")
     .limit(10)
     .withStats();
  req->execute();

  ASSERT_OK(req);
  EXPECT_EQ(1, req->getMatchCount());

  // Clean up
  ch.clear();
  ch.collection().setSchema(Schema::createDefaultSchema());
}


TEST_F(SchemaTest, fieldClassDefaults) {
  std::pmr::monotonic_buffer_resource arena;
  api::SchemaDef def;
  api::FieldDef* fields = build::allocArray(def.fields, 3, arena);

  // STRING field with no explicit flags - should get default indexed=true, column_stored=true
  auto& f1 = fields[0];
  f1.name = "str_field";
  f1.field_class = FieldClass::STRING;

  // INT field with no explicit flags - should get default indexed=false, column_stored=true
  auto& f2 = fields[1];
  f2.name = "int_field";
  f2.field_class = FieldClass::INT;

  // TEXT field with no explicit flags - should get default indexed=true, column_stored=false
  auto& f3 = fields[2];
  f3.name = "text_field";
  f3.field_class = FieldClass::TEXT;

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
  std::pmr::monotonic_buffer_resource arena;
  api::SchemaDef def;
  api::FieldDef* fields = build::allocArray(def.fields, 1, arena);

  // Field with no field_class and no parent - should error
  auto& f = fields[0];
  f.name = "broken";

  EXPECT_THROW(Schema::fromProto(def), std::runtime_error);
}


TEST_F(SchemaTest, schemaPersistence) {
  CollectionHelper ch;
  ch.clear();

  // Set a custom schema with an explicit "title" field
  auto defaultSchema = Schema::createDefaultSchema();
  std::pmr::monotonic_buffer_resource arena;
  api::SchemaDef customDef;
  api::FieldDef* fields = build::allocArray(customDef.fields, 1, arena);
  auto& titleField = fields[0];
  titleField.name = "title";
  titleField.field_class = FieldClass::TEXT;
  titleField.indexed = true;
  setAnalyzer(titleField, "whitespace", {"lowercase"}, arena);

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
  api::SchemaDef persistedDef;
  std::span<const char> persistedBytes(is.ptr(), (size_t)is.left());
  ASSERT_TRUE(api::decode(persistedDef, std::as_bytes(persistedBytes), arena));

  // The persisted def should contain "title" field
  bool foundTitle = false;
  for (size_t i = 0; i < persistedDef.fields.size(); i++) {
    if (persistedDef.fields[i].name == "title") {
      foundTitle = true;
      EXPECT_EQ(FieldClass::TEXT, *persistedDef.fields[i].field_class);
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
  std::pmr::monotonic_buffer_resource arena;

  // Set schema first time
  api::SchemaDef def1;
  api::FieldDef* f1arr = build::allocArray(def1.fields, 1, arena);
  auto& f1 = f1arr[0];
  f1.name = "field1";
  f1.field_class = FieldClass::STRING;
  auto schema1 = Schema::fromProto(def1, defaultSchema.get());
  ch.collection().setSchema(schema1);
  uint64_t gen1 = ch.collection().getSchema()->gen_;

  // Set schema second time
  api::SchemaDef def2;
  api::FieldDef* f2arr = build::allocArray(def2.fields, 1, arena);
  auto& f2 = f2arr[0];
  f2.name = "field2";
  f2.field_class = FieldClass::INT;
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
  std::pmr::monotonic_buffer_resource arena;
  api::SchemaDef customDef;
  api::FieldDef* fields = build::allocArray(customDef.fields, 1, arena);
  auto& f = fields[0];
  f.name = "title";
  f.field_class = FieldClass::TEXT;
  f.indexed = true;
  setAnalyzer(f, "whitespace", {}, arena);

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
  api::IndexInfo indexInfo;
  std::span<const char> indexInfoBytes(is.ptr(), (size_t)is.left());
  ASSERT_TRUE(api::decode(indexInfo, std::as_bytes(indexInfoBytes), arena));
  EXPECT_EQ(expectedGen, indexInfo.schema_gen) << "IndexInfo should contain the current schema_gen";

  // Check that SegmentInfo also has schema_gen
  ASSERT_GT(indexInfo.segments.size(), 0u);
  EXPECT_EQ(expectedGen, indexInfo.segments[0].schema_gen) << "SegmentInfo should have schema_gen";

  ch.clear();
  ch.collection().setSchema(Schema::createDefaultSchema());
}


TEST_F(SchemaTest, schemaLoadOnRestart) {
  CollectionHelper ch;
  ch.clear();

  // Set a custom schema with "title" field
  auto defaultSchema = Schema::createDefaultSchema();
  std::pmr::monotonic_buffer_resource arena;
  api::SchemaDef customDef;
  api::FieldDef* fields = build::allocArray(customDef.fields, 1, arena);
  auto& f = fields[0];
  f.name = "title";
  f.field_class = FieldClass::TEXT;
  f.indexed = true;
  setAnalyzer(f, "whitespace", {"lowercase"}, arena);

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
  std::pmr::monotonic_buffer_resource arena;
  api::SchemaDef def;
  api::FieldDef* fields = build::allocArray(def.fields, 2, arena);

  auto& parent = fields[0];
  parent.name = "_wl";
  parent.field_class = FieldClass::TEXT;
  parent.indexed = true;
  parent.abstract = true;
  setAnalyzer(parent, "whitespace", {"lowercase"}, arena);

  auto& child = fields[1];
  child.name = "title";
  child.parent = "_wl";

  auto schema = Schema::fromProto(def);
  ASSERT_FALSE(schema->sourceDef_.empty());

  // Parse back the sourceDef and verify it has parent references
  api::SchemaDef roundtripped;
  std::span<const char> sourceBytes(schema->sourceDef_.data(), schema->sourceDef_.size());
  ASSERT_TRUE(api::decode(roundtripped, std::as_bytes(sourceBytes), arena));

  bool foundChild = false;
  for (size_t i = 0; i < roundtripped.fields.size(); i++) {
    if (roundtripped.fields[i].name == "title") {
      foundChild = true;
      EXPECT_EQ("_wl", roundtripped.fields[i].parent)
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
  std::pmr::monotonic_buffer_resource arena;

  // setSchema - writes _schema.<gen1>
  api::SchemaDef def1;
  api::FieldDef* f1arr = build::allocArray(def1.fields, 1, arena);
  auto& f1 = f1arr[0];
  f1.name = "field1";
  f1.field_class = FieldClass::STRING;
  ch.collection().setSchema(Schema::fromProto(def1, baseSchema.get()));
  uint64_t gen1 = ch.collection().getSchema()->gen_;

  // Manually delete the schema file to simulate the race
  auto& dir = *ch.collection().getShard()->getDirectory();
  ASSERT_TRUE(dir.deleteFile(schemaFileName(gen1)));

  // loadSchema should fail - no schema files remain
  EXPECT_FALSE(ch.collection().loadSchema()) << "Should fail with no schema files";

  // Now do two setSchema calls so the first gen's file gets cleaned up naturally
  api::SchemaDef def2;
  api::FieldDef* f2arr = build::allocArray(def2.fields, 1, arena);
  auto& f2 = f2arr[0];
  f2.name = "field2";
  f2.field_class = FieldClass::INT;
  ch.collection().setSchema(Schema::fromProto(def2, baseSchema.get()));

  api::SchemaDef def3;
  api::FieldDef* f3arr = build::allocArray(def3.fields, 1, arena);
  auto& f3 = f3arr[0];
  f3.name = "field3";
  f3.field_class = FieldClass::INT;
  f3.column_stored = true;
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
