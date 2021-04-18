
#include <string>
#include <algorithm>
#include <boost/lockfree/detail/tagged_ptr.hpp>
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


GRPCServer::GRPCServer()
  : startLatch(1) {
}

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

  int nthreads = std::max(1u, std::thread::hardware_concurrency());
  nthreads = 2; // TODO TODO TODO: delete this line in the future... this is just to lower the number of threads to make debugging easier
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
  std::cout << "GRPCServer listening on " << server_address << std::endl;

  // inform everyone that the server is up and running
  startLatch.count_down();

  // Wait for the server to shutdown. Note that some other thread must be
  // responsible for shutting down the server for this call to ever return.
  server->Wait();
  std::cout << "server->Wait() returned!" << std::endl;
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
template <class RequestT, class ReplyT, class AsyncServiceT>
class UnaryCallData : public CallData {
public:
  RequestT request;
  ReplyT reply;
  AsyncServiceT& service;
  grpc::ServerAsyncResponseWriter<ReplyT> responder;
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
      fillReply();

      state = FINISH;
      responder.Finish(reply, grpc::Status::OK, make_tag());
      //std::cout << "finish called," << counter++ << std::endl;
    } else {
      // nothing left to do but delete ourselves
      // std::cout << "deleting " << (void*)this << std::endl;
      delete this;
    }

  }

  virtual void createNew() = 0;
  virtual void fillReply() = 0;
};


template <class RequestT, class ReplyT, class AsyncServiceT>
class StreamingCallData : public CallData {
public:
  RequestT request;
  ReplyT reply;
  AsyncServiceT& service;
  grpc::ServerAsyncReaderWriter<ReplyT,RequestT> readerWriter;
  enum CallStatus { READ = 1, WRITE = 2, CONNECT = 3, FINISH = 5 };

  CallStatus state;
  int numCalls = 0; // TODO: testing only... remove after stable.

  StreamingCallData(GRPCServer& server, AsyncServiceT& service, GRPCServer::ThreadInfo& threadInfo) : CallData(server, threadInfo), service(service), readerWriter(&ctx) {
    std::cout << "CREATE thread=" << threadInfo.threadno << " this=" << this << std::endl;

    // see https://stackoverflow.com/questions/60856240/grpc-c-async-server-how-differentiate-between-writesdone-and-broken-connection
    // TODO: not sure the right way to use this event yet.
    ctx.AsyncNotifyWhenDone(make_tag(1));
    state = CONNECT;
  }

  virtual void proceed(bool ok, uint32_t tag) override {
    // TODO: include testing for multiple threads calling back to this object (can that even happen?)

    std::cout << "PROCEED thread=" << threadInfo.threadno << " this=" << this << " state=" << state << " ok=" << ok
              << " numCalls=" << ++numCalls << " tag=" << tag <<  " cancelled=" << ctx.IsCancelled() << std::endl;


    if (tag == 1) {
      // result of AsyncNotifyWhenDone
      // TODO: how should we make use of this?
      return;
    }


    if (!ok && state != CallStatus::READ) {
      // canceled/errored... nothing else to do.
      std::cout << "Error or shutting down. deleting " << (void*)this << std::endl;
      delete this;
      return;
    }


    switch (state) {
      case CallStatus::READ:

        if (!ok) {
          // Client ended the stream.
          // TODO: ok==false when we are shutting down as well.... how to tell the difference?  I guess Finish will
          // error out (or complete) if we're shutting down???
          std::cout << "End of stream!" << std::endl;
          grpc::Status st(grpc::StatusCode::OK,"is OK message used/passed?");

          // TODO: only finish *after* writing all necessary responses!
          readerWriter.Finish(st,make_tag());
          state = CallStatus::FINISH;
          std::cout << "After calling Finish. thread=" << threadInfo.threadno << " this=" << this << " state=" << state << " ok="
                    << " numCalls=" << numCalls << " tag=" << tag <<  " cancelled=" << ctx.IsCancelled() << std::endl;
          break;
        }

        std::cout << "Read new message: " << request.name() << std::endl;

        fillReply();

        readerWriter.Write(reply, make_tag());

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
        std::cerr << "Unexpected state " << state << std::endl;
        assert(false);
    }
  }

  virtual void createNew() = 0;
  virtual void fillReply() = 0;
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
  virtual void fillReply() override {
    std::string prefix("HelloStreaming ");
    reply.set_message(prefix + request.name());
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
  virtual void fillReply() override {
    std::string prefix("Hello ");
    reply.set_message(prefix + request.name());
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
  virtual void fillReply() override {
    std::string prefix("Hello2 ");
    reply.set_message(prefix + request.name());
    // std::cout << "req name:" << request.name() << std::endl;
  }
};

class IndexerUpdateCall : public UnaryCallData<solux::proto::UpdateRequest, HelloReply, Indexer::AsyncService> {
public:
  IndexerUpdateCall(GRPCServer& server, Indexer::AsyncService& service, GRPCServer::ThreadInfo& threadInfo) : UnaryCallData(server, service, threadInfo) {
    std::cout << "Indexer.Update inserting " << (void*)this << std::endl;
    service.RequestUpdate(&ctx, &request, &responder, threadInfo.cq.get(), threadInfo.cq.get(), make_tag());
  }

  virtual void createNew() override {
    new IndexerUpdateCall(server, service, threadInfo);
  }
  virtual void fillReply() override {
    std::cout << "Update GRPCServer peer=" << ctx.peer() << std::endl;

    std::shared_ptr<Collection> collection;

    if (request.collection().name_size() == 0) {
      // TODO: do we support default collections (implicitly defined by something like an api-key?)
    }

    std::shared_ptr<Library> library = server.getSoluxNode().getLibrary(nullptr, "");
    for (int i=0; i<request.collection().name_size(); i++) {
      // TODO: walk from our implicit root to find the correct collection.
      if (i == request.collection().name_size()-1) {
        std::cout << "looking up collection name " << request.collection().name(i) << std::endl;

        // last element in path, so get collection.
        collection = server.getSoluxNode().getCollection(library.get(), request.collection().name(i));
        // TODO: handle lookup failure
      } else {
        // not last element... get sub-library
        library = server.getSoluxNode().getLibrary(library.get(), request.collection().name(i));
        // TODO: handle lookup failure
      }
    }


    if (request.docs_size() > 0) {
      std::cout << "\tindexer got docs: " << request.docs_size() << std::endl;
      auto shard = collection->getShard();
      auto iw = shard->getIndexWriter();
      Inverter& inverter = iw->getInverter();

      std::string reqStr;
      google::protobuf::TextFormat::PrintToString(request, &reqStr);
      std::cout << "REQ:( " << reqStr << " )" << std::endl;

      std::vector<Inverter::SegFieldPos*> segFields;
      // int ndocs = request->docs_size();
      for (auto& doc : request.docs()) {
        size_t nFields = doc.fields_size();
        if (segFields.size() < nFields) {
          segFields.resize(nFields);
        }

        inverter.startDoc();

        int idx=0;
        for (auto& [fname,fval] : doc.fields()) {
          auto segField = segFields[idx];
          if (segField == nullptr || *segField != fname) {
            segFields[idx] = segField = &inverter.getSegField(fname);
          }
          auto& sval = fval.s();
          std::cout << " Indexing " << fname << ":" << sval << std::endl;
          inverter.index(*segField, const_cast<char*>(sval.data()), sval.size());  // TODO: get rid of the const-cast
          idx++;
        }

        inverter.finishDoc();
      }

      // TODO: keep the current inverter around if we expect more docs coming.
      iw->flush();

    }
    if (request.has_columns()) {
      std::cout << "\tindexer got columns: " << request.columns().columns_size() << std::endl;

    }

    reply.set_message("Indexing Response");
    // return grpc::Status::OK;
  }
};



void GRPCServer::runThread(ThreadInfo& threadInfo) {
  // wait for the server to start before trying to use it.
  if (!waitForStart()) {
    return;
  };

  // Create one of each type of call.  They insert themselves into the completion queue.
  new SayHelloCall(*this, greeterService, threadInfo);
  new SayHelloCall2(*this, greeterService, threadInfo);
  new SayHelloStreamingCall(*this, greeterService, threadInfo);
  new IndexerUpdateCall(*this, indexerService, threadInfo);

  while (true) {
    void* tag;  // uniquely identifies a request.
    bool ok;
    bool gotEvent = threadInfo.cq->Next(&tag, &ok);

    if (!gotEvent) {
      // shutting down... completion queue should be empty at this point.
      std::cout << "thread " << threadInfo.threadno << " completionQueue->Next() returned false." << std::endl;
      break;
    }

    if (!ok) {
      // request failed or was terminated.... still call proceed() so that it can be cleaned up
      std::cout << "thread " << threadInfo.threadno << " ERROR in completionQueue->Next()" << std::endl;
    }

    CallData::TaggedPtrType taggedPtr = CallData::TaggedPtrType::fromTaggedPtrBits(tag);
    CallData* callData = taggedPtr.ptr();
    callData->proceed(ok, taggedPtr.tag());
  }

  std::cout << "GRPCServer thread shutting down: " << threadInfo << std::endl;
}


bool GRPCServer::waitForStart() {
  startLatch.wait();
  return true;
}

void solux::GRPCServer::shutdown() {
  std::cout << "Shutting down grpc server." << std::endl;

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
