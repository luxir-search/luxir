#pragma once

#include <span>
#include <string>
#include "solux/api/solux_types.hpp"

namespace solux {

// Render one solux::api::SearchResponse as a single bespoke JSON object, terminated
// with a newline (one NDJSON line). Columnar DocList and FacetResult values are
// flattened to row-major JSON; missing and non-finite slots render as JSON null.
//
// Shape:
//   {"found": <count>, "docs": [ {<field>: <val>, ...}, ... ],
//    "ops": {<name>: <row-shaped value>, ...},
//    "warnings": [ {"code": ..., "message": ...}, ... ],
//    "profile": {"ops": [...]}, "more": true}
// Optional keys are omitted when absent. The first DocList is promoted to
// found/docs; all remaining response ops stay under ops in response order.
// On engine error:
//   {"error": "<message>"}
std::string renderSearchResponseLine(const solux::api::SearchResponse& resp);

// Build a minimal JSON error body (no trailing newline) for transport-level
// failures (bad route, malformed request) that never reached the engine.
std::string renderErrorBody(std::string_view message);

// Streaming state for one request's docs-format run framing (held by the
// transport's request object; frameDocRun mutates it per run).
struct DocLinesState {
  bool multiOp = false;               // request has more than one DocList op
  std::string_view currentOp;         // op of the last emitted run
  bool anyHeaderEmitted = false;      // request warnings ride on the first header
  std::vector<std::string_view> headeredOps;  // ops whose first run was processed
};

// One DocList op's contribution to a response, rendered as bare NDJSON
// document lines (the response_format=docs shape): one JSON object per
// document, newline terminated, no envelope.  `body` is a pure function of
// the batch - renderDocRuns is stateless and safe to call without
// synchronization; only frameDocRun (below) touches shared framing state.
struct DocRun {
  std::string_view op;
  const solux::api::DocList* docs;
  std::string body;  // rendered document lines (may be empty)
};

// Render every DocList op in `resp` into a DocRun.  Intermediate responses
// carry one op's batch; the final response carries every op's tail.
std::vector<DocRun> renderDocRuns(const solux::api::SearchResponse& resp);

// Decide a run's framing and update `state` (the ONLY stateful step - callers
// with concurrent producers serialize calls, paired with their queue posts).
// Returns false when the run has nothing to emit (no docs, no header
// content).  Otherwise `marker` receives the run's meta record (or stays
// empty when none is needed):
//
// Single-op requests: pure document lines; a marker precedes them only when
// there is content to carry - found (set exactly when the request asked
// get_number) or warnings (degraded execution must not be silent):
//   {"_header_":{"found":N,"warnings":[...]}}
//
// Multi-op requests: ops' outputs may interleave in RUNS (batches are emitted
// as each op's collection completes), and every run is introduced by a meta
// record naming its op - the first run of an op also carries its
// found value:
//   {"_header_":{"op":"q1","found":N}}
// Documents between markers belong to the named op.
//
// The sole-underscore-field shape is the same reserved record class streaming
// ingest treats as a control line, so exported output pipes back into
// /update unchanged.  The caller handles error responses (errors have no
// in-band form in this shape); resp must not carry an error.
bool frameDocRun(const DocRun& run, DocLinesState& state,
                 std::span<const solux::api::Warning> warnings, std::string& marker);

} // namespace solux
