// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include <gtest/gtest.h>

#include <memory_resource>
#include <string>
#include <vector>

#include "luxir/analysis/Analyzer.h"
#include "luxir/plugin/Registries.h"
#include "test/LuxirTest.h"

using namespace luxir;

namespace {

// Terms of `text` through the analyzer `json`, as the example plugin's
// `describe` filter renders them: "text|wordClass|script|positionLength".
std::vector<std::string> describe(std::string_view json, std::string_view text) {
  std::pmr::monotonic_buffer_resource arena;
  api::AnalyzerDef def;
  std::string error;
  EXPECT_TRUE(api::read_json(def, json, arena, &error)) << error;
  auto chain = Analyzer::compile(def)->createChain();
  chain->setValue(text);
  chain->reset();
  std::vector<std::string> out;
  while (chain->tail->incrementToken()) out.emplace_back(chain->getToken().text);
  return out;
}

}  // namespace

class TokenAttributesExampleTest : public LuxirTest {};

// A plugin filter reads the class and script of each token's input word, on
// the fused unicode_word + nfkc_cf path (where the ASCII words arrive already
// classified) and on separate stages, where every word is classified on read.
TEST_F(TokenAttributesExampleTest, pluginFilterReadsWordClassAndScript) {
  const std::vector<std::string> expected{"hello|alpha|latin|1", "42|numeric|common|1", "привет|alpha|cyrillic|1",
                                          "中|alpha|han|1", "カタカナ|alpha|katakana|1", "abc1|alnum|latin|1"};
  std::string text = "Hello 42 Привет 中 カタカナ abc1";
  EXPECT_EQ(expected, describe(R"({"tokenizer":"unicode_word","filters":["nfkc_cf","describe"]})", text));
  EXPECT_EQ(expected, describe(R"({"tokenizer":"unicode_word","filters":["nfkc_cf","lowercase","describe"]})", text));
  EXPECT_EQ(expected, describe(R"({"tokenizer":"unicode_word","filters":["lowercase","describe"]})", text));
  EXPECT_EQ(expected, describe(R"({"tokenizer":"unicode_word","filters":["span2","nfkc_cf","describe"]})", text));
}

// Tokenizers that classify nothing still give a class on read: it comes from
// the input bytes and offsets, which index the text after char filters.
TEST_F(TokenAttributesExampleTest, anyTokenizerClassifiesOnRead) {
  EXPECT_EQ((std::vector<std::string>{"a1|alnum|latin|1", "中|alpha|han|1"}),
            describe(R"({"tokenizer":"whitespace","filters":["describe"]})", "a1 中"));
  EXPECT_EQ((std::vector<std::string>{"a 中|alpha|latin|1"}),
            describe(R"({"tokenizer":"keyword","filters":["describe"]})", "a 中"));
  EXPECT_EQ((std::vector<std::string>{"中|alpha|han|1", "9|numeric|common|1"}),
            describe(R"({"char_filters":["strip_x"],"tokenizer":"whitespace","filters":["describe"]})", "x中 9x"));
}

// positionLength set by a plugin filter reaches the end of the chain through
// the built-in filters, and the next token starts again at 1.
TEST_F(TokenAttributesExampleTest, positionLengthSurvivesBuiltinFilters) {
  const std::vector<std::string> expected{"xenon|alpha|latin|2", "apple|alpha|latin|1", "xylem|alpha|latin|2"};
  std::string text = "Xenon apple XYLEM";
  EXPECT_EQ(expected, describe(
      R"({"tokenizer":"unicode_word","filters":["nfkc_cf","span2","lowercase","english_possessive","kstem","fold","describe"]})",
      text));
  EXPECT_EQ(expected, describe(
      R"({"tokenizer":"unicode_word","filters":["lowercase","span2","nfkc_cf","fold","describe"]})", text));
  EXPECT_EQ((std::vector<std::string>{"xenon|alpha|latin|2", "apple|alpha|latin|1"}),
            describe(R"({"tokenizer":"whitespace","filters":["lowercase","span2","describe"]})", "Xenon apple"));
}

// A stop filter dropping a token between two others keeps the survivors' attributes.
TEST_F(TokenAttributesExampleTest, stopFilterKeepsAttributesOfSurvivors) {
  EXPECT_EQ((std::vector<std::string>{"xenon|alpha|latin|2", "42|numeric|common|1"}),
            describe(R"({"tokenizer":"unicode_word","filters":["nfkc_cf","span2",
              {"name":"stop","params":{"words":["the"]}},"describe"]})", "xenon the 42"));
}
