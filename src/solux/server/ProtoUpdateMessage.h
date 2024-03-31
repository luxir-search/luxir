#pragma once

#include "solux/index/IndexWriter.h"
#include "solux/schema/Schema.h"
#include "protos/solux.grpc.pb.h"

namespace solux {

class ProtoUpdateMessage : public UpdateMessage {
public:
  solux::proto::UpdateRequest* req;  // The request object may become unavailable after the callback is called

  // 0 means no commit.  -1 means immediate commit.  Other values are commit-within milliseconds.
  virtual int32_t commitWithin() override {
    return -1; // TODO: FIXME: this should not be hard-coded.
    // return req->commit_within();
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