#pragma once

#include <memory_resource>
#include <string_view>
#include "solux/api/solux_types.hpp"

namespace solux {

// Parse a bespoke JSON query body into NON-OWNING `out`, whose message data is
// allocated from `arena` (which must outlive `out`).  Fills a single top_docs op
// named "q".  The caller sets `out`'s collection target separately.
//
// Phase 0 grammar:
//   {
//     "query":  {"match": {"<field>": "<value>"}},   // required
//     "limit":  <int>,        "offset":     <int>,   // optional
//     "fields": ["f", ...],   "batch_size": <int>,   // optional
//     "count":  <bool>,       "scores":     <bool>   // optional
//   }
//
// Throws std::runtime_error with a client-facing message on malformed input.
void parseQueryRequest(std::string_view body, solux::api::SearchRequest& out,
                       std::pmr::memory_resource& arena);

} // namespace solux
