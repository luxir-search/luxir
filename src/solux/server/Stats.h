#pragma once

#include <memory_resource>

#include "solux/api/solux_types.hpp"

namespace solux {

class SoluxNode;

// Build one typed stats response. The response borrows all repeated/string
// backing from `resource`, which must outlive serialization.
void gatherStats(SoluxNode& node, const api::StatsRequest& request,
                 api::StatsResponse& response, std::pmr::memory_resource& resource);

} // namespace solux
