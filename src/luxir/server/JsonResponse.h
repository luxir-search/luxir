// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <span>
#include <string>
#include "luxir/api/luxir_types.hpp"
#include "luxir/util/ApiError.h"

namespace luxir {

// Render one luxir::api::SearchResponse as a compact JSON object without a trailing
// newline; the HTTP transport owns framing. Columnar DocList and FacetResult values are
// flattened to row-major JSON; missing and non-finite slots render as JSON null.
//
// Shape:
//   {"request_id": "...", "found": <count>, "docs": [ {<field>: <val>, ...}, ... ],
//    "ops": {<name>: <row-shaped value>, ...},
//    "warnings": [ {"code": ..., "message": ...}, ... ],
//    "profile": {"ops": [...]}, "more": true}
// Optional keys are omitted when absent (request_id when the request set
// none). The first DocList is promoted to found/docs; all remaining response
// ops stay under ops in response order.  On engine error:
//   {"request_id": "...", "error": {"kind": ..., "code": ..., "message": ...},
//    "warnings": [...]}
std::string renderSearchResponseBody(const luxir::api::SearchResponse& resp);

// The JSON error body (no trailing newline) for a failure answered with an
// HTTP error status - the same {request_id, error} shape as an in-band error
// line, restricted to those keys.  requestId is omitted when empty.
std::string renderErrorBody(const ErrorInfo& info, std::string_view requestId);

// Streaming state for one request's docs-format run framing (held by the
// transport's request object; frameDocRun mutates it per run).
struct DocLinesState {
  bool multiOp = false;               // request has more than one DocList op
  std::string_view currentOp;         // op of the last emitted run
  std::string_view requestId;         // echoed on the first header when set
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
  const luxir::api::DocList* docs;
  std::string body;  // rendered document lines (may be empty)
};

// Render every DocList op in `resp` into a DocRun.  Intermediate responses
// carry one op's batch; the final response carries every op's tail.
std::vector<DocRun> renderDocRuns(const luxir::api::SearchResponse& resp);

// Decide a run's framing and update `state` (the ONLY stateful step - callers
// with concurrent producers serialize calls, paired with their queue posts).
// Returns false when the run has nothing to emit (no docs, no header
// content).  Otherwise `marker` receives the run's meta record (or stays
// empty when none is needed):
//
// Single-op requests: pure document lines; a marker precedes them only when
// there is content to carry - found (set exactly when the request asked
// get_number), warnings (degraded execution must not be silent), or the
// request_id (correlation must not be silent either):
//   {"_header_":{"request_id":"...","found":N,"warnings":[...]}}
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
                 std::span<const luxir::api::Warning> warnings, std::string& marker);

} // namespace luxir
