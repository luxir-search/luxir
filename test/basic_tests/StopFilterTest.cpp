// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0
#include "luxir/analysis/Analyzer.h"
#include "luxir/api/luxir_types.hpp"
#include "luxir/schema/Schema.h"
#include "test/LuxirTest.h"
#include "test/CollectionHelper.h"
#include "test/LocalReq.h"
#include "test/TestUtils.h"

using namespace luxir;
using namespace luxir::test;

namespace {
std::shared_ptr<const Analyzer> compile(std::string_view json) {
  std::pmr::monotonic_buffer_resource arena;
  api::AnalyzerDef def;
  std::string error;
  if (!api::read_json(def, json, arena, &error)) throw std::runtime_error(error);
  return Analyzer::compile(def);
}
struct Analysis {
  std::vector<std::string> terms;
  std::vector<int> positions;
  int finalPositionIncrement;
};
Analysis analyze(TokenChain& chain, std::string_view value) {
  chain.setValue(value);
  chain.reset();
  Analysis out;
  int position = -1;
  while (chain.tail->incrementToken()) {
    auto& token = chain.getToken();
    out.terms.emplace_back(token.text);
    position += token.positionIncrement;
    out.positions.push_back(position);
  }
  out.finalPositionIncrement = chain.end();
  return out;
}
std::shared_ptr<Schema> schema(std::string_view json) {
  std::pmr::monotonic_buffer_resource arena;
  api::SchemaDef def;
  if (!api::read_json(def, json, arena)) throw std::runtime_error("Invalid test schema");
  return Schema::fromProto(def);
}
} // namespace

class StopFilterTest : public LuxirTest {};

TEST_F(StopFilterTest, foldingPositionsFusionAndLifetime) {
  auto plan = compile(R"({"tokenizer":"unicode_word","filters":["nfkc_cf","fold",
    {"name":"stop","params":{"words":["THE","Cafe\u0301","cafe"]}}]})");
  EXPECT_TRUE(plan->fusedFold);
  auto chain = plan->createChain();
  auto out = analyze(*chain, "THE Cafe\xcc\x81 tail");
  EXPECT_EQ((std::vector<std::string>{"tail"}), out.terms);
  EXPECT_EQ((std::vector<int>{2}), out.positions);
  auto head = plan->tokenizer->create();
  auto& headRef = *head;
  std::unique_ptr<TokenStream> tail = std::move(head);
  for (const auto& factory : plan->filters) tail = factory->create(std::move(tail));
  TokenChain unfused(headRef, std::move(tail), true);
  auto reference = analyze(unfused, "THE Cafe\xcc\x81 tail");
  EXPECT_EQ(out.terms, reference.terms);
  EXPECT_EQ(out.positions, reference.positions);
  plan.reset();
  EXPECT_TRUE(analyze(*chain, "THE").terms.empty());
  EXPECT_EQ((std::vector<int>{0}), analyze(*chain, "tail").positions);
  std::string term = "THE";
  chain->normalizeTerm(term);
  EXPECT_EQ("the", term);
}

TEST_F(StopFilterTest, onlyPrecedingAnalysisAndMultipleStops) {
  auto early = compile(R"({"filters":[{"name":"stop","params":{"words":["THE"]}},"lowercase"]})")->createChain();
  EXPECT_EQ((std::vector<std::string>{"the"}), analyze(*early, "the").terms);
  EXPECT_TRUE(analyze(*early, "THE").terms.empty());
  auto chain = compile(R"({"filters":["lowercase",{"name":"stop","params":{"words":["THE"]}},
    "nfkc_cf","fold",{"name":"stop","params":{"words":["Cafe\u0301"]}}]})")->createChain();
  EXPECT_EQ((std::vector<int>{2}), analyze(*chain, "THE CAFE tail").positions);
  auto stemmed = compile(R"({"filters":["lowercase","kstem",{"name":"stop","params":{"words":["ponies"]}}]})")->createChain();
  EXPECT_TRUE(analyze(*stemmed, "PONIES").terms.empty());
  auto possessive = compile(R"({"filters":["lowercase","english_possessive",
    {"name":"stop","params":{"words":["Winter's"]}}]})")->createChain();
  EXPECT_TRUE(analyze(*possessive, "WINTER'S").terms.empty());
  auto segmented = compile(R"({"tokenizer":"unicode_word",
    "filters":[{"name":"stop","params":{"words":["word!"]}}]})")->createChain();
  EXPECT_TRUE(analyze(*segmented, "word").terms.empty());
}

TEST_F(StopFilterTest, charFiltersParticipate) {
  auto chain = compile(R"({"char_filters":["html_strip","nfkc_cf",
    {"name":"mapping","params":{"mappings":["- =>"]}}],
    "filters":["fold",{"name":"stop","params":{"words":["<b>CAF&Eacute;-BAR</b>","&amp;"]}}]})")->createChain();
  auto out = analyze(*chain, "<b>CAFE-BAR</b> next");
  EXPECT_EQ((std::vector<std::string>{"next"}), out.terms);
  EXPECT_EQ((std::vector<int>{1}), out.positions);
  EXPECT_TRUE(analyze(*chain, "&amp;").terms.empty());
}

TEST_F(StopFilterTest, invalidTokenCountsOnSetAndStoredLoad) {
  for (auto [word, count] : {std::pair{"two words", 2}, {"<b></b>", 0}, {"", 0}}) {
    SCOPED_TRACE(word);
    std::string json = std::string(R"({"fields":{"body":{"type":"text","analyzer":{
      "char_filters":["html_strip"],"tokenizer":"unicode_word",
      "filters":[{"name":"stop","params":{"words":["keep",")") + word + R"("]}}]}}}})";
    auto message = fmt::format("word '{}' produced {} tokens", word, count);
    try {
      schema(json);
      FAIL() << "Accepted invalid stop word";
    } catch (const SchemaError& e) {
      EXPECT_NE(std::string::npos, std::string(e.what()).find(message));
      auto info = classifyException(e, ErrorKind::INTERNAL);
      EXPECT_EQ(ErrorKind::INVALID_REQUEST, info.kind);
      EXPECT_EQ("invalid_schema", info.code);
    }
    // Encode the authored definition directly, as if saved by an older version.
    std::pmr::monotonic_buffer_resource arena;
    api::SchemaDef def;
    ASSERT_TRUE(api::read_json(def, json, arena));
    std::vector<std::byte> bytes;
    ASSERT_TRUE(api::encode(def, bytes));
    api::SchemaInfo info;
    info.source_def = bytes;
    ExpectLog warning("Dropping stored stop word: filter 'stop': " + message);
    auto loaded = Schema::fromStored(info, 7);
    EXPECT_EQ(1u, warning.suppressed());
    EXPECT_EQ(7u, loaded->gen_);
    auto plan = ((TextFieldType*) loaded->physical("body"))->analyzer_;
    EXPECT_EQ((std::vector<std::string>{"two", "words", "next"}),
        analyze(*plan->createChain(), "keep two words next").terms);
  }
  EXPECT_THROW(compile(R"({"filters":[{"name":"stop","params":{"words":["the"]}},
    {"name":"stop","params":{"words":["the"]}}]})"), std::invalid_argument);
}

TEST_F(StopFilterTest, invalidWordsAndStringNormalizers) {
  for (auto component : {R"({"name":"stop"})", R"({"name":"stop","params":{"words":[]}})",
      R"({"name":"stop","params":{"words":[""]}})", R"({"name":"stop","params":{"words":"the"}})",
      R"({"name":"stop","params":{"words":[42]}})",
      R"({"name":"stop","params":{"words":["the"],"ignore_case":true}})",
      R"({"name":"stop","params":{"words":["\u00ad"]}})"}) {
    SCOPED_TRACE(component);
    EXPECT_THROW(compile(std::string(R"({"filters":["nfkc_cf",)") + component + "]}"), std::invalid_argument);
  }
  EXPECT_THROW(schema(R"({"fields":{"tag":{"type":"string","normalizer":[
    {"name":"stop","params":{"words":["the"]}}]}}})"), SchemaError);
}

TEST_F(StopFilterTest, finalPositionsMultipleStopsAndReuse) {
  auto chain = compile(R"({"filters":[{"name":"stop","params":{"words":["a"]}},
    {"name":"stop","params":{"words":["b"]}}]})")->createChain();
  for (auto value : {"red a b a", "red b a b", "a b a"}) {
    EXPECT_EQ(3, analyze(*chain, value).finalPositionIncrement) << value;
    EXPECT_FALSE(chain->tail->incrementToken());
    EXPECT_EQ(3, chain->end()); // repeated exhaustion adds no duplicate holes
    EXPECT_EQ(0, analyze(*chain, "").finalPositionIncrement);
    EXPECT_EQ(0, analyze(*chain, "red").finalPositionIncrement);
  }
  auto out = analyze(*chain, "red a b blue a b");
  EXPECT_EQ((std::vector<int>{0, 3}), out.positions);
  EXPECT_EQ(2, out.finalPositionIncrement);
  EXPECT_EQ(0, analyze(*compile("{}")->createChain(), "red").finalPositionIncrement);
  auto folded = compile(R"({"filters":[{"name":"stop","params":{"words":["a"]}},"nfkc_cf"]})")->createChain();
  EXPECT_EQ(1, analyze(*folded, "red a \xc2\xad").finalPositionIncrement);
  EXPECT_EQ(2, analyze(*folded, "red a \xc2\xad a").finalPositionIncrement);
  EXPECT_EQ((std::vector<int>{0, 2}), analyze(*folded, "red a \xc2\xad blue").positions);
  EXPECT_EQ(0, analyze(*folded, "red \xc2\xad").finalPositionIncrement);
}

TEST_F(StopFilterTest, phrasesAndTrailingHolesBetweenValues) {
  CollectionHelper ch;
  ch.collection().setSchema(schema(R"({"fields":{"id":"id","body":{"type":"text","multi":true,
    "analyzer":{"filters":["lowercase",{"name":"stop","params":{"words":["the"]}}]}}}})"));
  ch.index(flatdoc("id", "gap", "body", "red the blue"));
  ch.index(flatdoc("id", "adjacent", "body", "red blue"));
  ch.index(flatdoc("id", "values", "body", std::vector<std::string>{"red the", "blue"}), UpdateMessage::COMMIT);
  for (int mode = 0; mode < 9; ++mode) {
    auto req = localReq(ch.getSearchEngine());
    auto& q = req->topDocs("q");
    if (mode == 0) q.phraseText("body", "RED THE BLUE");
    if (mode == 1) q.phraseQuery("body", {"red the", "blue"});
    if (mode == 2) q.phraseQuery("body", {"red", "the blue"});
    if (mode == 3) q.matchQuery("body", "THE");
    if (mode == 4) q.phraseQuery("body", {"red the", "blue"});
    if (mode == 5 || mode == 6) q.phraseTerms("body", {"red", "blue"});
    if (mode == 7) q.phraseQuery("body", {"red", "the", "blue"});
    if (mode == 8) q.phraseQuery("body", {"red", "the the", "blue"});
    int32_t positions[] = {0, mode == 5 ? 102 : mode == 6 ? 101 : 1, 2};
    if (mode == 4 || mode == 5 || mode == 6 || mode == 8)
      std::get<api::PhraseQuery>(q.rawQuery().kind).positions = std::span(positions, mode == 8 ? 3 : 2);
    q.fields({"id"});
    req->execute();
    ASSERT_OK(req);
    auto docs = req->getDocs();
    if (mode == 3 || mode == 6 || mode == 8) { EXPECT_TRUE(docs.empty()); continue; }
    ASSERT_EQ(1u, docs.size());
    EXPECT_EQ(mode == 5 ? "values" : "gap", std::get<std::string>(*find(docs[0], "id")));
  }
}
