#include "RpcStatus.h"

#include <cstddef>
#include <span>
#include <tuple>
#include <vector>

#include <hpp_proto/binpb.hpp>

#include "luxir/api/padded_input.h"

namespace luxir::rpc {

// google.protobuf.Any and google.rpc.Status, non-owning, wire tags per the
// public definitions (any.proto, status.proto).
struct Any {
  std::string_view type_url;
  ::hpp_proto::bytes_view value;
};

struct Status {
  std::string_view message;
  std::span<const Any> details;
  int32_t code = 0;
};

auto pb_meta(const Any&) -> std::tuple<
    ::hpp_proto::field_meta<1, &Any::type_url, ::hpp_proto::field_option::utf8_validation>,
    ::hpp_proto::field_meta<2, &Any::value, ::hpp_proto::field_option::none>>;

auto pb_meta(const Status&) -> std::tuple<
    ::hpp_proto::field_meta<1, &Status::code, ::hpp_proto::field_option::none, ::hpp_proto::vint64_t>,
    ::hpp_proto::field_meta<2, &Status::message, ::hpp_proto::field_option::utf8_validation>,
    ::hpp_proto::field_meta<3, &Status::details, ::hpp_proto::field_option::none>>;

}  // namespace luxir::rpc

namespace luxir {

std::string encodeRpcStatusDetails(int32_t code, std::string_view message,
                                   const luxir::api::Error& error) {
  std::vector<std::byte> errorBytes;
  if (!luxir::api::encode(error, errorBytes)) return {};
  rpc::Any detail{ERROR_TYPE_URL, ::hpp_proto::bytes_view(errorBytes.data(), errorBytes.size())};
  rpc::Status status{message, std::span<const rpc::Any>(&detail, 1), code};
  std::vector<std::byte> out;
  if (!::hpp_proto::write_binpb(status, out).ok()) return {};
  return std::string((const char*)out.data(), out.size());
}

bool decodeRpcStatusDetails(std::string_view bytes, luxir::api::Error& error,
                            std::pmr::memory_resource& arena) {
  std::pmr::monotonic_buffer_resource scratch;
  auto padded = luxir::api::copyToPaddedInput(
      std::as_bytes(std::span<const char>(bytes.data(), bytes.size())), scratch);
  rpc::Status status;
  if (!::hpp_proto::read_binpb(status, padded, ::hpp_proto::padded_input,
                               ::hpp_proto::alloc_from(scratch)).ok()) {
    return false;
  }
  for (const auto& detail : status.details) {
    if (detail.type_url != ERROR_TYPE_URL) continue;
    // The decoded Error views its input bytes, so they must live in the
    // caller's arena, not the scratch that dies with this call.
    auto errorPadded = luxir::api::copyToPaddedInput(
        std::span<const std::byte>(detail.value.data(), detail.value.size()), arena);
    return luxir::api::decode(error, errorPadded, arena);
  }
  return false;
}

}  // namespace luxir
