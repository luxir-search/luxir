// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <string>
#include <vector>
#include <optional>
#include <stdexcept>

#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <glaze/glaze.hpp>
#include <glaze/json/generic.hpp>

#include "TestUtils.h"

namespace luxir::test {

namespace beast = boost::beast;
namespace http = beast::http;
namespace net = boost::asio;
using tcp = net::ip::tcp;

// Synchronous one-shot HTTP request against a locally running HttpServer.
// Returns the full (de-chunked) response so NDJSON bodies arrive as one string.
inline http::response<http::string_body>
httpRequest(int port, http::verb method, std::string_view target, std::string body = {},
            std::string_view contentType = "application/json") {
  net::io_context ioc;
  tcp::resolver resolver(ioc);
  beast::tcp_stream stream(ioc);
  auto results = resolver.resolve("127.0.0.1", std::to_string(port));
  stream.connect(results);

  http::request<http::string_body> req(method, target, 11);
  req.set(http::field::host, "127.0.0.1");
  req.set(http::field::content_type, contentType);
  if (!body.empty()) req.body() = std::move(body);
  req.prepare_payload();
  http::write(stream, req);

  beast::flat_buffer buffer;
  http::response<http::string_body> res;
  http::read(stream, buffer, res);

  beast::error_code ec;
  stream.socket().shutdown(tcp::socket::shutdown_both, ec);
  return res;
}

// Fluent query builder mirroring LocalReq's surface, but executing over HTTP and
// parsing the NDJSON response back into Doc objects.  Lets one test drive both
// the in-process engine (LocalReq) and the HTTP transport with the same shape.
class HttpReq {
public:
  explicit HttpReq(int port) : port_(port) {}

  HttpReq& collection(std::string_view n) { collection_ = n; return *this; }
  HttpReq& matchQuery(std::string_view f, std::string_view v) { field_ = f; value_ = v; return *this; }
  HttpReq& limit(int64_t n) { limit_ = n; return *this; }
  HttpReq& offset(int64_t n) { offset_ = n; return *this; }
  HttpReq& batchSize(int64_t n) { batch_ = n; return *this; }
  HttpReq& fields(std::initializer_list<std::string> fs) { fields_.assign(fs); return *this; }
  HttpReq& withStats() { count_ = true; scores_ = true; return *this; }

  // The Luxir JSON dialect for SearchRequest: one op "q" holding a top_docs with a
  // match query in sugar form ({"<field>": <value>}).
  std::string buildJson() const {
    std::string j = R"({"ops":{"q":{"top_docs":{"query":{"match":{)";
    appendJsonStr(j, field_); j += ':'; appendJsonStr(j, value_);
    j += "}}";
    if (limit_)  j += R"(,"limit":)"      + std::to_string(*limit_);
    if (offset_) j += R"(,"offset":)"     + std::to_string(*offset_);
    if (batch_)  j += R"(,"batch_size":)" + std::to_string(*batch_);
    if (count_)  j += R"(,"get_number":true)";
    if (scores_) j += R"(,"get_scores":true)";
    if (!fields_.empty()) {
      j += R"(,"fields":[)";
      for (size_t i = 0; i < fields_.size(); i++) { if (i) j += ','; appendJsonStr(j, fields_[i]); }
      j += ']';
    }
    j += "}}}}";
    return j;
  }

  HttpReq& execute() {
    auto res = httpRequest(port_, http::verb::post,
                           "/collections/" + collection_ + "/_search", buildJson());
    status_ = res.result_int();
    body_ = res.body();
    return *this;
  }

  int status() const { return status_; }
  const std::string& rawResponse() const { return body_; }

  // Number of NDJSON lines (response batches) in the body.
  size_t lineCount() const {
    size_t n = 0;
    for (char c : body_) if (c == '\n') n++;
    return n;
  }

  // q's "found" from the first response line carrying it (0 if absent).
  int64_t found() const {
    for (auto& line : splitLines()) {
      Json root;
      if (glz::read_json(root, line)) continue;
      if (!root.is_object() || !root.contains("ops") || !root["ops"].contains("q")) continue;
      const auto& q = root["ops"]["q"];
      if (!q.contains("found")) continue;
      if (auto* f = q["found"].get_if<int64_t>()) return *f;
    }
    return 0;
  }

  // All docs across all NDJSON lines, parsed back to Doc (null fields skipped,
  // matching LocalReq's "missing field omitted" contract).
  std::vector<Doc> getDocs() const {
    std::vector<Doc> out;
    for (auto& line : splitLines()) {
      Json root;
      if (glz::read_json(root, line)) continue;
      if (!root.is_object() || !root.contains("ops") || !root["ops"].contains("q")) continue;
      const auto& q = root["ops"]["q"];
      if (!q.contains("docs")) continue;
      auto* docs = q["docs"].get_if<Json::array_t>();
      if (!docs) continue;
      for (const Json& d : *docs) {
        auto* obj = d.get_if<Json::object_t>();
        if (!obj) continue;
        Doc doc;
        for (const auto& [key, el] : *obj) {
          FieldVal v;
          if (!toFieldVal(el, v)) continue;  // skip null / unsupported
          doc.push_back(NameVal{key, std::move(v)});
        }
        out.push_back(std::move(doc));
      }
    }
    return out;
  }

  std::vector<std::string> ids(const std::string& idField = "id") const {
    std::vector<std::string> out;
    for (auto& d : getDocs()) {
      if (auto* v = find(d, idField)) {
        if (auto* s = std::get_if<std::string>(v)) out.push_back(*s);
      }
    }
    return out;
  }

private:
  int port_;
  std::string collection_ = "main";
  std::string field_, value_;
  std::optional<int64_t> limit_, offset_, batch_;
  std::vector<std::string> fields_;
  bool count_ = false, scores_ = false;
  int status_ = 0;
  std::string body_;

  static void appendJsonStr(std::string& out, std::string_view s) {
    out += '"';
    for (char c : s) {
      if (c == '"' || c == '\\') out += '\\';
      out += c;
    }
    out += '"';
  }

  std::vector<std::string> splitLines() const {
    std::vector<std::string> lines;
    size_t start = 0;
    while (start < body_.size()) {
      size_t nl = body_.find('\n', start);
      if (nl == std::string::npos) { lines.push_back(body_.substr(start)); break; }
      if (nl > start) lines.push_back(body_.substr(start, nl - start));
      start = nl + 1;
    }
    return lines;
  }

  // Generic JSON DOM with int64 precision (response lines are small; owning is fine here).
  using Json = glz::generic_i64;

  static bool toFieldVal(const Json& el, FieldVal& out) {
    if (auto* s = el.get_if<std::string>()) { out = *s;          return true; }
    if (auto* i = el.get_if<int64_t>())     { out = *i;          return true; }
    if (auto* d = el.get_if<double>())      { out = *d;          return true; }
    if (auto* b = el.get_if<bool>())        { out = *b;          return true; }
    if (auto* a = el.get_if<Json::array_t>()) {
      // Best-effort: homogeneous string or numeric arrays.
      std::vector<std::string> svs;
      std::vector<int64_t> ivs;
      std::vector<double> dvs;
      bool isStr = true, isInt = true, isDbl = true;
      for (const Json& e : *a) {
        if (auto* s = e.get_if<std::string>()) { svs.push_back(*s); isInt = isDbl = false; }
        else if (auto* i = e.get_if<int64_t>()) { ivs.push_back(*i); dvs.push_back((double)*i); isStr = false; }
        else if (auto* d = e.get_if<double>()) { dvs.push_back(*d); isStr = isInt = false; }
        else { return false; }
      }
      if (isStr) out = std::move(svs);
      else if (isInt) out = std::move(ivs);
      else if (isDbl) out = std::move(dvs);
      else return false;
      return true;
    }
    return false;  // null / object
  }
};

} // namespace luxir::test
