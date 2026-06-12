
#include <string>
#include <algorithm>
#include <grpcpp/grpcpp.h>
#include <absl/strings/str_cat.h>
#include <google/protobuf/text_format.h>
#include <grpcpp/health_check_service_interface.h>
#include <grpcpp/ext/proto_server_reflection_plugin.h>
#include <oneapi/tbb/task_group.h>

#include "GRPCServer.h"
#include "SoluxNode.h"
#include "solux/util/random.h"
#include "solux/util/solux_util.h"
#include "solux/util/TaggedPtr.h"
#include "solux/query/ProtobufQueryParser.h"
#include "solux/util/thread.h"
#include "solux/util/proto.h"
#include "ProtoUpdateMessage.h"
#include "solux/schema/Schema.h"


namespace solux {
// TODO - use a different logger for RPC stuff some point
// redefine DEBUG to TRACE level which shouldn't currently be logged!
#define GRPC_DEBUG LOG_TRACE
// #define GRPC_DEBUG LOG_DEBUG

GRPCServer::GRPCServer(SoluxNode& node, int nthreads, int port)
  : soluxNode(node), startLatch(1), startLatchThreads(nthreads), nthreads(nthreads), requestedPort(port) {
}

// NOTE: as of gRPC 1.39 there is a new C++ async callback API: https://github.com/grpc/grpc/pull/25728 in addition to an EventEngine
// interface that may help with integration with external event loops.

void solux::GRPCServer::run() {
  pthread_setname_np(pthread_self(), "solux_grpc_main");

  // Use requestedPort (default 0 for dynamic allocation, or a specific port
  // like 50051).  Dynamic test ports bind localhost; configured ports keep the
  // existing all-interfaces behavior.
  std::string bindHost = requestedPort == 0 ? "127.0.0.1" : "0.0.0.0";
  std::string server_address = bindHost + ":" + std::to_string(requestedPort);

  grpc::EnableDefaultHealthCheckService(true);
  grpc::reflection::InitProtoReflectionServerBuilderPlugin();
  grpc::ServerBuilder builder;
  // Listen on the given address without any authentication mechanism.
  // The actual port will be stored in serverPort
  builder.AddListeningPort(server_address, grpc::InsecureServerCredentials(), &serverPort);
  // Register "service" as the instance through which we'll communicate with
  // clients. In this case it corresponds to a *synchronous* service.

  builder.RegisterService(&greeterService);
  builder.RegisterService(&indexerService);
  builder.RegisterService(&searcherService);
  builder.RegisterService(&adminService);

  threadInfos.reserve(nthreads);

  // Give each thread a completion queue, but don't let them use it
  // before the server starts (implemented with startLatch)
  for (int i=0; i<nthreads; i++) {
    threadInfos.emplace_back();
    threadInfos.back().threadno = i;
    threadInfos.back().cq = builder.AddCompletionQueue(); // each thread gets it's own completion queue
  }

  this->server = builder.BuildAndStart();
  if (!server) {
    LOG_ERROR("GRPCServer failed to listen on {}", server_address);
    startLatch.count_down();
    startLatchThreads.count_down(nthreads);
    return;
  }
  LOG_INFO("GRPCServer listening on {}:{}", bindHost, serverPort);

  threads.reserve(nthreads);
  for (int i=0; i<nthreads; i++) {
    threads.emplace_back([this,i]{ this->runThread(threadInfos[i]); });
  }

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
    unused(tag);
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
      state = FINISH;
      try {
        fillResponse();
        responder.Finish(response, grpc::Status::OK, make_tag());
      } catch (const std::exception& e) {
        responder.Finish(response, grpc::Status(grpc::StatusCode::INTERNAL, e.what()), make_tag());
      }
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
class BiStreamingRequest : public CallData {
public:

  // We want to support two streaming use-cases:
  // 1) common case of a single response for a single request
  //    ideally use a single arena for both
  // 2) multiple responses for a single request
  //    each response should be in a separate arena so they can be freed separately
  // We also need to buffer responses since only one write can be outstanding at once.

  using callback_type = std::function<void(google::protobuf::Message*)>;
  using MessageAndCallback = std::pair<google::protobuf::Message*, callback_type>;
  constexpr static size_t ARENA_BUF_SIZE = 1024;  // size of first buffer to give to the arena (includes space for arena itself!)

  std::mutex mutex;
  AsyncServiceT& service;
  grpc::ServerAsyncReaderWriter<ResponseT,RequestT> readerWriter;

  // The current request object (req->request) that is being read into.
  RequestT* req = nullptr;

  // these are protected by the mutex
  std::deque<MessageAndCallback> pending;  // pending writes
  bool errored = false;    // if true, something happened and additional reads/writes should fail.
  bool readsDone = false;  // true when client ends the stream and we can't read any more messages
  bool writeOutstanding = false;  // if true, a write was requested but not yet completed.  Only one write can be outstanding at a time in a streaming RPC.
  bool finishSent = false;  // if true, we sent a finish.
  int32_t responsesExpected = 0;  // number of future responses to requests we are expecting.

  // these are the tags we use in the completion queue to know what event/operation finished in the completion queue.
  enum CallTags { READ = 1, WRITE = 2, FINISH = 3, CONNECT = 4, ASYNC_NOTIFY_WHEN_DONE = 5 };

  int numCalls = 0; // TODO: testing only... remove after stable.



  BiStreamingRequest(GRPCServer& server, AsyncServiceT& service, GRPCServer::ThreadInfo& threadInfo) : CallData(server, threadInfo), service(service), readerWriter(&ctx) {
    GRPC_DEBUG("CREATE StreamingCallData this={}", (void*)this);

    // see https://stackoverflow.com/questions/60856240/grpc-c-async-server-how-differentiate-between-writesdone-and-broken-connection
    // TODO: not sure the right way to use this event yet.

    // NOTE: ASYNC_NOTIFY_WHEN_DONE can come in *after* the FINISH event (i.e. AFTER this object has been deleted!)
    // Hence we must not use it like this.
    // If we ever need to check for cancellation, then we will need ASYNC_NOTIFY_WHEN_DONE.  Perhaps delete only
    // after both Finish and ASYNC_NOTIFY_WHEN_DONE have been received?
    // ctx.AsyncNotifyWhenDone(make_tag(CallTags::ASYNC_NOTIFY_WHEN_DONE));
  }

  virtual ~BiStreamingRequest() {
    // release arena of req
    if (req) {
      releaseArena(req->GetArena());
      req = nullptr;
    }
  }


  // mutex is held when calling this method
  void doWrite(ResponseT* response, const callback_type& callback) {
    assert(!writeOutstanding);
    writeOutstanding = true;
    readerWriter.Write(*response, make_tag(WRITE));
    callback(response);
  }

  // mutex is held when calling this method
  void maybeSendFinish() {
    // TODO: what if errored state?
    if (!finishSent && readsDone && pending.empty() && !writeOutstanding && responsesExpected <= 0) {
      // There are no other outstanding requests, so we can finish now.
      readerWriter.Finish(grpc::Status::OK, make_tag(FINISH));
      finishSent = true;
      GRPC_DEBUG("After calling Finish. this={} numCalls={} cancelled={}", (void*)this, numCalls, ctx.IsCancelled());
    }
  }

  void writeFinished(bool ok) {
    {
      const std::lock_guard<std::mutex> lock(mutex);

      if (!ok) {
        errored = true;
        readsDone = true;  // is this needed? Our Read() call should also be returned with !ok and do the same thing?
      }
      assert(writeOutstanding);
      writeOutstanding = false;
      if (!pending.empty()) {
        auto& pendingResponse = pending.front();
        doWrite((ResponseT*)pendingResponse.first, pendingResponse.second);
        pending.pop_front();
      } else {
        maybeSendFinish();
      }
    }
  }


  // request a read
  void readRequest() {
    if (req == nullptr) {
      req = createRequestMessage();
    }
    readerWriter.Read(req, make_tag(READ));
  }


  /// response is the message to send back to the client.
  /// callback will be called with the response message after Write has been called and it is safe to free it.
  /// The callback object itself may be copied and called later, so it must be safe to do so.
  /// finishCount is the number of outstanding requests that this write will complete (i.e. responsesExpected is decremented).
  /// 0 if further writes will happen for the same request.
  /// 1 if the request is done and this is the last write.
  /// 2 or more if this write effectively coalesces responses to multiple requests.
  /// @returns the number of currently buffered write requests
  /// NOTE: calling with finishCount>0 may eventually cause "this" to be deleted in another thread,
  /// so make it the last thing you do with "this" in the calling code.
  size_t respond(ResponseT* response, const callback_type& callback, int32_t finishCount=1) {
    {
      const std::lock_guard<std::mutex> lock(mutex);

      responsesExpected -= finishCount;
      if (writeOutstanding) {
        // we can't write until the previous write is done.
        pending.emplace_back(MessageAndCallback{response, callback});
      } else {
        doWrite(response, callback);
      }

      return pending.size();
    }
  }

  /// Decrement the count of the number of outstanding responses expected.
  /// This can be used when something bad happened with a request and we don't want to send a response.
  /// This can also be used when multiple responses are sent for a request, but one doesn't know the order the
  /// responses will be completed.
  /// NOTE: this may eventually cause "this" to be deleted in another thread, so make it the last thing you do with "this" in the calling code.
  void decrementOutstanding(int32_t finishCount=1) {
    {
      const std::lock_guard<std::mutex> lock(mutex);
      responsesExpected -= finishCount;
      // Maybe we didn't send finish earlier because there were outstanding responses for requests.  Check again.
      maybeSendFinish();
    }
  }


  virtual void proceed(bool ok, uint32_t tag) override {
    // If we have a single thread per completion queue, then this can only be called from that thread.
    // Multiple threads is a little more unclear... There can only be one Read being completed at once (since only one can be outstanding)
    // and the same for writes.  But one thread could be notifying of a read and one of a write?
    // It's also unclear when ASYNC_NOTIFY_WHEN_DONE can be returned.

    // Ending the stream: currently outstanding requests will always cause a Write request to be done, hence we can just
    // check in writeFinished() if we are done and call Finish() if so.

    // NOTE: isCancelled can only be safely called after ASYNC_NOTIFY_WHEN_DONE has been returned.
    GRPC_DEBUG("PROCEED this={} ok={} numCalls={} tag={} cancelled={} responsesExpected={} readsDone={} writeOutstanding={} pwrites={}",
               (void*)this, ok, ++numCalls, tag, ctx.IsCancelled(), responsesExpected, readsDone, writeOutstanding, pending.size());

    switch(tag) {
      case READ:
        if (!ok) {
          // Client ended the stream.
          // TODO: ok==false when we are shutting down as well.... how to tell the difference?  I guess Write / Finish will
          // error out if we're shutting down???
          GRPC_DEBUG("End of stream!");
          readsDone = true;
          {
            const std::lock_guard<std::mutex> lock(mutex);
            maybeSendFinish();
          }
          break;
        }

        // respond() can be called before handleRequest returns, so we need to increment the number of
        // outstanding requests before that.
        {
          const std::lock_guard<std::mutex> lock(mutex);
          responsesExpected++;  // expect this request will eventually cause a response
        }

        // call handleRequest before requesting the next read. This allows for the handler to directly call respond()
        // or to copy what is needed from the request object.  When this is the case, it can be reused.
        //
        // This also allows for a flow control mechanism
        // where at some point handleRequest can block / help perform tasks if heavily loaded.
        {
          bool ownershipPassed = handleRequest(req);
          if (ownershipPassed) {
            req = nullptr;
          }
        }

        readRequest();
        break;
      case WRITE:
        // Last write succeeded.  Now check if we have any more pending.
        writeFinished(ok);
        break;
      case FINISH:
        // deleting self... be careful there are no destructors or scope guards that depend on
        // this object (like the mutex) that fire off after this!
        delete this;
        break;
      case CONNECT:
        if (!ok) {
          assert(responsesExpected == 0 && writeOutstanding == 0);  // first event... should be no requests outstanding.
          delete this;
          break;
        }

        // create a new instance of this to handle additional streaming calls.
        createNew();
        readRequest();
        break;
      case ASYNC_NOTIFY_WHEN_DONE:
        // In a simple test where the client sends two requests, calls finish, then reads the responses,
        // the last 3 events in the queue are Read(ok=false), ASYNC_NOTIFY_WHEN_DONE(ok=true), and FINISH(ok=true).
        // It doesn't seem like we currently need this event.
        // NOTE: this event has been removed (see comments in the constructor)
        break;
      default:
        // we tag everything going into the queue, so this shouldn't happen.
        LOG_ERROR("Unknown tag {} on {}", tag, (void*)this);
        break;
    }
  }

  /// This creates a request message with an Arena.  The arena may be used for other purposes as well.
  /// Use releaseArena() to destroy both the Arena and the message.  The pointer to the arena
  /// may be obtained from the message.
  RequestT* createRequestMessage() {
    auto arena = createArena();
    return google::protobuf::Arena::Create<RequestT>(arena);
  }

  virtual void createNew() = 0;

  /// Return true if you have taken ownership of the request message.
  /// If so, a new one will be created for the next request. If false,
  /// the request will be reused for the next request.
  virtual bool handleRequest(RequestT* request) = 0;
};



class SayHelloStreamingCall : public BiStreamingRequest<HelloRequest, HelloReply, Greeter::AsyncService> {
public:
  SayHelloStreamingCall(GRPCServer& server, Greeter::AsyncService& service, GRPCServer::ThreadInfo& threadInfo) : BiStreamingRequest(server, service, threadInfo) {
    //     void RequestSayHelloStreaming(::grpc::ServerContext* context, ::grpc::ServerAsyncReaderWriter< ::solux::HelloReply, ::solux::HelloRequest>* stream, ::grpc::CompletionQueue* new_call_cq, ::grpc::ServerCompletionQueue* notification_cq, void *tag) {
    service.RequestSayHelloStreaming(&ctx, &readerWriter, threadInfo.cq.get(), threadInfo.cq.get(), make_tag(CONNECT));
  }

  virtual void createNew() override {
    new SayHelloStreamingCall(server, service, threadInfo);
  }

  void fillResponse(HelloReply* response, HelloRequest* request, int responseNum) {
    response->set_message(absl::StrCat("Hello ", request->name()));
    response->set_response_number(responseNum);

    // sleep a random amount of time between the given min and max microseconds
    if (request->max_sleep_us() > 0) {
      auto now = std::chrono::high_resolution_clock::now();
      solux::Rng rng(now.time_since_epoch().count());
      auto sleepUs = rng.rint(request->min_sleep_us(), request->max_sleep_us());
      std::this_thread::sleep_for(std::chrono::microseconds(sleepUs));
    }
  }

  void doMultipleResponse(HelloRequest* request) {
    oneapi::tbb::task_group tg;

    for (int i = 0; i < request->response_count(); i++) {
      tg.run([this, request, i] {
        auto arena = createArena();
        HelloReply* response = google::protobuf::Arena::Create<HelloReply>(arena);
        fillResponse(response, request, i+1);
        respond(response,
                [](auto* response) { releaseArena(response->GetArena()); },
                0);  // never call with >0 here since we don't know the order of execution and that could end things prematurely.
      });
    }
    tg.wait();
  }

  bool handleRequest(HelloRequest* request) override {

    // single sync response, do the simplest way.
    if (request->response_count() <= 1) {
      // if sync, respond
      if (!request->async()) {
        // NOTE: we still can't use a single cached response object here because this call could be interleaved with
        // other calls that cause buffering of the responses and hence we don't know when the response will actually
        // be sent.  We would need to implement caching of responses.
        auto arena = createArena();
        HelloReply* response = google::protobuf::Arena::Create<HelloReply>(arena);
        fillResponse(response, request, 1);
        respond(response,
                [](auto* response) { releaseArena(response->GetArena()); },
                1);

        return false; // don't take ownership of request object
      }

      // if async, but single response (this will be most common in solux probably), then we can just use the
      // arena of the request object.
      auto arena = request->GetArena();
      // Create the response object immediately so it's in the same arena buffer as the request object.
      // If it's created in a different thread, a different arena buffer will be used.
      // In reality, this would probably only help responses that don't need to further allocate.
      HelloReply* response = google::protobuf::Arena::Create<HelloReply>(arena);
      auto& taskArena = server.getSoluxNode().getTaskArena();

      taskArena.enqueue([this,request,response]() {
        this->fillResponse(response, request, 1);
        this->respond(response,
                      [](auto* response) { releaseArena(response->GetArena()); },
                      1);
        // since the arena of the response will be freed, that will take care of the
        // request as well.
      });
      return true; // take ownership of request object
    }

    if (!request->async()) {
      doMultipleResponse(request);
      decrementOutstanding();
      return false; // don't take ownership of request object.. we don't need it anymore.
    }

    // if async, take ownership of request object so we can refer to it later.
    auto& taskArena = server.getSoluxNode().getTaskArena();
    taskArena.enqueue([this,request]() {
      this->doMultipleResponse(request);
      releaseArena(request->GetArena());
      this->decrementOutstanding();
    });
    return true; // take ownership of request object
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
    response.set_message(absl::StrCat("Hello ", request.name()));
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
    response.set_message(absl::StrCat("Hello2 ", request.name()));
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

    /** first test iteration before TBB update graph
    if (request.docs_size() != 0) {
      iw->update(request);
      iw->commit(); // TODO: remove this at some point...
    } else {
      // thread safety testing... only happened when we had actual docs.  try to simulate with a sleep.
      std::this_thread::sleep_for(std::chrono::microseconds (100));
    }
    */


    class BlockingUpdateMessage : public ProtoUpdateMessage {
    public:
      Blocker blocker;
      BlockingUpdateMessage(solux::proto::UpdateRequest* req, solux::proto::UpdateResponse* response) : ProtoUpdateMessage(req, response) {
      }
      virtual void done(IndexWriter& iw) override {
        unused(iw);
        blocker.notify();
      }
    };

    BlockingUpdateMessage updateMessage(&request, &response);
    bool success = iw->submitUpdate(&updateMessage);
    assert(success);

    updateMessage.blocker.wait();
    updateMessage.finishResponse();

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

class IndexerUpdateStreamingCall : public BiStreamingRequest<solux::proto::UpdateRequest, solux::proto::UpdateResponse, Indexer::AsyncService> {
public:
  IndexerUpdateStreamingCall(GRPCServer& server, Indexer::AsyncService& service, GRPCServer::ThreadInfo& threadInfo) : BiStreamingRequest(server, service, threadInfo) {
    service.RequestUpdateStream(&ctx, &readerWriter, threadInfo.cq.get(), threadInfo.cq.get(), make_tag(CONNECT));
  }

  virtual void createNew() override {
    new IndexerUpdateStreamingCall(server, service, threadInfo);
  }

  bool handleRequest(proto::UpdateRequest* request) override {
    // auto* arena = request->GetArena();

    std::shared_ptr<Collection> collection;

    if (request->collection().name_size() == 0) {
      // TODO: do we support default collections (implicitly defined by something like an api-key?)
    }

    std::shared_ptr<Library> library = server.getSoluxNode().getLibrary(nullptr, "");
    for (int i=0; i<request->collection().name_size(); i++) {
      // TODO: walk from our implicit root to find the correct collection.
      if (i == request->collection().name_size()-1) {
        GRPC_DEBUG("Looking up collection name '{}'", request->collection().name(i));

        // last element in path, so get collection.
        collection = server.getSoluxNode().getCollection(library.get(), request->collection().name(i));
        // TODO: handle lookup failure
      } else {
        // not last element... get sub-library
        library = server.getSoluxNode().getLibrary(library.get(), request->collection().name(i));
        // TODO: handle lookup failure
      }
    }

    GRPC_DEBUG("\tindexer got docs, num={}", request->docs_size());
    auto shard = collection->getShard();
    auto iw = shard->getIndexWriter();


    class Update : public ProtoUpdateMessage {
    public:
      IndexerUpdateStreamingCall* parent;
      Update(proto::UpdateRequest* req, IndexerUpdateStreamingCall* parent) : ProtoUpdateMessage(req), parent(parent) {}
      virtual void done(IndexWriter& iw) override {
        unused(iw);
        // LOG_DEBUG("done msg={}", (void*)this);

        // By the time this response is done, *this* object will already be deleted, so don't
        // reference anything in this Update instance.
        auto* p = parent;
        parent->respond(this->finishResponse(),
                      [p](auto* response) { unused(p); releaseArena(response->GetArena()); },
                      1);
        delete this; // TODO arena allocate this
      }
    };

    Update* updateMessage = new Update(req, this); // TODO arena allocate this.

    // TODO: test if submitting a task to an arena that does this would cause the updateMessage to start
    // faster.  A standalone test would be easist to see this.
    iw->submitUpdate(updateMessage);

    return true; // take ownership of request object since we used its arena (and we are handling async)
  }
};

//   rpc search(stream solux.proto.SearchRequest) returns (stream solux.proto.SearchResponse) {}
class SearcherSearchStreamingCall : public BiStreamingRequest<solux::proto::SearchRequest, solux::proto::SearchResponse, Searcher::AsyncService> {
public:
  SearcherSearchStreamingCall(GRPCServer& server, Searcher::AsyncService& service, GRPCServer::ThreadInfo& threadInfo) : BiStreamingRequest(server, service, threadInfo) {
    service.RequestSearch(&ctx, &readerWriter, threadInfo.cq.get(), threadInfo.cq.get(), make_tag(CONNECT));
  }

  virtual void createNew() override {
    new SearcherSearchStreamingCall(server, service, threadInfo);
  }


  bool handleRequest(solux::proto::SearchRequest* request) override {
    auto& engine = server.getSoluxNode().getSearchEngine();
    auto arena = request->GetArena();

    // Two ways to do synchronous:
    //   1) pass thread_group as null
    //   2) make a local thread_group, use that and wait() for it.
    // In both cases, there may be issues with multiple-responses? The first response will be written,
    // but since only one write can be outstanding at the same time, we won't have the chance
    // to get "write complete" from the completion queue, and any further writes will be buffered.
    // If we implement flow control, we could deadlock.  Hence, if operating synchronously, we can't do
    // flow control and multiple responses after the first will all be buffered.
    // One solution is to only allow single-response requests in synchronous mode.  It could be disastrous
    // for a large streaming response.
    //
    // Deadlock: if we synchronously handle a response here, it should not do work-stealing, as I believe
    // this could lead to deadlock (anything waiting for a response callback would be vulnerable).
    // TBB isolation should be able to prevent this, as could avoiding TBB (pass thread_group==null)

    class GRPCSearchRequest : public SearchRequest {
    public:
      SearcherSearchStreamingCall* parent;
      GRPCSearchRequest(SearchEngine& engine, solux::proto::SearchRequest& proto) : SearchRequest(engine, proto) {
      }

      int reply(SearchResponse& response) override {
        auto buffered = parent->respond(&response.proto,
                        // cause replyCallback() to be called after the write is done.
                        [&response](auto* responseProto) {
          unused(responseProto);
          assert(responseProto == &response.proto);
          response.req.replyCallback(response);
        });
        return (int)buffered;
      }
    };

    auto& req = *google::protobuf::Arena::Create<GRPCSearchRequest>(arena, engine, *request);
    req.parent = this;
    engine.submit(req, true);

    // Always return true (i.e. we have taken control of the request object thus it can't be reused)
    // The issue is that even for a single simple synchronous request, there may be other responses
    // that are concurrently being written to this connection.  Hence, our write may be buffered
    // and since it's connected to the request object via Arena, we can't reuse that request object.
    return true;
  }


};

// Helper to resolve a Collection from a Target proto
static std::shared_ptr<Collection> resolveCollection(GRPCServer& server, const proto::Target& target) {
  std::shared_ptr<Library> library = server.getSoluxNode().getLibrary(nullptr, "");
  std::shared_ptr<Collection> collection;
  for (int i = 0; i < target.name_size(); i++) {
    if (i == target.name_size() - 1) {
      collection = server.getSoluxNode().getCollection(library.get(), target.name(i));
    } else {
      library = server.getSoluxNode().getLibrary(library.get(), target.name(i));
    }
  }
  if (!collection) {
    collection = server.getSoluxNode().getCollection("");
  }
  return collection;
}

class AdminSetSchemaCall : public UnaryCallData<proto::SchemaRequest, proto::SchemaResponse, Admin::AsyncService> {
public:
  AdminSetSchemaCall(GRPCServer& server, Admin::AsyncService& service, GRPCServer::ThreadInfo& threadInfo)
    : UnaryCallData(server, service, threadInfo) {
    service.RequestSetSchema(&ctx, &request, &responder, threadInfo.cq.get(), threadInfo.cq.get(), make_tag());
  }

  void createNew() override {
    new AdminSetSchemaCall(server, service, threadInfo);
  }

  void fillResponse() override {
    auto collection = resolveCollection(server, request.collection());
    std::shared_ptr<Schema> newSchema;

    if (request.mode() == proto::SchemaRequest::MERGE) {
      auto currentSchema = collection->getSchema();
      newSchema = Schema::fromProto(request.schema(), currentSchema.get());
    } else {
      // REPLACE
      newSchema = Schema::fromProto(request.schema());
    }

    collection->setSchema(newSchema);
    newSchema->toProto(response.mutable_schema());
  }
};

class AdminGetSchemaCall : public UnaryCallData<proto::SchemaRequest, proto::SchemaResponse, Admin::AsyncService> {
public:
  AdminGetSchemaCall(GRPCServer& server, Admin::AsyncService& service, GRPCServer::ThreadInfo& threadInfo)
    : UnaryCallData(server, service, threadInfo) {
    service.RequestGetSchema(&ctx, &request, &responder, threadInfo.cq.get(), threadInfo.cq.get(), make_tag());
  }

  void createNew() override {
    new AdminGetSchemaCall(server, service, threadInfo);
  }

  void fillResponse() override {
    auto collection = resolveCollection(server, request.collection());
    auto schema = collection->getSchema();
    schema->toProto(response.mutable_schema());
  }
};

void GRPCServer::runThread(ThreadInfo& threadInfo) {
  // linux-only: give threads a nice name for debugging.
  std::string tname = "solux_grpc_" + std::to_string(threadInfo.threadno);
  pthread_setname_np(pthread_self(), tname.c_str());

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
  new AdminSetSchemaCall(*this, adminService, threadInfo);
  new AdminGetSchemaCall(*this, adminService, threadInfo);

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

    CallData::TaggedPtrType taggedPtr = CallData::TaggedPtrType::fromTaggedPtrBits(tag);
    CallData* callData = taggedPtr.ptr();
    GRPC_DEBUG("PRECALL: this={} tag={} ok={}", (void*)callData, taggedPtr.tag(), ok);
    callData->proceed(ok, taggedPtr.tag());
    assert(MemPool::sanityCheck());
  }

  GRPC_DEBUG("GRPCServer thread shutting down.");
}


bool GRPCServer::waitForStart() {
  startLatchThreads.wait();
  return server != nullptr;
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
  LOG_INFO("Shutting down solux grpc server.");
  if (!server) {
    return;
  }

  // server should be shut down before completion queues
  server->Shutdown();

  GRPC_DEBUG("server->Shutdown() returned.");

  // calling Shutdown on the completion queue will cause cq->Next() to return false
  // rather than block.
  for (auto& threadInfo : threadInfos) {
    GRPC_DEBUG("Calling completion queue Shutdown for cq={}", (void*)threadInfo.cq.get());
    threadInfo.cq->Shutdown();
  }

  GRPC_DEBUG("joining threads");

  for (auto& thread : threads) {
    thread.join();
  }

  GRPC_DEBUG("shutdown complete");
}

} //  namespace solux
