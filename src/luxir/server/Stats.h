// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <memory_resource>
#include <string_view>

#include "luxir/api/luxir_types.hpp"

namespace luxir {

class LuxirNode;

// Build one typed stats response. The response borrows all repeated/string
// backing from `resource`, which must outlive serialization. Both the JSON and
// the Prometheus renderings are produced from this one response.
void gatherStats(LuxirNode& node, const api::StatsRequest& request,
                 api::StatsResponse& response, std::pmr::memory_resource& resource);

// Read this process's, its data filesystem's and its cgroup's statistics into
// `system`, and host-wide statistics into `host`, from procfs and cgroupfs
// (Linux); either may be null. `dataDirectory` names the filesystem to report;
// empty skips it. Each section lists its collectors as available or
// unavailable. Same borrowing contract as gatherStats.
void gatherSystemStats(api::SystemStats* system, api::HostStats* host, std::string_view dataDirectory,
                       std::pmr::memory_resource& resource);

// Apply the requested filter-cache actions (flush / reset_admission /
// reset_counters) and gather post-action stats plus an optional resident-entry
// dump. Same borrowing contract as gatherStats.
void gatherCacheControl(LuxirNode& node, const api::CacheControlRequest& request,
                        api::CacheControlResponse& response,
                        std::pmr::memory_resource& resource);

} // namespace luxir
