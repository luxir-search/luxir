
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
}

TEST_F(RoaringTest, rank) {
  roaring::Roaring r1;
  r1.add(100);
  r1.add(105);
  r1.add(110);
  r1.add(120);

  std::vector<char> buf(1024);

  auto portableSize = r1.write(buf.data(), true);
  std::cout << "portable size = " << portableSize << std::endl;

  auto otherSz = r1.write(buf.data(), false);
  std::cout << "non-portable size = " << otherSz << std::endl;

  roaring::Roaring r2 = roaring::Roaring::read(buf.data(), false);
  // std::cout << " deserialized cardinality=" << r2.cardinality() << " sizeInBytes=" << r2.getSizeInBytes() << std::endl;
  ASSERT_EQ(4, r2.cardinality());

  auto rank = r1.rank(105);
  ASSERT_EQ(rank, 2);  // 105 appears at position 2 (1-based counting)

  auto iter = r1.begin();
  iter.equalorlarger(110);
  ASSERT_EQ(110, *iter);

  for (auto i : r1) {
    std::cout << "for loop val:" << i << std::endl;
  }

  // iterate example
  r1.iterate(
          [](uint32_t val, void* param){
              std::cout << "val=" << val << std::endl;
              return true;  // return false to stop iterating
            }
          , nullptr // this is passed in for every value as "param"
          );
}