#pragma once

#include <memory_resource>

#include "luxir/api/luxir_types.hpp"

namespace luxir {

class LuxirNode;

// Build one typed stats response. The response borrows all repeated/string
// backing from `resource`, which must outlive serialization.
void gatherStats(LuxirNode& node, const api::StatsRequest& request,
                 api::StatsResponse& response, std::pmr::memory_resource& resource);

} // namespace luxir
