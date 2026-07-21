#include "bench/solux_bench.h"
#include "test/CollectionHelper.h"
#include "test/LocalReq.h"
#include "test/QueryBuild.h"
#include "solux/search/SortField.h"

using namespace solux;
using namespace solux::test;

static void BM_StringSort(benchmark::State& state, int64_t nDocs,
                          std::string_view shape, qb::SortDir direction,
                          StringSortMode mode) {
  class ModeGuard {
    StringSortMode saved;
  public:
    explicit ModeGuard(StringSortMode mode)
        : saved(SortField::setStringSortModeForTests(mode)) {}
    ~ModeGuard() { SortField::setStringSortModeForTests(saved); }
  } guard(mode);

  if (solux::unit_tests) nDocs = 200;
  std::vector<int32_t> docsPerSeg;
  CollectionHelper::calcSegSizes(nDocs, 10, shape, docsPerSeg);

  CollectionHelper helper("string_sort_bm");
  bool reused = helper.indexMatchesShape(docsPerSeg);
  if (!reused) buildBenchIndex(helper, nDocs, docsPerSeg);

  int64_t fingerprint = -1;
  for (auto _ : state) {
    auto req = localReq(SoluxTest::soluxNode->getSearchEngine());
    req->collection("string_sort_bm");
    auto& top = req->topDocs("q").getNumber(true).allQuery().limit(100)
        .fields({"id", "short_u10k_s"});
    qb::sort(top, "short_u10k_s", direction);
    req->execute(false);
    const auto* docs = req->docList("q");
    int64_t current = docs->found.value_or(0);
    for (const auto& batch : req->responses) {
      const auto* list = batch->proto.ops.at("q")->docList();
      const auto& ids = std::get<api::ColStr>(list->columns.at("id").kind).v;
      const auto& values =
          std::get<api::ColStr>(list->columns.at("short_u10k_s").kind).v;
      for (size_t i = 0; i < ids.size(); i++) {
        current = current * 31 + java_string_hashcode(ids[i]);
        current = current * 31 + java_string_hashcode(values[i]);
      }
    }
    benchmark::DoNotOptimize(current);
    if (fingerprint != -1) {
      ASSERT_EQ(fingerprint, current);
    }
    fingerprint = current;
  }
  state.counters["fp"] = fingerprint;
  state.counters["reused"] = reused;
  state.counters["rate"] = benchmark::Counter(
      state.iterations(), benchmark::Counter::kIsRate);
}

static constexpr int64_t STRING_SORT_DOCS = 10'000'000;
static constexpr const char* STRING_SORT_SHAPE = "9555";

SOLUX_BENCHMARK_CAPTURE(BM_StringSort, global_asc, STRING_SORT_DOCS,
                        STRING_SORT_SHAPE, qb::ASC, StringSortMode::GLOBAL);
SOLUX_BENCHMARK_CAPTURE(BM_StringSort, global_desc, STRING_SORT_DOCS,
                        STRING_SORT_SHAPE, qb::DESC, StringSortMode::GLOBAL);
SOLUX_BENCHMARK_CAPTURE(BM_StringSort, segment_asc, STRING_SORT_DOCS,
                        STRING_SORT_SHAPE, qb::ASC, StringSortMode::SEGMENT);
SOLUX_BENCHMARK_CAPTURE(BM_StringSort, segment_desc, STRING_SORT_DOCS,
                        STRING_SORT_SHAPE, qb::DESC, StringSortMode::SEGMENT);
