#pragma once

#include <string_view>
#include "protos/solux_types.pb.h"

namespace solux {

// Parse a bespoke JSON query body into `out` (whose collection target the caller
// has already set).  Fills a single top_docs op named "q".
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
void parseQueryRequest(std::string_view body, proto::SearchRequest& out);

} // namespace solux
