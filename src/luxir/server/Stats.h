// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <memory_resource>

#include "luxir/api/luxir_types.hpp"

namespace luxir {

class LuxirNode;

// Build one typed stats response. The response borrows all repeated/string
// backing from `resource`, which must outlive serialization.
void gatherStats(LuxirNode& node, const api::StatsRequest& request,
                 api::StatsResponse& response, std::pmr::memory_resource& resource);

// Apply the requested filter-cache actions (flush / reset_admission /
// reset_counters) and gather post-action stats plus an optional resident-entry
// dump. Same borrowing contract as gatherStats.
void gatherCacheControl(LuxirNode& node, const api::CacheControlRequest& request,
                        api::CacheControlResponse& response,
                        std::pmr::memory_resource& resource);

} // namespace luxir
