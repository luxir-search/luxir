// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace luxir {

class LuxirNode;

// One request to a plugin HTTP route. Views are valid for the handler call.
struct HttpPluginRequest {
  LuxirNode& node;
  std::string_view method;  // "GET", "POST", ...
  std::string_view path;    // without the query string
  // The default tenant: plugin routes are not reachable under /tenants/{t}/.
  std::string_view tenant;
  std::string_view body;
  // {name} segments of the route pattern, by name.
  std::vector<std::pair<std::string, std::string>> pathParams;
  // URL query parameters in order (a repeated key appears more than once).
  std::vector<std::pair<std::string, std::string>> queryParams;

  const std::string* pathParam(std::string_view name) const;
  // The last value of a query parameter, or null.
  const std::string* queryParam(std::string_view name) const;
};

// The handler's answer: a body, or a search to run in its place.
struct HttpPluginResponse {
  int status = 200;
  std::string contentType = "application/json";
  std::string body;
  // When set, the server runs this search request body (JSON) against
  // `searchCollection` as a search with default URL options and answers as
  // POST /collections/{searchCollection}/_search would (streaming, errors and
  // all). The request's own URL parameters are not applied to it; they reach
  // the handler as queryParams.
  std::optional<std::string> searchBody;
  std::string searchCollection;
};

// Runs on a task-arena worker, off the connection's I/O thread, so it may
// block on engine work. Throw RequestError (or any ApiError) to answer with
// that error; other exceptions answer 500.
using HttpHandler = std::function<void(HttpPluginRequest& request, HttpPluginResponse& response)>;

struct HttpRoute {
  std::string method;
  std::string pattern;
  std::vector<std::string> segments;  // pattern split on '/', "{name}" for a capture
  HttpHandler handler;
  bool mutating = false;  // refused on a read-only node
};

// Plugin HTTP routes. A pattern is either under /_plugins/ (node level) or
// under /collections/{collection}/ (collection level; the collection must be
// captured by that name). Built-in endpoints always win, so a collection
// pattern may not end in a built-in endpoint name (_search, _update, _schema,
// _stats) or use _snapshot. Patterns may not overlap: two patterns with the
// same segment count must differ at some position where both are literals.
class HttpRouteRegistry {
  std::vector<HttpRoute> list;
  std::map<std::string, std::string, std::less<>> allowByPattern;
  bool isFrozen = false;

public:
  struct Match {
    std::vector<const HttpRoute*> routes;  // every method registered for the path
    std::vector<std::pair<std::string, std::string>> params;
    std::string_view allow;  // the methods, as an Allow header value
  };

  HttpRouteRegistry();
  static HttpRouteRegistry& global();

  // Throws std::invalid_argument on a bad method, pattern, duplicate or
  // overlapping pattern, or a null handler, std::logic_error after freeze.
  void add(std::string method, std::string pattern, HttpHandler handler, bool mutating = false);
  // The routes whose pattern matches `path`, or nullopt.
  std::optional<Match> match(std::string_view path) const;
  const std::vector<HttpRoute>& routes() const { return list; }
  bool frozen() const { return isFrozen; }
  void freeze() { isFrozen = true; }
};

}  // namespace luxir
