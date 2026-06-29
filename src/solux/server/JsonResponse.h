#pragma once

#include <string>
#include "solux/api/solux_types.hpp"

namespace solux {

// Render one solux::api::SearchResponse as a single bespoke JSON object, terminated
// with a newline (one NDJSON line).  The columnar DocList is flattened to a
// row-major array of objects; a column slot that holds its missing_val sentinel
// (or, for multi-valued columns, an empty list) renders as JSON null.
//
// Shape:
//   {"found": <matches>, "docs": [ {<field>: <val>, ...}, ... ], "more": true}
// or on engine error:
//   {"error": "<message>"}
//
// Phase 0 renders the first response op that carries a DocList (the single
// top_docs query).  Facet / multi-op shaping is deferred.
std::string renderSearchResponseLine(const solux::api::SearchResponse& resp);

// Build a minimal JSON error body (no trailing newline) for transport-level
// failures (bad route, malformed request) that never reached the engine.
std::string renderErrorBody(std::string_view message);

} // namespace solux
