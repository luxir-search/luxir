// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include <gtest/gtest.h>

#include <limits>
#include <stdexcept>
#include <string>

#include "luxir/search/RequestMemTracker.h"

using namespace luxir;

TEST(RequestMemTrackerTest, chargesReleasesAndNamesRejectedCharge) {
  RequestMemTracker tracker(10);
  tracker.charge(6, "sort state", "op 'products'");
  EXPECT_EQ(6u, tracker.bytes());
  EXPECT_EQ(10u, tracker.ceiling());

  try {
    tracker.charge(5, "sort state", "op 'products'");
    FAIL() << "expected request-memory charge to fail";
  } catch (const std::runtime_error& error) {
    std::string message = error.what();
    EXPECT_NE(message.find("request memory breaker 'sort state'"),
              std::string::npos);
    EXPECT_NE(message.find("op 'products'"), std::string::npos);
    EXPECT_NE(message.find("attempted total 11 bytes"), std::string::npos);
    EXPECT_NE(message.find("ceiling of 10 bytes"), std::string::npos);
  }
  EXPECT_EQ(6u, tracker.bytes());
  tracker.release(6);
  EXPECT_EQ(0u, tracker.bytes());
}

TEST(RequestMemTrackerTest, partialChargePreservesReservationSemantics) {
  RequestMemTracker tracker(10);
  tracker.charge(7, "test", "initial");
  EXPECT_EQ(3u, tracker.chargeUpTo(5, 2, "test", "tail"));
  EXPECT_EQ(10u, tracker.bytes());
  tracker.release(10);
}

TEST(RequestMemTrackerTest, nonThrowingChargeLeavesDeniedReservationAlone) {
  RequestMemTracker tracker(10);
  tracker.charge(6, "test", "initial");
  EXPECT_FALSE(tracker.tryCharge(5));
  EXPECT_EQ(6u, tracker.bytes());
  EXPECT_TRUE(tracker.tryCharge(4));
  EXPECT_EQ(10u, tracker.bytes());
  tracker.release(10);
}

TEST(RequestMemTrackerTest, unlimitedTrackerStillRejectsCounterOverflow) {
  RequestMemTracker tracker(0);
  constexpr size_t MAX = std::numeric_limits<size_t>::max();
  tracker.charge(MAX, "test", "overflow case");
  try {
    tracker.charge(1, "test", "overflow case");
    FAIL() << "expected request-memory counter overflow to fail";
  } catch (const std::runtime_error& error) {
    std::string message = error.what();
    EXPECT_NE(message.find("request memory breaker 'test'"),
              std::string::npos);
    EXPECT_NE(message.find("overflow case"), std::string::npos);
    EXPECT_NE(message.find("attempted total exceeds"), std::string::npos);
    EXPECT_NE(message.find("ceiling is unlimited"), std::string::npos);
  }
  EXPECT_EQ(MAX, tracker.bytes());
  tracker.release(MAX);
}
