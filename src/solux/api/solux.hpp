// HAND-WRITTEN concrete classes for solux.proto (Hello* demo messages).
// Members within each struct are ordered by descending alignment to minimize padding; pb_meta
// binds by member pointer + explicit tag, so declaration order is free / independent of the wire.
#pragma once

#include <cstdint>
#include <memory_resource>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

#include "solux_types.hpp"

namespace solux::api {

struct HelloRequest {
  std::string_view name;
  std::int32_t response_count = {};
  std::int32_t min_sleep_us = {};
  std::int32_t max_sleep_us = {};
  std::int32_t n_threads = {};
  bool async = {};
  bool debug = {};
};

struct HelloReply {
  std::string_view message;
  std::int32_t response_number = {};
};

#define SOLUX_ENTRY(M)                                                                      \
  bool decode(M &, std::span<const std::byte> data, std::pmr::memory_resource &arena);      \
  bool encode(const M &, std::vector<std::byte> &out);                                      \
  bool write_json(const M &, std::string &out);                                             \
  bool read_json(M &, std::string_view json, std::pmr::memory_resource &arena);
SOLUX_ENTRY(HelloRequest) SOLUX_ENTRY(HelloReply)
#undef SOLUX_ENTRY

} // namespace solux::api
