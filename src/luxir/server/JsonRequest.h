// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <memory_resource>
#include <string_view>
#include "luxir/api/luxir_types.hpp"

namespace luxir {

// Parse an HTTP query body into NON-OWNING `out`, whose message data is allocated
// from `arena` (which must outlive `out`).  The body is the Luxir JSON dialect
// (snake_case keys, untagged Val, Match sugar - see src/luxir/api/json_dialect.h)
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
void parseQueryRequest(std::string_view body, luxir::api::SearchRequest& out,
                       std::pmr::memory_resource& arena);

// Apply a root-field JSON overlay to an already parsed request. The unified
// SearchRequest reader reuses an existing top_docs op named q for shorthand
// TopDocs fields and otherwise preserves request-level fields not present here.
void overlayQueryRequest(std::string_view overlay, luxir::api::SearchRequest& out,
                         std::pmr::memory_resource& arena);

} // namespace luxir
