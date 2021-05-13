
#include <gtest/gtest.h>
#include <iostream>
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