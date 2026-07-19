
#include <gtest/gtest.h>

#include "solux/schema/Schema.h"
#include "solux/schema/FieldType.h"
#include "solux/store/InputStream.h"
#include "solux/reader/Postings.h"
#include "solux/api/padded_input.h"
#include "solux/api/solux_types.hpp"
#include "test/SchemaBuilder.h"
#include "test/SoluxTest.h"
#include "test/CollectionHelper.h"
#include "test/LocalReq.h"
#include "test/TestUtils.h"

#include <memory_resource>
#include <span>

using namespace solux;
using namespace solux::test;

namespace api = solux::api;
using FieldClass = solux::api::FieldDef::FieldClass;
using IndexMode = solux::api::FieldDef::IndexMode;

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

  // Templates should NOT be found by exact name
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

  SchemaBuilder b;
  b.field("custom_text").type = FieldClass::TEXT;
  b.set(ch.collection());
  ASSERT_NE(nullptr, ch.collection().getSchema()->getFieldTypePtr("custom_text"));

  ch.clear();

  auto resetSchema = ch.collection().getSchema();
  EXPECT_EQ(nullptr, resetSchema->getFieldTypePtr("custom_text"));
  EXPECT_NE(nullptr, resetSchema->getFieldTypePtr("id"));
  EXPECT_NE(nullptr, resetSchema->getFieldTypePtr("body_w"));
}


TEST_F(SchemaTest, collectionHelperClearSkipsDefaultSchemaReset) {
  CollectionHelper ch;

  auto schema = ch.collection().getSchema();
  uint64_t nextGen = ch.collection().schemaGen();

  ch.clear();

  EXPECT_EQ(nextGen, ch.collection().schemaGen());
  EXPECT_EQ(schema.get(), ch.collection().getSchema().get());
}


TEST_F(SchemaTest, fromProtoBasic) {
  SchemaBuilder b;
  auto& f = b.field("title");
  f.type = FieldClass::TEXT;
  f.index = IndexMode::MATCH;
  b.analyzer(f, "whitespace", {"lowercase"});
  auto& f2 = b.field("price");
  f2.type = FieldClass::INT;
  f2.column = true;

  auto schema = b.build();

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
  SchemaBuilder b;
  auto& parent = b.templ("_wl");
  parent.type = FieldClass::TEXT;
  parent.index = IndexMode::MATCH;
  b.analyzer(parent, "whitespace", {"lowercase"});
  b.field("title").parent = "_wl";

  auto schema = b.build();

  // title should inherit TEXT type and analyzer from _wl
  auto* title = schema->getFieldTypePtr("title");
  ASSERT_NE(nullptr, title);
  EXPECT_EQ(FieldType::TEXT, title->type());
  EXPECT_TRUE(title->indexed());
  EXPECT_FALSE(title->isAbstract());

  // _wl is a template, should not be found by exact name
  EXPECT_EQ(nullptr, schema->getFieldTypePtr("_wl"));
  // But suffix matching should work
  EXPECT_NE(nullptr, schema->getFieldTypePtr("body_wl"));
}


TEST_F(SchemaTest, inheritanceOverride) {
  SchemaBuilder b;
  auto& parent = b.templ("_wl");
  parent.type = FieldClass::TEXT;
  parent.index = IndexMode::MATCH;
  b.analyzer(parent, "whitespace", {"lowercase"});
  // Child overrides the analyzer
  auto& child = b.field("title");
  child.parent = "_wl";
  b.analyzer(child, "keyword");

  auto schema = b.build();

  auto* title = schema->getFieldTypePtr("title");
  ASSERT_NE(nullptr, title);
  EXPECT_EQ(FieldType::TEXT, title->type());

  // Check that the analyzer was overridden
  auto* textFt = (TextFieldType*)(title);
  EXPECT_EQ("keyword", textFt->tokenizer_);
  EXPECT_TRUE(textFt->filters_.empty());  // override is atomic, so parent's filters are not kept
}


TEST_F(SchemaTest, circularInheritance) {
  SchemaBuilder b;
  auto& a = b.field("a");
  a.parent = "b";
  a.type = FieldClass::STRING;
  auto& c = b.field("b");
  c.parent = "a";
  c.type = FieldClass::STRING;

  EXPECT_THROW(b.build(), SchemaError);
}


TEST_F(SchemaTest, setMode) {
  SchemaBuilder baseB;
  auto& f1 = baseB.field("title");
  f1.type = FieldClass::TEXT;
  f1.index = IndexMode::MATCH;
  baseB.analyzer(f1, "whitespace");
  baseB.field("author").type = FieldClass::STRING;
  auto baseSchema = baseB.build();

  // Merge: add a new field, existing "author" should survive
  SchemaBuilder mergeB;
  auto& f3 = mergeB.field("price");
  f3.type = FieldClass::INT;
  f3.column = true;
  auto mergedSchema = mergeB.build(baseSchema.get());

  // All three fields should exist
  EXPECT_NE(nullptr, mergedSchema->getFieldTypePtr("title"));
  EXPECT_NE(nullptr, mergedSchema->getFieldTypePtr("author"));
  EXPECT_NE(nullptr, mergedSchema->getFieldTypePtr("price"));
}


TEST_F(SchemaTest, setWithParentFromBase) {
  auto baseSchema = Schema::createDefaultSchema();

  // Merge: add "title" that inherits from "_wl" in the base schema
  SchemaBuilder b;
  b.field("title").parent = "_wl";
  auto merged = b.build(baseSchema.get());

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


TEST_F(SchemaTest, setReResolvesDescendants) {
  // Replacing a template through MERGE re-resolves fields that inherit from
  // it (the whole merged source graph is rebuilt, not just the new entries).
  SchemaBuilder baseB;
  auto& tmpl = baseB.templ("_body");
  tmpl.type = FieldClass::TEXT;
  baseB.analyzer(tmpl, "whitespace");
  baseB.field("title").parent = "_body";
  auto base = baseB.build();
  EXPECT_EQ("whitespace", ((TextFieldType*)base->getFieldTypePtr("title"))->tokenizer_);

  SchemaBuilder mergeB;
  auto& newTmpl = mergeB.templ("_body");
  newTmpl.type = FieldClass::TEXT;
  mergeB.analyzer(newTmpl, "keyword");
  auto merged = mergeB.build(base.get());

  EXPECT_EQ("keyword", ((TextFieldType*)merged->getFieldTypePtr("title"))->tokenizer_)
    << "descendants must pick up the replaced template";
}


TEST_F(SchemaTest, replaceMode) {
  SchemaBuilder baseB;
  auto& f1 = baseB.field("title");
  f1.type = FieldClass::TEXT;
  f1.index = IndexMode::MATCH;
  baseB.analyzer(f1, "whitespace");
  baseB.field("author").type = FieldClass::STRING;
  auto baseSchema = baseB.build();

  // Replace: only "price" (plus the materialized reserved fields) remains
  SchemaBuilder replaceB;
  auto& f3 = replaceB.field("price");
  f3.type = FieldClass::INT;
  f3.column = true;
  auto replacedSchema = replaceB.build();  // no base = replace

  EXPECT_EQ(nullptr, replacedSchema->getFieldTypePtr("title"));
  EXPECT_EQ(nullptr, replacedSchema->getFieldTypePtr("author"));
  EXPECT_NE(nullptr, replacedSchema->getFieldTypePtr("price"));
}


TEST_F(SchemaTest, replaceMaterializesReservedFields) {
  // A replace that never mentions id/_version_ still gets working ones: the
  // engine keys overwrite/delete on them, so their absence is not expressible.
  SchemaBuilder b;
  b.field("price").type = FieldClass::INT;
  auto schema = b.build();

  auto* id = schema->getFieldTypePtr("id");
  ASSERT_NE(nullptr, id);
  EXPECT_EQ(FieldType::ID, id->type());
  EXPECT_TRUE(id->indexed());
  EXPECT_TRUE(id->hasColumn());

  auto* version = schema->getFieldTypePtr("_version_");
  ASSERT_NE(nullptr, version);
  EXPECT_EQ(FieldType::INT, version->type());
  EXPECT_TRUE(version->hasColumn());
}


TEST_F(SchemaTest, reservedFieldValidation) {
  {
    SchemaBuilder b;
    b.field("id").type = FieldClass::STRING;  // id must be type id
    EXPECT_THROW(b.build(), SchemaError);
  }
  {
    SchemaBuilder b;
    b.field("_version_").type = FieldClass::STRING;  // _version_ must be int
    EXPECT_THROW(b.build(), SchemaError);
  }
  {
    SchemaBuilder b;
    b.field("other_id").type = FieldClass::ID;  // only "id" may be ID-class
    EXPECT_THROW(b.build(), SchemaError);
  }
  {
    SchemaBuilder b;
    b.templ("id").type = FieldClass::ID;  // reserved names cannot be templates
    EXPECT_THROW(b.build(), SchemaError);
  }
}


TEST_F(SchemaTest, fieldAndTemplateNameCollision) {
  SchemaBuilder b;
  b.field("thing").type = FieldClass::STRING;
  b.templ("thing").type = FieldClass::INT;
  EXPECT_THROW(b.build(), SchemaError);
}


TEST_F(SchemaTest, suffixMatchingIsTemplateOnly) {
  // A CONCRETE field that happens to be named like a suffix is exact-match
  // only; it never captures other fields via suffix resolution.
  SchemaBuilder b;
  b.field("_w").type = FieldClass::STRING;
  auto schema = b.build();

  EXPECT_NE(nullptr, schema->getFieldTypePtr("_w"));
  EXPECT_EQ(nullptr, schema->getFieldTypePtr("body_w"))
    << "concrete '_w' must not act as a suffix template";
}


TEST_F(SchemaTest, analyzerComponentValidation) {
  {
    SchemaBuilder b;
    auto& f = b.field("title");
    f.type = FieldClass::TEXT;
    b.analyzer(f, "standard");  // unknown tokenizer
    EXPECT_THROW(b.build(), SchemaError);
  }
  {
    SchemaBuilder b;
    auto& f = b.field("title");
    f.type = FieldClass::TEXT;
    b.analyzer(f, "whitespace", {"stemmer"});  // unknown filter
    EXPECT_THROW(b.build(), SchemaError);
  }
  {
    // The old "nocopy_whitespace" alias is gone.
    SchemaBuilder b;
    auto& f = b.field("title");
    f.type = FieldClass::TEXT;
    b.analyzer(f, "nocopy_whitespace");
    EXPECT_THROW(b.build(), SchemaError);
  }
}


TEST_F(SchemaTest, unknownEnumValuesRejected) {
  // The JSON reader accepts bare integers for enums (and gRPC can carry
  // unknown values), so out-of-range values must die in validation, not be
  // persisted or silently drive flag construction.
  {
    SchemaBuilder b;
    auto& f = b.field("x");
    f.type = FieldClass::STRING;
    f.index = (IndexMode)99;
    EXPECT_THROW(b.build(), SchemaError);
  }
  {
    SchemaBuilder b;
    auto& f = b.field("v");
    f.type = FieldClass::VECTOR;
    f.metric = (api::VectorMetric)99;
    EXPECT_THROW(b.build(), SchemaError);
  }
  {
    SchemaBuilder b;
    b.field("y").type = (FieldClass)99;
    EXPECT_THROW(b.build(), SchemaError);
  }
}


TEST_F(SchemaTest, directPropertyTypeValidation) {
  {
    SchemaBuilder b;
    auto& f = b.field("price");
    f.type = FieldClass::INT;
    b.analyzer(f, "whitespace");  // analyzer on a non-text field
    EXPECT_THROW(b.build(), SchemaError);
  }
  {
    SchemaBuilder b;
    auto& f = b.field("title");
    f.type = FieldClass::TEXT;
    f.dims = 4;  // vector property on a non-vector field
    EXPECT_THROW(b.build(), SchemaError);
  }
  {
    SchemaBuilder b;
    auto& f = b.field("title");
    f.type = FieldClass::TEXT;
    f.column = true;  // the full-text handler does not write a value column
    EXPECT_THROW(b.build(), SchemaError);
  }
  {
    SchemaBuilder b;
    auto& f = b.field("vec");
    f.type = FieldClass::VECTOR;
    f.dims = -1;
    EXPECT_THROW(b.build(), SchemaError);
  }
}


TEST_F(SchemaTest, inheritStoredFromParent) {
  // A child field that inherits from a STORED parent picks up the STORED
  // flag, and the resulting FieldType has STORED set.
  SchemaBuilder b;
  auto& parent = b.templ("_body_");
  parent.type = FieldClass::TEXT;
  parent.index = IndexMode::MATCH;
  parent.stored = true;
  b.analyzer(parent, "whitespace");

  // Child: no explicit stored flag; should inherit true.
  b.field("title").parent = "_body_";

  // Child that explicitly disables stored (override wins).
  auto& child2 = b.field("summary");
  child2.parent = "_body_";
  child2.stored = false;

  auto schema = b.build();

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
  SchemaBuilder b;
  auto& t = b.field("body");
  t.type = FieldClass::TEXT;
  t.index = IndexMode::MATCH;
  t.stored = true;
  auto& s = b.field("tag");
  s.type = FieldClass::STRING;
  s.index = IndexMode::MATCH;
  s.column = false;
  s.stored = true;
  // A non-stored field for contrast.
  auto& p = b.field("plain");
  p.type = FieldClass::TEXT;
  p.index = IndexMode::MATCH;
  p.stored = false;

  auto original = b.build();
  EXPECT_TRUE(original->getFieldTypePtr("body")->isStored());
  EXPECT_TRUE(original->getFieldTypePtr("tag")->isStored());
  EXPECT_FALSE(original->getFieldTypePtr("plain")->isStored());

  std::pmr::monotonic_buffer_resource arena;
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
  SchemaBuilder b;
  auto& custom = b.field("paragraphs");
  custom.type = FieldClass::TEXT;
  custom.index = IndexMode::MATCH;
  custom.stored = true;
  custom.stored_resource = "_stored_embeddings_";

  auto& defaulted = b.field("body");
  defaulted.type = FieldClass::TEXT;
  defaulted.index = IndexMode::MATCH;
  defaulted.stored = true;

  auto s1 = b.build();
  EXPECT_EQ("_stored_embeddings_", s1->getFieldTypePtr("paragraphs")->storedResource_);
  EXPECT_EQ("_stored_", s1->getFieldTypePtr("body")->storedResource_);

  std::pmr::monotonic_buffer_resource arena;
  api::SchemaDef round;
  s1->toProto(&round, arena);
  auto s2 = Schema::fromProto(round);
  EXPECT_EQ("_stored_embeddings_", s2->getFieldTypePtr("paragraphs")->storedResource_);
  EXPECT_EQ("_stored_", s2->getFieldTypePtr("body")->storedResource_);
}

TEST_F(SchemaTest, storedResourceInheritedFromParent) {
  // A child inheriting from a parent with a non-default stored_resource
  // picks up the parent's choice.  Explicit override on the child wins.
  SchemaBuilder b;
  auto& parent = b.templ("_emb_");
  parent.type = FieldClass::TEXT;
  parent.index = IndexMode::MATCH;
  parent.stored = true;
  parent.stored_resource = "_stored_embeddings_";

  b.field("paragraphs").parent = "_emb_";

  auto& override_ = b.field("captions");
  override_.parent = "_emb_";
  override_.stored_resource = "_stored_captions_";

  auto s = b.build();
  EXPECT_EQ("_stored_embeddings_", s->getFieldTypePtr("paragraphs")->storedResource_);
  EXPECT_EQ("_stored_captions_", s->getFieldTypePtr("captions")->storedResource_);
}

TEST_F(SchemaTest, setPreservesStoredResource) {
  // MERGE rebuilds the whole graph from the AUTHORED source, so a base
  // field's proto-declared stored_resource must carry through a merge that
  // doesn't mention the field.  (Programmatic FieldType mutations are NOT
  // preserved: the source def is the single source of truth.)
  SchemaBuilder baseB;
  auto& body = baseB.field("body");
  body.type = FieldClass::TEXT;
  body.index = IndexMode::MATCH;
  body.stored = true;
  body.stored_resource = "_stored_embeddings_";
  auto base = baseB.build();
  ASSERT_EQ("_stored_embeddings_", base->getFieldTypePtr("body")->storedResource_);

  SchemaBuilder mergeB;
  mergeB.field("price").type = FieldClass::INT;
  auto merged = mergeB.build(base.get());
  EXPECT_EQ("_stored_embeddings_", merged->getFieldTypePtr("body")->storedResource_);
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


TEST_F(SchemaTest, toProtoEmitsAuthoredSourceDeterministically) {
  // toProto echoes the AUTHORED def: sparse presence, parent references, and
  // deterministic order (fields: id, _version_, then alpha; templates alpha).
  SchemaBuilder b;
  auto& z = b.field("zebra");
  z.type = FieldClass::INT;
  b.field("apple").parent = "_wl";
  auto schema = b.build(Schema::createDefaultSchema().get());

  std::pmr::monotonic_buffer_resource arena;
  api::SchemaDef out;
  schema->toProto(&out, arena);

  ASSERT_GE(out.fields.size(), 4u);
  EXPECT_EQ("id", out.fields[0].first);
  EXPECT_EQ("_version_", out.fields[1].first);
  EXPECT_EQ("apple", out.fields[2].first);
  EXPECT_EQ("zebra", out.fields[3].first);

  // Sparse: zebra only authored `type`; nothing else materialized.
  const api::FieldDef* zebra = out.fields.find("zebra");
  ASSERT_NE(nullptr, zebra);
  EXPECT_TRUE(zebra->type.has_value());
  EXPECT_FALSE(zebra->index.has_value());
  EXPECT_FALSE(zebra->column.has_value());

  // Parent reference preserved (not resolved away).
  const api::FieldDef* apple = out.fields.find("apple");
  ASSERT_NE(nullptr, apple);
  EXPECT_EQ("_wl", apple->parent);

  // Templates carried in their own map, alpha-ordered.
  ASSERT_GT(out.templates.size(), 0u);
  for (std::size_t i = 1; i < out.templates.size(); i++) {
    EXPECT_LT(out.templates[i - 1].first, out.templates[i].first);
  }
  EXPECT_NE(nullptr, out.templates.find("_wl"));
}


TEST_F(SchemaTest, indexAndSearchWithExplicitField) {
  CollectionHelper ch;

  // Merge an explicit "title" TEXT field (whitespace+lowercase) onto the default schema.
  SchemaBuilder b;
  auto& titleField = b.field("title");
  titleField.type = FieldClass::TEXT;
  titleField.index = IndexMode::MATCH;
  b.analyzer(titleField, "whitespace", {"lowercase"});
  b.set(ch.collection());

  // Verify the collection has the new schema
  auto collSchema = ch.collection().getSchema();
  ASSERT_NE(nullptr, collSchema->getFieldTypePtr("title"));
  ASSERT_EQ(FieldType::TEXT, collSchema->getFieldTypePtr("title")->type());

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
  ch.collection().setSchema(Schema::createDefaultSchema());
}


TEST_F(SchemaTest, fieldClassDefaults) {
  SchemaBuilder b;
  // STRING field with no explicit flags - should get default index=MATCH, column=true
  b.field("str_field").type = FieldClass::STRING;
  // INT field with no explicit flags - should get default index=NONE, column=true
  b.field("int_field").type = FieldClass::INT;
  // TEXT field with no explicit flags - should get default index=MATCH, column=false
  b.field("text_field").type = FieldClass::TEXT;

  auto schema = b.build();

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


// RANGE is accepted only for numeric columns. MATCH on numerics (no numeric
// term postings) and on VECTOR remains rejected. Absence = the type default.
TEST_F(SchemaTest, indexModeValidation) {
  auto trySchema = [](const char* name, FieldClass fc, std::optional<IndexMode> mode) {
    SchemaBuilder b;
    auto& f = b.field(name);
    f.type = fc;
    f.index = mode;
    return b.build();
  };

  EXPECT_TRUE(trySchema("price", FieldClass::INT, IndexMode::RANGE)->getFieldTypePtr("price")->rangeIndexed());
  EXPECT_TRUE(trySchema("score", FieldClass::FLOAT, IndexMode::RANGE)->getFieldTypePtr("score")->rangeIndexed());
  EXPECT_TRUE(trySchema("score", FieldClass::DOUBLE, IndexMode::RANGE)->getFieldTypePtr("score")->rangeIndexed());
  EXPECT_TRUE(trySchema("when", FieldClass::DATE, IndexMode::RANGE)->getFieldTypePtr("when")->rangeIndexed());
  EXPECT_TRUE(trySchema("location", FieldClass::GEO_POINT, IndexMode::RANGE)->getFieldTypePtr("location")->rangeIndexed());
  EXPECT_THROW(trySchema("title", FieldClass::TEXT, IndexMode::RANGE), SchemaError);
  EXPECT_THROW(trySchema("tag", FieldClass::STRING, IndexMode::RANGE), SchemaError);
  EXPECT_THROW(trySchema("price", FieldClass::INT, IndexMode::MATCH), SchemaError);
  EXPECT_THROW(trySchema("score", FieldClass::FLOAT, IndexMode::MATCH), SchemaError);
  EXPECT_THROW(trySchema("location", FieldClass::GEO_POINT, IndexMode::MATCH), SchemaError);
  EXPECT_THROW(trySchema("emb", FieldClass::VECTOR, IndexMode::MATCH), SchemaError);

  // Accepted modes map onto the internal flags
  EXPECT_TRUE(trySchema("tag", FieldClass::STRING, IndexMode::MATCH)->getFieldTypePtr("tag")->indexed());
  EXPECT_FALSE(trySchema("tag", FieldClass::STRING, IndexMode::NONE)->getFieldTypePtr("tag")->indexed());
  EXPECT_FALSE(trySchema("price", FieldClass::INT, IndexMode::NONE)->getFieldTypePtr("price")->indexed());

  // Absent: the type default applies
  EXPECT_TRUE(trySchema("tag", FieldClass::STRING, std::nullopt)->getFieldTypePtr("tag")->indexed());
  EXPECT_FALSE(trySchema("price", FieldClass::INT, std::nullopt)->getFieldTypePtr("price")->indexed());
}

TEST_F(SchemaTest, geoPointRoundTripsAndSurvivesMergeReconstruction) {
  SchemaBuilder b;
  auto& loc = b.field("location");
  loc.type = FieldClass::GEO_POINT;
  loc.index = IndexMode::RANGE;
  loc.multi = true;

  auto schema = b.build();
  FieldType* location = schema->getFieldTypePtr("location");
  ASSERT_NE(nullptr, location);
  EXPECT_EQ(FieldType::GEO_POINT, location->type());
  EXPECT_TRUE(location->rangeIndexed());
  EXPECT_TRUE(location->hasColumn());
  EXPECT_TRUE(location->multiValued());

  std::pmr::monotonic_buffer_resource arena;
  api::SchemaDef persisted;
  schema->toProto(&persisted, arena);
  auto roundTripped = Schema::fromProto(persisted);
  EXPECT_EQ(FieldType::GEO_POINT,
            roundTripped->getFieldTypePtr("location")->type());
  EXPECT_TRUE(roundTripped->getFieldTypePtr("location")->rangeIndexed());

  SchemaBuilder mergeB;
  mergeB.field("quantity").type = FieldClass::INT;
  auto merged = mergeB.build(schema.get());
  FieldType* mergedLocation = merged->getFieldTypePtr("location");
  ASSERT_NE(nullptr, mergedLocation);
  EXPECT_EQ(FieldType::GEO_POINT, mergedLocation->type());
  EXPECT_TRUE(mergedLocation->rangeIndexed());
  EXPECT_TRUE(mergedLocation->multiValued());
}

TEST_F(SchemaTest, rangeRoundTripsAndRequiresColumn) {
  SchemaBuilder b;
  auto& price = b.field("price");
  price.type = FieldClass::INT;
  price.index = IndexMode::RANGE;
  auto schema = b.build();
  ASSERT_TRUE(schema->getFieldTypePtr("price")->rangeIndexed());
  ASSERT_TRUE(schema->getFieldTypePtr("price")->hasColumn());

  std::pmr::monotonic_buffer_resource arena;
  api::SchemaDef roundTrip;
  schema->toProto(&roundTrip, arena);
  auto roundTripped = Schema::fromProto(roundTrip);
  EXPECT_TRUE(roundTripped->getFieldTypePtr("price")->rangeIndexed());

  SchemaBuilder mergeB;
  mergeB.field("quantity").type = FieldClass::INT;
  auto merged = mergeB.build(schema.get());
  EXPECT_TRUE(merged->getFieldTypePtr("price")->rangeIndexed());

  SchemaBuilder badB;
  auto& bad = badB.field("bad");
  bad.type = FieldClass::DOUBLE;
  bad.index = IndexMode::RANGE;
  bad.column = false;
  EXPECT_THROW(badB.build(), SchemaError);
}


// toProto echoes the authored def: an explicitly-set index mode survives, an
// unset one stays absent (the type default is applied at resolve time, not
// materialized into the source).
TEST_F(SchemaTest, indexModeToProto) {
  SchemaBuilder b;
  auto& tag = b.field("tag");
  tag.type = FieldClass::STRING;
  tag.index = IndexMode::MATCH;
  b.field("price").type = FieldClass::INT;

  auto schema = b.build();
  std::pmr::monotonic_buffer_resource arena;
  api::SchemaDef out;
  schema->toProto(&out, arena);

  const api::FieldDef* tagOut = out.fields.find("tag");
  ASSERT_NE(nullptr, tagOut);
  ASSERT_TRUE(tagOut->index.has_value());
  EXPECT_EQ(IndexMode::MATCH, *tagOut->index);

  const api::FieldDef* priceOut = out.fields.find("price");
  ASSERT_NE(nullptr, priceOut);
  EXPECT_FALSE(priceOut->index.has_value()) << "unset index stays absent in the authored echo";
}


TEST_F(SchemaTest, missingFieldClass) {
  SchemaBuilder b;
  // Field with no type and no parent - should error
  b.field("broken");
  EXPECT_THROW(b.build(), SchemaError);
}


TEST_F(SchemaTest, schemaPersistence) {
  CollectionHelper ch;

  // Set a custom schema with an explicit "title" field
  SchemaBuilder b;
  auto& titleField = b.field("title");
  titleField.type = FieldClass::TEXT;
  titleField.index = IndexMode::MATCH;
  b.analyzer(titleField, "whitespace", {"lowercase"});
  b.set(ch.collection());

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
  std::pmr::monotonic_buffer_resource arena;
  InputStream is = file->getInputStream();
  api::SchemaDef persistedDef;
  std::span<const char> persistedBytes(is.ptr(), (size_t)is.left());
  auto paddedPersistedBytes = api::copyToPaddedInput(std::as_bytes(persistedBytes), arena);
  ASSERT_TRUE(api::decode(persistedDef, paddedPersistedBytes, arena));

  // The persisted def should contain "title" field
  const api::FieldDef* title = persistedDef.fields.find("title");
  ASSERT_NE(nullptr, title) << "Persisted schema should contain 'title' field";
  EXPECT_EQ(FieldClass::TEXT, *title->type);

  ch.collection().setSchema(Schema::createDefaultSchema());
}


TEST_F(SchemaTest, schemaGenIncrements) {
  CollectionHelper ch;

  // Set schema first time
  SchemaBuilder b1;
  b1.field("field1").type = FieldClass::STRING;
  b1.set(ch.collection());
  uint64_t gen1 = ch.collection().getSchema()->gen_;

  // Set schema second time
  SchemaBuilder b2;
  b2.field("field2").type = FieldClass::INT;
  b2.set(ch.collection());
  uint64_t gen2 = ch.collection().getSchema()->gen_;

  EXPECT_GT(gen2, gen1) << "Schema gen should increment on each setSchema";

  // Old schema file should be cleaned up
  auto oldFile = ch.collection().getShard()->getDirectory()->openFile(schemaFileName(gen1));
  EXPECT_EQ(nullptr, oldFile.get()) << "Old schema file should be deleted";

  // New schema file should exist
  auto newFile = ch.collection().getShard()->getDirectory()->openFile(schemaFileName(gen2));
  EXPECT_NE(nullptr, newFile.get()) << "New schema file should exist";

  ch.collection().setSchema(Schema::createDefaultSchema());
}


TEST_F(SchemaTest, schemaGenWrittenToIndexInfo) {
  CollectionHelper ch;

  // Set a custom schema
  SchemaBuilder b;
  auto& f = b.field("title");
  f.type = FieldClass::TEXT;
  f.index = IndexMode::MATCH;
  b.analyzer(f, "whitespace");
  b.set(ch.collection());
  uint64_t expectedGen = ch.collection().getSchema()->gen_;

  // Index a doc and commit so IndexInfo gets written
  Doc doc = {{"id", std::string("1")}, {"title_s", std::string("test")}};
  ch.index(doc);
  ch.commit();

  // Read IndexInfo directly and check schema_gen
  auto& dir = *ch.collection().getShard()->getDirectory();
  auto indexInfoFile = dir.openFile("s.olux");
  ASSERT_NE(nullptr, indexInfoFile.get());

  std::pmr::monotonic_buffer_resource arena;
  InputStream is = indexInfoFile->getInputStream();
  api::IndexInfo indexInfo;
  std::span<const char> indexInfoBytes(is.ptr(), (size_t)is.left());
  auto paddedIndexInfoBytes = api::copyToPaddedInput(std::as_bytes(indexInfoBytes), arena);
  ASSERT_TRUE(api::decode(indexInfo, paddedIndexInfoBytes, arena));
  EXPECT_EQ(expectedGen, indexInfo.schema_gen) << "IndexInfo should contain the current schema_gen";

  // Check that SegmentInfo also has schema_gen
  ASSERT_GT(indexInfo.segments.size(), 0u);
  EXPECT_EQ(expectedGen, indexInfo.segments[0].schema_gen) << "SegmentInfo should have schema_gen";

  ch.collection().setSchema(Schema::createDefaultSchema());
}


TEST_F(SchemaTest, schemaLoadOnRestart) {
  CollectionHelper ch;

  // Set a custom schema with "title" field
  SchemaBuilder b;
  auto& f = b.field("title");
  f.type = FieldClass::TEXT;
  f.index = IndexMode::MATCH;
  b.analyzer(f, "whitespace", {"lowercase"});
  b.set(ch.collection());
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

  ch.collection().setSchema(Schema::createDefaultSchema());
}


TEST_F(SchemaTest, sourceDef) {
  // Verify that sourceDef_ preserves the original SchemaDef (including parent
  // references and the fields/templates split).
  SchemaBuilder b;
  auto& parent = b.templ("_wl");
  parent.type = FieldClass::TEXT;
  parent.index = IndexMode::MATCH;
  b.analyzer(parent, "whitespace", {"lowercase"});
  b.field("title").parent = "_wl";

  auto schema = b.build();
  ASSERT_FALSE(schema->sourceDef_.empty());

  // Parse back the sourceDef and verify it has parent references
  std::pmr::monotonic_buffer_resource arena;
  api::SchemaDef roundtripped;
  std::span<const char> sourceBytes(schema->sourceDef_.data(), schema->sourceDef_.size());
  auto paddedSourceBytes = api::copyToPaddedInput(std::as_bytes(sourceBytes), arena);
  ASSERT_TRUE(api::decode(roundtripped, paddedSourceBytes, arena));

  const api::FieldDef* title = roundtripped.fields.find("title");
  ASSERT_NE(nullptr, title);
  EXPECT_EQ("_wl", title->parent) << "sourceDef should preserve parent references";
  EXPECT_NE(nullptr, roundtripped.templates.find("_wl"))
    << "templates persist in their own map";
}


TEST_F(SchemaTest, loadSchemaAfterOldFileDeleted) {
  // Simulates the race where the schema file from IndexInfo's gen was already
  // cleaned up by a newer setSchema. loadSchema() must find the newer file.
  // Without the retry logic, this test fails because the only schema file
  // is manually deleted before calling loadSchema().
  CollectionHelper ch;

  // setSchema - writes _schema.<gen1>
  SchemaBuilder b1;
  b1.field("field1").type = FieldClass::STRING;
  b1.set(ch.collection());
  uint64_t gen1 = ch.collection().getSchema()->gen_;

  // Manually delete the schema file to simulate the race
  auto& dir = *ch.collection().getShard()->getDirectory();
  ASSERT_TRUE(dir.deleteFile(schemaFileName(gen1)));

  // loadSchema should fail - no schema files remain
  EXPECT_FALSE(ch.collection().loadSchema()) << "Should fail with no schema files";

  // Now do two setSchema calls so the first gen's file gets cleaned up naturally
  SchemaBuilder b2;
  b2.field("field2").type = FieldClass::INT;
  b2.set(ch.collection());

  SchemaBuilder b3;
  auto& f3 = b3.field("field3");
  f3.type = FieldClass::INT;
  f3.column = true;
  b3.set(ch.collection());
  uint64_t gen3 = ch.collection().getSchema()->gen_;

  // loadSchema should find the latest file
  ASSERT_TRUE(ch.collection().loadSchema());
  auto loadedSchema = ch.collection().getSchema();
  EXPECT_EQ(gen3, loadedSchema->gen_) << "Should have loaded the newest schema generation";
  EXPECT_NE(nullptr, loadedSchema->getFieldTypePtr("field3"))
    << "Loaded schema should contain 'field3' from the newest schema";

  ch.collection().setSchema(Schema::createDefaultSchema());
}
