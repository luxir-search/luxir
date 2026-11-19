// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include "UpdateSubmission.h"

#include <mutex>
#include <optional>
#include <stdexcept>
#include <utility>

#include "LuxirNode.h"
#include "luxir/store/Directory.h"
#include "luxir/util/log.h"

namespace luxir {

void UpdateSubmission::parseJson(std::string_view json) {
  std::string err;
  if (!api::read_json(request, json, arena, &err)) {
    throw RequestError(err.empty() ? "malformed update request" : err, "invalid_json");
  }
  bytes = (int64_t)json.size();
}

void submitUpdateMessage(LuxirNode& node, std::string_view tenant, std::unique_ptr<ProtoUpdateMessage> message) {
  if (node.readOnly()) throw ReadOnlyError("cannot update collections on a read-only node");
  auto collection = node.getOrCreateCollection(LuxirNode::target(tenant, message->req->collection));
  message->target = collection->getId();
  auto iw = collection->getShard()->requireIndexWriter();
  if (!iw->submitUpdate(message.get())) throw std::runtime_error("update was not admitted");
  // The writer owns it now; it may already be done.
  (void)message.release();
}

namespace {

// One submission from submitUpdate() until its callback has run.  Shared by
// the parked retry and the message, so whichever path ends it delivers; one
// dropped undelivered (a parked retry discarded at node shutdown) delivers
// UNAVAILABLE from its destructor.
struct Pending {
  std::shared_ptr<UpdateSubmissionGate> gate;
  std::unique_ptr<UpdateSubmission> submission;
  UpdateCallback done;
  UpdateAdmissionPolicy policy;

  Pending(std::shared_ptr<UpdateSubmissionGate> gate, std::unique_ptr<UpdateSubmission> submission,
          UpdateCallback done, UpdateAdmissionPolicy policy)
    : gate(std::move(gate)), submission(std::move(submission)), done(std::move(done)), policy(policy) {}
  Pending(const Pending&) = delete;
  Pending& operator=(const Pending&) = delete;
  ~Pending() { fail({ErrorKind::UNAVAILABLE, "node_closing", "the node shut down before the update was submitted"}); }

  // Frees the request before the callback runs: nothing reads it any more.
  void deliver(std::shared_ptr<UpdateResult> result) noexcept {
    auto callback = std::exchange(done, nullptr);
    submission.reset();
    if (!callback) return;
    try {
      callback(std::move(result));
    } catch (const std::exception& e) {
      LOG_WARN("In-process update callback failed: {}", e.what());
    } catch (...) {
      LOG_WARN("In-process update callback failed");
    }
  }

  void fail(const ErrorInfo& info) noexcept {
    if (!done) return;
    std::shared_ptr<UpdateResult> result;
    try {
      result = std::make_shared<UpdateResult>();
      result->arena = std::make_unique<std::pmr::monotonic_buffer_resource>();
      if (submission) result->response.request_id = api::build::arenaStr(*result->arena, submission->request.request_id);
      result->response.status = api::UpdateResponse_::Status::ERROR;
      result->response.error = api::build::arenaError(*result->arena, info);
    } catch (...) {
      LOG_ERROR("In-process update: could not build the error result for: {}", info.message);
      result = nullptr;
    }
    deliver(std::move(result));
  }
};

// The writer's message for a submission.  It borrows the request from the
// pending submission, which outlives it.
class InProcessUpdate final : public ProtoUpdateMessage {
  LuxirNode& node;
  std::shared_ptr<Pending> pending;

public:
  InProcessUpdate(LuxirNode& node, std::shared_ptr<Pending> pending)
    : ProtoUpdateMessage(&pending->submission->request), node(node), pending(std::move(pending)) {}

  void done(IndexWriter&) override {
    auto owner = pending;
    std::shared_ptr<UpdateResult> completed;
    std::optional<ErrorInfo> failure;
    try {
      completed = takeCompletion(node);
    } catch (...) {
      failure = currentExceptionInfo(ErrorKind::INTERNAL);
    }
    auto& at = node;
    delete this;  // releases the admission ticket
    if (failure) { owner->fail(*failure); return; }
    owner->submission.reset();  // the completion copied what it keeps
    try {
      completed->await(at, [owner](std::shared_ptr<UpdateResult> result) { owner->deliver(std::move(result)); });
    } catch (...) {
      owner->deliver(std::move(completed));
    }
  }
};

void attempt(const std::shared_ptr<Pending>& pending) noexcept {
  std::optional<ErrorInfo> failure;
  {
    std::shared_lock lock(pending->gate->mutex);
    LuxirNode* node = pending->gate->node;
    if (node == nullptr) return;  // the destructor reports node_closing
    try {
      auto& admission = node->getUpdateAdmission();
      UpdateAdmission::Ticket ticket;
      if (pending->policy == UpdateAdmissionPolicy::REFUSE) {
        auto admitted = admission.tryAdmit(pending->submission->bytes);
        if (!admitted) {
          throw ApiError(ErrorKind::RESOURCE_EXHAUSTED, "too_many_updates",
                         "the node has too many updates in flight; retry later");
        }
        ticket = std::move(*admitted);
      } else {
        // The resume runs on the releasing thread (often an indexing worker):
        // it only queues the retry.  The task arena outlives every ticket.
        auto& arena = node->getTaskArena();
        auto resume = [&arena, pending] { arena.enqueue([pending] { attempt(pending); }); };
        if (!admission.whenAvailable(std::move(resume))) return;
        ticket = admission.admit(pending->submission->bytes);
      }
      auto message = std::make_unique<InProcessUpdate>(*node, pending);
      message->admission = std::move(ticket);
      submitUpdateMessage(*node, pending->submission->tenant, std::move(message));
    } catch (...) {
      failure = currentExceptionInfo(ErrorKind::INTERNAL);
    }
  }
  // Outside the gate: the callback may submit again.
  if (failure) pending->fail(*failure);
}

}  // namespace

void submitUpdate(LuxirNode& node, std::unique_ptr<UpdateSubmission> submission, UpdateCallback done,
                  UpdateAdmissionPolicy policy) noexcept {
  std::shared_ptr<Pending> pending;
  try {
    pending = std::make_shared<Pending>(node.updateSubmissionGate(), std::move(submission), std::move(done), policy);
  } catch (...) {
    // Nothing was allocated to report with: the callback gets a null result.
    LOG_ERROR("In-process update: out of memory before submission");
    try {
      if (done) done(nullptr);
    } catch (...) {
    }
    return;
  }
  if (!pending->submission) {
    pending->fail(ErrorInfo::of(ErrorKind::INVALID_REQUEST, "no update submission"));
    return;
  }
  attempt(pending);
}

}  // namespace luxir
