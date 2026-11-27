// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include "luxir/util/log.h"

#include <stdexcept>

#include "spdlog/pattern_formatter.h"

namespace luxir::logs {

void appendJsonEscaped(std::string& out, std::string_view text) {
  static constexpr char hex[] = "0123456789abcdef";
  for (char c : text) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if ((unsigned char)c < 0x20) {
          out += "\\u00";
          out += hex[((unsigned char)c >> 4) & 0xf];
          out += hex[(unsigned char)c & 0xf];
        } else {
          out += c;
        }
    }
  }
}

namespace {

void append(spdlog::memory_buf_t& dest, std::string_view text) { dest.append(text.data(), text.data() + text.size()); }

// %k: "file:line" of the call site, or nothing for a line logged without one
// (spdlog::info() rather than LOG_INFO), so no line shows a bare ":".
class SourceFlag final : public spdlog::custom_flag_formatter {
public:
  void format(const spdlog::details::log_msg& msg, const std::tm&, spdlog::memory_buf_t& dest) override {
    if (msg.source.empty()) return;
    std::string_view file = msg.source.filename;
    if (auto slash = file.rfind('/'); slash != std::string_view::npos) file.remove_prefix(slash + 1);
    append(dest, file);
    dest.push_back(':');
    append(dest, std::to_string(msg.source.line));
  }
  std::unique_ptr<custom_flag_formatter> clone() const override { return std::make_unique<SourceFlag>(); }
};

// %q: the message, escaped for a JSON string body.
class JsonMessageFlag final : public spdlog::custom_flag_formatter {
public:
  void format(const spdlog::details::log_msg& msg, const std::tm&, spdlog::memory_buf_t& dest) override {
    std::string escaped;
    escaped.reserve(msg.payload.size() + 16);
    appendJsonEscaped(escaped, std::string_view(msg.payload.data(), msg.payload.size()));
    append(dest, escaped);
  }
  std::unique_ptr<custom_flag_formatter> clone() const override { return std::make_unique<JsonMessageFlag>(); }
};

} // namespace

void setFormat(std::string_view format) {
  const char* pattern;
  if (format == "text") {
    pattern = "%Y-%m-%dT%H:%M:%S.%fZ %l T%t %k %v";
  } else if (format == "json") {
    pattern = R"({"ts":"%Y-%m-%dT%H:%M:%S.%fZ","level":"%l","thread":%t,"src":"%k","msg":"%q"})";
  } else {
    throw std::invalid_argument("log.format must be text or json, not '" + std::string(format) + "'");
  }
  auto formatter = std::make_unique<spdlog::pattern_formatter>(spdlog::pattern_time_type::utc);
  formatter->add_flag<SourceFlag>('k').add_flag<JsonMessageFlag>('q').set_pattern(pattern);
  spdlog::set_formatter(std::move(formatter));
}

} // namespace luxir::logs
