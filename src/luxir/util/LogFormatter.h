// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <spdlog/pattern_formatter.h>

namespace luxir {

// One physical line per record, including exceptions, stack traces and values
// supplied by clients. UTC makes records comparable across nodes and days.
class LogFormatter final : public spdlog::formatter {
  spdlog::pattern_formatter prefix{"%Y-%m-%dT%H:%M:%S.%fZ %l T%t %s:%# %v",
                                   spdlog::pattern_time_type::utc};
public:
  void format(const spdlog::details::log_msg& msg, spdlog::memory_buf_t& out) override {
    spdlog::memory_buf_t escaped;
    constexpr char hex[] = "0123456789abcdef";
    for (unsigned char c : msg.payload) {
      if (c == '\n' || c == '\r' || c == '\t' || c == '\\') {
        escaped.push_back('\\');
        escaped.push_back(c == '\n' ? 'n' : c == '\r' ? 'r' : c == '\t' ? 't' : '\\');
      } else if (c < 32 || c == 127) {
        escaped.push_back('\\'); escaped.push_back('x');
        escaped.push_back(hex[c >> 4]); escaped.push_back(hex[c & 15]);
      } else escaped.push_back((char)c);
    }
    auto record = msg;
    record.payload = {escaped.data(), escaped.size()};
    prefix.format(record, out);
  }
  std::unique_ptr<spdlog::formatter> clone() const override {
    return std::make_unique<LogFormatter>();
  }
};

} // namespace luxir
