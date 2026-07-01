#pragma once

#include <memory_resource>
#include <string_view>
#include "solux/api/solux_types.hpp"

namespace solux {

// Parse an HTTP query body into NON-OWNING `out`, whose message data is allocated
// from `arena` (which must outlive `out`).  The body is the Solux JSON dialect
// (snake_case keys, untagged Val, Match sugar - see src/solux/api/json_dialect.h)
// in one of two forms:
//
//   shorthand - the root object is a single top_docs op (becomes ops["q"]):
//     {"query": {"match": {"title_w": "dune"}}, "limit": 10, "fields": ["id"]}
//   full      - a SearchRequest, for multiple named ops / request-level fields:
//     {"ops": {"q": {"top_docs": {...}}, "facet": {...}}}
//
// A body whose only root key is "ops" is taken as the full form (sub-ops-only
// shorthand must spell out top_docs).  The caller sets `out`'s collection target
// from the URL path afterwards (a body-supplied "collection" is overwritten).
// Unknown keys are rejected.
//
// Throws std::runtime_error with a client-facing message on malformed input.
void parseQueryRequest(std::string_view body, solux::api::SearchRequest& out,
                       std::pmr::memory_resource& arena);

} // namespace solux
