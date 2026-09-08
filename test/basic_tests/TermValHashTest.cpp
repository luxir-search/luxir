// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include <gtest/gtest.h>
#include <iostream>

#include "luxir/index/DocStream.h"
#include "luxir/index/Inverter.h"
#include "luxir/util/TermValHash.h"

using namespace std;
using namespace luxir;

TEST(TermValHash, testTypes) {
  // PackedTerm isn't trivial, but it should be trivially copyable
  ASSERT_TRUE(std::is_trivially_copyable<PackedTerm>::value);
  ASSERT_TRUE(std::is_trivially_copyable<TermValRef<PackedTerm>>::value);
  ASSERT_TRUE(std::is_trivially_copyable<TermValRef<DocStream>>::value);  // DocStream may not be trivially copyable, but a TermValRef of anything should be.
  ASSERT_TRUE(std::is_trivially_copyable<TermValRef<DocFreqPosStream>>::value);
}

