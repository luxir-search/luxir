#include <charconv>
#include <test/SegmentTest.h>
#include "bench/solux_bench.h"
#include "solux/index/Inverter.h"

using namespace solux;

static void BM_BitSet(benchmark::State& state, int32_t blocks, bool getRank) {
  Rng rng;
  std::ostringstream ss;
  screaming::StringStreamBuilder builder(ss);

  int32_t doc = -1;
  int32_t limit = 65536*blocks;  // stop at blocksize so we know what we are testing and don't have a tail block.
  for (;;) {
    doc += (rng() & 0x03) + 1;  // add 1-4 docs (gap of 0-3)
    if (doc >= limit) break;
    builder.add(doc);
  }
  builder.flush();
  const auto& encoded = ss.str();
  screaming::BitSet bs(&encoded[0] + encoded.size());

  int64_t ret;
  int64_t count;
  for (auto _ : state) {
    ret = 0;
    count = 0;
    screaming::BitSet::Iterator it(bs);

    while (it.next() != screaming::BitSet::END) {
      ret += it.val();
      if (getRank) {
        ret += it.rank();
      }
      count++;
    }

    benchmark::DoNotOptimize(ret);
  }

  state.counters["count"] = count;
  state.counters["rate"] = benchmark::Counter(count, benchmark::Counter::kIsIterationInvariantRate);
}

static void BM_BitSet_block(benchmark::State& state, int32_t blocks, bool getRank) {
  Rng rng;
  std::ostringstream ss;
  screaming::StringStreamBuilder builder(ss);

  int32_t doc = -1;
  int32_t limit = 65536*blocks;  // stop at blocksize so we know what we are testing and don't have a tail block.
  for (;;) {
    doc += (rng() & 0x03) + 1;  // add 1-4 docs (gap of 0-3)
    if (doc >= limit) break;
    builder.add(doc);
  }
  builder.flush();
  const auto& encoded = ss.str();
  screaming::BitSet bs(&encoded[0] + encoded.size());

  int64_t ret;
  int64_t count;
  for (auto _ : state) {
    ret = 0;
    count = 0;
    screaming::BitSet::Iterator it(bs);

    while (it.next() != screaming::BitSet::END) {

      ret += it.val();
      if (getRank) {
        ret += it.rank();
      }
      count++;
    }

    benchmark::DoNotOptimize(ret);
  }

  state.counters["count"] = count;
  state.counters["rate"] = benchmark::Counter(count, benchmark::Counter::kIsIterationInvariantRate);
}

BENCHMARK_CAPTURE(BM_BitSet, denseIter, 1, false);
BENCHMARK_CAPTURE(BM_BitSet, denseIterRank, 1, true);
