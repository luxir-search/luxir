// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace luxir {

class LuxirNode;
class PluginRegistry;

// Per-node plugin work. Construction starts it; destruction stops it.
class PluginNodeState {
public:
  virtual ~PluginNodeState() noexcept = default;
};

// Owned by LuxirNode. Every exit destroys states explicitly in reverse order.
class PluginNodeStates {
  std::vector<std::pair<std::string, std::unique_ptr<PluginNodeState>>> states;

public:
  ~PluginNodeStates() { clear(); }
  // Called once, after the node has opened its collections.
  void create(LuxirNode& node, const PluginRegistry& plugins);
  void clear() noexcept;
  PluginNodeState* find(std::string_view name) const;
};

}  // namespace luxir
