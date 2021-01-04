#include <vector>
#include <algorithm>
#include "simdcomp/include/codecfactory.h"
#include "solux/util/random.h"
#include <benchmark/benchmark.h>
#include <gtest/gtest.h>

// how to share code between benchmarks and unit tests?
// run a benchmark (at lower iterations) as part of a unit test?

namespace solux {

constexpr uint32_t INT_BLOCK_SIZE = 128;
using IntegerCodec = SIMDCompressionLib::IntegerCODEC;


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

/// No compression, just remembers the length and uses memcpy to copy the array
class SimpleCodec : public IntegerCodec {
public:
  void encodeArray(uint32_t *in, const size_t length, uint32_t *out, size_t &nvalue) override {
    *out = length;
    memcpy(out+1, in, length * sizeof(uint32_t));
    nvalue = length+1;
  }

  const uint32_t *decodeArray(const uint32_t *in, const size_t length, uint32_t *out, size_t &nvalue) override {
    nvalue = *in;
    memcpy(out, in+1, length * sizeof(uint32_t));
    return out;
  }

  ~SimpleCodec() override {

  }

  std::vector<uint32_t> compress(std::vector<uint32_t> &data) override {
    return IntegerCODEC::compress(data);
  }

  std::vector<uint32_t> uncompress(std::vector<uint32_t> &compresseddata, size_t expected_uncompressed_size) override {
    return IntegerCODEC::uncompress(compresseddata, expected_uncompressed_size);
  }

  std::string name() const override {
    return "SimpleCodec";
  }
};


// TODO: how to chose encoder... use simdcomp IntegerCodec for now...
// TODO: support different block sizes?
inline std::unique_ptr<IntegerCodec> getIntegerCodec(const std::string& name) {
  if (name=="FastPFor") {
    return std::make_unique<SIMDCompressionLib::FastPFor<4, false>>(); // corresponds to a block size of 128 and non-delta coding
  } else if (name=="SIMDFastPFor") {
    return std::make_unique<SIMDCompressionLib::SIMDFastPFor<4>>();
  } else if (name=="SIMDFastPForDelta1") {
    return std::make_unique<SIMDCompressionLib::SIMDFastPFor<4, SIMDCompressionLib::RegularDeltaSIMD>>();
  } else if (name=="SimpleCodec") {
    return std::make_unique<SimpleCodec>();
  }
}

// input size is in ints, output size is size in bytes
inline void encodeBlock(IntegerCodec& codec, uint32_t* in, uint32_t inSz, char* out, uint32_t &outSz) {
  size_t compressedSize = outSz / sizeof(uint32_t); // this gets changed to the actual size... simdcomp lib uses size in units of words.
  codec.encodeArray(in, inSz, (uint32_t*)out, compressedSize);
  outSz = compressedSize * sizeof(uint32_t);  // convert to bytes
}

// input size is in bytes, output is in ints
inline void decodeBlock(IntegerCodec& codec, char* in, uint32_t inSz, uint32_t* out, uint32_t &outSz) {
  uint64_t recoveredSz = outSz;
  codec.decodeArray( (uint32_t*)in, inSz / sizeof(uint32_t), out, recoveredSz);
  outSz = recoveredSz;
}

static void BM_blockDecode(benchmark::State& state, std::string codecName, bool sorted) {
  // std::cout << "state.range[0]=" << state.range(0) << std::endl;
  Rng rng(1);

  auto codec = getIntegerCodec(codecName);

  std::vector<uint32_t> values;
  std::vector<uint32_t> decoded;
  std::vector<char> encoded;

  uint32_t nvalues = INT_BLOCK_SIZE;
  values.resize(nvalues);
  decoded.resize(nvalues);
  encoded.resize(nvalues*sizeof(uint32_t) * 2);  // may result in SIMDCompressionLib::NotEnoughStorage if not big enough


  for (auto _ : state) {
    state.PauseTiming();
    fillBlock(rng, &values[0], (uint32_t)values.size(), sorted);

    // some codecs modify the input array (calculating deltas in place), so make a copy.
    std::vector<uint32_t> orig(values);

    uint32_t encodedSz = encoded.size();
    encodeBlock(*codec, &values[0], values.size(), &encoded[0], encodedSz);


    state.ResumeTiming();
    uint32_t decodedSz = decoded.size();
    decodeBlock(*codec, &encoded[0], encodedSz, &decoded[0], decodedSz);
    benchmark::DoNotOptimize(&decoded[0]);
    benchmark::ClobberMemory();

    state.PauseTiming();
    ASSERT_EQ(nvalues, decodedSz);
    ASSERT_EQ(orig, decoded);
    // state.ResumeTiming();
  }
};

// TODO: is there a way to get test name and avoid the duplication with codec here?
BENCHMARK_CAPTURE(BM_blockDecode, SimpleCodec, "SimpleCodec", false);
BENCHMARK_CAPTURE(BM_blockDecode, FastPFor, "FastPFor", false); // ->Range(8, 8<<10);
BENCHMARK_CAPTURE(BM_blockDecode, SIMDFastPFor, "SIMDFastPFor", false);
BENCHMARK_CAPTURE(BM_blockDecode, SIMDFastPForDelta1, "SIMDFastPForDelta1", true);


} // end solux
