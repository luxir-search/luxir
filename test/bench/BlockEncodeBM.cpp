#include <vector>
#include "solux/util/random.h"
#include "solux/util/solux_util.h"
#include "test/CodecTest.h"
#include "bench/solux_bench.h"
#include <gtest/gtest.h>


namespace solux {

constexpr uint32_t INT_BLOCK_SIZE = 128;

// TODO: how to chose distribution?  bias toward small values?
// figure out if sorting is needed based on codec?
inline void fillBlock(Rng& rng, uint64_t maxValue, uint32_t* out, uint32_t outSz, bool sorted) {
  if (sorted) {
    maxValue = maxValue / outSz;
  }
  maxValue = std::max((uint64_t)2,maxValue);
  for (uint32_t i = 0; i < outSz; i++) {
    out[i] = rng.rint(maxValue);
    if (sorted && i>0) {
      out[i] += out[i-1];
    }
  }
}

inline void fillBlock(Rng& rng, uint32_t* out, uint32_t outSz, bool sorted) {
  uint32_t maxBits = rng.rint(33);
  uint64_t maxValue = 1 << maxBits;
  return fillBlock(rng, maxValue, out, outSz, sorted);
}



static void BM_blockDecode(benchmark::State& state, std::string codecName, bool sorted) {
  // std::cout << "state.range[0]=" << state.range(0) << std::endl;
  Rng rng(1);

  auto codec = U32CodecFactory::getCodec(codecName);

  std::vector<uint32_t> values;
  std::vector<uint32_t> decoded;
  std::vector<char> encoded;

  uint32_t nvalues = INT_BLOCK_SIZE;
  values.resize(nvalues);
  decoded.resize(nvalues);
  encoded.resize(nvalues*sizeof(uint32_t) * 2);  // may result in SIMDCompressionLib::NotEnoughStorage if not big enough

  fillBlock(rng, &values[0], (uint32_t) values.size(), sorted);
  // some codecs modify the input array (calculating deltas in place), so make a copy.
  std::vector<uint32_t> orig(values);
  uint32_t encodedSz = encoded.size();
  codec->encodeBlock(&values[0], values.size(), &encoded[0], encodedSz);
  uint32_t decodedSz = 0;

  for (auto _ : state) {
    if (solux::unit_tests) {
      // state.PauseTiming();  // Only use in unit tests or slow tests!  See BenchTimer
      fillBlock(rng, &values[0], (uint32_t) values.size(), sorted);
      // some codecs modify the input array (calculating deltas in place), so make a copy.
      orig = values;
      encodedSz = encoded.size();
      codec->encodeBlock(&values[0], values.size(), &encoded[0], encodedSz);
      // state.ResumeTiming();
    }

    decodedSz = decoded.size();
    codec->decodeBlock(&encoded[0], encodedSz, &decoded[0], decodedSz);
    benchmark::DoNotOptimize(&decoded[0]);
    benchmark::ClobberMemory();

    if (solux::unit_tests) {
      // state.PauseTiming();
      ASSERT_EQ(nvalues, decodedSz);
      ASSERT_EQ(orig, decoded);
      // state.ResumeTiming();
    }
  }

  ASSERT_EQ(nvalues, decodedSz);
  ASSERT_EQ(orig, decoded);
};


// TODO: is there a way to get test name and avoid the duplication with codec here?
BENCHMARK_CAPTURE(BM_blockDecode, SimpleCodec, "SimpleCodec", false);
BENCHMARK_CAPTURE(BM_blockDecode, FastPFor, "FastPFor", false); // ->Range(8, 8<<10);
BENCHMARK_CAPTURE(BM_blockDecode, SIMDFastPFor, "SIMDFastPFor", false);
BENCHMARK_CAPTURE(BM_blockDecode, SIMDFastPForDelta1, "SIMDFastPForDelta1", true);


} // end solux
