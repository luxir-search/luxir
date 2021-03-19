
#include <string>
#include <grpcpp/grpcpp.h>
#include <google/protobuf/text_format.h>
#include <grpcpp/health_check_service_interface.h>
#include <grpcpp/ext/proto_server_reflection_plugin.h>
#include "GRPCServer.h"
#include "SoluxNode.h"
#include "solux/index/IndexWriter.h"
#include "solux/util/solux_util.h"

using grpc::Server;
using grpc::ServerBuilder;
using grpc::ServerContext;
using grpc::Status;

using namespace solux;

// can call this example with evans via:
// $ echo '{"name":"dude"}' | evans -r cli call solux.Greeter.SayHello
class GreeterServiceImpl final : public Greeter::Service {
  Status SayHello(ServerContext* context, const HelloRequest* request,
                  HelloReply* reply) override {
    unused(context);
    std::string prefix("Hello ");
    reply->set_message(prefix + request->name());
    return Status::OK;
  }
};

class IndexerServiceImpl final : public Indexer::Service {
public:
  IndexerServiceImpl(GRPCServer& server) : server(server) {
  }

private:
  GRPCServer& server;

  Status Update(ServerContext* context, const solux::proto::UpdateRequest* request,
                  HelloReply* reply) override {
    unused(context);
    std::cout << "GRPCServer peer=" << context->peer() << std::endl;

    std::shared_ptr<Collection> collection;

    if (request->collection().name_size() == 0) {
      // TODO: do we support default collections (implicitly defined by something like an api-key?)
    }

    std::shared_ptr<Library> library = server.getSoluxNode().getLibrary(nullptr, "");
    for (int i=0; i<request->collection().name_size(); i++) {
      // TODO: walk from our implicit root to find the correct collection.
      if (i == request->collection().name_size()-1) {
        std::cout << "looking up collection name " << request->collection().name(i) << std::endl;

        // last element in path, so get collection.
        collection = server.getSoluxNode().getCollection(library.get(), request->collection().name(i));
        // TODO: handle lookup failure
      } else {
        // not last element... get sub-library
        library = server.getSoluxNode().getLibrary(library.get(), request->collection().name(i));
        // TODO: handle lookup failure
      }
    }


    if (request->docs_size() > 0) {
      std::cout << "\tindexer got docs: " << request->docs_size() << std::endl;
      auto shard = collection->getShard();
      auto iw = shard->getIndexWriter();
      Inverter& inverter = iw->getInverter();

      std::string reqStr;
      google::protobuf::TextFormat::PrintToString(*request, &reqStr);
      std::cout << "REQ:( " << reqStr << " )" << std::endl;

      std::vector<Inverter::SegFieldPos*> segFields;
      // int ndocs = request->docs_size();
      for (auto& doc : request->docs()) {
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
    if (request->has_columns()) {
      std::cout << "\tindexer got columns: " << request->columns().columns_size() << std::endl;

    }


    reply->set_message("Indexing Response");
    return Status::OK;
  }
};



GRPCServer::GRPCServer()
  : startLatch(1) {

}

// adapted from the grpc helloworld example
void solux::GRPCServer::run() {
  std::string server_address("0.0.0.0:50051");
  GreeterServiceImpl service;
  IndexerServiceImpl indexerService(*this);

  grpc::EnableDefaultHealthCheckService(true);
  grpc::reflection::InitProtoReflectionServerBuilderPlugin();
  ServerBuilder builder;
  // Listen on the given address without any authentication mechanism.
  builder.AddListeningPort(server_address, grpc::InsecureServerCredentials());
  // Register "service" as the instance through which we'll communicate with
  // clients. In this case it corresponds to a *synchronous* service.
  builder.RegisterService(&service);
  builder.RegisterService(&indexerService);
  // Finally assemble the server.
  this->server = builder.BuildAndStart();
  std::cout << "GRPCServer listening on " << server_address << std::endl;

  startLatch.count_down();

  // Wait for the server to shutdown. Note that some other thread must be
  // responsible for shutting down the server for this call to ever return.
  server->Wait();
}

bool GRPCServer::waitForStart() {
  startLatch.wait();
  return true;
}

void solux::GRPCServer::shutdown() {
  std::cout << "Shutting down grpc server." << std::endl;
  server->Shutdown();
}
