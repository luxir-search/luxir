#include <span>
#include <string>
#include <vector>

#include "bench/solux_bench.h"
#include "solux/server/JsonResponse.h"
#include "test/CollectionHelper.h"
#include "test/LocalReq.h"

using namespace solux;
using namespace solux::test;

// Field-retrieval micro-benchmarks over the shared bench index:
//
//   BM_FieldLoad   - a top-100 page retrieval with a fixed, cheap query.
//                    The "none" variant (no fields) is the control: subtract
//                    it to isolate field-loading cost.  ROWS vs COLUMNS
//                    measures the scatter pass on top of the same loaders.
//
//   BM_JsonRender  - renderSearchResponseLine over an already-built
//                    response (built once, outside the timing loop): the
//                    columns->rows transpose vs the rows passthrough.

static constexpr int32_t benchDocs = 10'000'000;
static constexpr const char* shape = "9555";  // 9 segments, 555 docs per segment

static const std::vector<std::string> FIELDS_NONE = {};
static const std::vector<std::string> FIELDS_INT = {"u10k_i"};
static const std::vector<std::string> FIELDS_STR = {"med_u10k_s"};
static const std::vector<std::string> FIELDS_MIXED = {"id", "u10k_i", "short_u10k_s", "med_u10k_s"};

// Build or reuse the standard bench index; returns whether it was reused.
static bool setupIndex(CollectionHelper& helper, int64_t& nDocs) {
  nDocs = solux::unit_tests ? 200 : benchDocs;
  std::vector<int32_t> docsPerSeg;
  CollectionHelper::calcSegSizes(nDocs, 10, shape, docsPerSeg);
  bool reuse = helper.indexMatchesShape(docsPerSeg);
  if (!reuse) buildBenchIndex(helper, nDocs, docsPerSeg);
  return reuse;
}

// Fixed page query.  At full scale short_u10k_s:"0" matches ~nDocs/10k docs
// (~1k), so collection stays cheap relative to loading; the tiny unit-test
// corpus needs the lower-cardinality field to match anything at all.
static const char* pageQueryField() {
  return solux::unit_tests ? "short_u10_s" : "short_u10k_s";
}

// Order-sensitive value hash over both carriers of a DocList (columns and
// docs), for the per-iteration determinism assert.
static int64_t fingerprintDocList(const solux::api::DocList& dl) {
  int64_t h = dl.row_count;
  for (const auto& [name, col] : dl.columns) {
    h = h * 31 + java_string_hashcode(name);
    if (const auto* c = std::get_if<solux::api::ColStr>(&col.kind)) {
      for (size_t i = 0; i < c->v.size(); i++)
        if (c->v[i] != c->missing_val) h = h * 31 + java_string_hashcode(c->v[i]);
    } else if (const auto* c = std::get_if<solux::api::ColInt>(&col.kind)) {
      for (size_t i = 0; i < c->v.size(); i++)
        if (c->v[i] != c->missing_val) h = h * 31 + c->v[i];
    }
  }
  for (const auto& row : dl.docs) {
    for (const auto& [name, val] : row.fields) {
      h = h * 31 + java_string_hashcode(name);
      if (const auto* s = std::get_if<std::string_view>(&val->kind)) {
        h = h * 31 + java_string_hashcode(*s);
      } else if (const auto* i = std::get_if<int64_t>(&val->kind)) {
        h = h * 31 + *i;
      }
    }
  }
  return h;
}

static void BM_FieldLoad(benchmark::State& state, const std::vector<std::string>& fields,
                         solux::api::DocFormat format) {
  CollectionHelper helper;
  int64_t nDocs = 0;
  bool reuseIndex = setupIndex(helper, nDocs);

  int64_t fp = -1;
  for (auto _ : state) {
    auto req = localReq(SoluxTest::soluxNode->getSearchEngine());
    req->collection("main");
    req->topDocs("q")
        .matchQuery(pageQueryField(), "0")
        .fields(std::span<const std::string>(fields))
        .documentFormat(format)
        .limit(100);
    req->execute(false);

    const auto* dl = req->docList("q");
    ASSERT_NE(dl, nullptr);
    int64_t ret = fingerprintDocList(*dl);
    benchmark::DoNotOptimize(ret);
    if (fp != -1) {
      ASSERT_EQ(fp, ret);
    }
    fp = ret;
  }

  state.counters["fp"] = fp;
  state.counters["reused"] = reuseIndex;
  state.counters["rate"] = benchmark::Counter(state.iterations(), benchmark::Counter::kIsRate);
}

static void BM_JsonRender(benchmark::State& state, const std::vector<std::string>& fields,
                          solux::api::DocFormat format) {
  CollectionHelper helper;
  int64_t nDocs = 0;
  bool reuseIndex = setupIndex(helper, nDocs);

  // Build the response once; the handle keeps the response arena alive for
  // the whole timing loop.
  auto req = localReq(SoluxTest::soluxNode->getSearchEngine());
  req->collection("main");
  req->topDocs("q")
      .matchQuery(pageQueryField(), "0")
      .fields(std::span<const std::string>(fields))
      .documentFormat(format)
      .limit(100);
  req->execute(false);
  ASSERT_NE(req->docList("q"), nullptr);
  const auto& proto = req->responses[0]->proto;

  int64_t fp = -1;
  for (auto _ : state) {
    std::string line = renderSearchResponseLine(proto);
    int64_t ret = (int64_t)line.size();
    benchmark::DoNotOptimize(line.data());
    if (fp != -1) {
      ASSERT_EQ(fp, ret);
    }
    fp = ret;
  }

  state.counters["bytes"] = fp;
  state.counters["reused"] = reuseIndex;
  state.counters["rate"] = benchmark::Counter(state.iterations(), benchmark::Counter::kIsRate);
}

static constexpr auto ROWS = solux::api::DocFormat::ROWS;
static constexpr auto COLS = solux::api::DocFormat::COLUMNS;

SOLUX_BENCHMARK_CAPTURE(BM_FieldLoad, none, FIELDS_NONE, COLS);
SOLUX_BENCHMARK_CAPTURE(BM_FieldLoad, int_cols, FIELDS_INT, COLS);
SOLUX_BENCHMARK_CAPTURE(BM_FieldLoad, int_rows, FIELDS_INT, ROWS);
SOLUX_BENCHMARK_CAPTURE(BM_FieldLoad, str_cols, FIELDS_STR, COLS);
SOLUX_BENCHMARK_CAPTURE(BM_FieldLoad, str_rows, FIELDS_STR, ROWS);
SOLUX_BENCHMARK_CAPTURE(BM_FieldLoad, mixed4_cols, FIELDS_MIXED, COLS);
SOLUX_BENCHMARK_CAPTURE(BM_FieldLoad, mixed4_rows, FIELDS_MIXED, ROWS);

SOLUX_BENCHMARK_CAPTURE(BM_JsonRender, str_cols, FIELDS_STR, COLS);
SOLUX_BENCHMARK_CAPTURE(BM_JsonRender, str_rows, FIELDS_STR, ROWS);
SOLUX_BENCHMARK_CAPTURE(BM_JsonRender, mixed4_cols, FIELDS_MIXED, COLS);
SOLUX_BENCHMARK_CAPTURE(BM_JsonRender, mixed4_rows, FIELDS_MIXED, ROWS);
