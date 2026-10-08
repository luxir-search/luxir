// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include <vector>

#include "test/LuxirTest.h"
#include "luxir/reader/TermsEnum.h"
#include "luxir/util/ChunkedArray.h"
#include "luxir/util/proto.h"

using namespace luxir;

namespace {
using State = TermsEnum::PostingsState;
using States = ChunkedArray<State>;
static_assert(std::is_trivially_destructible_v<State>);
static_assert(std::is_trivially_copyable_v<State>);
static_assert(std::is_trivially_destructible_v<States>);
static_assert(std::is_trivially_destructible_v<States::View>);
static_assert(std::forward_iterator<States::View::Iterator>);
}

TEST(ChunkedArrayTest, sizesAndIteration) {
  for (size_t size : {0u, 1u, 8u, 9u, 511u, 512u, 513u, 2065u}) {
    SCOPED_TRACE(size);
    google::protobuf::Arena arena;
    ArenaResource resource(&arena);
    States::View view;
    {
      States states(resource, size);
      for (size_t i = 0; i < size; ++i) {
        State state;
        state.termOrdinal = (int64_t)i;
        state.docFreq = (int32_t)i + 1;
        states.push_back(state);
      }
      EXPECT_EQ(size, states.size());
      view = states.view();
    }
    EXPECT_EQ(size, view.size());
    EXPECT_EQ(size == 0, view.empty());
    for (size_t i = 0; i < size; ++i) {
      size_t index = (i * 37) % size;
      EXPECT_EQ((int64_t)index, view[index].termOrdinal);
      EXPECT_EQ((int32_t)index + 1, view[index].docFreq);
    }
    size_t index = 0;
    for (const auto& state : view) {
      EXPECT_EQ((int64_t)index++, state.termOrdinal);
    }
    EXPECT_EQ(size, index);
  }
}

TEST(ChunkedArrayTest, filledChunksStayInPlace) {
  google::protobuf::Arena arena;
  ArenaResource resource(&arena);
  States states(resource, 4097);
  std::vector<const State*> starts;
  for (size_t i = 0; i < 4097; ++i) {
    State state;
    state.termOrdinal = (int64_t)i;
    states.push_back(state);
    if ((i + 1) % States::CHUNK_SIZE == 0) {
      starts.push_back(&states.view()[i + 1 - States::CHUNK_SIZE]);
    }
    for (size_t chunk = 0; chunk < starts.size(); ++chunk) {
      EXPECT_EQ(starts[chunk], &states.view()[chunk * States::CHUNK_SIZE]);
      EXPECT_EQ((int64_t)(chunk * States::CHUNK_SIZE), starts[chunk]->termOrdinal);
    }
  }
}

TEST(ChunkedArrayTest, arenaAllocationBounds) {
  google::protobuf::Arena arena;
  ArenaResource resource(&arena);
  size_t before = arena.SpaceUsed();
  States states(resource, 32u * 1024 * 1024 / sizeof(State));
  EXPECT_EQ(before, arena.SpaceUsed());
  states.push_back(State{});
  EXPECT_LE(arena.SpaceUsed() - before, 8 * sizeof(State) + 64);
  for (size_t i = 1; i < 513; ++i) states.push_back(State{});
  // The abandoned first-chunk buffers total fewer than 512 states. Only
  // one additional chunk and a tiny pointer directory have been allocated.
  EXPECT_LE(arena.SpaceUsed() - before, 3 * 512 * sizeof(State) + 64);
}

TEST(ChunkedArrayTest, reservedFillNeverCopies) {
  for (size_t size : {0u, 1u, 9u, 511u, 512u, 513u, 2065u}) {
    SCOPED_TRACE(size);
    google::protobuf::Arena arena;
    ArenaResource resource(&arena);
    size_t before = arena.SpaceUsed();
    States states(resource, size);
    states.reserve(size);
    EXPECT_EQ(0u, states.size());
    std::vector<const State*> starts;
    for (size_t i = 0; i < size; ++i) {
      State state;
      state.termOrdinal = (int64_t)i;
      states.push_back(state);
      if (i % States::CHUNK_SIZE == 0) starts.push_back(&states.view()[i]);
      for (size_t chunk = 0; chunk < starts.size(); ++chunk) {
        EXPECT_EQ(starts[chunk], &states.view()[chunk * States::CHUNK_SIZE]);
      }
    }
    size_t index = 0;
    for (const auto& state : states.view()) {
      EXPECT_EQ((int64_t)index++, state.termOrdinal);
    }
    EXPECT_EQ(size, index);
    // Only the states and one exact-size directory, with no abandoned buffers.
    EXPECT_EQ(size * sizeof(State) + starts.size() * sizeof(State*),
              arena.SpaceUsed() - before);
  }
}
