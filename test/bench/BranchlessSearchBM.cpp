#include <benchmark/benchmark.h>
#include <gtest/gtest.h>
#include <random>
#include <vector>
#include <map>
#include <algorithm>
#include "solux/util/BranchlessSearch.h"
#include "solux/util/screaming.h"
#include "solux/util/random.h"
#include "test/SoluxTest.h"

using namespace solux;
using screaming::gallopLowerBound;

namespace solux { extern bool unit_tests; }

// Low-level micro-benchmarks for the screaming-bitset / prefix-array search
// internals (branchless vs std vs gallop).  These are NOT regression benchmarks
// and are disabled by default -- otherwise they would run on every solux_test
// invocation via Benchmarks.all and slow the (ASan/valgrind) suite.  They are
// only worth running when working on the bitset / search code itself.
//
// To run them: uncomment the `#define RUN_DISABLED_BENCHMARKS` below, rebuild, then
//   solux_test --bench --benchmark_filter='UpperBound|ColdSparse|SparseSkip|DenseRank'
// The findings are recorded in the project memory; the production choices they
// justify (gallop in the SPARSE advance, branchless in select / findSeg / the
// dense rank index) are covered by the normal Screaming/Knn tests plus the
// always-on correctness test below.

// Always-on: cheap fuzz check that the production gallop and branchless searches
// match std::lower_bound across edge-case lengths and every key position.
TEST(BranchlessSearchBM, gallopAndBranchlessMatchStd) {
  auto& rng = SoluxTest::rng;  // gtest-seeded (per-test) fast RNG
  for (size_t len : std::initializer_list<size_t>{0, 1, 2, 3, 7, 16, 100, 4096}) {
    std::uniform_int_distribution<int> v(0, (int)len + 5);
    std::vector<uint16_t> arr(len);
    for (auto& x : arr) x = (uint16_t)v(rng);
    std::sort(arr.begin(), arr.end());
    for (int t = -1; t <= (int)len + 6; t++) {
      uint16_t key = (uint16_t)std::max(0, t);
      auto* expect = std::lower_bound(arr.data(), arr.data() + len, key);
      EXPECT_EQ(gallopLowerBound(arr.data(), arr.data() + len, key), expect) << "gallop len=" << len << " key=" << key;
      EXPECT_EQ(BranchlessIndex<uint16_t>::lowerBound(arr.data(), len, key), expect) << "branchless len=" << len << " key=" << key;
    }
  }
}

namespace {

// One probe per array, summed so nothing is optimized away.  N keys are cycled
// through to keep the branch predictor cold without dominating with key-gen.
constexpr int NUM_KEYS = 1024;

template <typename T>
std::vector<T> sortedArray(size_t len, uint64_t span) {
  Rng rng(0x5eed ^ len);  // local, per-len seed: same data for std/branchless A/B
  std::uniform_int_distribution<uint64_t> dist(0, span);
  std::vector<T> v;
  v.reserve(len);
  for (size_t i = 0; i < len; i++) v.push_back((T)dist(rng));
  std::sort(v.begin(), v.end());
  return v;
}

template <typename T>
std::vector<T> randomKeys(uint64_t span) {
  Rng rng(0xc0ffee);  // local, fixed seed: identical keys across A/B variants
  std::uniform_int_distribution<uint64_t> dist(0, span);
  std::vector<T> keys(NUM_KEYS);
  for (auto& k : keys) k = (T)dist(rng);
  return keys;
}

// ----------------------------------------------------------------------------
// Prefix-array upperBound (KnnQuery::findSeg int64, Selector::select int32):
// small arrays searched repeatedly with unpredictable keys -- branchless wins,
// and remembering the layout (stateful) wins again.  These justify the branchless
// + stateful choices at those call sites.
template <typename T>
void BM_StdUpperBound(benchmark::State& state) {
  size_t len = (size_t)state.range(0);
  uint64_t span = (sizeof(T) == 2) ? 60000 : (len * 16);
  auto arr = sortedArray<T>(len, span);
  auto keys = randomKeys<T>(span);
  size_t ki = 0;
  for (auto _ : state) {
    auto it = std::upper_bound(arr.begin(), arr.end(), keys[ki]);
    benchmark::DoNotOptimize(it);
    ki = (ki + 1) & (NUM_KEYS - 1);
  }
  state.SetItemsProcessed(state.iterations());
}

template <typename T>
void BM_BranchlessUpperBound(benchmark::State& state) {
  size_t len = (size_t)state.range(0);
  uint64_t span = (sizeof(T) == 2) ? 60000 : (len * 16);
  auto arr = sortedArray<T>(len, span);
  auto keys = randomKeys<T>(span);
  size_t ki = 0;
  for (auto _ : state) {
    auto* it = BranchlessIndex<T>::upperBound(arr.data(), arr.size(), keys[ki]);
    benchmark::DoNotOptimize(it);
    ki = (ki + 1) & (NUM_KEYS - 1);
  }
  state.SetItemsProcessed(state.iterations());
}

// Stateful: layout (bit_floor + subtract) precomputed once, reused every probe.
// Tests whether remembering the layout beats recomputing it per call, or is noise.
template <typename T>
void BM_BranchlessUpperBoundStateful(benchmark::State& state) {
  size_t len = (size_t)state.range(0);
  uint64_t span = (sizeof(T) == 2) ? 60000 : (len * 16);
  auto arr = sortedArray<T>(len, span);
  auto keys = randomKeys<T>(span);
  BranchlessIndex<T> index(arr.size());
  size_t ki = 0;
  for (auto _ : state) {
    auto* it = index.upperBound(arr.data(), keys[ki]);
    benchmark::DoNotOptimize(it);
    ki = (ki + 1) & (NUM_KEYS - 1);
  }
  state.SetItemsProcessed(state.iterations());
}

// ----------------------------------------------------------------------------
// Cold working set of sorted-uint16 buckets, visited in random order (defeats
// the HW prefetcher) so each search hits cold data.  Shared by the sparse-bucket
// (BM_ColdSparse, BM_SparseSkip) and dense rank-index (BM_DenseRank) benchmarks.

// 256MB working set, ~8x the per-CCD 32MB L3 on a 5950X (the full 64MB is split
// 2x32MB across CCDs, so a single-threaded search sees only 32MB).  1GB sharpens
// the numbers but does not change any conclusion.
constexpr size_t COLD_TARGET_BYTES = 256ull << 20;

struct SparseArena {
  std::vector<uint16_t> data;       // all buckets concatenated
  std::vector<uint32_t> offsets;    // bucket b is [offsets[b], offsets[b]+bucketSize)
  std::vector<uint32_t> order;      // shuffled visitation order (cold access pattern)
  size_t bucketSize = 0;
};

// [[maybe_unused]]: only reached from the (template) benchmark bodies below, which
// are not instantiated unless RUN_DISABLED_BENCHMARKS registers them.
[[maybe_unused]] SparseArena buildArena(size_t bucketSize, size_t targetBytes) {
  SparseArena a;
  a.bucketSize = bucketSize;
  size_t numBuckets = std::max<size_t>(1, targetBytes / (bucketSize * sizeof(uint16_t)));
  a.data.resize(numBuckets * bucketSize);
  a.offsets.resize(numBuckets + 1);
  a.order.resize(numBuckets);
  Rng rng(0x5af3 ^ bucketSize);  // local, per-bucketSize seed for reproducible arena
  std::uniform_int_distribution<uint32_t> v(0, 60000);
  for (size_t b = 0; b < numBuckets; b++) {
    a.offsets[b] = (uint32_t)(b * bucketSize);
    uint16_t* base = a.data.data() + b * bucketSize;
    for (size_t i = 0; i < bucketSize; i++) base[i] = (uint16_t)v(rng);
    std::sort(base, base + bucketSize);
    a.order[b] = (uint32_t)b;
  }
  a.offsets[numBuckets] = (uint32_t)(numBuckets * bucketSize);
  std::shuffle(a.order.begin(), a.order.end(), rng);
  return a;
}

// Built once per distinct bucket size and reused across the std/branchless/
// prefetch runs (and across benchmark's repeated convergence calls).
[[maybe_unused]] const SparseArena& coldArena(size_t bucketSize) {
  static std::map<size_t, SparseArena> cache;
  size_t target = solux::unit_tests ? (4ull << 20) : COLD_TARGET_BYTES;
  auto it = cache.find(bucketSize);
  if (it == cache.end()) it = cache.emplace(bucketSize, buildArena(bucketSize, target)).first;
  return it->second;
}

// Branchless monobound lower_bound with two-child prefetch: before each probe,
// prefetch both candidate addresses for the *next* level so their misses overlap.
// The one variant that beats std on cold sparse buckets (plain branchless does not).
template <typename T>
const T* branchlessLBPrefetch(const T* begin, size_t len, const T& key) noexcept {
  if (len == 0) return begin;
  size_t step = std::bit_floor(len);
  size_t rem = len - step;
  const T* base = begin;
  if (rem > 0) base = (base[rem] < key) ? base + rem : base;
  for (size_t s = step; s > 1; ) {
    s >>= 1;
    size_t half = s >> 1;
    __builtin_prefetch(base + half);       // next probe if we don't advance
    __builtin_prefetch(base + s + half);    // next probe if we do advance
    base = (base[s] < key) ? base + s : base;
  }
  return base + (*base < key);
}

// gallopLowerBound lives in screaming.h (the production SPARSE-advance search).
// Variant: same exponential ramp, but the inner bracket search is branchless
// instead of std::lower_bound.  Rejected -- the gallop-narrowed bracket is too
// small for branchless's fixed overhead to pay off (kept to document that).
template <typename T>
const T* gallopLowerBoundBL(const T* lo, const T* hi, const T& key) noexcept {
  size_t n = (size_t)(hi - lo);
  if (n == 0) return lo;
  size_t bound = 1;
  while (bound < n && lo[bound] < key) bound <<= 1;
  size_t loIdx = bound >> 1;
  size_t hiIdx = bound < n ? bound + 1 : n;
  return BranchlessIndex<T>::lowerBound(lo + loIdx, hiIdx - loIdx, key);
}

enum ColdKind { COLD_STD, COLD_BRANCHLESS, COLD_PREFETCH };
enum SkipKind { SKIP_STD, SKIP_BRANCHLESS, SKIP_GALLOP, SKIP_GALLOP_BL };

// Independent cold searches over full buckets: the worst case for branchless
// (each probe a fresh DRAM miss, no speculation/prefetch). Arg is bucket size.
template <ColdKind Kind>
void BM_ColdSparse(benchmark::State& state) {
  size_t bucketSize = (size_t)state.range(0);
  const SparseArena& a = coldArena(bucketSize);
  auto keys = randomKeys<uint16_t>(60000);
  size_t numBuckets = a.order.size();
  size_t bi = 0, ki = 0;
  uint64_t sink = 0;
  for (auto _ : state) {
    // Uniform bucket size -> offset is direct; avoids an extra random offsets[]
    // miss confounding the measurement.  order[] is read sequentially (warm).
    const uint16_t* base = a.data.data() + (size_t)a.order[bi] * bucketSize;
    uint16_t key = keys[ki];
    const uint16_t* r;
    if constexpr (Kind == COLD_STD)         r = std::lower_bound(base, base + bucketSize, key);
    else if constexpr (Kind == COLD_BRANCHLESS) r = BranchlessIndex<uint16_t>::lowerBound(base, bucketSize, key);
    else                                    r = branchlessLBPrefetch(base, bucketSize, key);
    sink += (uint64_t)(r - base);
    if (++bi == numBuckets) bi = 0;         // random-order sweep keeps buckets cold
    ki = (ki + 1) & (NUM_KEYS - 1);
  }
  benchmark::DoNotOptimize(sink);
  state.SetItemsProcessed(state.iterations());
}

// Forward-skipping iterator over cold buckets: each bucket is entered cold (large
// working set) then iterated front-to-back, advancing by ~`skip` elements per
// step and searching the tail [cursor+1, end) for the next target -- the real
// screaming SPARSE advance pattern.  Small skips => target sits next to the warm
// cursor, favoring gallop; large skips => wide tail searches, like the cold case.
// Arg is the skip distance (elements). Bucket size fixed at BUCKET_SPARSE_MAX.
template <SkipKind Kind>
void BM_SparseSkip(benchmark::State& state) {
  constexpr size_t BSZ = 4096; // BUCKET_SPARSE_MAX
  size_t skip = (size_t)state.range(0);
  const SparseArena& a = coldArena(BSZ);
  size_t numBuckets = a.order.size();
  size_t bk = 0;                                   // which bucket (random order)
  const uint16_t* base = a.data.data() + (size_t)a.order[0] * BSZ;
  size_t cur = 0;                                  // cursor within bucket
  uint64_t sink = 0;
  for (auto _ : state) {
    size_t tIdx = cur + skip;
    if (tIdx >= BSZ) {                             // bucket exhausted -> next cold bucket
      if (++bk == numBuckets) bk = 0;
      base = a.data.data() + (size_t)a.order[bk] * BSZ;
      cur = 0;
      tIdx = skip < BSZ ? skip : BSZ - 1;
    }
    uint16_t target = base[tIdx];
    const uint16_t* lo = base + cur + 1;
    const uint16_t* hi = base + BSZ;
    const uint16_t* r;
    if constexpr (Kind == SKIP_STD)             r = std::lower_bound(lo, hi, target);
    else if constexpr (Kind == SKIP_BRANCHLESS) r = BranchlessIndex<uint16_t>::lowerBound(lo, (size_t)(hi - lo), target);
    else if constexpr (Kind == SKIP_GALLOP)     r = gallopLowerBound(lo, hi, target);
    else                                        r = gallopLowerBoundBL(lo, hi, target);
    cur = (size_t)(r - base);
    sink += cur;
  }
  benchmark::DoNotOptimize(sink);
  state.SetItemsProcessed(state.iterations());
}

// Dense-block rank-index search (screaming.h selectInBucket): upperBound over a
// uint16 array of *constexpr* length RANK_INDEX_ENTRIES (128 entries = 256B),
// keyed by an int32 localRank.  The constexpr length folds bit_floor at compile
// time (unlike the runtime-length BM_BranchlessUpperBound), matching the real
// call site.  Hot = one warm array (repeated selects on one block); cold =
// random arrays across a >L3 arena (selects scattered across many dense blocks).
enum DenseKind { DR_STD, DR_BRANCHLESS };

template <DenseKind Kind, bool Cold>
void BM_DenseRank(benchmark::State& state) {
  constexpr size_t N = 128; // RANK_INDEX_ENTRIES
  auto keys = randomKeys<int32_t>(60000);
  std::vector<uint16_t> hot;
  const SparseArena* cold = nullptr;
  size_t numArr = 1;
  if constexpr (Cold) { cold = &coldArena(N); numArr = cold->order.size(); }
  else hot = sortedArray<uint16_t>(N, 60000);
  size_t ai = 0, ki = 0;
  uint64_t sink = 0;
  for (auto _ : state) {
    const uint16_t* base = Cold ? (cold->data.data() + (size_t)cold->order[ai] * N) : hot.data();
    int32_t key = keys[ki];
    const uint16_t* r;
    if constexpr (Kind == DR_STD) r = std::upper_bound(base, base + N, key);
    else                          r = BranchlessIndex<uint16_t>::upperBound(base, N, key);
    sink += (uint64_t)(r - base);
    if constexpr (Cold) { if (++ai == numArr) ai = 0; }
    ki = (ki + 1) & (NUM_KEYS - 1);
  }
  benchmark::DoNotOptimize(sink);
  state.SetItemsProcessed(state.iterations());
}

} // namespace

// Uncomment to register the micro-benchmarks above (see file header).
// #define RUN_DISABLED_BENCHMARKS
#ifdef RUN_DISABLED_BENCHMARKS

// findSeg: int64 prefix sums, nSegs+1 entries.
BENCHMARK_TEMPLATE(BM_StdUpperBound, int64_t)->Arg(8)->Arg(32)->Arg(64)->Arg(100)->UseRealTime();
BENCHMARK_TEMPLATE(BM_BranchlessUpperBound, int64_t)->Arg(8)->Arg(32)->Arg(64)->Arg(100)->UseRealTime();
BENCHMARK_TEMPLATE(BM_BranchlessUpperBoundStateful, int64_t)->Arg(8)->Arg(32)->Arg(64)->Arg(100)->UseRealTime();

// Selector::select: int32 per-bucket rank prefix (nBuckets+1).
BENCHMARK_TEMPLATE(BM_StdUpperBound, int32_t)->Arg(8)->Arg(64)->Arg(256)->Arg(1024)->UseRealTime();
BENCHMARK_TEMPLATE(BM_BranchlessUpperBound, int32_t)->Arg(8)->Arg(64)->Arg(256)->Arg(1024)->UseRealTime();
BENCHMARK_TEMPLATE(BM_BranchlessUpperBoundStateful, int32_t)->Arg(8)->Arg(64)->Arg(256)->Arg(1024)->UseRealTime();

// uint16 prefix-array proxy (runtime length, no constexpr fold).
BENCHMARK_TEMPLATE(BM_StdUpperBound, uint16_t)->Arg(128)->Arg(512)->Arg(4096)->UseRealTime();
BENCHMARK_TEMPLATE(BM_BranchlessUpperBound, uint16_t)->Arg(128)->Arg(512)->Arg(4096)->UseRealTime();
BENCHMARK_TEMPLATE(BM_BranchlessUpperBoundStateful, uint16_t)->Arg(128)->Arg(512)->Arg(4096)->UseRealTime();

// Cold sparse buckets: std vs plain branchless vs prefetch-branchless.
BENCHMARK_TEMPLATE(BM_ColdSparse, COLD_STD)->Arg(64)->Arg(256)->Arg(1024)->Arg(4096)->UseRealTime();
BENCHMARK_TEMPLATE(BM_ColdSparse, COLD_BRANCHLESS)->Arg(64)->Arg(256)->Arg(1024)->Arg(4096)->UseRealTime();
BENCHMARK_TEMPLATE(BM_ColdSparse, COLD_PREFETCH)->Arg(64)->Arg(256)->Arg(1024)->Arg(4096)->UseRealTime();

// Forward-skip SPARSE advance: std vs branchless vs gallop vs gallop+branchless-bracket.
BENCHMARK_TEMPLATE(BM_SparseSkip, SKIP_STD)->Arg(1)->Arg(8)->Arg(64)->Arg(512)->UseRealTime();
BENCHMARK_TEMPLATE(BM_SparseSkip, SKIP_BRANCHLESS)->Arg(1)->Arg(8)->Arg(64)->Arg(512)->UseRealTime();
BENCHMARK_TEMPLATE(BM_SparseSkip, SKIP_GALLOP)->Arg(1)->Arg(8)->Arg(64)->Arg(512)->UseRealTime();
BENCHMARK_TEMPLATE(BM_SparseSkip, SKIP_GALLOP_BL)->Arg(1)->Arg(8)->Arg(64)->Arg(512)->UseRealTime();

// Dense rank index: std vs branchless, hot and cold.
BENCHMARK_TEMPLATE(BM_DenseRank, DR_STD, false)->UseRealTime();        // hot
BENCHMARK_TEMPLATE(BM_DenseRank, DR_BRANCHLESS, false)->UseRealTime(); // hot
BENCHMARK_TEMPLATE(BM_DenseRank, DR_STD, true)->UseRealTime();         // cold
BENCHMARK_TEMPLATE(BM_DenseRank, DR_BRANCHLESS, true)->UseRealTime();  // cold

#endif // RUN_DISABLED_BENCHMARKS
