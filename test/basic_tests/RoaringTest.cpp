
#include <gtest/gtest.h>
#include <iostream>
#include <vector>
#include "test/SoluxTest.h"
#include "roaring.hh"

using namespace solux;

// For testing our correct use of roaring bitmaps, and any modifications/extensions to CRoaring
class RoaringTest : public solux::SoluxTest {
public:
};

TEST_F(RoaringTest, basic) {
  roaring::Roaring r1;
  uint32_t i=0;
  int count = 0;
  while (i<2000000000) {
    i += (rng() & 0x0fff) + 1;  // 2000000000 / (0xfff / 2) ~= 977K bits set
    r1.add(i);
    count++;
  }

  ASSERT_EQ(count, r1.cardinality());

  /* TODO: roaring isn't good for all (or mostly all) values! Seems worse than raw bitset!!!
   * either come up with our own format that can handle that case, or store things in the negative sense.
   * We want to optimize for speed in dense case, and optimize for size in the sparse case.
  std::cout << "cardinality="  << count << "size1=" << r1.getFrozenSizeInBytes() << std::endl;
  roaring::Roaring r;
  for (int i=0; i<1000000; i++) {
    r.add(i);
  }
  std::cout << "1M all set size="  << r.getFrozenSizeInBytes() << std::endl;  // output: 1M all set size=131156

  roaring::Roaring r2;
  r2.add(1000000-1);
  std::cout << "1 high bit set size="  << r2.getFrozenSizeInBytes() << std::endl;  // output: 1 high bit set size=11
  */

}

TEST_F(RoaringTest, rank) {
  uint32_t values[] = {100, 105, 110, 120};
  roaring::Roaring r1;
  for (auto v : values) {
    r1.add(v);
  }
  r1.add(100);
  r1.add(105);
  r1.add(110);
  r1.add(120);

  auto frozenSize = r1.getFrozenSizeInBytes();
  auto bufSize = frozenSize + 32; // need space to align
  std::vector<char> buf(bufSize);
  void* buffer = buf.data();
  buffer = std::align(32, frozenSize, buffer, bufSize);

  // std::cout << "getSizeInBytes=" << r1.getSizeInBytes(true) << " non-portable=" << r1.getSizeInBytes(false) << " frozen=" << r1.getFrozenSizeInBytes() << std::endl;

  r1.writeFrozen((char*)buffer);

  // roaring::Roaring r2 = roaring::Roaring::read(buf.data(), false);
  roaring::Roaring r2 = roaring::Roaring::frozenView((char*)buffer, frozenSize);

  // std::cout << " deserialized cardinality=" << r2.cardinality() << " sizeInBytes=" << r2.getSizeInBytes() << std::endl;
  ASSERT_EQ(4, r2.cardinality());

  auto rank = r1.rank(105);
  ASSERT_EQ(rank, 2);  // 105 appears at position 2 (1-based counting)

  auto iter = r1.begin();
  iter.equalorlarger(110);
  ASSERT_EQ(110, *iter);

  int idx = 0;
  for (auto i : r1) {
    ASSERT_EQ(i, values[idx++]);
    // std::cout << "for loop val:" << i << std::endl;
  }


  // iterate example
  r1.iterate(
          [](uint32_t val, void* param){
              unused(val,param);
              // can't assert correct values here since lambda with captures can't be converted to function pointer
              // std::cout << "val=" << val << std::endl;
              return true;  // return false to stop iterating
            }
          , nullptr // this is passed in for every value as "param"
          );

}