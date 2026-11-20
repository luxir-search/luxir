// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>
#include "luxir/api/luxir_types.hpp"

namespace luxir {
// Runtime backing for request profiling. Each calculator owns one run with a
// fixed slot per segment; the response wire objects are built only after all
// segment work has drained.
struct ExecutionProfilePieceState {
  luxir::api::ExecutionProfilePiece wire;
  // Human-readable execution notes, one clause per entry; profiling is off
  // the hot path by definition, so ops just push composed strings here and
  // fillExecutionProfile builds the wire span over them.
  std::vector<std::string> details;
  bool complete = false;
};

struct ExecutionProfileRun {
  std::vector<ExecutionProfilePieceState> pieces;

  explicit ExecutionProfileRun(std::size_t numPieces) : pieces(numPieces) {}
};

struct ExecutionProfileOpState {
  std::string name;
  std::mutex mutex;
  std::vector<std::unique_ptr<ExecutionProfileRun>> runs;

  explicit ExecutionProfileOpState(std::string_view name) : name(name) {}
};

// Request-owned sink shared by all query planning contexts, including
// nested domains. Implementations accept concurrent prepare calls.
class ExecutionProfileSink {
public:
  virtual ExecutionProfileRun* addQueryProfile(std::string_view name) = 0;
  virtual ~ExecutionProfileSink() = default;
};
}
