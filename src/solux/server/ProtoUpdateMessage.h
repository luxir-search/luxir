#pragma once

#include "solux/index/UpdateMessage.h"
#include "protos/solux.grpc.pb.h"

namespace solux {

class ProtoUpdateMessage : public UpdateMessage {
private:
  // response is created on-demand.
  proto::UpdateResponse* response;  // The response object is created in the same arena as the request.

  void initResponse(proto::UpdateResponse* rsp) {
    rsp->set_request_id(req->request_id());
    rsp->set_status(proto::UpdateResponse::OK);  // default status
  }

public:
  proto::UpdateRequest* req;  // The request object may become unavailable after the callback is called

  ProtoUpdateMessage(proto::UpdateRequest* req, proto::UpdateResponse* rsp=nullptr) : response(rsp), req(req) {
    if (response != nullptr) {
      // A caller-supplied response (unary path) gets the same initialization an
      // on-demand one gets in getResponse().
      initResponse(response);
    }
    if (req->has_commit()) {
      commit = COMMIT;
      const auto& params = req->commit();
      commit_within = params.commit_within_us();
      waitForMerges = params.wait_for_merges();
      buildAuxIndexes.reserve(params.build_aux_indexes_size());
      for (const auto& name : params.build_aux_indexes()) {
        buildAuxIndexes.emplace_back(name);
      }
    } else {
      commit = NO_COMMIT;
    }
  }

  proto::UpdateResponse* getResponse() {
    if (response == nullptr) {
      assert(req->GetArena() != nullptr);
      response = google::protobuf::Arena::Create<proto::UpdateResponse>(req->GetArena());
      initResponse(response);
    }
    return response;
  }

  // Folds the update version and any message-level error (commit pipeline failures,
  // unexpected exceptions caught by the update graph) into the response.  Doc-level
  // errors are already recorded during handle().  Call once processing is complete,
  // typically from done().
  proto::UpdateResponse* finishResponse() {
    auto* rsp = getResponse();
    rsp->set_update_version(updateVersion);
    if (result.errored()) {
      rsp->set_status(proto::UpdateResponse::ERROR);
      rsp->set_error_message(std::string(result.what()));
    }
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

} // namespace solux