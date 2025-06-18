#include <gtest/gtest.h>
#include <iostream>

#include "solux/index/DocStream.h"
#include "solux/index/Inverter.h"
#include "solux/util/TermValHash.h"

using namespace std;
using namespace solux;

TEST(TermValHash, testTypes) {
  // PackedTerm isn't trivial, but it should be trivially copyable
  ASSERT_TRUE(std::is_trivially_copyable<PackedTerm>::value);
  ASSERT_TRUE(std::is_trivially_copyable<TermValRef<PackedTerm>>::value);
  ASSERT_TRUE(std::is_trivially_copyable<TermValRef<DocStream>>::value);  // DocStream may not be trivially copyable, but a TermValRef of anything should be.
  ASSERT_TRUE(std::is_trivially_copyable<TermValRef<DocFreqPosStream>>::value);
}

