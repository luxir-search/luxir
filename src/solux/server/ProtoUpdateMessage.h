#pragma once

#include "solux/index/UpdateMessage.h"
#include "protos/solux.grpc.pb.h"

namespace solux {

class ProtoUpdateMessage : public UpdateMessage {
public:
  solux::proto::UpdateRequest* req;  // The request object may become unavailable after the callback is called
  ProtoUpdateMessage(solux::proto::UpdateRequest* req) : req(req) {
    commit = static_cast<CommitType>(req->commit());
    commit_within = req->commit_within_us();
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