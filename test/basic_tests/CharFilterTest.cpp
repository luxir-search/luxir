// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0
#include <gtest/gtest.h>
#include <random>
#include <uni_algo/conv.h>
#include "luxir/analysis/Analyzer.h"
#include "luxir/api/luxir_types.hpp"
#include "luxir/schema/Schema.h"
#include "test/LuxirTest.h"
#include "test/CollectionHelper.h"
#include "test/LocalReq.h"
#include "test/TestUtils.h"
#include "test/QueryBuild.h"

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
struct Tokens {
  std::vector<std::string> terms;
  std::vector<int> starts, ends;
};
Tokens analyze(TokenChain& chain, std::string_view input) {
  chain.setValue(input);
  chain.reset();
  Tokens result;
  while (chain.tail->incrementToken()) {
    const auto& token = chain.head.getToken();
    result.terms.emplace_back(token.text);
    result.starts.push_back(token.startOffset);
    result.ends.push_back(token.endOffset);
  }
  return result;
}
std::shared_ptr<Schema> schema(std::string_view json) {
  std::pmr::monotonic_buffer_resource arena;
  api::SchemaDef def;
  if (!api::read_json(def, json, arena)) throw std::runtime_error("Invalid test schema");
  return Schema::fromProto(def);
}
} // namespace

class CharFilterTest : public LuxirTest {};

TEST_F(CharFilterTest, removeUnicodeCharactersBeforeSegmentation) {
  auto plan = compile(R"({"tokenizer":"unicode_word","filters":["nfkc_cf"],
    "char_filters":[{"name":"remove_chars","params":{"chars":["-","\u2014","\ud83d\ude00","-"]}}]})");
  auto chain = plan->createChain();
  std::string source = "-FOO-bar\xe2\x80\x94\xf0\x9f\x98\x80 baz-";
  auto result = analyze(*chain, source);
  EXPECT_EQ((std::vector<std::string>{"foobar", "baz"}), result.terms);
  EXPECT_EQ((std::vector<int>{1, 16}), result.starts);
  EXPECT_EQ((std::vector<int>{8, 19}), result.ends);
  EXPECT_EQ("-FOO-bar\xe2\x80\x94\xf0\x9f\x98\x80 baz-", source);
  EXPECT_TRUE(analyze(*chain, "---").terms.empty());
  result = analyze(*chain, "fresh");
  EXPECT_EQ((std::vector<int>{0}), result.starts);
  EXPECT_EQ((std::vector<int>{5}), result.ends);
  // Chains own their buffers and configuration independently of the plan.
  auto other = plan->createChain();
  plan.reset();
  EXPECT_EQ((std::vector<std::string>{"ab"}), analyze(*other, "a-b").terms);
  std::string prefix = "FOO-B";
  chain->normalizeTerm(prefix);
  EXPECT_EQ("foob", prefix);
}

TEST_F(CharFilterTest, orderAndCanonicalIdentity) {
  auto first = compile(R"({"char_filters":["lowercase",{"name":"remove_chars","params":{"chars":["a"]}}]})");
  auto second = compile(R"({"char_filters":[{"name":"remove_chars","params":{"chars":["a"]}},"lowercase"]})");
  EXPECT_NE(first->canonical, second->canonical);
  EXPECT_EQ((std::vector<std::string>{"b"}), analyze(*first->createChain(), "AB").terms);
  EXPECT_EQ((std::vector<std::string>{"ab"}), analyze(*second->createChain(), "AB").terms);
  EXPECT_EQ(compile("{}")->canonical, compile(R"({"char_filters":[]})")->canonical);
  auto empty = compile(R"({"char_filters":[{"name":"remove_chars","params":{"chars":[]}}]})")->createChain();
  std::string source = "unchanged";
  analyze(*empty, source);
  EXPECT_EQ(source.data(), empty->head.getToken().text.data());
}

TEST_F(CharFilterTest, htmlMarkupEntitiesAndOffsets) {
  auto chain = compile(R"({"tokenizer":"unicode_word","char_filters":["html_strip","nfkc_cf","fold"]})")->createChain();
  auto result = analyze(*chain, "<p>Caf&eacute;</p><b>ONE</b>&#x20;two");
  EXPECT_EQ((std::vector<std::string>{"cafe", "one", "two"}), result.terms);
  EXPECT_EQ((std::vector<int>{3, 21, 34}), result.starts);
  EXPECT_EQ((std::vector<int>{14, 24, 37}), result.ends);
  for (auto input : {
      "<p a='>' b=\"<fake>\">ok</p>",
      "<!-- hidden --><STYLE>secret &amp; text</sTyLe>ok<script>bad < x</SCRIPT>",
      "<?processing?><!DOCTYPE html SYSTEM 'https://invalid.example/'>ok",
      "<!-- --!>ok", "ok<!-- unclosed", "ok<script>unclosed", "ok<unfinished x='>'"}) {
    SCOPED_TRACE(input);
    EXPECT_EQ((std::vector<std::string>{"ok"}), analyze(*chain, input).terms);
  }
  EXPECT_EQ((std::vector<std::string>{"one", "two"}), analyze(*chain, "one<b></b>two").terms);
  EXPECT_EQ((std::vector<std::string>{"a", "b"}), analyze(*chain, "a&#32;b").terms);
  EXPECT_TRUE(analyze(*chain, "<script>secret</script><style>hidden</style><!--comment-->").terms.empty());
}

TEST_F(CharFilterTest, htmlReferencesAreDecodedOnce) {
  auto chain = compile(R"({"tokenizer":"keyword","char_filters":["html_strip"]})")->createChain();
  for (auto [input, expected] : {
      std::pair{"&lt;b&gt;&amp;lt;", "<b>&lt;"},
      {"&eacute;&NotEqualTilde;", "\xc3\xa9\xe2\x89\x82\xcc\xb8"},
      {"&#0;&#xD800;&#1114112;&#999999999999999999999999;", "\xef\xbf\xbd\xef\xbf\xbd\xef\xbf\xbd\xef\xbf\xbd"},
      {"&#x80;", "\xe2\x82\xac"}, {"&amp &amp;", "& &"},
      {"&#65 &#x41;", "A A"}, {"1 < 3 &unknown; &#x; &", "1 < 3 &unknown; &#x; &"}}) {
    SCOPED_TRACE(input);
    EXPECT_EQ((std::vector<std::string>{expected}), analyze(*chain, input).terms);
  }
}

TEST_F(CharFilterTest, htmlAndRemovalComposeOffsets) {
  auto chain = compile(R"({"tokenizer":"unicode_word","char_filters":["html_strip",
    {"name":"remove_chars","params":{"chars":["-"]}},"nfkc_cf"]})")->createChain();
  auto result = analyze(*chain, "<b>FOO&#45;BAR</b> baz");
  EXPECT_EQ((std::vector<std::string>{"foobar", "baz"}), result.terms);
  EXPECT_EQ((std::vector<int>{3, 19}), result.starts);
  EXPECT_EQ((std::vector<int>{14, 22}), result.ends);
  EXPECT_EQ((std::vector<std::string>{"next"}), analyze(*chain, "next").terms);
}

TEST_F(CharFilterTest, unicodeCleanupMatchesWholeValueNormalization) {
  std::vector<std::string> inputs = {
    "", "ASCII a-B", "\xce\x9f\xce\xa3 \xce\x9f\xce\xa3\xce\x91", // contextual sigma
    "\xc4\xb0\xc3\x9f\xef\xac\x83", // expanding mappings
    "A\xcc\x8a\xcc\x81\xc2\xad", "a\xcc\x95\xcc\x80", // composition/reordering
    "\xe1\x84\x80\xe1\x85\xa1\xe1\x86\xa8", // Hangul Jamo
    "\xe3\x84\xb1\xe3\x85\x8f", // compatibility Jamo
    std::string("a\0B", 3), "\xff\xc0\x80"};
  std::string marks = "a";
  for (int i = 0; i < 100; ++i) marks += "\xcc\x81";
  inputs.push_back(marks);
  std::mt19937 random(7182);
  constexpr char32_t alphabet[] = {0x41, 0x20, 0xAD, 0x301, 0x315, 0x345, 0x3A3, 0x130, 0x1100,
    0x1161, 0x11A8, 0x3131, 0x314F, 0x1F88, 0x212B, 0xFB03, 0x034F, 0xFE0F, 0xFF9E};
  for (int i = 0; i < 500; ++i) {
    std::u32string scalars;
    for (int j = 0; j < 30; ++j) scalars += alphabet[random() % std::size(alphabet)];
    inputs.push_back(una::utf32to8(scalars));
  }
  for (auto name : {"lowercase", "nfkc_cf", "fold"}) {
    auto cleaned = compile(std::string(R"({"tokenizer":"keyword","char_filters":[")") + name + R"("]})")->createChain();
    auto reference = compile(std::string(R"({"tokenizer":"keyword","filters":[")") + name + R"("]})")->createChain();
    for (const auto& input : inputs) {
      SCOPED_TRACE(name);
      std::string expected = input;
      reference->normalizeTerm(expected);
      auto out = analyze(*cleaned, input);
      EXPECT_EQ(expected.empty() ? std::vector<std::string>{} : std::vector<std::string>{expected}, out.terms);
      for (size_t i = 0; i < out.starts.size(); ++i) {
        EXPECT_GE(out.starts[i], 0);
        EXPECT_LE(out.starts[i], out.ends[i]);
        EXPECT_LE(out.ends[i], (int) input.size());
      }
    }
  }
}

TEST_F(CharFilterTest, rejectsInvalidComponentsAndParameters) {
  for (auto component : {R"("unknown")", R"({})", R"("kstem")", R"("english_possessive")",
      R"({"name":"html_strip","params":{"x":1}})", R"({"name":"remove_chars"})",
      R"({"name":"remove_chars","params":{"chars":"-"}})",
      R"({"name":"remove_chars","params":{"chars":["ab"]}})",
      R"({"name":"remove_chars","params":{"chars":[""]}})",
      R"({"name":"remove_chars","params":{"chars":[42]}})",
      R"({"name":"remove_chars","params":{"chars":["-"],"unknown":true}})"}) {
    SCOPED_TRACE(component);
    EXPECT_THROW(compile(std::string("{\"char_filters\":[") + component + "]}"), std::invalid_argument);
  }
}

TEST_F(CharFilterTest, adversarialHtmlAndReuse) {
  auto chain = compile(R"({"tokenizer":"unicode_word","char_filters":["html_strip"]})")->createChain();
  std::string comments;
  for (int i = 0; i < 20000; ++i) comments += "<!--x-->";
  EXPECT_EQ((std::vector<std::string>{"ok"}), analyze(*chain, comments + "ok").terms);
  EXPECT_TRUE(analyze(*chain, "<script>" + std::string(100000, '<')).terms.empty());
  EXPECT_TRUE(analyze(*chain, "<b x='" + std::string(100000, '>')).terms.empty());
  std::mt19937 random(57);
  constexpr std::string_view alphabet = "<>&;!/'\"-scriptSTYLE012 x\n";
  for (int i = 0; i < 1000; ++i) {
    std::string input;
    for (int j = 0; j < 128; ++j) input += alphabet[random() % alphabet.size()];
    auto out = analyze(*chain, input);
    for (size_t j = 0; j < out.starts.size(); ++j) {
      EXPECT_GE(out.starts[j], 0);
      EXPECT_LE(out.starts[j], out.ends[j]);
      EXPECT_LE(out.ends[j], (int) input.size());
    }
  }
  EXPECT_EQ((std::vector<std::string>{"fresh"}), analyze(*chain, "fresh").terms);
}

TEST_F(CharFilterTest, schemaInheritanceAndIndexQueryParity) {
  auto s = schema(R"({"templates":{"_base":{"type":"text","analyzer":{"tokenizer":"keyword"}}},
    "fields":{"id":"id","body":{"parent":"_base","stored":true,"multi":true,
      "analyzer":{"tokenizer":"unicode_word","char_filters":["html_strip","nfkc_cf","fold",
        {"name":"remove_chars","params":{"chars":["-"]}}]}},
      "only":{"parent":"_base","analyzer":{"char_filters":["html_strip"]}}}})");
  auto* only = (TextFieldType*) s->getFieldTypePtr("only");
  EXPECT_EQ("whitespace", only->analyzer_->tokenizer->name);
  ASSERT_EQ(1u, only->analyzer_->charFilters.size());
  CollectionHelper ch;
  ch.collection().setSchema(s);
  ch.index(flatdoc("id", "one", "body", std::vector<std::string>{"<b>CAF&Eacute;-BAR</b>", "<p>two words</p>"}), UpdateMessage::COMMIT);
  for (auto mode : {0, 1, 2, 3, 4, 5, 6, 7}) {
    auto req = localReq(ch.getSearchEngine());
    auto& q = req->topDocs("q");
    if (mode == 0) q.matchQuery("body", "<i>CAF&Eacute;-BAR</i>");
    if (mode == 1) q.prefixQuery("body", "CAFE-B");
    if (mode == 2) q.phraseText("body", "<p>two</p> words");
    if (mode == 3) q.phraseQuery("body", {"two", "<b>words</b>"});
    if (mode == 4) q.fuzzyQuery("body", "CAFE-BXR", 1);
    if (mode == 5) q.rawQuery() = qb::wildcard(q.mr(), "body", "CAFE-B*");
    if (mode == 6) q.rawQuery() = qb::regex(q.mr(), "body", "CAFE-B.*");
    if (mode == 7) q.exprQuery("body:=\"<b>CAFE-BAR</b>\"");
    q.fields({"id", "body"});
    req->execute();
    ASSERT_TRUE(req->ok()) << req->errorMsg();
    auto docs = req->getDocs();
    ASSERT_EQ(1u, docs.size()) << mode;
    auto* stored = find(docs[0], "body");
    ASSERT_NE(nullptr, stored);
    EXPECT_EQ((std::vector<std::string>{"<b>CAF&Eacute;-BAR</b>", "<p>two words</p>"}), std::get<std::vector<std::string>>(*stored));
  }
  auto req = localReq(ch.getSearchEngine());
  req->topDocs("q").matchQuery("body", "b").fields({"id"});
  req->execute();
  EXPECT_TRUE(req->getDocs().empty());
}

TEST_F(CharFilterTest, normalizationExpansionOffsets) {
  auto chain = compile(R"({"tokenizer":"unicode_word","char_filters":["nfkc_cf","fold"]})")->createChain();
  auto out = analyze(*chain, "\xef\xac\x83 A\xcc\x8a \xe2\x84\x80 end");
  EXPECT_EQ((std::vector<std::string>{"ffi", "a", "a", "c", "end"}), out.terms);
  EXPECT_EQ((std::vector<int>{0, 4, 8, 8, 12}), out.starts);
  EXPECT_EQ((std::vector<int>{3, 7, 11, 11, 15}), out.ends);
}

TEST_F(CharFilterTest, schemaRoundTripInheritanceAndValidation) {
  auto original = schema(R"({"templates":{"_clean":{"type":"text","analyzer":{
    "char_filters":["html_strip",{"name":"remove_chars","params":{"chars":["-"]}}]}}},
    "fields":{"body":{"parent":"_clean"},"title":{"type":"string","variants":{"text":{"parent":"_clean"}}}}})");
  std::pmr::monotonic_buffer_resource arena;
  api::SchemaDef def;
  original->toProto(&def, arena);
  auto restored = Schema::fromProto(def);
  for (auto field : {"body", "dynamic_clean", "title__text"}) {
    auto* before = (TextFieldType*) original->physical(field);
    auto* after = (TextFieldType*) restored->physical(field);
    ASSERT_NE(nullptr, after);
    EXPECT_EQ(before->analyzer_->canonical, after->analyzer_->canonical);
    ASSERT_EQ(2u, after->analyzer_->charFilters.size());
    EXPECT_EQ((std::vector<std::string>{"ab"}), analyze(*after->analyzer_->createChain(), "<b>a-b</b>").terms);
  }
  for (auto field : {R"({"type":"string","analyzer":{"char_filters":["html_strip"]}})",
      R"({"type":"int","analyzer":{"char_filters":["html_strip"]}})",
      R"({"type":"text","analyzer":{"char_filters":["no_such_filter"]}})"}) {
    EXPECT_THROW(schema(std::string("{\"fields\":{\"body\":") + field + "}}"), SchemaError);
  }
}
