// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include <benchmark/benchmark.h>

#include <string>
#include <vector>

#include "bench/luxir_bench.h"
#include "luxir/reader/BruteDfaTermsEnum.h"
#include "luxir/reader/DfaIntersectEnum.h"
#include "luxir/util/automaton/RegExpParser.h"
#include "luxir/util/automaton/WildcardCompiler.h"
#include "test/TestIndex.h"

using namespace luxir;
using namespace luxir::automaton;
using namespace luxir::test;

namespace {

class AutomatonBenchIndex {
  TestIndex index;
  TestField field{index, "value_s"};

public:
  AutomatonBenchIndex() {
    field.startIndexing();
    int32_t count = unit_tests ? 2'000 : 200'000;
    for (int32_t i = 0; i < count; i++) {
      std::string term = i % 19 == 0 ? "match_" : "noise_";
      term += (char)('a' + i % 26);
      term += std::to_string(i);
      term += i % 7 == 0 ? "_end" : "_tail";
      field.add(i, term);
    }
    index.flush();
    field.startReading();
  }

  TestIndex& getIndex() { return index; }
  TestField& getField() { return field; }
};

AutomatonBenchIndex& benchIndex() {
  static AutomatonBenchIndex fixture;
  return fixture;
}

template <typename Enum>
int32_t count(Enum& terms) {
  int32_t result = 0;
  while (terms.next()) result++;
  return result;
}

void BM_AutomatonEnum(benchmark::State& state, std::string_view pattern, bool regex, bool brute) {
  Budget budget(10'000'000);
  ByteDfa dfa = regex ? compileRegex(pattern, budget) : compileWildcard(pattern, budget);
  auto& fixture = benchIndex();
  auto* segment = fixture.getField().currentSegment();
  int32_t matched = 0;
  for (auto _ : state) {
    auto guard = fixture.getIndex().pool.rewindScopeGuard();
    TermsEnum source(guard.pool(), segment->postingsReader(), fixture.getField().fieldInfo);
    if (brute) {
      BruteDfaTermsEnum terms(source, dfa.view());
      matched = count(terms);
    } else {
      auto view = dfa.view();
      auto [prefix, initialState] = view.commonPrefixAndState();
      std::string commonSuffix = dfa.commonSuffix();
      DfaScanPlan plan{prefix, commonSuffix, initialState};
      DfaIntersectEnum terms(guard.pool(), source, view, plan);
      matched = count(terms);
    }
    benchmark::DoNotOptimize(matched);
  }
  state.counters["matches"] = matched;
}

} // namespace

LUXIR_BENCHMARK_CAPTURE(BM_AutomatonEnum, wildcard_trailing_smart, "match_*", false, false);
LUXIR_BENCHMARK_CAPTURE(BM_AutomatonEnum, wildcard_trailing_brute, "match_*", false, true);
LUXIR_BENCHMARK_CAPTURE(BM_AutomatonEnum, wildcard_mid_smart, "match_*_end", false, false);
LUXIR_BENCHMARK_CAPTURE(BM_AutomatonEnum, wildcard_mid_brute, "match_*_end", false, true);
LUXIR_BENCHMARK_CAPTURE(BM_AutomatonEnum, wildcard_leading_smart, "*_end", false, false);
LUXIR_BENCHMARK_CAPTURE(BM_AutomatonEnum, wildcard_leading_brute, "*_end", false, true);
LUXIR_BENCHMARK_CAPTURE(BM_AutomatonEnum, regex_alternation_smart, "(match|noise)_[a-m].*", true, false);
LUXIR_BENCHMARK_CAPTURE(BM_AutomatonEnum, regex_alternation_brute, "(match|noise)_[a-m].*", true, true);
LUXIR_BENCHMARK_CAPTURE(BM_AutomatonEnum, regex_repeat_smart, "match_[a-z][0-9]{1,4}_end", true, false);
LUXIR_BENCHMARK_CAPTURE(BM_AutomatonEnum, regex_repeat_brute, "match_[a-z][0-9]{1,4}_end", true, true);
