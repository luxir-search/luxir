// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include <map>
#include <memory>
#include <string>

#include "bench/luxir_bench.h"
#include "test/TestIndex.h"
#include "luxir/query/PrefixQuery.h"

// A/B matrix for the constant-score multiterm union scorers (eager bitset vs
// all-cursors windowed vs heap-gated windows), across term count, per-term
// docFreq mix, and consumption depth. Every term starts with "q" so one
// prefix query unions the whole field. The scorer build runs inside the
// timed loop: that is where eager pays the union and heap pays sniff+heapify.
//
// The retained-state byte-budget edge (~235k terms) is deliberately absent:
// probing it needs a corpus too large for a routine bench; measure it on the
// bench box against a real index.

using namespace luxir;
using namespace luxir::test;

namespace {

using ScorerMode = MultiTermQuery::Weight::ScorerMode;

// dist encoding for benchmark args
enum Dist : int64_t { DENSE = 0, SPARSE = 1, ZIPF = 2, PULSED = 3, COMPACT = 4 };
constexpr const char* kDistNames[] = {"dense", "sparse", "zipf", "pulsed",
                                      "compact"};
constexpr ScorerMode kModes[] = {ScorerMode::FORCE_EAGER,
                                 ScorerMode::FORCE_WINDOWED,
                                 ScorerMode::FORCE_HEAP};
constexpr const char* kModeNames[] = {"eager", "windowed", "heap"};

struct MTCorpus {
  TestIndex ti;
  TestField field{ti, "body_w"};
  int32_t docSpace = 0;
};

// Deterministic tiny LCG so corpora are stable across runs.
struct Lcg {
  uint64_t s;
  uint32_t next(uint32_t bound) {
    s = s * 6364136223846793005ULL + 1442695040888963407ULL;
    return (uint32_t) ((s >> 33) % bound);
  }
};

std::unique_ptr<MTCorpus> buildCorpus(Dist dist, int32_t termCount) {
  auto corpus = std::make_unique<MTCorpus>();
  Lcg rng{(uint64_t) (dist * 1000003 + termCount)};
  std::map<int32_t, std::string> docs;
  auto place = [&](int32_t doc, int32_t term) {
    auto& body = docs[doc];
    if (!body.empty()) body.push_back(' ');
    body.append("q").append(std::to_string(term));
  };

  int32_t docSpace;
  switch (dist) {
    case DENSE:
      // A few heavy multi-block terms carry the union to high density over a
      // doc space spanning many windows; the rest are small. Mirrors the
      // *ing-style dense-union shape.
      docSpace = std::max(16384, termCount * 4);
      for (int32_t t = 0; t < termCount; t++) {
        if (t < 8) {
          for (int32_t d = t; d < docSpace; d += 2) place(d, t);
        } else {
          int32_t f = 4 + (int32_t) rng.next(12);
          for (int32_t i = 0; i < f; i++) place((int32_t) rng.next((uint32_t) docSpace), t);
        }
      }
      break;
    case COMPACT:
      // Doc space barely larger than one window, union covering every doc:
      // every term is active in every window, the worst case for heap-gating
      // (pure churn bound over the flat cursor scan).
      docSpace = std::max(4096, termCount / 2);
      for (int32_t t = 0; t < termCount; t++) {
        if (t < 8) {
          for (int32_t d = t; d < docSpace; d += 2) place(d, t);
        } else {
          int32_t f = 4 + (int32_t) rng.next(12);
          for (int32_t i = 0; i < f; i++) place((int32_t) rng.next((uint32_t) docSpace), t);
        }
      }
      break;
    case SPARSE:
      docSpace = termCount * 8;
      for (int32_t t = 0; t < termCount; t++) {
        int32_t f = 1 + (int32_t) rng.next(4);
        for (int32_t i = 0; i < f; i++) place((int32_t) rng.next((uint32_t) docSpace), t);
      }
      break;
    case ZIPF:
      docSpace = termCount * 2;
      for (int32_t t = 0; t < termCount; t++) {
        int32_t f = std::max(1, docSpace / (8 * (1 + t % 997)));
        int32_t start = (int32_t) rng.next((uint32_t) docSpace);
        int32_t step = std::max(1, docSpace / (f + 1));
        for (int32_t i = 0; i < f; i++) place((start + i * step) % docSpace, t);
      }
      break;
    case PULSED:
    default:
      docSpace = termCount * 8;
      for (int32_t t = 0; t < termCount; t++) {
        place((int32_t) rng.next((uint32_t) docSpace), t);
      }
      break;
  }

  corpus->docSpace = docSpace;
  corpus->field.startIndexing();
  for (auto& [doc, body] : docs) corpus->field.add(doc, body);
  corpus->ti.flush();
  corpus->field.startReading();
  return corpus;
}

MTCorpus& corpusFor(Dist dist, int32_t termCount) {
  static std::map<std::pair<int64_t, int32_t>, std::unique_ptr<MTCorpus>> cache;
  auto& slot = cache[{dist, termCount}];
  if (!slot) slot = buildCorpus(dist, termCount);
  return *slot;
}

class ScorerModeGuard {
  ScorerMode saved;

public:
  explicit ScorerModeGuard(ScorerMode mode)
    : saved(MultiTermQuery::Weight::scorerModeForTests) {
    MultiTermQuery::Weight::scorerModeForTests = mode;
  }

  ~ScorerModeGuard() {
    MultiTermQuery::Weight::scorerModeForTests = saved;
  }
};

// depth > 0: consume depth docs, then signal min-competitive (the pruned
// TOP_k shape under a constant score). depth == 0: full exhaustion.
// depth == -1: strided advance() consumption (conjunction-driven shape).
int64_t consume(TestIndex& ti, ScorerMode mode, int64_t depth, int32_t docSpace) {
  ScorerModeGuard guard(mode);
  auto g = ti.pool.rewindScopeGuard();
  PrefixQuery pq("body_w", "q");
  Query::Context ctx(ti.pool, *ti.reader);
  auto* weight = pq.createWeight(ctx, Query::NEED_SCORES | Query::ALLOW_PRUNING);
  auto* scorer = weight->createScorer(ti.pool, ctx.topReader.segments()[0]);
  if (scorer == nullptr) return 0;
  int64_t produced = 0;
  if (depth == -1) {
    const int32_t stride = std::max(64, docSpace / 512);
    int32_t sinceAdvance = 0;
    for (int32_t d = scorer->next(); d != PostingsReader::END;) {
      produced++;
      if (++sinceAdvance >= 3) {
        sinceAdvance = 0;
        d = scorer->advance(d + stride);
      } else {
        d = scorer->next();
      }
    }
    return produced;
  }
  for (int32_t d = scorer->next(); d != PostingsReader::END; d = scorer->next()) {
    produced++;
    if (depth > 0 && produced >= depth) {
      scorer->setMinCompetitiveScore(
          std::nextafter(1.0f, std::numeric_limits<float>::infinity()));
      if (scorer->next() != PostingsReader::END) return -1;  // exit failed
      break;
    }
  }
  return produced;
}

void BM_MultiTermUnion(benchmark::State& state) {
  const Dist dist = (Dist) state.range(0);
  // Unit-test mode caps corpus size: same code paths, fast setup.
  const int32_t termCount = unit_tests
      ? std::min<int32_t>((int32_t) state.range(1), 512)
      : (int32_t) state.range(1);
  const ScorerMode mode = kModes[state.range(2)];
  const int64_t depth = state.range(3);

  auto& corpus = corpusFor(dist, termCount);
  state.SetLabel(std::string(kDistNames[dist]) + "/T" + std::to_string(termCount)
                 + "/" + kModeNames[state.range(2)] + "/"
                 + (depth == -1 ? std::string("strided")
                    : depth == 0 ? std::string("exhaust")
                                 : "top" + std::to_string(depth)));

  int64_t produced = 0;
  for (auto _ : state) {
    produced = consume(corpus.ti, mode, depth, corpus.docSpace);
    benchmark::DoNotOptimize(produced);
  }
  if (produced < 0) {
    state.SkipWithError("min-competitive exit failed");
    return;
  }
  state.counters["produced"] = (double) produced;

  if (unit_tests) {
    // Cheap oracle: all three modes agree on the produced count.
    for (ScorerMode other : kModes) {
      if (consume(corpus.ti, other, depth, corpus.docSpace) != produced) {
        state.SkipWithError("mode disagreement");
        return;
      }
    }
  }
}

}  // namespace

BENCHMARK(BM_MultiTermUnion)
    ->ArgsProduct({{DENSE, SPARSE, ZIPF, PULSED, COMPACT},
                   {256, 2048, 8192, 65536},
                   {0, 1, 2},
                   {10, 1000, 0, -1}})
    ->UseRealTime();
