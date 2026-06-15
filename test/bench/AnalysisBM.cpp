#include "bench/solux_bench.h"
#include "test/TestData.h"
#include "solux/analysis/Analyzer.h"
#include "solux/schema/FieldType.h"

using namespace solux;

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
                        std::vector<std::string> filters) {
  Book& book = TestData::data->getBook();
  if (skipBenchIfDataMissing(state, !book.text().empty(), "book.txt")) return;
  std::string_view text = book.text();

  TextFieldType ft("body", FieldType::INDEX_DOCS_FREQS_POSITIONS, tokenizer, std::move(filters));
  auto chain = ft.createAnalyzer("body");
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
SOLUX_BENCHMARK_CAPTURE(BM_Tokenize<false>, whitespace_text, "whitespace", std::vector<std::string>{});
SOLUX_BENCHMARK_CAPTURE(BM_Tokenize<true>, whitespace_off, "whitespace", std::vector<std::string>{});
SOLUX_BENCHMARK_CAPTURE(BM_Tokenize<false>, standard_text, "unicode_word", std::vector<std::string>{"nfkc_cf"});
SOLUX_BENCHMARK_CAPTURE(BM_Tokenize<true>, standard_off, "unicode_word", std::vector<std::string>{"nfkc_cf"});
