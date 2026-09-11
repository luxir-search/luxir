// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0


#include <gtest/gtest.h>

#include "luxir/schema/Schema.h"
#include "luxir/schema/FieldType.h"
#include "luxir/store/InputStream.h"
#include "luxir/store/OutputStream.h"
#include "luxir/reader/Postings.h"
#include "luxir/api/padded_input.h"
#include "luxir/api/luxir_index.hpp"
#include "luxir/api/luxir_types.hpp"
#include "test/SchemaBuilder.h"
#include "test/LuxirTest.h"
#include "test/CollectionHelper.h"
#include "test/LocalReq.h"
#include "test/TestUtils.h"

#include <memory_resource>
#include <span>

using namespace luxir;
using namespace luxir::test;

namespace api = luxir::api;
using FieldClass = luxir::api::FieldDef::FieldClass;
using IndexMode = luxir::api::FieldDef::IndexMode;

static std::string schemaFileName(uint64_t gen) {
  return "_schema_" + Postings::getSortableString(gen);
}

class SchemaTest : public LuxirTest {};


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
  ASSERT_EQ(nullptr, schema->getFieldTypePtr("_un"));
  ASSERT_EQ(nullptr, schema->getFieldTypePtr("_i"));

  // But suffix matching should work
  ASSERT_NE(nullptr, schema->getFieldTypePtr("title_s"));
  ASSERT_NE(nullptr, schema->getFieldTypePtr("body_w"));
  ASSERT_NE(nullptr, schema->getFieldTypePtr("body_un"));
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
  auto& parent = b.templ("_un");
  parent.type = FieldClass::TEXT;
  parent.index = IndexMode::MATCH;
  b.analyzer(parent, "whitespace", {"lowercase"});
  b.field("title").parent = "_un";

  auto schema = b.build();

  // title should inherit TEXT type and analyzer from _un
  auto* title = schema->getFieldTypePtr("title");
  ASSERT_NE(nullptr, title);
  EXPECT_EQ(FieldType::TEXT, title->type());
  EXPECT_TRUE(title->indexed());
  EXPECT_FALSE(title->isAbstract());

  // _un is a template, should not be found by exact name
  EXPECT_EQ(nullptr, schema->getFieldTypePtr("_un"));
  // But suffix matching should work
  EXPECT_NE(nullptr, schema->getFieldTypePtr("body_un"));
}


TEST_F(SchemaTest, inheritanceOverride) {
  SchemaBuilder b;
  auto& parent = b.templ("_un");
  parent.type = FieldClass::TEXT;
  parent.index = IndexMode::MATCH;
  b.analyzer(parent, "whitespace", {"lowercase"});
  // Child overrides the analyzer
  auto& child = b.field("title");
  child.parent = "_un";
  b.analyzer(child, "keyword");

  auto schema = b.build();

  auto* title = schema->getFieldTypePtr("title");
  ASSERT_NE(nullptr, title);
  EXPECT_EQ(FieldType::TEXT, title->type());

  // Check that the analyzer was overridden
  auto* textFt = (TextFieldType*)(title);
  EXPECT_EQ("keyword", textFt->analyzer_->tokenizer->name);
  EXPECT_TRUE(textFt->analyzer_->filters.empty());  // override is atomic, so parent's filters are not kept
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

  // Merge: add "title" that inherits from "_un" in the base schema
  SchemaBuilder b;
  b.field("title").parent = "_un";
  auto merged = b.build(baseSchema.get());

  // "title" should inherit TEXT type and analyzer from base's _un
  auto* title = merged->getFieldTypePtr("title");
  ASSERT_NE(nullptr, title);
  EXPECT_EQ(FieldType::TEXT, title->type());
  EXPECT_TRUE(title->indexed());
  EXPECT_FALSE(title->isAbstract());

  auto* textFt = (TextFieldType*)(title);
  EXPECT_EQ("unicode_word", textFt->analyzer_->tokenizer->name);  // inherited from _un
  ASSERT_EQ(1, textFt->analyzer_->filters.size());
  EXPECT_EQ("nfkc_cf", textFt->analyzer_->filters[0]->name);

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
  EXPECT_EQ("whitespace", ((TextFieldType*)base->getFieldTypePtr("title"))->analyzer_->tokenizer->name);

  SchemaBuilder mergeB;
  auto& newTmpl = mergeB.templ("_body");
  newTmpl.type = FieldClass::TEXT;
  mergeB.analyzer(newTmpl, "keyword");
  auto merged = mergeB.build(base.get());

  EXPECT_EQ("keyword", ((TextFieldType*)merged->getFieldTypePtr("title"))->analyzer_->tokenizer->name)
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
  // A CONCRETE field cannot even be named like a suffix: the leading
  // underscore namespace is reserved, so nothing concrete can shadow
  // template resolution.
  SchemaBuilder b;
  b.field("_w").type = FieldClass::STRING;
  EXPECT_THROW(b.build(), SchemaError);
}


TEST_F(SchemaTest, nameRules) {
  {
    SchemaBuilder b;
    b.field("9lives").type = FieldClass::STRING;
    EXPECT_THROW(b.build(), SchemaError);
  }
  {
    SchemaBuilder b;
    b.field("bad-name").type = FieldClass::STRING;
    EXPECT_THROW(b.build(), SchemaError);
  }
  {
    SchemaBuilder b;
    b.templ("noUnderscore").type = FieldClass::STRING;
    EXPECT_THROW(b.build(), SchemaError);
  }
  {
    // Field names land in filenames, so length is bounded.
    SchemaBuilder b;
    b.field(std::string(128, 'a')).type = FieldClass::STRING;
    EXPECT_THROW(b.build(), SchemaError);
  }
  {
    // Mixed case is fine for fields; _version_ passes as the one reserved
    // name; 127 bytes is the length bound.
    SchemaBuilder b;
    b.field("camelCase").type = FieldClass::STRING;
    b.field(std::string(127, 'a')).type = FieldClass::STRING;
    auto s = b.build();
    EXPECT_NE(nullptr, s->getFieldTypePtr("camelCase"));
    EXPECT_NE(nullptr, s->getFieldTypePtr("_version_"));
  }
}


TEST_F(SchemaTest, caseFoldedDuplicateRejected) {
  // Two names differing only by case are a typo, not two fields.
  SchemaBuilder b;
  b.field("Title").type = FieldClass::STRING;
  b.field("title").type = FieldClass::STRING;
  EXPECT_THROW(b.build(), SchemaError);
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
  {
    // Parameters on a component that takes none: teaching error naming them.
    std::pmr::monotonic_buffer_resource mr;
    api::SchemaDef def;
    std::string err;
    ASSERT_TRUE(api::read_json(def,
        R"({"fields":{"t":{"type":"text","analyzer":{"filters":[{"name":"lowercase","params":{"lang":"en"}}]}}}})",
        mr, &err)) << err;
    try {
      Schema::fromProto(def);
      FAIL() << "params on a parameterless filter must be rejected";
    } catch (const SchemaError& e) {
      EXPECT_NE(std::string::npos,
                std::string(e.what()).find("filter 'lowercase' takes no parameters (got: lang) (field: t)"))
          << e.what();
    }
  }
  {
    // A tokenizer component without a name is an error, not a silent
    // whitespace default: params-only input must not be discarded.
    std::pmr::monotonic_buffer_resource mr;
    api::SchemaDef def;
    std::string err;
    ASSERT_TRUE(api::read_json(def,
        R"({"fields":{"t":{"type":"text","analyzer":{"tokenizer":{"params":{"x":1}}}}}})", mr, &err)) << err;
    try {
      Schema::fromProto(def);
      FAIL() << "nameless tokenizer accepted";
    } catch (const SchemaError& e) {
      EXPECT_NE(std::string::npos, std::string(e.what()).find("tokenizer component has no name")) << e.what();
    }
  }
  {
    // Tokenizer params are validated too, including on the fused
    // unicode_word + nfkc_cf pair (fusion happens after validation).
    std::pmr::monotonic_buffer_resource mr;
    api::SchemaDef def;
    std::string err;
    ASSERT_TRUE(api::read_json(def,
        R"({"fields":{"t":{"type":"text","analyzer":{"tokenizer":{"name":"unicode_word","params":{"max":5}},"filters":["nfkc_cf"]}}}})",
        mr, &err)) << err;
    try {
      Schema::fromProto(def);
      FAIL() << "tokenizer params accepted";
    } catch (const SchemaError& e) {
      EXPECT_NE(std::string::npos,
                std::string(e.what()).find("tokenizer 'unicode_word' takes no parameters (got: max)")) << e.what();
    }
  }
}

TEST_F(SchemaTest, analyzerPresenceSemantics) {
  // An analyzer that says nothing inherits; one that names a tokenizer or any
  // filter replaces the parent's whole analyzer (filters-only gets whitespace).
  SchemaBuilder b;
  auto& parent = b.templ("_body");
  parent.type = FieldClass::TEXT;
  b.analyzer(parent, "unicode_word", {"nfkc_cf"});
  auto& inherits = b.field("inherits");
  inherits.parent = "_body";
  inherits.analyzer.emplace();  // {}
  auto& filtersOnly = b.field("filters_only");
  filtersOnly.parent = "_body";
  b.analyzer(filtersOnly, std::nullopt, {"lowercase"});
  auto schema = b.build();

  auto analyzerOf = [&](const char* name) {
    return ((TextFieldType*)schema->getFieldTypePtr(name))->analyzer_;
  };
  EXPECT_EQ("unicode_word", analyzerOf("inherits")->tokenizer->name);
  ASSERT_EQ(1u, analyzerOf("inherits")->filters.size());
  EXPECT_EQ("whitespace", analyzerOf("filters_only")->tokenizer->name);
  ASSERT_EQ(1u, analyzerOf("filters_only")->filters.size());
  EXPECT_EQ("lowercase", analyzerOf("filters_only")->filters[0]->name);
}

TEST_F(SchemaTest, analyzerCompiledOncePerDefinition) {
  // Fields resolving to the same authored definition share one compiled
  // Analyzer; an identical definition authored separately is its own.
  SchemaBuilder b;
  auto& tmpl = b.templ("_body");
  tmpl.type = FieldClass::TEXT;
  b.analyzer(tmpl, "unicode_word", {"nfkc_cf", "fold"});
  b.field("a").parent = "_body";
  b.field("b").parent = "_body";
  auto& own = b.field("c");
  own.type = FieldClass::TEXT;
  b.analyzer(own, "unicode_word", {"nfkc_cf", "fold"});
  auto schema = b.build();

  auto analyzerOf = [&](const char* name) {
    return ((TextFieldType*)schema->getFieldTypePtr(name))->analyzer_;
  };
  EXPECT_EQ(analyzerOf("a"), analyzerOf("b"));
  EXPECT_NE(analyzerOf("a"), analyzerOf("c"));
  EXPECT_TRUE(analyzerOf("c")->fusedHead);
  EXPECT_TRUE(analyzerOf("c")->stateful);
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
  b.field("apple").parent = "_un";
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
  const api::FieldDef* zebra = out.fields.at("zebra").operator->();
  ASSERT_NE(nullptr, zebra);
  EXPECT_TRUE(zebra->type.has_value());
  EXPECT_FALSE(zebra->index.has_value());
  EXPECT_FALSE(zebra->column.has_value());

  // Parent reference preserved (not resolved away).
  const api::FieldDef* apple = out.fields.at("apple").operator->();
  ASSERT_NE(nullptr, apple);
  EXPECT_EQ("_un", apple->parent);

  // Templates carried in their own map, alpha-ordered.
  ASSERT_GT(out.templates.size(), 0u);
  for (std::size_t i = 1; i < out.templates.size(); i++) {
    EXPECT_LT(out.templates[i - 1].first, out.templates[i].first);
  }
  EXPECT_NE(nullptr, out.templates.find("_un"));
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

  const api::FieldDef* tagOut = out.fields.at("tag").operator->();
  ASSERT_NE(nullptr, tagOut);
  ASSERT_TRUE(tagOut->index.has_value());
  EXPECT_EQ(IndexMode::MATCH, *tagOut->index);

  const api::FieldDef* priceOut = out.fields.at("price").operator->();
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
  auto persistedSchema = Schema::decodeStored(std::as_bytes(persistedBytes));
  persistedSchema->toProto(&persistedDef, arena);

  // The persisted def should contain "title" field
  const api::FieldDef* title = persistedDef.fields.at("title").operator->();
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

// A protobuf length-delimited field: tag byte + one-byte length + payload.
static std::string pbLen(int field, std::string_view payload) {
  std::string out;
  out += (char)((field << 3) | 2);
  out += (char)payload.size();
  out += payload;
  return out;
}

TEST_F(SchemaTest, oldAnalyzerSchemaFileIsRejectedAtLoad) {
  // AnalyzerDef.tokenizer / filters used to be strings at the same tags.  A
  // schema file from then must fail to load loudly, not decode to a schema
  // that silently lost its analyzer.  Hand-encoded old shape, for every
  // component name that existed:
  //   SchemaDef{fields: {"t": FieldDef{type: TEXT, analyzer: {tokenizer, [filter]}}}}
  CollectionHelper ch;
  auto& dir = *ch.collection().getShard()->getDirectory();
  auto installed = ch.collection().getSchema();
  // Tokenizer-only and filter-only payloads for every old name, so each field
  // is rejected on its own, plus the common default pair.
  const std::pair<const char*, const char*> combos[] = {
      {"whitespace", nullptr}, {"keyword", nullptr}, {"unicode_word", nullptr},
      {nullptr, "lowercase"}, {nullptr, "nfkc_cf"}, {nullptr, "fold"},
      {"unicode_word", "nfkc_cf"}};
  for (const auto& [tokenizer, filter] : combos) {
    std::string analyzer;
    if (tokenizer != nullptr) analyzer += pbLen(1, tokenizer);
    if (filter != nullptr) analyzer += pbLen(2, filter);
    std::string fieldDef = std::string("\x10\x01") + pbLen(6, analyzer);
    std::string entry = pbLen(1, "t") + pbLen(2, fieldDef);
    std::string schemaDef = pbLen(1, entry);

    std::string fileName = schemaFileName(1000000);  // sorts after any live generation
    auto file = dir.createFile(fileName);
    OutputStream out;
    out.setFile(&*file);
    out.write(schemaDef.data(), schemaDef.size());
    out.close();
    dir.finishFile(*file);
    std::vector<std::string> syncFiles = {fileName, "."};
    dir.sync(syncFiles);

    try {
      ch.collection().loadSchema();
      FAIL() << "old-format schema file loaded for " << (tokenizer ? tokenizer : "-") << "/"
             << (filter ? filter : "-");
    } catch (const std::runtime_error& e) {
      EXPECT_NE(std::string::npos, std::string(e.what()).find("Failed to parse schema file")) << e.what();
    }
    EXPECT_EQ(installed, ch.collection().getSchema()) << "a failed load leaves the installed schema alone";
    dir.deleteFile(fileName);
  }
  ch.collection().setSchema(Schema::createDefaultSchema());
}

TEST_F(SchemaTest, sourceDef) {
  // Verify that sourceDef_ preserves the original SchemaDef (including parent
  // references and the fields/templates split).
  SchemaBuilder b;
  auto& parent = b.templ("_un");
  parent.type = FieldClass::TEXT;
  parent.index = IndexMode::MATCH;
  b.analyzer(parent, "whitespace", {"lowercase"});
  b.field("title").parent = "_un";

  auto schema = b.build();
  ASSERT_FALSE(schema->sourceDef_.empty());

  // Parse back the sourceDef and verify it has parent references
  std::pmr::monotonic_buffer_resource arena;
  api::SchemaDef roundtripped;
  std::span<const char> sourceBytes(schema->sourceDef_.data(), schema->sourceDef_.size());
  auto paddedSourceBytes = api::copyToPaddedInput(std::as_bytes(sourceBytes), arena);
  ASSERT_TRUE(api::decode(roundtripped, paddedSourceBytes, arena));

  const api::FieldDef* title = roundtripped.fields.at("title").operator->();
  ASSERT_NE(nullptr, title);
  EXPECT_EQ("_un", title->parent) << "sourceDef should preserve parent references";
  EXPECT_NE(nullptr, roundtripped.templates.find("_un"))
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

// Keep the parser, schema compiler, and persisted source on the same path in
// these fixtures. Returned schemas own all resolved state after this arena dies.
static std::shared_ptr<Schema> schemaJson(std::string_view json, const Schema* base = nullptr) {
  std::pmr::monotonic_buffer_resource arena;
  api::SchemaDef def;
  std::string error;
  if (!api::read_json(def, json, arena, &error)) throw std::runtime_error(error);
  return Schema::fromProto(def, base);
}

static std::string authoredJson(const Schema& schema) {
  std::pmr::monotonic_buffer_resource arena;
  api::SchemaDef def;
  schema.toProto(&def, arena);
  std::string json;
  if (!api::write_json(def, json)) throw std::runtime_error("schema JSON encode failed");
  return json;
}

TEST_F(SchemaTest, longTermsInheritanceAndWirePresence) {
  auto s = schemaJson(R"({"templates":{
    "_strict":{"type":"string","long_terms":"reject"},
    "_child":{"parent":"_strict"},
    "_loose":{"parent":"_child","long_terms":"truncate"},
    "_hash":{"parent":"_child","long_terms":"hash128"}
  },"fields":{
    "plain":"string", "text":"text", "column":{"type":"string","index":"none"},
    "inherited":{"parent":"_child"},
    "title":{"type":"text","long_terms":"reject","variants":{
      "s":{"parent":"_child"}, "loose":{"parent":"_child","long_terms":"truncate"}, "raw":"string"
    }}
  }})");
  for (auto name : {"inherited", "title", "title__s", "dynamic_child"}) {
    EXPECT_EQ(TermPolicy::REJECT, s->physical(name)->longTerms) << name;
  }
  for (auto name : {"plain", "text", "column", "title__raw", "dynamic_hash", "id"}) {
    EXPECT_EQ(TermPolicy::HASH128, s->physical(name)->longTerms) << name;
  }
  for (auto name : {"title__loose", "dynamic_loose"}) {
    EXPECT_EQ(TermPolicy::TRUNCATE, s->physical(name)->longTerms) << name;
  }
  std::pmr::monotonic_buffer_resource arena;
  api::SchemaDef out;
  s->toProto(&out, arena);
  EXPECT_FALSE(out.fields.at("plain")->long_terms);
  EXPECT_FALSE(out.fields.at("inherited")->long_terms);
  EXPECT_EQ(api::FieldDef::LongTerms::TRUNCATE, out.templates.at("_loose")->long_terms);
  EXPECT_EQ(api::FieldDef::LongTerms::HASH128, out.templates.at("_hash")->long_terms);
  std::vector<std::byte> bytes;
  ASSERT_TRUE(api::encode(out, bytes));
  api::SchemaDef decoded;
  ASSERT_TRUE(api::decode(decoded, api::copyToPaddedInput(bytes, arena), arena));
  EXPECT_EQ(s->sourceDef_, Schema::fromProto(decoded)->sourceDef_);
  auto json = authoredJson(*s);
  EXPECT_NE(std::string::npos, json.find("\"long_terms\":\"truncate\""));
  EXPECT_NE(std::string::npos, json.find("\"long_terms\":\"reject\""));
  EXPECT_NE(std::string::npos, json.find("\"long_terms\":\"hash128\""));
  EXPECT_EQ(s->sourceDef_, schemaJson(json)->sourceDef_);
}

TEST_F(SchemaTest, longTermsRequiresEligibleTypeAndKnownPolicy) {
  for (auto policy : {"hash128", "truncate", "reject"}) {
    for (auto type : {"int", "float", "double", "date", "vector", "geo_point"}) {
      auto json = std::format(R"({{"fields":{{"{}":{{"type":"{}","long_terms":"{}"}}}}}})",
                              "bad", type, policy);
      SCOPED_TRACE(json);
      EXPECT_THROW(schemaJson(json), SchemaError);
    }
    EXPECT_NO_THROW(schemaJson(std::format(
        R"({{"fields":{{"id":{{"type":"id","long_terms":"{}"}}}}}})", policy)));
    EXPECT_THROW(schemaJson(std::format(
        R"({{"fields":{{"bad":{{"type":"string","index":"none","long_terms":"{}"}}}}}})", policy)), SchemaError);
  }
  for (auto json : {
      R"({"templates":{"_s":{"type":"string","long_terms":"reject"}},"fields":{"bad":{"parent":"_s","index":"none"}}})",
      R"({"templates":{"_s":{"type":"string","long_terms":"reject"}},"fields":{"bad":{"parent":"_s","type":"int"}}})",
      R"({"fields":{"bad":{"type":"text","variants":{"s":{"type":"string","index":"none","long_terms":"truncate"}}}}})"}) {
    SCOPED_TRACE(json);
    EXPECT_THROW(schemaJson(json), SchemaError);
  }
  EXPECT_THROW(schemaJson(R"({"fields":{"bad":{"type":"string","long_terms":"unknown"}}})"), std::runtime_error);
  EXPECT_THROW(schemaJson(R"({"fields":{"bad":{"type":"string","long_terms":99}}})"), std::runtime_error);
  SchemaBuilder b;
  b.field("bad").type = FieldClass::TEXT;
  b.field("bad").long_terms = (api::FieldDef::LongTerms)-1;
  EXPECT_THROW(b.build(), SchemaError);
  // TEXT accepts the policy independently of its index mode.
  EXPECT_NO_THROW(schemaJson(R"({"fields":{"body":{"type":"text","index":"none","long_terms":"reject"}}})"));
}

TEST_F(SchemaTest, variantsResolveByOperationAndExplicitSelectors) {
  auto s = schemaJson(R"({"fields":{
    "author":{"type":"text","variants":{"s":"string"},"defaults":{"value":"s"}},
    "genre":{"type":"string","variants":{"t":"text"},"defaults":{"search":"t"}},
    "edition":{"type":"int","index":"range","variants":{"label":"string"}},
    "self":"string", "f_":{"type":"text","variants":{"s":"string"}}
  }})");
  auto input = s->resolveInput("author");
  EXPECT_EQ("author", input.logicalName);
  EXPECT_EQ("author", input.physicalName);
  EXPECT_EQ(FieldRole::PRIMARY, input.role);
  EXPECT_EQ(input.fieldType, input.owner->primary.get());
  EXPECT_TRUE(input.fieldType->isStored());
  EXPECT_EQ(LogicalField::Shape::SCALAR, input.owner->shape);
  EXPECT_EQ("author", s->resolveFor("author", OpClass::SEARCH).physicalName);
  EXPECT_EQ("author__s", s->resolveFor("author", OpClass::VALUE).physicalName);
  EXPECT_EQ("genre__t", s->resolveFor("genre", OpClass::SEARCH).physicalName);
  EXPECT_EQ("genre", s->resolveFor("genre", OpClass::VALUE).physicalName);
  EXPECT_EQ("edition", s->resolveFor("edition", OpClass::VALUE).physicalName);
  EXPECT_EQ(FieldType::STRING, s->physical("edition__label")->type());
  for (auto op : {OpClass::SEARCH, OpClass::VALUE, OpClass::EXISTS, OpClass::RETRIEVE}) {
    EXPECT_EQ("author", s->resolveFor("author__self", op).physicalName);
    auto variant = s->resolveFor("author__s", op);
    EXPECT_EQ("author__s", variant.physicalName);
    EXPECT_EQ(FieldRole::VARIANT, variant.role);
    EXPECT_EQ(input.owner, variant.owner);
    EXPECT_EQ(variant.fieldType, variant.owner->variants.at("s").get());
    EXPECT_EQ("_version_", s->resolveFor("_version___self", op).physicalName);
    EXPECT_EQ("self", s->resolveFor("self__self", op).physicalName);
    EXPECT_EQ("f___s", s->resolveFor("f___s", op).physicalName);
  }
  EXPECT_EQ("author", s->resolveFor("author", OpClass::EXISTS).physicalName);
  EXPECT_EQ("author", s->resolveFor("author", OpClass::RETRIEVE).physicalName);
  auto* derived = s->physical("author__s");
  EXPECT_TRUE(derived->isDerived());
  EXPECT_FALSE(derived->isAbstract());
  EXPECT_FALSE(derived->isStored());
  EXPECT_EQ(derived, s->fieldTypeMap.at("author__s").get());
  EXPECT_EQ(derived, s->getFieldTypePtr("author__s"));
  EXPECT_EQ(input.fieldType, s->physical("author")); // physical never follows a binding
  EXPECT_THROW(s->physical("author__self"), RequestError);
  EXPECT_EQ(nullptr, s->getFieldTypePtr("author__self"));
  EXPECT_FALSE(s->fieldTypeMap.contains("author__self"));
}

TEST_F(SchemaTest, variantSelectorErrorsAndInputSeparation) {
  auto s = schemaJson(R"({"templates":{"_t":{"type":"text","variants":{"s":"int"}}},
                         "fields":{"a":{"parent":"_t"}}})");
  for (auto name : {"a__s", "a__self", "_version___self", "a__", "external__a"}) {
    SCOPED_TRACE(name);
    EXPECT_THROW(s->resolveInput(name), RequestError);
  }
  for (auto name : {"missing", "missing__s", "a__missing", "a__S", "a__", "a__1s",
                    "a__s__s", "a__s_!", "a__*", "_t", "_t__s", "_t__self", "_stored_"}) {
    SCOPED_TRACE(name);
    EXPECT_THROW(s->resolveFor(name, OpClass::SEARCH), RequestError);
  }
  EXPECT_EQ("a", s->resolveFor("a__SeLf", OpClass::SEARCH).physicalName);
  EXPECT_EQ(nullptr, s->getFieldTypePtr("a__missing"));
}

TEST_F(SchemaTest, variantsTemplateInstancesResolveRootBeforeSuffix) {
  auto base = Schema::createDefaultSchema();
  auto s = schemaJson(R"({"templates":{
    "_t":{"type":"text","variants":{"s":"int"},"defaults":{"value":"s"}},
    "_ts":{"parent":"_t","multi":true}
  },"fields":{"concrete_t":"string"}})", base.get());
  auto variant = s->resolveFor("book_t__s", OpClass::VALUE);
  EXPECT_EQ(FieldType::INT, variant.fieldType->type());
  EXPECT_EQ("book_t", variant.logicalName);
  EXPECT_EQ("book_t__s", variant.physicalName);
  EXPECT_EQ("_t__s", variant.fieldType->name());
  EXPECT_TRUE(variant.fieldType->isDerived());
  EXPECT_TRUE(variant.fieldType->isAbstract());
  EXPECT_EQ(variant.fieldType, s->physical("book_t__s"));
  EXPECT_EQ(variant.fieldType, s->getFieldTypePtr("book_t__s"));
  EXPECT_EQ(variant.fieldType, s->resolveFor("book_t", OpClass::VALUE).fieldType);
  EXPECT_EQ("book_t", s->resolveInput("book_t").physicalName);
  EXPECT_EQ("book_t", s->resolveFor("book_t__self", OpClass::VALUE).physicalName);
  EXPECT_TRUE(s->resolveFor("book_ts__s", OpClass::VALUE).fieldType->multiValued());
  EXPECT_EQ(FieldType::STRING, s->physical("other_s")->type());
  EXPECT_THROW(s->resolveFor("concrete_t__s", OpClass::VALUE), RequestError);
  EXPECT_EQ(nullptr, s->getFieldTypePtr("unknown__s")); // never falls through to _s
  EXPECT_EQ(nullptr, s->getFieldTypePtr("_t__s"));
  EXPECT_FALSE(s->fieldTypeMap.contains("book_t"));
  EXPECT_EQ("_t", variant.owner->name);
  EXPECT_EQ(variant.fieldType, variant.owner->variants.at("s").get());
  auto other = s->resolveFor("other_t", OpClass::VALUE);
  EXPECT_EQ("other_t", other.logicalName);
  EXPECT_EQ("other_t__s", other.physicalName);
  EXPECT_EQ(variant.owner, other.owner);
  EXPECT_EQ(variant.fieldType, other.fieldType);
  auto input = s->resolveInput("book_t");
  EXPECT_EQ(variant.owner, input.owner);
  EXPECT_EQ(s->fieldTypeMap.at("_t").get(), input.fieldType);
  EXPECT_EQ("_t", input.fieldType->name());
  EXPECT_TRUE(input.fieldType->isAbstract());
}

TEST_F(SchemaTest, variantsAtomicInheritanceAndDefaultsClearing) {
  auto s = schemaJson(R"({"templates":{
    "_base":{"type":"text","multi":true,"variants":{"s":"string","i":"int"},
             "defaults":{"search":"s","value":"i"}},
    "_child":{"parent":"_base"}
  },"fields":{
    "inherited":{"parent":"_child"},
    "replaced":{"parent":"_base","variants":{"x":"float"},"defaults":{}},
    "cleared":{"parent":"_base","variants":{},"defaults":{}},
    "partial":{"parent":"_base","defaults":{"value":"s"}}
  }})");
  EXPECT_EQ("inherited__s", s->resolveFor("inherited", OpClass::SEARCH).physicalName);
  EXPECT_EQ("inherited__i", s->resolveFor("inherited", OpClass::VALUE).physicalName);
  EXPECT_TRUE(s->physical("inherited__s")->multiValued());
  EXPECT_EQ(1u, s->resolveInput("replaced").owner->variants.size());
  EXPECT_EQ(nullptr, s->getFieldTypePtr("replaced__s"));
  EXPECT_EQ("replaced", s->resolveFor("replaced", OpClass::VALUE).physicalName);
  EXPECT_TRUE(s->resolveInput("cleared").owner->variants.empty());
  EXPECT_EQ("cleared", s->resolveFor("cleared", OpClass::SEARCH).physicalName);
  EXPECT_EQ("partial", s->resolveFor("partial", OpClass::SEARCH).physicalName);
  EXPECT_EQ("partial__s", s->resolveFor("partial", OpClass::VALUE).physicalName);
}

TEST_F(SchemaTest, variantParentsBorrowOnlyPhysicalSettings) {
  auto s = schemaJson(R"({"templates":{
    "_strings":{"type":"string","multi":true,"stored":true,"stored_resource":"cold",
                "normalizer":["lowercase"],"variants":{"i":"int"},"defaults":{"value":"i"}},
    "_text":{"type":"text","multi":true,"stored":true,"analyzer":{"filters":["fold"]}}
  },"fields":{
    "a":{"type":"text","variants":{"s":{"parent":"_strings"},"t":{"parent":"_text"}}},
    "b":{"type":"string","multi":true,"variants":{"t":{"parent":"_text"}}},
    "c":{"type":"text","variants":{"again":{"parent":"c"}}}
  }})");
  auto input = s->resolveInput("a");
  EXPECT_TRUE(input.fieldType->isStored());
  EXPECT_FALSE(input.owner->multi);
  for (const auto& [label, ft] : input.owner->variants) {
    EXPECT_FALSE(ft->multiValued()) << label;
    EXPECT_FALSE(ft->isStored()) << label;
    EXPECT_EQ("_stored_", ft->storedResource_);
    EXPECT_EQ(input.owner, s->resolveFor("a__" + label, OpClass::VALUE).owner);
  }
  EXPECT_EQ(2u, input.owner->variants.size());
  EXPECT_TRUE(s->physical("b__t")->multiValued());
  EXPECT_FALSE(s->physical("b__t")->isStored()); // TEXT type default belongs to owner
  EXPECT_EQ("a", s->resolveFor("a", OpClass::VALUE).physicalName);
  auto* str = (StrFieldType*)s->physical("a__s");
  std::string value = "Whole VALUE";
  str->normalize(value);
  EXPECT_EQ("whole value", value);
  EXPECT_EQ(((TextFieldType*)s->physical("c"))->analyzer_,
            ((TextFieldType*)s->physical("c__again"))->analyzer_); // no spurious owner cycle
}

TEST_F(SchemaTest, variantOwnershipAndDepthErrors) {
  for (auto property : {R"("multi":false)", R"("multi":true)", R"("stored":false)",
                        R"("stored":true)", R"("stored_resource":"cold")", R"("stored_resource":"")",
                        R"("variants":{})", R"("variants":{"nested":"string"})", R"("defaults":{})",
                        R"("defaults":{"value":"self"})"}) {
    SCOPED_TRACE(property);
    EXPECT_THROW(schemaJson(std::string(R"({"fields":{"a":{"type":"text","variants":{"s":{"type":"string",)") +
                            property + "}}}}}"), SchemaError);
  }
  EXPECT_THROW(schemaJson(R"({"fields":{"a":{"type":"text","variants":{"s":"id"}}}})"), SchemaError);
  EXPECT_THROW(schemaJson(R"({"fields":{"a":{"type":"text","variants":{"s":{"parent":"id"}}}}})"), SchemaError);
  for (auto name : {"id", "_version_"}) {
    SCOPED_TRACE(name);
    std::string type = std::string(name) == "id" ? "id" : "int";
    EXPECT_THROW(schemaJson("{\"fields\":{\"" + std::string(name) + "\":{\"type\":\"" + type +
                            "\",\"variants\":{\"s\":\"string\"}}}}"), SchemaError);
    EXPECT_THROW(schemaJson("{\"templates\":{\"_base\":{\"type\":\"int\",\"variants\":{\"s\":\"string\"}}},"
                            "\"fields\":{\"" + std::string(name) + "\":{\"parent\":\"_base\",\"type\":\"" + type + "\"}}}"), SchemaError);
  }
}

TEST_F(SchemaTest, variantShapeFamiliesAndVectorDimensions) {
  for (auto primary : {"text", "string", "int", "float", "double", "date"}) {
    SCOPED_TRACE(primary);
    auto s = schemaJson(std::string(R"({"fields":{"a":{"type":")") + primary +
      R"(","variants":{"t":"text","s":"string","i":"int","f":"float","d":"double","dt":"date"}}}})");
    EXPECT_EQ(6u, s->resolveInput("a").owner->variants.size());
  }
  auto s = schemaJson(R"({"templates":{"_v":{"type":"vector","dims":3,"metric":"cosine"}},"fields":{
    "v":{"parent":"_v","multi":true,"variants":{"l2":{"type":"vector","dims":3,"metric":"l2"},
                                                   "same":{"parent":"_v"}}},
    "g":{"type":"geo_point","multi":true,"variants":{"range":{"type":"geo_point","index":"range"}}}
  }})");
  EXPECT_EQ(LogicalField::Shape::VECTOR, s->resolveInput("v").owner->shape);
  EXPECT_EQ(3, ((VectorFieldType*)s->physical("v__same"))->dims());
  EXPECT_TRUE(s->physical("v__l2")->multiValued());
  EXPECT_EQ(LogicalField::Shape::GEO, s->resolveInput("g").owner->shape);
  EXPECT_TRUE(s->physical("g__range")->rangeIndexed());
  for (auto field : {
      R"({"type":"vector","dims":2,"variants":{"s":"string"}})",
      R"({"type":"string","variants":{"v":{"type":"vector","dims":2}}})",
      R"({"type":"geo_point","variants":{"s":"int"}})",
      R"({"type":"text","variants":{"g":"geo_point"}})",
      R"({"type":"vector","dims":2,"variants":{"g":"geo_point"}})",
      R"({"type":"vector","variants":{"v":{"type":"vector","dims":2}}})",
      R"({"type":"vector","dims":0,"variants":{"v":{"type":"vector","dims":0}}})",
      R"({"type":"vector","dims":2,"variants":{"v":{"type":"vector"}}})",
      R"({"type":"vector","dims":2,"variants":{"v":{"type":"vector","dims":3}}})",
      R"({"type":"vector","dims":2,"variants":{"v":{"type":"vector","dims":-1}}})"}) {
    SCOPED_TRACE(field);
    EXPECT_THROW(schemaJson(std::string("{\"fields\":{\"a\":") + field + "}}"), SchemaError);
  }
}

TEST_F(SchemaTest, variantLabelGrammarAndReservedDelimiter) {
  for (auto label : {"", "_s", "1s", "s__x", "Self", "SELF", "self", "s-x", "s.x", "s!", "\xc3\xa9"}) {
    SCOPED_TRACE(label);
    EXPECT_FALSE(Schema::validVariantLabel(label));
    EXPECT_THROW(schemaJson(std::string("{\"fields\":{\"a\":{\"type\":\"text\",\"variants\":{\"") +
                            label + "\":\"string\"}}}}"), SchemaError);
  }
  for (auto label : {"s", "S1", "my_label", "x_"}) EXPECT_TRUE(Schema::validVariantLabel(label));
  EXPECT_THROW(schemaJson(R"({"fields":{"a":{"type":"text","variants":{"s":"int","S":"string"}}}})"), SchemaError);
  EXPECT_THROW(schemaJson(R"({"fields":{"a":{"type":"text","variants":{"s":"int","s":"string"}}}})"), SchemaError);
  EXPECT_FALSE(Schema::validFieldName("a__s"));
  EXPECT_FALSE(Schema::validTemplateName("__s"));
  EXPECT_FALSE(Schema::validTemplateName("_t__s"));
  EXPECT_TRUE(Schema::validFieldName("self"));
  EXPECT_THROW(schemaJson(R"({"fields":{"a__s":"string"}})"), SchemaError);
  EXPECT_THROW(schemaJson(R"({"templates":{"__s":"string"}})"), SchemaError);
  EXPECT_THROW(schemaJson(R"({"templates":{"_t__s":"string"}})"), SchemaError);
}

TEST_F(SchemaTest, variantParentsMustBeAuthoredAndDefaultsMustExist) {
  for (auto field : {R"({"type":"text","defaults":{"value":"s"}})",
                     R"({"type":"text","defaults":{"search":"missing"}})",
                     R"({"type":"text","defaults":{"value":""}})",
                     R"({"type":"text","variants":{"s":"string"},"defaults":{"value":"S"}})",
                     R"({"parent":"a__s"})",
                     R"({"type":"text","variants":{"s":{"parent":"a__self"}}})",
                     R"({"type":"text","variants":{"s":{"parent":"unknown"}}})",
                     R"({"type":"text","variants":{"s":{}}})"}) {
    SCOPED_TRACE(field);
    EXPECT_THROW(schemaJson(std::string("{\"fields\":{\"a\":") + field + "}}"), SchemaError);
  }
  auto base = schemaJson(R"({"templates":{"_t":{"type":"text","variants":{"s":"string"},
                                               "defaults":{"value":"s"}}}})");
  for (auto variants : {"{}", R"({"x":"int"})"}) {
    EXPECT_THROW(schemaJson(std::string(R"({"fields":{"a":{"parent":"_t","variants":)") +
                            variants + "}}}", base.get()), SchemaError);
  }
  auto explicitSelf = schemaJson(R"({"fields":{"a":{"type":"string","defaults":{"search":"SELF","value":"self"}}}})");
  EXPECT_EQ("a", explicitSelf->resolveFor("a", OpClass::SEARCH).physicalName);
}

TEST_F(SchemaTest, variantPhysicalLengthsAndUnboundedSelfSelector) {
  const std::string root(124, 'a');
  auto s = schemaJson("{\"fields\":{\"" + root + "\":{\"type\":\"text\",\"variants\":{\"s\":\"string\"}}}}");
  EXPECT_EQ(127u, s->resolveFor(root + "__s", OpClass::VALUE).physicalName.size());
  EXPECT_THROW(schemaJson("{\"fields\":{\"" + root + "b\":{\"type\":\"text\",\"variants\":{\"s\":\"string\"}}}}"), SchemaError);
  auto templ = schemaJson(R"({"templates":{"_t":{"type":"text","variants":{"short":"string","longest":"int"}}}})");
  std::string dynamic = std::string(116, 'a') + "_t"; // 127 - 2 - 7 = 118
  EXPECT_EQ(127u, templ->resolveFor(dynamic + "__longest", OpClass::VALUE).physicalName.size());
  dynamic = "a" + dynamic;
  EXPECT_THROW(templ->resolveInput(dynamic), RequestError);
  EXPECT_THROW(templ->resolveFor(dynamic + "__short", OpClass::VALUE), RequestError);
  EXPECT_THROW(templ->getFieldTypePtr(dynamic), RequestError);
  SchemaBuilder b;
  std::string longRoot(127, 'x');
  b.field(longRoot).type = FieldClass::STRING;
  auto longSchema = b.build();
  EXPECT_EQ(longRoot, longSchema->resolveFor(longRoot + "__self", OpClass::VALUE).physicalName);
}

TEST_F(SchemaTest, variantSparseJsonAndProtobufRoundTrips) {
  auto s = schemaJson(R"({"templates":{
    "_name":{"type":"text","variants":{"z":"int","s":{"type":"string","normalizer":["nfkc_cf","fold"]}},
             "defaults":{"value":"s"}},
    "_names":{"parent":"_name","multi":true}
  },"fields":{
    "author":{"parent":"_names"},
    "clear":{"parent":"_name","variants":{},"defaults":{}},
    "raw":{"type":"string","normalizer":[],"stored_resource":""}
  }})");
  std::pmr::monotonic_buffer_resource arena;
  api::SchemaDef out;
  s->toProto(&out, arena);
  const auto& author = *out.fields.at("author");
  EXPECT_EQ("_names", author.parent);
  EXPECT_FALSE(author.type);
  EXPECT_FALSE(author.variants);
  EXPECT_FALSE(author.defaults);
  const auto& clear = *out.fields.at("clear");
  ASSERT_TRUE(clear.variants);
  EXPECT_TRUE(clear.variants->entries.empty());
  ASSERT_TRUE(clear.defaults);
  EXPECT_FALSE(clear.defaults->search);
  EXPECT_FALSE(clear.defaults->value);
  const auto& labels = out.templates.at("_name")->variants->entries;
  ASSERT_EQ(2u, labels.size());
  EXPECT_EQ("s", labels[0].first);
  EXPECT_EQ("z", labels[1].first);
  ASSERT_TRUE(out.fields.at("raw")->normalizer);
  EXPECT_TRUE(out.fields.at("raw")->normalizer->filters.empty());
  ASSERT_TRUE(out.fields.at("raw")->stored_resource);
  EXPECT_TRUE(out.fields.at("raw")->stored_resource->empty());
  std::vector<std::byte> bytes;
  ASSERT_TRUE(api::encode(out, bytes));
  api::SchemaDef decoded;
  ASSERT_TRUE(api::decode(decoded, api::copyToPaddedInput(bytes, arena), arena));
  auto protoRound = Schema::fromProto(decoded);
  EXPECT_EQ(s->sourceDef_, protoRound->sourceDef_);
  std::string json = authoredJson(*s);
  EXPECT_NE(std::string::npos, json.find("\"variants\":{}"));
  EXPECT_NE(std::string::npos, json.find("\"defaults\":{}"));
  EXPECT_NE(std::string::npos, json.find("\"normalizer\":[]"));
  auto jsonRound = schemaJson(json);
  EXPECT_EQ(s->sourceDef_, jsonRound->sourceDef_);
  auto postedGet = schemaJson(json, s.get());
  EXPECT_EQ(s->sourceDef_, postedGet->sourceDef_);
  EXPECT_EQ("author__s", postedGet->resolveFor("author", OpClass::VALUE).physicalName);
  EXPECT_TRUE(postedGet->resolveInput("clear").owner->variants.empty());
  EXPECT_EQ("book_name__s", postedGet->resolveFor("book_name", OpClass::VALUE).physicalName);
}

TEST_F(SchemaTest, variantSetRecompilesParentsAndReplacesWholeDefinitions) {
  auto s = schemaJson(R"({"templates":{
    "_s":{"type":"string","normalizer":["lowercase"]},
    "_t":{"type":"text","variants":{"s":{"parent":"_s"}},"defaults":{"value":"s"}}
  },"fields":{"author":{"parent":"_t"}}})");
  auto changed = schemaJson(R"({"templates":{"_s":{"type":"int"}}})", s.get());
  EXPECT_EQ(FieldType::INT, changed->physical("author__s")->type());
  EXPECT_EQ(FieldType::INT, changed->physical("book_t__s")->type());
  auto cleared = schemaJson(R"({"templates":{"_t":{"type":"text"}}})", changed.get());
  EXPECT_TRUE(cleared->resolveInput("author").owner->variants.empty());
  EXPECT_EQ("author", cleared->resolveFor("author", OpClass::VALUE).physicalName);
  auto invalidBase = schemaJson(R"({"templates":{"_t":{"type":"text","variants":{"s":"string"}}},
                                  "fields":{"author":{"parent":"_t","defaults":{"value":"s"}}}})");
  EXPECT_THROW(schemaJson(R"({"templates":{"_t":{"type":"text","variants":{}}}})", invalidBase.get()), SchemaError);
}

TEST_F(SchemaTest, stringNormalizerInheritanceWholeValueAndClearing) {
  auto s = schemaJson(R"({"templates":{"_s":{"type":"string","normalizer":["nfkc_cf","fold"]}},
    "fields":{"a":{"parent":"_s"},"b":{"parent":"_s"},"raw":{"parent":"_s","normalizer":[]},
              "t":{"parent":"_s","type":"text"},
              "owner":{"type":"text","variants":{"s":{"parent":"_s"}}}}})");
  auto* a = (StrFieldType*)s->physical("a");
  EXPECT_EQ(a->normalizer, ((StrFieldType*)s->physical("b"))->normalizer);
  EXPECT_EQ(a->normalizer, ((StrFieldType*)s->physical("owner__s"))->normalizer);
  EXPECT_EQ(a->normalizer, ((StrFieldType*)s->physical("dynamic_s"))->normalizer);
  EXPECT_EQ("keyword", a->normalizer->tokenizer->name);
  std::string value = "LE GUIN Caf\xc3\xa9";
  a->normalize(value);
  EXPECT_EQ("le guin cafe", value);
  value.clear();
  a->normalize(value);
  EXPECT_TRUE(value.empty());
  value = "KEEP Case";
  ((StrFieldType*)s->physical("raw"))->normalize(value);
  EXPECT_EQ("KEEP Case", value);
  StrFieldType identity("identity");
  identity.normalize(value);
  EXPECT_EQ("KEEP Case", value);
}

TEST_F(SchemaTest, stringNormalizerSchemaValidation) {
  for (auto field : {
      R"({"type":"text","normalizer":[]})", R"({"type":"int","normalizer":["lowercase"]})",
      R"({"type":"string","normalizer":["whitespace"]})", R"({"type":"string","normalizer":["keyword"]})",
      R"({"type":"string","normalizer":["unicode_word"]})", R"({"type":"string","normalizer":["unknown"]})",
      R"({"type":"string","normalizer":[{}]})",
      R"({"type":"string","normalizer":["kstem"]})",
      R"({"type":"string","normalizer":["english_possessive"]})",
      R"({"type":"string","normalizer":[{"name":"kstem","params":{"possessive":true}}]})",
      R"({"type":"string","normalizer":[{"name":"fold","params":{"invalid":true}}]})",
      R"({"type":"string","analyzer":{"tokenizer":"keyword"}})",
      R"({"type":"text","variants":{"s":{"type":"string","normalizer":["keyword"]}}})"}) {
    SCOPED_TRACE(field);
    EXPECT_THROW(schemaJson(std::string("{\"fields\":{\"a\":") + field + "}}"), SchemaError);
  }
  std::pmr::monotonic_buffer_resource arena;
  api::SchemaDef def;
  EXPECT_FALSE(api::read_json(def, R"({"fields":{"a":{"type":"string","normalizer":{"tokenizer":"keyword"}}}})", arena));
}

TEST_F(SchemaTest, storedResourceEmptyStillInheritsOnPrimary) {
  auto s = schemaJson(R"({"templates":{"_t":{"type":"text","stored_resource":"cold"}},
                         "fields":{"a":{"parent":"_t","stored_resource":""}}})");
  EXPECT_EQ("cold", s->physical("a")->storedResource_);
  EXPECT_EQ("cold", schemaJson(authoredJson(*s))->physical("a")->storedResource_);
}

TEST_F(SchemaTest, resolvedHandlesUseHandBuiltFieldTypesWithoutOwners) {
  Schema s;
  auto concrete = std::make_shared<TextFieldType>("different_type_name");
  auto prototype = std::make_shared<StrFieldType>("_s", FieldType::INDEX_DOCS | FieldType::ABSTRACT);
  s.fieldTypeMap["plain"] = concrete;
  s.fieldTypeMap["_s"] = prototype;
  for (auto name : {"plain", "dynamic_s"}) {
    SCOPED_TRACE(name);
    auto input = s.resolveInput(name);
    EXPECT_EQ(nullptr, input.owner);
    EXPECT_EQ(name, input.logicalName);
    EXPECT_EQ(name, input.physicalName);
    EXPECT_EQ(FieldRole::PRIMARY, input.role);
    EXPECT_EQ(s.getFieldTypePtr(name), input.fieldType);
    for (auto op : {OpClass::SEARCH, OpClass::VALUE, OpClass::EXISTS, OpClass::RETRIEVE}) {
      auto bare = s.resolveFor(name, op);
      auto explicitSelf = s.resolveFor(std::string(name) + "__self", op);
      EXPECT_EQ(nullptr, bare.owner);
      EXPECT_EQ(nullptr, explicitSelf.owner);
      EXPECT_EQ(input.fieldType, bare.fieldType);
      EXPECT_EQ(input.fieldType, explicitSelf.fieldType);
      EXPECT_EQ(name, bare.physicalName);
      EXPECT_EQ(name, explicitSelf.physicalName);
      EXPECT_THROW(s.resolveFor(std::string(name) + "__x", op), RequestError);
    }
  }
  EXPECT_EQ(concrete, s.getFieldTypeEx("plain"));
  EXPECT_EQ(prototype, s.getFieldTypeOrNull("dynamic_s"));
  EXPECT_EQ(nullptr, s.getFieldTypePtr("dynamic_s__x"));
  EXPECT_EQ(nullptr, s.getFieldTypePtr("_s"));
  EXPECT_EQ(nullptr, s.getFieldTypeOrNull("missing"));
  EXPECT_THROW(s.getFieldTypeEx("missing"), RequestError);
}

TEST_F(SchemaTest, resolvedHandleNamesSurviveCopiesAndMoves) {
  auto s = schemaJson(R"({"templates":{"_t":{"type":"text","variants":{"s":"string"},
                                               "defaults":{"value":"s"}}}})");
  for (auto name : {"short_t", "longer_than_small_string_storage_t"}) {
    auto handle = [&] {
      std::string selector = std::string(name) + "__s";
      return s->resolveFor(selector, OpClass::VALUE);
    }();
    auto copy = handle;
    auto moved = std::move(handle);
    EXPECT_EQ(name, copy.logicalName);
    EXPECT_EQ(name, moved.logicalName);
    EXPECT_EQ(std::string(name) + "__s", copy.physicalName);
    EXPECT_EQ(copy.physicalName, moved.physicalName);
    EXPECT_EQ(copy.fieldType, moved.fieldType);
    EXPECT_EQ(copy.owner, moved.owner);
  }
}
