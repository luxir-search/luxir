// Out-of-line ByteBuffer<->bytes adapters for the gRPC test client. The heavy (en/de)code
// lives in the solux_proto_concrete lib (solux::api::encode/decode); this TU only owns the
// grpc::Slice/ByteBuffer glue so the test TUs don't pull it in. See GrpcClient.h.
#include "test/GrpcClient.h"

#include <grpcpp/support/slice.h>

namespace solux::test {
namespace {

template <class Msg>
std::string serialize(const Msg& msg, grpc::ByteBuffer& out) {
  std::vector<std::byte> v;
  if (!solux::api::encode(msg, v)) return "gRPC: failed to serialize request";
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
  storage.reserve(size);
  for (const auto& s : slices) {
    const auto* data = (const std::byte*)s.begin();
    storage.insert(storage.end(), data, data + s.size());
  }
  if (!solux::api::decode(msg, storage, arena)) return "gRPC: failed to parse response";
  return {};
}

}  // namespace

std::string grpcSerialize(const solux::api::SearchRequest& msg, grpc::ByteBuffer& out) {
  return serialize(msg, out);
}
std::string grpcSerialize(const solux::api::UpdateRequest& msg, grpc::ByteBuffer& out) {
  return serialize(msg, out);
}
std::string grpcSerialize(const solux::api::HelloRequest& msg, grpc::ByteBuffer& out) {
  return serialize(msg, out);
}

std::string grpcParse(solux::api::SearchResponse& msg, const grpc::ByteBuffer& in,
                      std::vector<std::byte>& storage, std::pmr::memory_resource& arena) {
  return parse(msg, in, storage, arena);
}
std::string grpcParse(solux::api::UpdateResponse& msg, const grpc::ByteBuffer& in,
                      std::vector<std::byte>& storage, std::pmr::memory_resource& arena) {
  return parse(msg, in, storage, arena);
}
std::string grpcParse(solux::api::HelloReply& msg, const grpc::ByteBuffer& in,
                      std::vector<std::byte>& storage, std::pmr::memory_resource& arena) {
  return parse(msg, in, storage, arena);
}

}  // namespace solux::test
