// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>

#include "luxir/analysis/KStemmer.h"
#include "test/LuxirTest.h"

using namespace luxir;

class KStemTest : public LuxirTest {};

TEST_F(KStemTest, luceneVocabulary) {
  // Lucene's TestKStemmer fixture, generated with the original Java stemmer.
  std::ifstream in(std::filesystem::path(__FILE__).parent_path() / "data/kstem_examples.txt");
  ASSERT_TRUE(in.is_open());
  KStemmer stemmer;
  std::string word, expected;
  int count = 0;
  while (in >> word >> expected) {
    EXPECT_EQ(expected, stemmer.stem<false>(word)) << word;
    EXPECT_EQ(expected, stemmer.stem(word)) << word;
    for (auto suffix : {"'s", "'S", "\xe2\x80\x99s", "\xef\xbc\x87S"}) {
      EXPECT_EQ(expected, stemmer.stem(word + suffix)) << word << suffix;
    }
    ++count;
  }
  EXPECT_TRUE(in.eof());
  EXPECT_EQ(12130, count);
}

TEST_F(KStemTest, possessiveShorteningAndPassThrough) {
  KStemmer stemmer, reference;
  for (std::string word : {"", "a", "as", "'s", "american", "greek", "Ponies", "PONIES",
                           "caf\xc3\xa9", "don't", "james", "winter", "ponies1", "pony'S"}) {
    for (auto suffix : {"", "'s", "'S", "\xe2\x80\x99s", "\xef\xbc\x87S", "s'", "\xe2\x80\x98s"}) {
      std::string input = word + suffix;
      std::string original = input;
      EXPECT_EQ(reference.stem<false>(removeEnglishPossessive(input)), stemmer.stem(input)) << input;
      EXPECT_EQ(original, input);
    }
  }
  for (int len : {0, 1, 2, 3, 47, 48, 49, 50, 51, 10000}) {
    std::string word(len, 'b');
    if (len >= 4) word.replace(len - 4, 4, "ness");
    for (auto suffix : {"'s", "\xe2\x80\x99s", "\xef\xbc\x87S"}) {
      EXPECT_EQ(reference.stem<false>(word), stemmer.stem(word + suffix)) << len;
    }
  }
  std::string embeddedNull("a\0b's", 5);
  EXPECT_EQ(std::string_view(embeddedNull.data(), 3), stemmer.stem<true>(embeddedNull));
  std::string unchanged = "caf\xc3\xa9's";
  auto result = stemmer.stem<true>(unchanged);
  EXPECT_EQ(unchanged.data(), result.data()); // removal itself still borrows source
  EXPECT_EQ("caf\xc3\xa9", result);
}

TEST_F(KStemTest, rulesAndDictionaryMappings) {
  KStemmer stemmer;
  for (auto [word, expected] : {
         std::pair{"aides", "aide"}, {"aided", "aid"}, {"crosses", "cross"},
         {"calories", "calorie"}, {"ponies", "pony"}, {"aging", "age"},
         {"italians", "italy"}, {"political", "politics"}, {"canonic", "canonical"},
         {"fingerspelling", "fingerspell"}, {"microcoding", "microcode"},
         {"happiness", "happiness"}, {"running", "running"},
         {"netherlands", "netherlands"},
         // Direct mappings still apply when no suffix rule could change the key.
         {"greek", "greece"}, {"iraqi", "iraq"}, {"thai", "thailand"},
         {"irish", "ireland"}, {"died", "die"}, {"fled", "flee"},
         {"aide", "aide"}, {"bly", "b"}}) {
    EXPECT_EQ(expected, stemmer.stem(word)) << word;
  }
}

TEST_F(KStemTest, passThroughAndLengthLimits) {
  KStemmer stemmer;
  for (std::string_view word : {"", "a", "as", "Ponies", "PONIES", "ponies1",
                               "pony's", "caf\xc3\xa9s", "dog", "the", "with"}) {
    auto result = stemmer.stem<false>(word);
    EXPECT_EQ(word, result);
    EXPECT_EQ(word.data(), result.data());
  }
  std::string_view embeddedNull("ponies\0", 7);
  EXPECT_EQ(embeddedNull, stemmer.stem(embeddedNull));
  for (std::string_view word : {std::string_view("ha\0", 3), std::string_view("go\0\0", 4)}) {
    EXPECT_EQ(word, stemmer.stem(word));
  }

  std::string longest = std::string(45, 'b') + "ness";
  EXPECT_EQ(longest.substr(0, 45), stemmer.stem(longest)); // 49 bytes: stemmed
  for (int len : {50, 51, 10000}) {
    std::string word = std::string(len - 4, 'b') + "ness";
    EXPECT_EQ(word, stemmer.stem(word));
  }
  EXPECT_EQ("pony", stemmer.stem("ponies")); // reuse after every pass-through path
}

TEST_F(KStemTest, sharedDictionaryAndNoTokenAllocations) {
  if (!memtrack::counting_enabled) GTEST_SKIP() << "allocation counter disabled under ASan";
  KStemmer warm;
  warm.stem("ponies");
  size_t bytes = 0;
  memtrack::AllocScope scope;
  KStemmer stemmer;
  for (int i = 0; i < 10; ++i) {
    for (auto word : {"dog", "ponies", "canonic", "italians", "aided", "microcoding"}) {
      bytes += stemmer.stem<false>(word).size();
      bytes += stemmer.stem(word).size();
    }
    bytes += stemmer.stem<true>("american's").size();
    bytes += stemmer.stem<true>("ponies\xe2\x80\x99s").size();
  }
  long allocs = scope.count();
  EXPECT_EQ(0, allocs);
  EXPECT_GT(bytes, 0u);
}
