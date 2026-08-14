#pragma once

#include <memory>
#include <memory_resource>
#include <limits>
#include <string>

#include "luxir/index/UpdateMessage.h"
#include "luxir/api/build.h"
#include "luxir/util/Clock.h"

namespace luxir {

class ProtoUpdateMessage : public UpdateMessage {
public:
  using RequestProto = luxir::api::UpdateRequest;
  using ResponseProto = luxir::api::UpdateResponse;
  using ResponseStatus = luxir::api::UpdateResponse_::Status;
  using Error = luxir::api::UpdateResponse_::Error;

private:
  // response is created on-demand.
  std::unique_ptr<ResponseProto> ownedResponse;
  ResponseProto* response;
  // The response is NON-OWNING; its message data (request_id, error strings, ids) and the
  // variable-count errors/ids arrays are backed by this monotonic arena. Update processing
  // for one message is single-threaded, so a plain monotonic resource suffices.
  std::pmr::monotonic_buffer_resource mr_;
  luxir::api::build::SpanBuilder<Error> errors_{mr_};
  luxir::api::build::SpanBuilder<std::string_view> ids_{mr_};

  void initResponse(ResponseProto* rsp) {
    rsp->request_id = luxir::api::build::arenaStr(mr_, req->request_id);
    rsp->status = ResponseStatus::OK;  // default status
  }

public:
  // Build-side helpers: errors/ids accumulate at unknown count; finishResponse() seals them
  // into the response spans. Transient strings are copied into the response arena.
  std::pmr::memory_resource& responseArena() { return mr_; }
  Error& addError() { return errors_.emplace_back(); }
  void addId(std::string_view id) { ids_.push_back(luxir::api::build::arenaStr(mr_, id)); }
  void clearIds() { ids_.clear(); }

  const RequestProto* req;  // The request object may become unavailable after the callback is called
  const int64_t dateMathNowEpochMillis;

  ProtoUpdateMessage(const RequestProto* req, ResponseProto* rsp=nullptr)
    : response(rsp), req(req), dateMathNowEpochMillis(currentEpochMillis()) {
    if (response != nullptr) {
      // A caller-supplied response (unary path) gets the same initialization an
      // on-demand one gets in getResponse().
      initResponse(response);
    }
    if (req->commit.has_value()) {
      commit = COMMIT;
      const auto& params = *req->commit;
      commit_within = params.commit_within_us;
      waitForMerges = params.wait_for_merges;
      constexpr uint32_t maxInt32 = (uint32_t)std::numeric_limits<int32_t>::max();
      maxSegments = params.max_segments > maxInt32
                      ? std::numeric_limits<int32_t>::max()
                      : (int32_t)params.max_segments;
      buildAuxIndexes.reserve(params.build_aux_indexes.size());
      for (const auto& name : params.build_aux_indexes) {
        buildAuxIndexes.emplace_back(name);
      }
    } else {
      commit = NO_COMMIT;
    }
  }

  ResponseProto* getResponse() {
    if (response == nullptr) {
      ownedResponse = std::make_unique<ResponseProto>();
      response = ownedResponse.get();
      initResponse(response);
    }
    return response;
  }

  // Folds the update version and any message-level error (commit pipeline failures,
  // unexpected exceptions caught by the update graph) into the response.  Doc-level
  // errors are already recorded during handle().  Call once processing is complete,
  // typically from done().
  ResponseProto* finishResponse() {
    auto* rsp = getResponse();
    rsp->update_version = updateVersion;
    if (result.errored()) {
      rsp->status = ResponseStatus::ERROR;
      rsp->error_message = luxir::api::build::arenaStr(mr_, result.what());
    }
    // Seal the accumulated errors/ids into the non-owning response spans.
    rsp->errors = errors_.finish();
    rsp->ids = ids_.finish();
    return rsp;
  }

  // For now, we will allow the handler to obtain/release an inverter.  We could also optionally pass it
  // as a param in the future if obtain/release becomes more complex.
  virtual void handle(IndexWriter& iw);

  // This defaults to messages allocated with new, but can be overridden to use a pool or other allocator.
  virtual void done(IndexWriter& iw) {
    unused(iw);
    delete this;
  }

  virtual ~ProtoUpdateMessage() = default;
};

} // namespace luxir
