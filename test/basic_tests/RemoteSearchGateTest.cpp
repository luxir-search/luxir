// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include <gtest/gtest.h>

#include <optional>
#include <stop_token>
#include <vector>
#include <string>

#include "luxir/search/RemoteSearchGate.h"

using namespace luxir;

namespace {

// Records which searches started or were cancelled; a started one keeps its slot.
struct Searches {
  std::vector<std::optional<RemoteSearchGate::Slot>> slots{4};
  std::string log;

  void admit(RemoteSearchGate& gate, int id, std::stop_token stop = {}) {
    gate.admit(stop,
        [this, id](RemoteSearchGate::Slot slot) { slots[id] = std::move(slot); log += "start" + std::to_string(id) + " "; },
        [this, id] { log += "cancel" + std::to_string(id) + " "; });
  }
};

}  // namespace

TEST(RemoteSearchGateTest, queuesInOrderAndHonoursCancellation) {
  RemoteSearchGate gate(1);
  Searches searches;
  std::stop_source stop1, stop2;
  searches.admit(gate, 0);
  searches.admit(gate, 1, stop1.get_token());
  searches.admit(gate, 2, stop2.get_token());
  EXPECT_EQ("start0 ", searches.log);
  auto stats = gate.snapshot();
  EXPECT_EQ(1u, stats.running);
  EXPECT_EQ(2u, stats.queued);

  stop1.request_stop();  // leaves the queue without starting
  EXPECT_EQ("start0 cancel1 ", searches.log);
  searches.slots[0].reset();  // the slot passes to the next waiter
  EXPECT_EQ("start0 cancel1 start2 ", searches.log);
  stop2.request_stop();  // already started: nothing to cancel
  searches.slots[2].reset();

  stats = gate.snapshot();
  EXPECT_EQ(0u, stats.running);
  EXPECT_EQ(0u, stats.queued);
  EXPECT_EQ(2u, stats.admitted);
  EXPECT_EQ(1u, stats.waited);
  EXPECT_EQ(1u, stats.cancelled);

  // A stop requested before the search queues cancels it at once.
  std::stop_source stopped;
  stopped.request_stop();
  searches.admit(gate, 0);
  searches.admit(gate, 3, stopped.get_token());
  EXPECT_EQ("start0 cancel1 start2 start0 cancel3 ", searches.log);
  EXPECT_EQ(0u, gate.snapshot().queued);
}
