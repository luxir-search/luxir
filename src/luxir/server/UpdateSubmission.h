// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <memory_resource>
#include <shared_mutex>
#include <string>
#include <string_view>

#include "ProtoUpdateMessage.h"

namespace luxir {

class LuxirNode;

// An update built in process - by a plugin, a background job, an embedding -
// for submitUpdate() below.  It owns what the request borrows until the
// update completes: build the request's strings and arrays in `arena`, and
// put anything else it views (a buffer it was decoded from) in `borrowed`.
struct UpdateSubmission {
  std::pmr::monotonic_buffer_resource arena;
  api::UpdateRequest request;
  // The request's tenant; empty is the default tenant.  The collection is
  // request.collection, as for a client (empty: the default collection).
  std::string tenant;
  // The raw size UpdateAdmission charges.  parseJson() sets it to the body's
  // size; a request built field by field should set an estimate.
  int64_t bytes = 0;
  std::shared_ptr<const void> borrowed;

  // Decodes an UpdateRequest JSON body (the /update body) into `request`,
  // copying its strings into `arena`, and charges its size.  Throws
  // RequestError on malformed JSON.
  void parseJson(std::string_view json);
};

// The finished update: `response` is the UpdateResponse a client would get
// (status OK / PARTIAL / ERROR, document errors, ids, commit, replicas),
// backed by the result's own arena.
using UpdateResult = ProtoUpdateMessage::Completion;
// Runs exactly once per submission, on whichever thread finished it (an
// indexing worker, a commit-wait thread, the submitting thread when refused
// up front, or the node's destructor for an update it drops unsubmitted).
// Keep it short and non-blocking; hand longer work to a task arena.  It may
// outlive the plugin's node state, so capture shared ownership, not the state.
using UpdateCallback = std::function<void(std::shared_ptr<UpdateResult>)>;

enum class UpdateAdmissionPolicy {
  // Over the node's UpdateAdmission limit the update parks, holding no
  // thread, and is submitted once a slot frees (overshooting the byte limit
  // by at most this update, as a stream's next message does).
  WAIT,
  // Over the limit the update completes at once with RESOURCE_EXHAUSTED
  // (code too_many_updates), as a unary client update is refused.
  REFUSE,
};

// Submits an in-process update through the path a client update takes:
// node-wide UpdateAdmission, read-only and unavailable-collection checks,
// target resolution (tenant + collection, auto-creating as configured) and
// the collection's writer, whose handling counts the batch in the node and
// collection metrics.  Every outcome - refused before submission, a closed
// writer, document errors, success - reaches `done` exactly once, as an
// UpdateResult: a failure before indexing has status ERROR and `error` set
// (null only when even that could not be allocated).
// A commit with wait_for_replicas completes after that wait.  Never throws.
void submitUpdate(LuxirNode& node, std::unique_ptr<UpdateSubmission> submission, UpdateCallback done,
                  UpdateAdmissionPolicy policy = UpdateAdmissionPolicy::WAIT) noexcept;

// The transports' last step, shared with submitUpdate(): resolves the
// message's target collection in `tenant`, refuses a read-only node or a
// collection without a writer, records the target and hands the message to
// the writer.  The message must already carry its admission ticket.  On
// failure the message is destroyed (releasing the ticket) and the error
// rethrown; on success the writer owns it until done().
void submitUpdateMessage(LuxirNode& node, std::string_view tenant, std::unique_ptr<ProtoUpdateMessage> message);

// Node-owned: lets a parked submission's retry, which runs after its slot
// frees, know whether the node still takes updates.  The node clears `node`
// as it shuts down; a retry holds the shared lock while it submits.
struct UpdateSubmissionGate {
  std::shared_mutex mutex;
  LuxirNode* node;
  explicit UpdateSubmissionGate(LuxirNode* node) : node(node) {}
};

}  // namespace luxir
