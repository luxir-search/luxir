// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include "bench/luxir_bench.h"
#include "test/TestData.h"
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
                        std::vector<std::string> filters, std::optional<bool> possessive = std::nullopt) {
  Book& book = TestData::data->getBook();
  if (skipBenchIfDataMissing(state, !book.text().empty(), "book.txt")) return;
  std::string_view text = book.text();

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
    chain->head.setValue(text);
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
  chain->head.setValue(book.text());
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
