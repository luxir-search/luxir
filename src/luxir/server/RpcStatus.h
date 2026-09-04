#pragma once
// google.rpc.Status, the payload gRPC clients expect in the
// grpc-status-details-bin trailer: {code, message, details: [Any]} with the
// luxir.proto.Error packed as the one detail, so generated clients can read a
// failure's kind and code without a Luxir-specific transport convention.
// Hand-declared over hpp-proto: the engine carries no generated googleapis
// classes.

#include <cstdint>
#include <memory_resource>
#include <string>
#include <string_view>

#include "luxir/api/luxir_types.hpp"

namespace luxir {

inline constexpr std::string_view ERROR_TYPE_URL = "type.googleapis.com/luxir.proto.Error";

// The encoded google.rpc.Status for a gRPC status `code` and `message`
// carrying `error` as its detail.
std::string encodeRpcStatusDetails(int32_t code, std::string_view message,
                                   const luxir::api::Error& error);

// The inverse, for clients and tests: the first luxir.proto.Error detail in
// an encoded google.rpc.Status.  `error` views `arena`; `bytes` need not
// outlive the call.
bool decodeRpcStatusDetails(std::string_view bytes, luxir::api::Error& error,
                            std::pmr::memory_resource& arena);

}  // namespace luxir
