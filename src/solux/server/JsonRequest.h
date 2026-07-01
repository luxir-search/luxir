#pragma once

#include <memory_resource>
#include <string_view>
#include "solux/api/solux_types.hpp"

namespace solux {

// Parse an HTTP query body into NON-OWNING `out`, whose message data is allocated
// from `arena` (which must outlive `out`).  The body is the Solux JSON dialect for
// SearchRequest (snake_case keys, untagged Val - see src/solux/api/json_dialect.h),
// e.g. {"ops": {"q": {"top_docs": {"query": {"match": {...}}, "limit": 10}}}}.
// The caller sets `out`'s collection target from the URL path afterwards (a
// body-supplied "collection" is overwritten).  Unknown keys are rejected.
//
// This function stays the seam for endpoint-level envelope sugar (e.g. a root-level
// {"query": ..., "limit": ...} shorthand lowering into a single op) as the surface
// evolves.
//
// Throws std::runtime_error with a client-facing message on malformed input.
void parseQueryRequest(std::string_view body, solux::api::SearchRequest& out,
                       std::pmr::memory_resource& arena);

} // namespace solux
