// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

// Shared gRPC test client over the server's generic ByteBuffer transport.
//
// The server is an AsyncGenericService routed by method name (no protobuf-generated stubs),
// so tests build a CONCRETE luxir::api request, serialize it to a grpc::ByteBuffer (via
// luxir::api::encode), call by method name, and parse the ByteBuffer reply back into a
// CONCRETE luxir::api response. Responses are NON-OWNING: the Reply holder keeps the raw
// reply bytes + a pmr arena alive so the parsed view stays valid while the test inspects it.
//
// Method names are string constants here (mirroring the server's lookupMethod table) so the
// test side no longer depends on the templated luxir.service.hpp / *.pb.hpp.

#include <memory>
#include <memory_resource>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <gtest/gtest.h>
#include <grpcpp/impl/client_unary_call.h>
#include <grpcpp/impl/rpc_method.h>
#include <grpcpp/support/byte_buffer.h>
#include <grpcpp/support/sync_stream.h>

#include "luxir/api/luxir_types.hpp"

namespace luxir::test {

struct TrivialSearchRequest {
  using Pair = std::pair<std::string_view,
                         ::hpp_proto::indirect_view<luxir::api::SearchOp>>;

  luxir::api::SearchOp op;
  Pair opPair{"q", {&op}};
  luxir::api::SearchRequest request;

  TrivialSearchRequest() {
    op.kind.emplace<luxir::api::TopDocs>().limit = 0;
    request.ops = luxir::api::map_view<
        std::string_view, ::hpp_proto::indirect_view<luxir::api::SearchOp>>(
        std::span<const Pair>(&opPair, 1));
  }
};

// RPC method paths (match GRPCServer.cpp lookupMethod()).
namespace rpc {
inline constexpr const char* Search            = "/luxir.Searcher/Search";
inline constexpr const char* Update            = "/luxir.Indexer/Update";
inline constexpr const char* UpdateStream      = "/luxir.Indexer/UpdateStream";
inline constexpr const char* SetSchema         = "/luxir.Admin/SetSchema";
inline constexpr const char* GetSchema         = "/luxir.Admin/GetSchema";
inline constexpr const char* CreateCollection  = "/luxir.Admin/CreateCollection";
inline constexpr const char* DeleteCollection  = "/luxir.Admin/DeleteCollection";
inline constexpr const char* Stats             = "/luxir.Admin/Stats";
}  // namespace rpc

// Out-of-line (de)serialization for the RPC message types (defined in GrpcClient.cpp). The
// heavy (en/de)code lives in the luxir_proto_concrete lib (luxir::api::encode/decode); these
// are thin ByteBuffer<->bytes adapters. Return "" on success, else the error message.
std::string grpcSerialize(const luxir::api::SearchRequest& msg, grpc::ByteBuffer& out);
std::string grpcSerialize(const luxir::api::UpdateRequest& msg, grpc::ByteBuffer& out);
std::string grpcSerialize(const luxir::api::StatsRequest& msg, grpc::ByteBuffer& out);
std::string grpcSerialize(const luxir::api::CreateCollectionRequest& msg, grpc::ByteBuffer& out);
std::string grpcSerialize(const luxir::api::DeleteCollectionRequest& msg, grpc::ByteBuffer& out);
std::string grpcSerialize(const luxir::api::SchemaRequest& msg, grpc::ByteBuffer& out);
// `storage` retains the raw reply bytes the non-owning `msg` views; `arena` backs nested
// message allocations. Both must outlive any read of `msg`.
std::string grpcParse(luxir::api::SearchResponse& msg, const grpc::ByteBuffer& in,
                      std::vector<std::byte>& storage, std::pmr::memory_resource& arena);
std::string grpcParse(luxir::api::UpdateResponse& msg, const grpc::ByteBuffer& in,
                      std::vector<std::byte>& storage, std::pmr::memory_resource& arena);
std::string grpcParse(luxir::api::StatsResponse& msg, const grpc::ByteBuffer& in,
                      std::vector<std::byte>& storage, std::pmr::memory_resource& arena);
std::string grpcParse(luxir::api::CreateCollectionResponse& msg, const grpc::ByteBuffer& in,
                      std::vector<std::byte>& storage, std::pmr::memory_resource& arena);
std::string grpcParse(luxir::api::DeleteCollectionResponse& msg, const grpc::ByteBuffer& in,
                      std::vector<std::byte>& storage, std::pmr::memory_resource& arena);
std::string grpcParse(luxir::api::SchemaResponse& msg, const grpc::ByteBuffer& in,
                      std::vector<std::byte>& storage, std::pmr::memory_resource& arena);

// Holds a parsed non-owning concrete reply + the storage/arena it views. Reuse across reads:
// each parse() reclaims the prior parse's storage. `msg` is the public surface (Val accessors
// / std::get_if on .kind, map_view .find / .at).
template <class Msg>
struct Reply {
  std::vector<std::byte> storage;
  std::pmr::monotonic_buffer_resource arena;
  Msg msg;

  std::string parse(const grpc::ByteBuffer& in) {
    arena.release();
    return grpcParse(msg, in, storage, arena);
  }
};

// Bidi-streaming client: concrete requests in, Reply<Response> out, over a generic
// ByteBuffer ClientReaderWriter.
template <typename Request, typename Response>
class HppClientReaderWriter {
  std::unique_ptr<grpc::ClientReaderWriter<grpc::ByteBuffer, grpc::ByteBuffer>> stream_;

public:
  HppClientReaderWriter(grpc::ChannelInterface* channel, const char* methodName,
                        grpc::ClientContext* context)
    : stream_(grpc::internal::ClientReaderWriterFactory<grpc::ByteBuffer, grpc::ByteBuffer>::Create(
        channel,
        grpc::internal::RpcMethod(methodName, nullptr, grpc::internal::RpcMethod::BIDI_STREAMING),
        context)) {}

  bool Write(const Request& request, grpc::WriteOptions options = grpc::WriteOptions()) {
    grpc::ByteBuffer buf;
    if (auto err = grpcSerialize(request, buf); !err.empty()) { ADD_FAILURE() << err; return false; }
    return stream_->Write(buf, options);
  }

  void WriteLast(const Request& request, grpc::WriteOptions options) {
    grpc::ByteBuffer buf;
    if (auto err = grpcSerialize(request, buf); !err.empty()) { ADD_FAILURE() << err; return; }
    stream_->WriteLast(buf, options);
  }

  bool WritesDone() { return stream_->WritesDone(); }

  bool Read(Reply<Response>* reply) {
    grpc::ByteBuffer buf;
    if (!stream_->Read(&buf)) return false;
    if (auto err = reply->parse(buf); !err.empty()) { ADD_FAILURE() << err; return false; }
    return true;
  }

  grpc::Status Finish() { return stream_->Finish(); }
};

template <typename Request, typename Response>
grpc::Status hppUnaryCall(grpc::ChannelInterface* channel, const char* methodName,
                          grpc::ClientContext* context, const Request& request,
                          Reply<Response>* reply) {
  grpc::ByteBuffer reqBuf;
  if (auto err = grpcSerialize(request, reqBuf); !err.empty()) {
    return grpc::Status(grpc::StatusCode::INTERNAL, err);
  }
  grpc::ByteBuffer respBuf;
  grpc::internal::RpcMethod method(methodName, nullptr, grpc::internal::RpcMethod::NORMAL_RPC);
  auto grpcStatus = grpc::internal::BlockingUnaryCall<grpc::ByteBuffer, grpc::ByteBuffer>(
    channel, method, context, reqBuf, &respBuf);
  if (!grpcStatus.ok()) return grpcStatus;
  if (auto err = reply->parse(respBuf); !err.empty()) {
    return grpc::Status(grpc::StatusCode::INTERNAL, err);
  }
  return grpc::Status::OK;
}

}  // namespace luxir::test
