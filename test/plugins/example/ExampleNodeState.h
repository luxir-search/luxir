// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <atomic>
#include <memory>

#include "luxir/server/LuxirNode.h"

namespace example {

class NodeState final : public luxir::PluginNodeState {
public:
  const luxir::LuxirNode::Mode mode;
  std::atomic<int> queries{0};
  std::atomic<int> ops{0};
  std::atomic<int> routes{0};
  std::shared_ptr<std::atomic<bool>> stopped = std::make_shared<std::atomic<bool>>(false);

  explicit NodeState(luxir::LuxirNode& node) : mode(node.getMode()) {}
  ~NodeState() noexcept override { stopped->store(true); }
};

}  // namespace example
