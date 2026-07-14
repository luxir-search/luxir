#pragma once

#include <string>
#include "solux/api/solux_types.hpp"

namespace solux {

// Render one solux::api::SearchResponse as a single bespoke JSON object, terminated
// with a newline (one NDJSON line). Columnar DocList and FacetResult values are
// flattened to row-major JSON; missing and non-finite slots render as JSON null.
//
// Shape:
//   {"found": <matches>, "docs": [ {<field>: <val>, ...}, ... ],
//    "ops": {<name>: <row-shaped value>, ...},
//    "warnings": [ {"code": ..., "message": ...}, ... ], "more": true}
// Optional keys are omitted when absent. The first DocList is promoted to
// found/docs; all remaining response ops stay under ops in response order.
// On engine error:
//   {"error": "<message>"}
std::string renderSearchResponseLine(const solux::api::SearchResponse& resp);

// Build a minimal JSON error body (no trailing newline) for transport-level
// failures (bad route, malformed request) that never reached the engine.
std::string renderErrorBody(std::string_view message);

} // namespace solux
