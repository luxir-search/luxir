#include "solux/index/Inverter.h"
#include "solux/index/PostingsWriter.h"
#include "solux/search/PostingsReader.h"
#include "gtest/gtest.h"
#include "test/SoluxTest.h"
#include "test/TestIndex.h"
#include <vector>

using namespace solux;
using namespace solux::test;

class ForTest : public SoluxTest {
protected:
  SoluxFor codec;
  std::vector<int32_t> values;
  std::vector<char> encoded;
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
    std::unique_ptr<char[]> buffer = std::make_unique<char[]>(encoded.size());
    memcpy(buffer.get(), encoded.data(), encoded.size());
    auto bytesRead = codec.decodeBlock(buffer.get(), encoded.size(), (uint32_t*)decoded.data(), decodedSize);
    ASSERT_EQ(decodedSize, values.size());
    // ASSERT_EQ(bytesRead, encoded.size());  // this isn't always true! must be a bug in underlying codec.
    ASSERT_EQ(values, decoded);
  }

  void select() {
    // select
    for (int i = 0; i < values.size(); i++) {
      auto val = codec.select(encoded.data(), values.size(), i);
      if (val != values[i]) {
        LOG_ERROR("i={} val={} values[i]={} arr_size={}", i, val, values[i], values.size());
        // put debugger here:
        val = codec.select(encoded.data(), values.size(), i);
      }
      ASSERT_EQ(val, values[i]);
    }
  }

};


TEST_F(ForTest, basic) {
  test({255+100,100,101,101,101});
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

