// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

// Combined-field (BM25F) relevance top-k over title/body/tag text fields, in
// the shape of the Lucene nightly Combined* tasks: multi_match in combined
// mode, so each query term becomes one CombinedFieldQuery and several terms
// form a disjunction (or a conjunction with operator AND) of them.
//
// Every field draws tokens from one Zipfian vocabulary, so the term rank sets
// its document frequency across all fields:
//   t20 ~ 30% of docs, t50 ~ 15%, t500 ~ 2%, t5000 ~ 0.2%

#include <charconv>
#include <cmath>
#include <vector>

#include <tbb/task_group.h>

#include "bench/luxir_bench.h"
#include "test/CollectionHelper.h"
#include "test/LocalReq.h"
#include "test/QueryBuild.h"

using namespace luxir;
using namespace luxir::test;

namespace {

using Mode = api::MultiMatchQuery_::Mode;
using MatchOp = api::Match_::Operator;

constexpr const char* collectionName = "combinedbm";
constexpr int64_t benchDocs = 2'000'000;
constexpr const char* shape = "5555";
constexpr int vocab = 50000;

struct Zipf {
  std::vector<double> cdf;

  Zipf() {
    cdf.resize(vocab);
    double sum = 0;
    for (int r = 1; r <= vocab; r++) {
      sum += 1.0 / (double)r;
      cdf[(size_t)r - 1] = sum;
    }
  }

  int sample(uint64_t bits) const {
    double target = (double)(bits >> 11) * 0x1.0p-53 * cdf.back();
    auto it = std::lower_bound(cdf.begin(), cdf.end(), target);
    return std::min((int)(it - cdf.begin()), vocab - 1);
  }
};

void appendTokens(std::string& out, SplitMix64& r, const Zipf& zipf, int n) {
  out.resize(0);
  for (int t = 0; t < n; t++) {
    if (t) out.push_back(' ');
    out.push_back('t');
    out.append(std::to_string(zipf.sample(r())));
  }
}

void buildIndex(CollectionHelper& helper, std::span<const int32_t> docsPerSeg) {
  helper.clear();
  static const Zipf zipf;
  auto iw = helper.getIndexWriter();
  std::vector<Inverter*> inverters;
  std::vector<int64_t> starts;
  int64_t next = 0;
  for (int32_t docs : docsPerSeg) {
    inverters.push_back(&iw->obtainInverter());
    starts.push_back(next);
    next += docs;
  }
  tbb::task_group tg;
  for (size_t seg = 0; seg < docsPerSeg.size(); seg++) {
    tg.run([&, seg]() {
      Inverter& inverter = *inverters[seg];
      auto& id = inverter.getIndexHandler("id");
      auto& title = inverter.getIndexHandler("title_w");
      auto& body = inverter.getIndexHandler("body_w");
      auto& tag = inverter.getIndexHandler("tag_w");
      std::string text;
      for (int64_t doc = starts[seg]; doc < starts[seg] + docsPerSeg[seg]; doc++) {
        SplitMix64 r((uint64_t)doc);
        inverter.startDoc();
        id.index(inverter, std::to_string(doc));
        appendTokens(text, r, zipf, 2 + (int)r.rint(9));
        title.index(inverter, text);
        appendTokens(text, r, zipf, 20 + (int)r.rint(101));
        body.index(inverter, text);
        // Most docs have no tag: the three-field query sees sparse norms.
        if (r.rint(4) == 0) {
          appendTokens(text, r, zipf, 1 + (int)r.rint(4));
          tag.index(inverter, text);
        }
        inverter.finishDoc();
      }
      iw->releaseInverter(inverter, true);
    });
  }
  tg.wait();
  helper.commit();
}

int64_t runQuery(std::string_view text, int fields, MatchOp op, int32_t topK) {
  auto req = localReq(LuxirTest::luxirNode->getSearchEngine());
  req->collection(collectionName);
  auto& topDocs = req->topDocs("q");
  auto& mr = topDocs.mr();
  if (fields == 2) {
    topDocs.rawQuery() = qb::multiMatch(mr, text, {{"title_w", 2.0f}, {"body_w", {}}},
                                        Mode::COMBINED, op);
  } else {
    topDocs.rawQuery() = qb::multiMatch(mr, text, {{"title_w", 2.0f}, {"body_w", {}}, {"tag_w", 1.5f}},
                                        Mode::COMBINED, op);
  }
  topDocs.limit(topK).fields({"id"}).getScores();
  req->execute(false);
  EXPECT_TRUE(req->ok()) << req->errorMsg();
  const auto* docs = req->docList();
  if (docs == nullptr || docs->row_count == 0) return 0;
  const auto& ids = std::get<api::ColStr>(docs->columns.at("id").kind).v;
  const auto& scores = std::get<api::ColFloat>(docs->columns.at("_score_").kind).v;
  int64_t fp = (int64_t)ids.size();
  for (size_t i = 0; i < ids.size(); i++) {
    int64_t id = 0;
    std::from_chars(ids[i].data(), ids[i].data() + ids[i].size(), id);
    fp = fp * 31 + id;
    fp = fp * 31 + (int64_t)std::bit_cast<uint32_t>(scores[i]);
  }
  return fp;
}

}  // namespace

static void BM_CombinedField(benchmark::State& state, std::string_view text, int fields, MatchOp op) {
  int64_t nDocs = luxir::unit_tests ? LuxirTest::scaleTestWork(2000) : benchDocs;
  std::vector<int32_t> docsPerSeg;
  CollectionHelper::calcSegSizes(nDocs, 10, shape, docsPerSeg);
  CollectionHelper helper(collectionName);
  bool reused = helper.indexMatchesShape(docsPerSeg);
  if (!reused) buildIndex(helper, docsPerSeg);

  int64_t fp = runQuery(text, fields, op, 100);
  for (auto _ : state) {
    int64_t ret = runQuery(text, fields, op, 100);
    benchmark::DoNotOptimize(ret);
    ASSERT_EQ(fp, ret);
  }
  // The low bits survive the double conversion; compare across builds.
  state.counters["fp"] = (double)(fp & ((1LL << 50) - 1));
  state.counters["reused"] = reused;
  state.counters["rate"] = benchmark::Counter(state.iterations(), benchmark::Counter::kIsRate);
}

static constexpr auto OR = MatchOp::OPERATOR_UNSPECIFIED;
static constexpr auto AND = MatchOp::AND;

LUXIR_BENCHMARK_CAPTURE(BM_CombinedField, term_high,        "t50", 2, OR);
LUXIR_BENCHMARK_CAPTURE(BM_CombinedField, term_med,         "t500", 2, OR);
LUXIR_BENCHMARK_CAPTURE(BM_CombinedField, or_high_high,     "t20 t50", 2, OR);
LUXIR_BENCHMARK_CAPTURE(BM_CombinedField, or_high_med,      "t50 t500", 2, OR);
LUXIR_BENCHMARK_CAPTURE(BM_CombinedField, or_high_low,      "t50 t5000", 2, OR);
LUXIR_BENCHMARK_CAPTURE(BM_CombinedField, and_high_high,    "t20 t50", 2, AND);
LUXIR_BENCHMARK_CAPTURE(BM_CombinedField, and_high_med,     "t50 t500", 2, AND);
LUXIR_BENCHMARK_CAPTURE(BM_CombinedField, or_high_med_3f,   "t50 t500", 3, OR);
LUXIR_BENCHMARK_CAPTURE(BM_CombinedField, term_high_3f,     "t50", 3, OR);
