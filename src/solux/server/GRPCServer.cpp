
#include <string>
#include <algorithm>
#include <grpcpp/grpcpp.h>
#include <google/protobuf/text_format.h>
#include <grpcpp/health_check_service_interface.h>
#include <grpcpp/ext/proto_server_reflection_plugin.h>
#include "GRPCServer.h"
#include "SoluxNode.h"
#include "solux/index/IndexWriter.h"
#include "solux/util/solux_util.h"
#include "solux/util/TaggedPtr.h"


using namespace solux;

// TODO - use a different logger for RPC stuff some point
// redefine DEBUG to TRACE level whish shouldn't currently be logged!
#define GRPC_DEBUG LOG_TRACE

GRPCServer::GRPCServer(int nthreads)
  : startLatch(1), startLatchThreads(nthreads), nthreads(nthreads) {
}

// NOTE: as of gRPC 1.39 there is a new C++ async callback API: https://github.com/grpc/grpc/pull/25728 in addition to an EventEngine
// interface that may help with integration with external event loops.
// adapted from the grpc helloworld example
void solux::GRPCServer::run() {
  std::string server_address("0.0.0.0:50051");

  grpc::EnableDefaultHealthCheckService(true);
  grpc::reflection::InitProtoReflectionServerBuilderPlugin();
  grpc::ServerBuilder builder;
  // Listen on the given address without any authentication mechanism.
  builder.AddListeningPort(server_address, grpc::InsecureServerCredentials());
  // Register "service" as the instance through which we'll communicate with
  // clients. In this case it corresponds to a *synchronous* service.

  builder.RegisterService(&greeterService);
  builder.RegisterService(&indexerService);
  builder.RegisterService(&searcherService);

  threads.reserve(nthreads);
  threadInfos.reserve(nthreads);

  // Give each thread a completion queue, but don't let them use it
  // before the server starts (implemented with startLatch)
  for (int i=0; i<nthreads; i++) {
    threadInfos.emplace_back();
    threadInfos.back().threadno = i;
    threadInfos.back().cq = builder.AddCompletionQueue(); // each thread gets it's own completion queue
    threads.emplace_back([this,i]{ this->runThread(threadInfos[i]); });
  }

  this->server = builder.BuildAndStart();
  GRPC_DEBUG("GRPCServer listening on {}", server_address);

  // inform everyone that the server is up and running
  startLatch.count_down();
  // At this point the completion queues have all been created, but runThread() may not have continued (hence we have
  // not requested to the completion queue to handle certain messages.) If a client request comes in, it will still
  // wait to be handled (and then will be handled correctly.)  This was tested manually by adding a long sleep
  // in runThread() before requesting calls.

  // Wait for the server to shutdown. Note that some other thread must be
  // responsible for shutting down the server for this call to ever return.
  server->Wait();
  GRPC_DEBUG("server->Wait() returned!");
}


class CallData {
public:
  // The tagged pointer is to enable adding extra correlation information outside of the internal state machine
  // of each CallData.  Useful for handling things like AsyncNotifyWhenDone so you can tell what operation
  // happened on this CallData.
  using TaggedPtrType = TaggedPtr<CallData>;

  void* make_tag(uint32_t tag=0) {
    return TaggedPtrType(this, tag).packedBitsAsVoid();
  }

  CallData(GRPCServer& server, GRPCServer::ThreadInfo& threadInfo) : server(server), threadInfo(threadInfo) {
  }

  virtual ~CallData() = default;

  // The GRPC "tag" is a tagged pointer to this.  The pointer is unwrapped and proceed is called,
  // also passing the tag on "this".
  virtual void proceed(bool ok, uint32_t tag) = 0;

  GRPCServer& server;
  GRPCServer::ThreadInfo& threadInfo;

  grpc::ServerContext ctx;
  // TODO: ServerContext objects should not be used across different RPC calls (but does this apply to multiple messages in a streaming RPC???)
};


// TODO: make a template class for request/response
template <class RequestT, class ResponseT, class AsyncServiceT>
class UnaryCallData : public CallData {
public:
  RequestT request;
  ResponseT response;
  AsyncServiceT& service;
  grpc::ServerAsyncResponseWriter<ResponseT> responder;
  enum CallStatus { CREATE, PROCESS, FINISH };
  CallStatus state;

  UnaryCallData(GRPCServer& server, AsyncServiceT& service, GRPCServer::ThreadInfo& threadInfo) : CallData(server, threadInfo), service(service), responder(&ctx) {
    // TODO: how to do this in a generic way?  I would need to get the index of the method and then call
    // ::grpc::Service::RequestAsyncUnary(0, context, request, response, new_call_cq, notification_cq, tag);
    // service.RequestSayHello(&ctx, &request, &responder, &cq, &cq, (void*)this);
    state = PROCESS;
  }

  virtual void proceed(bool ok, uint32_t tag) override {
    // std::cout << "UnaryCallData.proceed(" << ok << ") this=" << (void*)this << std::endl;
    if (!ok) {
      // canceled/errored... nothing else to do.
      // std::cout << "deleting " << (void*)this << std::endl;
      delete this;
      return;
    }

    if (state == PROCESS) {
      threadInfo.requests++;

      // create a new instance of this to handle additional calls.
      createNew();

      // The actual processing.
      fillResponse();

      state = FINISH;
      responder.Finish(response, grpc::Status::OK, make_tag());
      //std::cout << "finish called," << counter++ << std::endl;
    } else {
      // nothing left to do but delete ourselves
      // std::cout << "deleting " << (void*)this << std::endl;
      delete this;
    }

  }

  virtual void createNew() = 0;
  virtual void fillResponse() = 0;
};


template <class RequestT, class ResponseT, class AsyncServiceT>
class StreamingCallData : public CallData {
public:
  RequestT request;
  ResponseT response;
  AsyncServiceT& service;
  grpc::ServerAsyncReaderWriter<ResponseT,RequestT> readerWriter;
  enum CallStatus { READ = 1, WRITE = 2, CONNECT = 3, FINISH = 5 };

  CallStatus state;
  int numCalls = 0; // TODO: testing only... remove after stable.

  StreamingCallData(GRPCServer& server, AsyncServiceT& service, GRPCServer::ThreadInfo& threadInfo) : CallData(server, threadInfo), service(service), readerWriter(&ctx) {
    GRPC_DEBUG("CREATE StreamingCallData this={}", (void*)this);

    // see https://stackoverflow.com/questions/60856240/grpc-c-async-server-how-differentiate-between-writesdone-and-broken-connection
    // TODO: not sure the right way to use this event yet.
    ctx.AsyncNotifyWhenDone(make_tag(1));
    state = CONNECT;
  }

  virtual void proceed(bool ok, uint32_t tag) override {
    // TODO: include testing for multiple threads calling back to this object (can that even happen?)
    GRPC_DEBUG("PROCEED this={} state={} ok={} numCalls={} tag={} cancelled={}", (void*)this, state, ok, ++numCalls, tag, ctx.IsCancelled());

    if (tag == 1) {
      // result of AsyncNotifyWhenDone
      // TODO: how should we make use of this?
      return;
    }


    if (!ok && state != CallStatus::READ) {
      // canceled/errored... nothing else to do.
      GRPC_DEBUG("Error or shutting down. deleting this={}", (void*)this);
      delete this;
      return;
    }


    switch (state) {
      case CallStatus::READ:

        if (!ok) {
          // Client ended the stream.
          // TODO: ok==false when we are shutting down as well.... how to tell the difference?  I guess Finish will
          // error out (or complete) if we're shutting down???
          GRPC_DEBUG("End of stream!");
          grpc::Status st(grpc::StatusCode::OK,"is OK message used/passed?");

          // TODO: only finish *after* writing all necessary responses!
          readerWriter.Finish(st,make_tag());
          state = CallStatus::FINISH;
          GRPC_DEBUG("After calling Finish. this={} state={} ok={} numCalls={} tag={} cancelled={}", (void*)this, state, ok, numCalls, tag, ctx.IsCancelled());
          break;
        }


#ifdef GRPC_DEBUG
        {
          std::string reqStr;
          google::protobuf::TextFormat::PrintToString(request, &reqStr);  // TODO: figure out something better for this
          GRPC_DEBUG("Server read new streaming message:({})", reqStr);
        }
#endif

        fillResponse();

        readerWriter.Write(response, make_tag());

        // TODO: we should be able to do multiple reads before the first write if we want.
        state = CallStatus::WRITE;
        break;

      case CallStatus::WRITE:
        readerWriter.Read(&request, make_tag());
        state = CallStatus::READ;
        break;

      case CallStatus::CONNECT:
        createNew();
        readerWriter.Read(&request, make_tag());
        state = CallStatus::READ;
        break;

      case CallStatus::FINISH:
        delete this;
        break;

      default:
        LOG_ERROR("Unexpected state {}", state);
        assert(false);
    }
  }

  virtual void createNew() = 0;
  virtual void fillResponse() = 0;
};

class SayHelloStreamingCall : public StreamingCallData<HelloRequest, HelloReply, Greeter::AsyncService> {
public:
  SayHelloStreamingCall(GRPCServer& server, Greeter::AsyncService& service, GRPCServer::ThreadInfo& threadInfo) : StreamingCallData(server, service, threadInfo) {
    //     void RequestSayHelloStreaming(::grpc::ServerContext* context, ::grpc::ServerAsyncReaderWriter< ::solux::HelloReply, ::solux::HelloRequest>* stream, ::grpc::CompletionQueue* new_call_cq, ::grpc::ServerCompletionQueue* notification_cq, void *tag) {
    service.RequestSayHelloStreaming(&ctx, &readerWriter, threadInfo.cq.get(), threadInfo.cq.get(), make_tag());
  }

  virtual void createNew() override {
    new SayHelloStreamingCall(server, service, threadInfo);
  }
  virtual void fillResponse() override {
    std::string prefix("HelloStreaming ");
    response.set_message(prefix + request.name());
    // std::cout << "req name:" << request.name() << std::endl;
  }
};

class SayHelloCall : public UnaryCallData<HelloRequest, HelloReply, Greeter::AsyncService> {
public:
  SayHelloCall(GRPCServer& server, Greeter::AsyncService& service, GRPCServer::ThreadInfo& threadInfo) : UnaryCallData(server, service, threadInfo) {
    // std::cout << "hello inserting " << (void*)this << std::endl;
    // TODO: how to do this in a generic way?  I would need to get the index of the method and then call
    // ::grpc::Service::RequestAsyncUnary(0, context, request, response, new_call_cq, notification_cq, tag);
    service.RequestSayHello(&ctx, &request, &responder, threadInfo.cq.get(), threadInfo.cq.get(), make_tag());
  }

  virtual void createNew() override {
    new SayHelloCall(server, service, threadInfo);
  }
  virtual void fillResponse() override {
    std::string prefix("Hello ");
    response.set_message(prefix + request.name());
    // std::cout << "req name:" << request.name() << std::endl;
  }
};

class SayHelloCall2 : public UnaryCallData<HelloRequest, HelloReply, Greeter::AsyncService> {
public:
  SayHelloCall2(GRPCServer& server, Greeter::AsyncService& service, GRPCServer::ThreadInfo& threadInfo) : UnaryCallData(server, service, threadInfo) {
    // std::cout << "hello2 inserting " << (void*)this << std::endl;
    service.RequestSayHello2(&ctx, &request, &responder, threadInfo.cq.get(), threadInfo.cq.get(), make_tag());
  }

  virtual void createNew() override {
    new SayHelloCall2(server, service, threadInfo);
  }
  virtual void fillResponse() override {
    std::string prefix("Hello2 ");
    response.set_message(prefix + request.name());
    // std::cout << "req name:" << request.name() << std::endl;
  }
};

class IndexerUpdateCall : public UnaryCallData<solux::proto::UpdateRequest, solux::proto::UpdateResponse, Indexer::AsyncService> {
public:
  IndexerUpdateCall(GRPCServer& server, Indexer::AsyncService& service, GRPCServer::ThreadInfo& threadInfo) : UnaryCallData(server, service, threadInfo) {
    GRPC_DEBUG("Indexer.Update inserting this={}", (void*)this);
    service.RequestUpdate(&ctx, &request, &responder, threadInfo.cq.get(), threadInfo.cq.get(), make_tag());
  }

  virtual void createNew() override {
    new IndexerUpdateCall(server, service, threadInfo);
  }
  virtual void fillResponse() override {
    GRPC_DEBUG("Update GRPCServer peer={}", ctx.peer());

    auto code = handleUpdate(server, request, response);
    unused(code); // TOOD: pass back?
  }

  // static methods meant to be usable by streaming server as well
  static grpc::Status handleUpdate(GRPCServer& server, solux::proto::UpdateRequest& request, solux::proto::UpdateResponse& response) {
    std::shared_ptr<Collection> collection;

    if (request.collection().name_size() == 0) {
      // TODO: do we support default collections (implicitly defined by something like an api-key?)
    }

    std::shared_ptr<Library> library = server.getSoluxNode().getLibrary(nullptr, "");
    for (int i=0; i<request.collection().name_size(); i++) {
      // TODO: walk from our implicit root to find the correct collection.
      if (i == request.collection().name_size()-1) {
        GRPC_DEBUG("Looking up collection name '{}'", request.collection().name(i));

        // last element in path, so get collection.
        collection = server.getSoluxNode().getCollection(library.get(), request.collection().name(i));
        // TODO: handle lookup failure
      } else {
        // not last element... get sub-library
        library = server.getSoluxNode().getLibrary(library.get(), request.collection().name(i));
        // TODO: handle lookup failure
      }
    }

    GRPC_DEBUG("\tindexer got docs, num={}", request.docs_size());
    auto shard = collection->getShard();
    auto iw = shard->getIndexWriter();
    iw->update(request);
    iw->flush();

    auto& singleResponse = *response.add_responses();
    singleResponse.set_request_id(request.request_id());
    return grpc::Status::OK;
  }
};



// client-server interaction scenarios:
//   single streaming client to single index
//   single streaming client to multiple indexes
//   multiple streaming clients to single index
//   multiple streaming clients to multiple indexes
//
// Server partition:
//   For updates, the solux server enables indexing one or more update requests in parallel by partitioning by
//   document id and ensuring that updates to the same document are in-order.
//
// A) Single large index, all unique docs:
//   1) N streaming clients connected to N server threads, all updates processed in receiving thread on cached Inverter.
//       # more complicated client, but potentially fastest due to fewer context switches?
//   2) 1 streaming client connected to 1 streaming server, handing out messages to N indexing threads, each with their own cached Inverter.
//
// B) Single large index, non-unique docs:
//   1) N streaming clients connected to N server threads, handing out messages to M indexing threads partitioned by docid
//

class IndexerUpdateStreamingCall : public StreamingCallData<solux::proto::UpdateRequest, solux::proto::UpdateResponse, Indexer::AsyncService> {
public:
  IndexerUpdateStreamingCall(GRPCServer& server, Indexer::AsyncService& service, GRPCServer::ThreadInfo& threadInfo) : StreamingCallData(server, service, threadInfo) {
    service.RequestUpdateStream(&ctx, &readerWriter, threadInfo.cq.get(), threadInfo.cq.get(), make_tag());
  }

  virtual void createNew() override {
    new IndexerUpdateStreamingCall(server, service, threadInfo);
  }
  virtual void fillResponse() override {
    GRPC_DEBUG("StreamingUpdate GRPCServer peer={}", ctx.peer());
    auto ok = IndexerUpdateCall::handleUpdate(server, request, response);
    unused(ok);
  }
};

//   rpc search(stream solux.proto.SearchRequest) returns (stream solux.proto.SearchResponse) {}
class SearcherSearchStreamingCall : public StreamingCallData<solux::proto::SearchRequest, solux::proto::SearchResponse, Searcher::AsyncService> {
public:
  SearcherSearchStreamingCall(GRPCServer& server, Searcher::AsyncService& service, GRPCServer::ThreadInfo& threadInfo) : StreamingCallData(server, service, threadInfo) {
    service.RequestSearch(&ctx, &readerWriter, threadInfo.cq.get(), threadInfo.cq.get(), make_tag());
  }

  virtual void createNew() override {
    new SearcherSearchStreamingCall(server, service, threadInfo);
  }
  virtual void fillResponse() override {
    GRPC_DEBUG("StreamingSearch GRPCServer peer={}", ctx.peer());

    std::shared_ptr<Collection> collection;

    if (request.collection().name_size() == 0) {
      // TODO: do we support default collections (implicitly defined by something like an api-key?)
    }

    std::shared_ptr<Library> library = server.getSoluxNode().getLibrary(nullptr, "");
    for (int i=0; i<request.collection().name_size(); i++) {
      // TODO: walk from our implicit root to find the correct collection.
      if (i == request.collection().name_size()-1) {
        GRPC_DEBUG("Looking up collection name '{}'", request.collection().name(i));

        // last element in path, so get collection.
        collection = server.getSoluxNode().getCollection(library.get(), request.collection().name(i));
        // TODO: handle lookup failure
      } else {
        // not last element... get sub-library
        library = server.getSoluxNode().getLibrary(library.get(), request.collection().name(i));
        // TODO: handle lookup failure
      }
    }

    for (auto& [opKey, searchOp] : request.ops()) {
      switch (searchOp.kind_case()) {
        case solux::proto::SearchOp::kTopDocs:
        {
          auto shard = collection->getShard();
          auto iw = shard->getIndexWriter();
          std::shared_ptr<IndexReader> reader = iw->getIndexReader();
          response.set_request_id(request.request_id());
          solux::proto::SearchResult& srsp = (*response.mutable_ops())[opKey];
          solux::proto::DocList& docList = *srsp.mutable_docs();
          docList.set_matches(reader->maxDoc());
          break;
        }
        case solux::proto::SearchOp::kFieldFacet:
          break;
        default:
          break;
      }
    }
  }
};

void GRPCServer::runThread(ThreadInfo& threadInfo) {
  // wait for the server to start before trying to use it.
  startLatch.wait();

  // Used to test that a client request will still be handled correctly if it comes in before we've registered the calls
  // std::this_thread::sleep_for (std::chrono::seconds(10));

  // Create one of each type of call.  They insert themselves into the completion queue.
  new SayHelloCall(*this, greeterService, threadInfo);
  new SayHelloCall2(*this, greeterService, threadInfo);
  new SayHelloStreamingCall(*this, greeterService, threadInfo);
  new IndexerUpdateCall(*this, indexerService, threadInfo);
  new IndexerUpdateStreamingCall(*this, indexerService, threadInfo);
  new SearcherSearchStreamingCall(*this, searcherService, threadInfo);

  /*
   * This startLatchThreads was added because shutting down the server very quickly would generate this failed assertion:
   * E0723 22:24:54.308864118 3078 server_cc.cc:216]  assertion failed: grpc_server_request_registered_call( server_->server(), registered_method, &call_, &context_->deadline_, context_->client_metadata_.arr(), payload, call_cq_->cq(), notification_cq->cq(), this) == GRPC_CALL_OK
   * It only happened in release mode (not debug mode) presumably because of timing.  Now GRPCServer::waitForStart()
   * waits for the calls to be registered into the completion queue before signalling that we are ready.
   * Best guess is that it was these registrations that were failing after the completion queue was shut down.
   */
  startLatchThreads.count_down(); // signal that we registered calls

  while (true) {
    void* tag;  // uniquely identifies a request.
    bool ok;
    bool gotEvent = threadInfo.cq->Next(&tag, &ok);

    if (!gotEvent) {
      // shutting down... completion queue should be empty at this point.
      GRPC_DEBUG("completionQueue->Next() returned false.");
      break;
    }

    if (!ok) {
      // request failed or was terminated.... still call proceed() so that it can be cleaned up
      GRPC_DEBUG("ERROR in completionQueue->Next()");
    }

    CallData::TaggedPtrType taggedPtr = CallData::TaggedPtrType::fromTaggedPtrBits(tag);
    CallData* callData = taggedPtr.ptr();
    callData->proceed(ok, taggedPtr.tag());
  }

  GRPC_DEBUG("GRPCServer thread shutting down.");
}


bool GRPCServer::waitForStart() {
  startLatchThreads.wait();
  return true;
}

/*** NOTE: after upgrading from grpc1.41 (and dependencies) to grpc1.44 via vcpkg,
 * valgrind reports a definite memory leak (along with a possible memory leak in abseil):
    Leak_DefinitelyLost
    ares_library_init.c
    10 bytes in 1 blocks are definitely lost in loss record 12 of 76
    0x4848899 malloc
    0x11C77D1 default_malloc ares_library_init.c:48
    0x11CD532 ares_strdup ares_strdup.c:33
    0x11D3C1D ares__readaddrinfo ares__readaddrinfo.c:62
    0x11D0FB2 file_lookup ares_getaddrinfo.c:477
    0x11D10B2 next_lookup ares_getaddrinfo.c:513
    0x11D1663 ares_getaddrinfo ares_getaddrinfo.c:698
    0x11C2BCA ares_gethostbyname ares_gethostbyname.c:113
    0xF94E55 grpc_dns_lookup_ares_continue_after_check_localhost_and_ip_literals_locked grpc_ares_wrapper.cc:875
    0xF959FA grpc_dns_lookup_ares_impl grpc_ares_wrapper.cc:1069
    0xF8C8DF grpc_core::AresClientChannelDNSResolver::StartResolvingLocked dns_resolver_ares.cc:454
    0xF8C710 grpc_core::AresClientChannelDNSResolver::MaybeStartResolvingLocked dns_resolver_ares.cc:443

    Another grpc example (written by someone else) also showed a leak after upgrading.
 */
void solux::GRPCServer::shutdown() {
  LOG_INFO("Shutting down grpc server.");

  // server should be shut down before completion queues
  server->Shutdown();

  // calling Shutdown on the completion queue will cause cq->Next() to return false
  // rather than block.
  for (auto& threadInfo : threadInfos) {
    threadInfo.cq->Shutdown();
  }

  for (auto& thread : threads) {
    thread.join();
  }

}
