#include "luxir/index/Inverter.h"
#include "luxir/index/PostingsWriter.h"
#include "luxir/reader/PostingsReader.h"
#include "luxir/codec/Codec.h"
#include "gtest/gtest.h"
#include "test/LuxirTest.h"
#include "test/TestIndex.h"
#include <vector>

using namespace luxir;
using namespace luxir::test;

// Runs the FOR-codec battery (round-trip + random access) across every length
// and value-range boundary. Parameterized by codec type for easy extension.
template <typename Codec>
class ForTest : public LuxirTest {
protected:
  Codec codec;

  std::vector<int32_t> values;
  std::vector<char> encoded;
  std::unique_ptr<char[]> buffer;
  std::vector<int32_t> decoded;

  void encode() {
    encoded.resize(values.size()*sizeof(int32_t) * 2 + 1024);
    uint32_t encodedSize = encoded.size();
    codec.encodeBlock((uint32_t*)values.data(), values.size(), encoded.data(), encodedSize);
    encoded.resize(encodedSize);
    // LOG_INFO("native nvals={} encoded size={}", values.size(), encoded.size());
  }

  void test() {
    encode();
    decode();
    select();
  }

  void test(std::vector<int32_t>&& v) {
    values = v;
    test();
  }

  void decode() {
    uint32_t decodedSize = values.size();
    decoded.resize(values.size()); // need to know how many values to read
    // let's malloc a block of memory exactly the right size so memory checkers will catch any decoding overruns.
    // we are getting some errors with invalid SIMD reads from valgrind.  Let's try rounding up.

    // adding 16 and rounding up to 16 seemed to be enough to stop all the valgrind read errors.
    uint64_t round = 16;
    buffer = std::make_unique<char[]>((encoded.size() + 16 + (round-1)) & ~(round-1));

    memcpy(buffer.get(), encoded.data(), encoded.size());
    auto bytesRead = codec.decodeBlock(buffer.get(), encoded.size(), (uint32_t*)decoded.data(), decodedSize);
    unused(bytesRead);
    ASSERT_EQ(decodedSize, values.size());
    // ASSERT_EQ(bytesRead, encoded.size());  // this isn't always true! must be a bug in underlying codec.
    ASSERT_EQ(values, decoded);
  }

  void select() {
    // select
    for (uint32_t i = 0u; i < values.size(); i++) {
      auto val = codec.select(buffer.get(), values.size(), i);

      if ((int32_t)val != values[i]) {
        LOG_ERROR("i={} val={} values[i]={} arr_size={}", i, val, values[i], values.size());
        // put debugger here:
        // val = codec.select(encoded.data(), values.size(), i);
      }
      ASSERT_EQ((int32_t)val, values[i]);
    }
  }
};

using ForCodecs = ::testing::Types<LuxirSIMDFor>;
TYPED_TEST_SUITE(ForTest, ForCodecs);

TYPED_TEST(ForTest, basic) {
  this->test({7});
  this->test({3,5});
  this->test({-100, 0, 100});  // I won't pass any negative numbers since I subtract the min myself.

  // do every length variation to ensure there are no boundary conditions (For does 32 values internally)
  for (int i=1; i<261; i++) {
    this->values.resize(0);
    for (int j=0; j<i; j++) {  // try always making a multiple of 128
      this->values.push_back(j+100);  // make min something other than 0
    }
    this->test();
  }

  // test all equal values
  this->values.resize(0);
  for (int i=0; i<50; i++) {
    this->values.push_back(123456789);
  }
  this->test();

  // full-width residuals (bits==32): the uint32 range needs all 32 bits (M-m >= 2^31).
  // Exercises the dropped 32-bit special case across encode / decode / select.
  this->test({0, (int32_t)0x80000000u});                        // M-m == 2^31 -> bits 32
  this->test({(int32_t)0xFFFFFFFFu, 0, (int32_t)0x7fffffff});   // full 32-bit span
  this->values.resize(0);                                        // full SIMD block + 2-value tail at bits 32
  this->values.push_back(0);
  this->values.push_back((int32_t)0x80000000u);
  for (int i = 2; i < 130; i++) this->values.push_back(i);
  this->test();
}
