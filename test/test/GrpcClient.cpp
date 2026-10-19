// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

// Out-of-line ByteBuffer<->bytes adapters for the gRPC test client. The heavy (en/de)code
// lives in the luxir_proto_concrete lib (luxir::api::encode/decode); this TU only owns the
// grpc::Slice/ByteBuffer glue so the test TUs don't pull it in. See GrpcClient.h.
#include "test/GrpcClient.h"

#include "luxir/api/padded_input.h"

#include <grpcpp/support/slice.h>
#include <span>

namespace luxir::test {
namespace {

template <class Msg>
std::string serialize(const Msg& msg, grpc::ByteBuffer& out) {
  std::vector<std::byte> v;
  if (!luxir::api::encode(msg, v)) return "gRPC: failed to serialize request";
  grpc::Slice slice((const void*)v.data(), v.size());
  out = grpc::ByteBuffer(&slice, 1);
  return {};
}

template <class Msg>
std::string parse(Msg& msg, const grpc::ByteBuffer& in, std::vector<std::byte>& storage,
                  std::pmr::memory_resource& arena) {
  std::vector<grpc::Slice> slices;
  if (auto status = in.Dump(&slices); !status.ok()) return std::string(status.error_message());
  storage.clear();
  std::size_t size = 0;
  for (const auto& s : slices) size += s.size();
  storage.reserve(size + luxir::api::PADDED_PROTO_INPUT_BYTES);
  for (const auto& s : slices) {
    const auto* data = (const std::byte*)s.begin();
    storage.insert(storage.end(), data, data + s.size());
  }
  storage.resize(size + luxir::api::PADDED_PROTO_INPUT_BYTES);
  std::span<const std::byte> payload(storage.data(), size);
  if (!luxir::api::decode(msg, payload, arena)) return "gRPC: failed to parse response";
  return {};
}

}  // namespace

std::string grpcSerialize(const luxir::api::SearchRequest& msg, grpc::ByteBuffer& out) {
  return serialize(msg, out);
}
std::string grpcSerialize(const luxir::api::UpdateRequest& msg, grpc::ByteBuffer& out) {
  return serialize(msg, out);
}
std::string grpcSerialize(const luxir::api::StatsRequest& msg, grpc::ByteBuffer& out) {
  return serialize(msg, out);
}
std::string grpcSerialize(const luxir::api::CreateCollectionRequest& msg, grpc::ByteBuffer& out) {
  return serialize(msg, out);
}
std::string grpcSerialize(const luxir::api::DeleteCollectionRequest& msg, grpc::ByteBuffer& out) {
  return serialize(msg, out);
}
std::string grpcSerialize(const luxir::api::SchemaRequest& msg, grpc::ByteBuffer& out) {
  return serialize(msg, out);
}

std::string grpcParse(luxir::api::SearchResponse& msg, const grpc::ByteBuffer& in,
                      std::vector<std::byte>& storage, std::pmr::memory_resource& arena) {
  return parse(msg, in, storage, arena);
}
std::string grpcParse(luxir::api::UpdateResponse& msg, const grpc::ByteBuffer& in,
                      std::vector<std::byte>& storage, std::pmr::memory_resource& arena) {
  return parse(msg, in, storage, arena);
}
std::string grpcParse(luxir::api::StatsResponse& msg, const grpc::ByteBuffer& in,
                      std::vector<std::byte>& storage, std::pmr::memory_resource& arena) {
  return parse(msg, in, storage, arena);
}
std::string grpcParse(luxir::api::CreateCollectionResponse& msg, const grpc::ByteBuffer& in,
                      std::vector<std::byte>& storage, std::pmr::memory_resource& arena) {
  return parse(msg, in, storage, arena);
}
std::string grpcParse(luxir::api::DeleteCollectionResponse& msg, const grpc::ByteBuffer& in,
                      std::vector<std::byte>& storage, std::pmr::memory_resource& arena) {
  return parse(msg, in, storage, arena);
}
std::string grpcParse(luxir::api::SchemaResponse& msg, const grpc::ByteBuffer& in,
                      std::vector<std::byte>& storage, std::pmr::memory_resource& arena) {
  return parse(msg, in, storage, arena);
}

}  // namespace luxir::test
