#include <vector>
#include "solux/util/random.h"
#include "solux/util/solux_util.h"
#include "solux/codec/NumColumnFormat.h"
#include "test/CodecTest.h"
#include "bench/solux_bench.h"
#include <gtest/gtest.h>


namespace solux {

//constexpr uint32_t INT_BLOCK_SIZE = SoluxPFOR::BLOCK_SIZE;
constexpr uint32_t INT_BLOCK_SIZE = NumColumnFormat::BLOCK_SIZE;

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
  uint64_t maxValue = maxBits==32 ? 0 : (1 << maxBits);
  return fillBlock(rng, maxValue, out, outSz, sorted);
}



static void BM_blockDecode(benchmark::State& state, std::string codecName, uint32_t blockSize, bool sorted, bool testSelect=false) {
  // std::cout << "state.range[0]=" << state.range(0) << std::endl;
  Rng rng(SoluxTest::global_random_seed);  // make same data for different test variants

  auto codec = U32CodecFactory::getCodec(codecName);

  std::vector<std::vector<uint32_t>> values;
  std::vector<std::vector<uint32_t>> decoded;
  std::vector<std::vector<char>> encoded;
  std::vector<uint32_t> maxValues = {1<<3,1<<5,1<<7,1<<9,1<<11,1<<13,1<<15,1<<17,1<<19,1<<21};  // 10 diff sizes

  uint32_t nvalues = blockSize;

  values.resize(maxValues.size());
  encoded.resize(maxValues.size());
  decoded.resize(maxValues.size());

  for (auto i = 0u; i<maxValues.size(); i++) {
    values[i].resize(nvalues);
    decoded[i].resize(nvalues);
    encoded[i].resize(nvalues*sizeof(uint32_t) * 2);  // must be large enough for the codec's worst-case output
    fillBlock(rng, maxValues[i], &values[i][0], nvalues, sorted);
    // some codecs modify the input array (calculating deltas in place), so make a copy.
    std::vector<uint32_t> orig(values[i]);
    uint32_t encodedSz = encoded[i].size();
    codec->encodeBlock(&orig[0], nvalues, &encoded[i][0], encodedSz);
    encoded[i].resize(encodedSz);
  }


  for (auto _ : state) {

    for (auto i=0u; i<maxValues.size(); i++) {
      if (testSelect) {
        for (auto j=0u; j<10; j++) {
          // single value decode
          size_t which = rng.rint(blockSize);
          auto val = codec->select(&encoded[i][0], values[i].size(), which);

          ASSERT_EQ(values[i][which], val);
        }
      } else {
        // block decode
        uint32_t decodedSz = decoded[i].size();
        codec->decodeBlock(&encoded[i][0], encoded[i].size(), &decoded[i][0], decodedSz);
        ASSERT_EQ(nvalues, decodedSz);

        if (solux::unit_tests) {
          // state.PauseTiming();
          ASSERT_EQ(nvalues, decodedSz);
          ASSERT_EQ(values[i], decoded[i]);
          // state.ResumeTiming();
        }
      }
    }

    benchmark::DoNotOptimize(&decoded);
    benchmark::ClobberMemory();
  }

  // calculate total size of encoded data
  double totalSz = 0;
  for (auto& enc : encoded) {
    totalSz += enc.size();
  }
  state.counters["size"] = totalSz;
};


// TODO: is there a way to get test name and avoid the duplication with codec here?
BENCHMARK_CAPTURE(BM_blockDecode, SimpleCodec, "SimpleCodec", INT_BLOCK_SIZE, false);
BENCHMARK_CAPTURE(BM_blockDecode, SoluxPFOR128, "SoluxPFOR", 128, false);  // these two codecs only do 128
BENCHMARK_CAPTURE(BM_blockDecode, SoluxPFORd128, "SoluxPFORd", 128, true);
BENCHMARK_CAPTURE(BM_blockDecode, SoluxSIMDFor, "SoluxSIMDFor", INT_BLOCK_SIZE, false);
BENCHMARK_CAPTURE(BM_blockDecode, SoluxSIMDFor_select, "SoluxSIMDFor", INT_BLOCK_SIZE, false, true);


} // end solux
