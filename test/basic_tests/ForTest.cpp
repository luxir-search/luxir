#include "solux/index/Inverter.h"
#include "solux/index/PostingsWriter.h"
#include "solux/reader/PostingsReader.h"
#include "gtest/gtest.h"
#include "test/SoluxTest.h"
#include "test/TestIndex.h"
#include <vector>

using namespace solux;
using namespace solux::test;

class ForTest : public SoluxTest {
protected:
  // SoluxFor codec;
  SoluxSIMDFor codec;
  // IntegerCODECTypeWrapper<SIMDCompressionLib::SIMDFrameOfReference> codec;

  std::vector<int32_t> values;
  std::vector<char> encoded;
  std::unique_ptr<char[]> buffer;
  std::vector<int32_t> decoded;

  void encode() {
    encoded.resize(values.size()*sizeof(int32_t) * 2 + 1024);
    uint32_t encodedSize = encoded.size();
    codec.encodeBlock((uint32_t*)values.data(), values.size(), encoded.data(), encodedSize);
    encoded.resize(encodedSize);
    LOG_INFO("native nvals={} encoded size={}", values.size(), encoded.size());
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
    ASSERT_EQ(decodedSize, values.size());
    // ASSERT_EQ(bytesRead, encoded.size());  // this isn't always true! must be a bug in underlying codec.
    ASSERT_EQ(values, decoded);
  }

  void select() {
    // select
    for (int i = 0; i < values.size(); i++) {
      auto val = codec.select(buffer.get(), values.size(), i);

      /* for directly testing SIMDFrameOfReference:
      SIMDCompressionLib::SIMDFrameOfReference& c = ((IntegerCODECTypeWrapper<SIMDCompressionLib::SIMDFrameOfReference>*)&codec)->getCodec();
      char* buf = buffer.get();
      // make it easier to step into
      auto val = c.select((uint32_t*)buf, i);
      */

      if (val != values[i]) {
        LOG_ERROR("i={} val={} values[i]={} arr_size={}", i, val, values[i], values.size());
        // put debugger here:
        // val = codec.select(encoded.data(), values.size(), i);
      }
      ASSERT_EQ(val, values[i]);
    }
  }

  template <typename T>  // int64_t vs uint64_t
  int bitWidth(T* values, int nvalues) {
    // test gcd
    auto g = values[0];
    auto min = values[0];
    auto max = values[0];
    for (int i = 0; i < nvalues; i++) {
      LOG_INFO("\t\tvalues[{}]={:x}", i, values[i]);
      g = std::gcd(g, values[i]);
      min = std::min(min, values[i]);
      max = std::max(max, values[i]);
    }
    auto bits = std::bit_width(uint64_t((max - min)/g));
    LOG_INFO("\tgcd={:x} min={:x} max={:x} max-min={:x} max/gcd={:x} min/gcd={:x} (max-min)/gcd={:x} bits={}",
             g, min, max, max-min, max/g, min/g, (max-min)/g, bits);
    return bits;
  }

};


TEST_F(ForTest, basic) {
  std::vector<double> v{1.0, -2.0, 3.0, 4.0, -5.0, 1.3};
  LOG_INFO("bits<uint64_t>={} bits<int64_t>={}", bitWidth((uint64_t*)v.data(), v.size()), bitWidth((int64_t*)v.data(), v.size()));
  // for positive and negative whole numbers (as doubles), we want to calculate in unsigned space (or convert the doubles)
  // But a simple 1.3 blows us out to full 64 bit space.

  // test({255+100,100,101,101,101});
  test({7});
  test({3,5});
  test({-100, 0, 100});  // I won't pass any negative numbers since I subtract the min myself.

  // do every length variation to ensure there are no boundary conditions (For does 32 values internally)
  for (int i=1; i<261; i++) {
    values.resize(0);
    for (int j=0; j<i; j++) {  // try always making a multiple of 128
      values.push_back(j+100);  // make min something other than 0
      // values.push_back((j&255) + 100);  // make everything into a single byte and make it increase by 1 to see what memory layout is like.
    }
    test();
  }

  // test all equal values
  values.resize(0);
  for (int i=0; i<50; i++) {
    values.push_back(123456789);
  }
  test();

}

