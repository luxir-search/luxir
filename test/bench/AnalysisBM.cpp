// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include "bench/luxir_bench.h"
#include "test/TestData.h"
#include "bench/ScientificText.h"
#include "luxir/analysis/Analyzer.h"
#include "luxir/analysis/KStemmer.h"
#include "luxir/api/build.h"
#include "luxir/schema/FieldType.h"

#include <optional>

using namespace luxir;

// Tokenization-only throughput over the book corpus. This isolates the analyzer
// from inversion/postings (unlike IndexBookBM), so the per-token cost of offset
// tracking is measurable directly.
//
// Two consumer shapes, because they bound very different costs:
//   ConsumeOffset=false  - touches only tok.text, like the indexing path
//                          (FullTextHandler reads text + positionIncrement, not
//                          offsets). Inlined header tokenizers can DCE the unread
//                          offset store; the out-of-line StandardTokenizer cannot.
//   ConsumeOffset=true   - touches the whole Token, like a highlighter that reads
//                          every offset. Upper bound on the offset cost.
template <bool ConsumeOffset>
static void BM_Tokenize(benchmark::State& state, std::string tokenizer,
                        std::vector<std::string> filters, std::optional<bool> possessive = std::nullopt,
                        bool multilingual = false) {
  std::string_view text;
  std::string synthetic;
  if (multilingual) {
    for (int i = 0; i < 4096; ++i)
      synthetic += "CAF\u00c9 Stra\u00dfe \u03b1\u1f50\u03c4\u03cc\u03c2 \u03c4\u1fc6\u03c2 "
                   "\u0411\u041e\u041b\u042c\u0428\u041e\u0419 \u5168\u6587 \uff21\uff22\uff23 ";
    text = synthetic;
  } else {
    Book& book = TestData::data->getBook();
    if (skipBenchIfDataMissing(state, !book.text().empty(), "book.txt")) return;
    text = book.text();
  }

  std::pmr::monotonic_buffer_resource arena;
  api::AnalyzerDef def;
  def.tokenizer.emplace().name = tokenizer;
  auto* components = api::build::allocArray(def.filters, filters.size(), arena);
  for (size_t i = 0; i < filters.size(); ++i) components[i].name = filters[i];
  if (possessive.has_value()) {
    api::build::mapSlot(components[filters.size() - 1].params, 1, "possessive", arena)->kind = *possessive;
  }
  auto chain = Analyzer::compile(def)->createChain();
  Token& tok = chain->head.getToken();
  TokenStream& tail = *chain->tail;

  int64_t ntokens = 0;
  for (auto _ : state) {
    chain->setValue(text);
    chain->reset();
    while (tail.incrementToken()) {
      if constexpr (ConsumeOffset) {
        benchmark::DoNotOptimize(tok);       // force offsets to be materialized
      } else {
        benchmark::DoNotOptimize(tok.text);  // indexing path: text only
      }
      ntokens++;
    }
  }

  state.counters["byte_rate"] =
      benchmark::Counter(text.size() * state.iterations(), benchmark::Counter::kIsRate);
  state.counters["tok_rate"] =
      benchmark::Counter(ntokens, benchmark::Counter::kIsRate);
  state.counters["ntokens"] = (double) (ntokens / state.iterations());
}

// _text = indexing-representative (text only); _off = offset-consuming (highlighter).
TUNING_BENCHMARK_CAPTURE(BM_Tokenize<false>, whitespace_text, "whitespace", std::vector<std::string>{})->UseRealTime();
TUNING_BENCHMARK_CAPTURE(BM_Tokenize<true>, whitespace_off, "whitespace", std::vector<std::string>{})->UseRealTime();
TUNING_BENCHMARK_CAPTURE(BM_Tokenize<false>, standard_text, "unicode_word", std::vector<std::string>{"nfkc_cf"})->UseRealTime();
TUNING_BENCHMARK_CAPTURE(BM_Tokenize<true>, standard_off, "unicode_word", std::vector<std::string>{"nfkc_cf"})->UseRealTime();
TUNING_BENCHMARK_CAPTURE(BM_Tokenize<false>, multilingual, "unicode_word",
    std::vector<std::string>{"nfkc_cf"}, std::nullopt, true)->UseRealTime();
TUNING_BENCHMARK_CAPTURE(BM_Tokenize<false>, multilingual_fold_only, "whitespace",
    std::vector<std::string>{"nfkc_cf"}, std::nullopt, true)->UseRealTime();
TUNING_BENCHMARK_CAPTURE(BM_Tokenize<false>, folded_text, "unicode_word", std::vector<std::string>{"nfkc_cf", "fold"})->UseRealTime();
TUNING_BENCHMARK_CAPTURE(BM_Tokenize<false>, kstem_text, "unicode_word", std::vector<std::string>{"nfkc_cf", "fold", "kstem"}, false)->UseRealTime();
TUNING_BENCHMARK_CAPTURE(BM_Tokenize<false>, possessive_separate, "unicode_word",
    std::vector<std::string>{"nfkc_cf", "fold", "english_possessive", "kstem"}, false)->UseRealTime();
TUNING_BENCHMARK_CAPTURE(BM_Tokenize<false>, possessive_integrated, "unicode_word",
    std::vector<std::string>{"nfkc_cf", "fold", "kstem"})->UseRealTime();

// Isolate stemming from segmentation/folding, preserving the book's token
// distribution. Tokens own their bytes because the analysis chain is transient.
enum class PossessiveMode { NONE, SEPARATE, INTEGRATED };

template <PossessiveMode Mode>
static void BM_KStem(benchmark::State& state) {
  Book& book = TestData::data->getBook();
  if (skipBenchIfDataMissing(state, !book.text().empty(), "book.txt")) return;
  TextFieldType ft("body", FieldType::INDEX_DOCS_FREQS_POSITIONS,
                   "unicode_word", {"nfkc_cf", "fold"});
  auto chain = ft.createAnalyzer("body");
  chain->setValue(book.text());
  chain->reset();
  std::vector<std::string> terms;
  while (chain->tail->incrementToken()) terms.emplace_back(chain->head.getToken().text);
  KStemmer stemmer;
  for (auto _ : state) {
    for (const auto& term : terms) {
      if constexpr (Mode == PossessiveMode::INTEGRATED) benchmark::DoNotOptimize(stemmer.stem<true>(term));
      else if constexpr (Mode == PossessiveMode::SEPARATE) {
        benchmark::DoNotOptimize(stemmer.stem<false>(removeEnglishPossessive(term)));
      } else benchmark::DoNotOptimize(stemmer.stem<false>(term));
    }
  }
  state.counters["tok_rate"] =
      benchmark::Counter(terms.size() * state.iterations(), benchmark::Counter::kIsRate);
}
TUNING_BENCHMARK(BM_KStem<PossessiveMode::NONE>)->UseRealTime();
TUNING_BENCHMARK(BM_KStem<PossessiveMode::SEPARATE>)->UseRealTime();
TUNING_BENCHMARK(BM_KStem<PossessiveMode::INTEGRATED>)->UseRealTime();

// Per-stage cost over ~40KB of synthetic scientific English (the PMC-like
// shape of production text fields): mostly ASCII with some Unicode. Stages
// are attributed by difference between chains that add one stage at a time:
//   whitespace       floor: a byte scan that emits views
//   unicode_word     uni-algo UAX#29 alone (no fused fast path)
//   standard         unicode_word + nfkc_cf, fused (the _un chain)
//   standard_fold    + fold
//   t_chain          + kstem (possessive on): the _t chain
// The *_ascii variants drop every non-ASCII byte first, isolating the ASCII
// fast paths from the uni-algo fallback.
static void BM_SciAnalyze(benchmark::State& state, std::string tokenizer, std::vector<std::string> filters,
                          bool asciiOnly = false) {
  std::string text = scientificText(40 * 1024, 42);
  if (asciiOnly) std::erase_if(text, [](char c) { return (unsigned char) c >= 0x80; });
  std::pmr::monotonic_buffer_resource arena;
  api::AnalyzerDef def;
  def.tokenizer.emplace().name = tokenizer;
  auto* components = api::build::allocArray(def.filters, filters.size(), arena);
  for (size_t i = 0; i < filters.size(); ++i) components[i].name = filters[i];
  auto chain = Analyzer::compile(def)->createChain();
  Token& tok = chain->head.getToken();
  TokenStream& tail = *chain->tail;
  int64_t ntokens = 0;
  for (auto _ : state) {
    chain->head.setValue(text);
    chain->reset();
    while (tail.incrementToken()) {
      benchmark::DoNotOptimize(tok.text);
      ntokens++;
    }
  }
  state.counters["byte_rate"] =
      benchmark::Counter((double) text.size() * (double) state.iterations(), benchmark::Counter::kIsRate);
  state.counters["ns_per_byte"] = benchmark::Counter((double) text.size() * (double) state.iterations(),
      benchmark::Counter::kIsRate | benchmark::Counter::kInvert);
  state.counters["ntokens"] = (double) (ntokens / state.iterations());
}
TUNING_BENCHMARK_CAPTURE(BM_SciAnalyze, whitespace, "whitespace", std::vector<std::string>{})->UseRealTime();
TUNING_BENCHMARK_CAPTURE(BM_SciAnalyze, unicode_word, "unicode_word", std::vector<std::string>{})->UseRealTime();
TUNING_BENCHMARK_CAPTURE(BM_SciAnalyze, standard, "unicode_word", std::vector<std::string>{"nfkc_cf"})->UseRealTime();
TUNING_BENCHMARK_CAPTURE(BM_SciAnalyze, standard_fold, "unicode_word",
    std::vector<std::string>{"nfkc_cf", "fold"})->UseRealTime();
TUNING_BENCHMARK_CAPTURE(BM_SciAnalyze, t_chain, "unicode_word",
    std::vector<std::string>{"nfkc_cf", "fold", "kstem"})->UseRealTime();
TUNING_BENCHMARK_CAPTURE(BM_SciAnalyze, standard_ascii, "unicode_word", std::vector<std::string>{"nfkc_cf"},
    true)->UseRealTime();
TUNING_BENCHMARK_CAPTURE(BM_SciAnalyze, t_chain_ascii, "unicode_word",
    std::vector<std::string>{"nfkc_cf", "fold", "kstem"}, true)->UseRealTime();
