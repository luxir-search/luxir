// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include "luxir/server/HttpRouteRegistry.h"

#include <algorithm>
#include <array>
#include <stdexcept>

namespace luxir {

namespace {

std::vector<std::string_view> splitPath(std::string_view path) {
  std::vector<std::string_view> out;
  if (path.starts_with('/')) path.remove_prefix(1);
  while (true) {
    auto slash = path.find('/');
    out.push_back(path.substr(0, slash));
    if (slash == std::string_view::npos) break;
    path.remove_prefix(slash + 1);
  }
  return out;
}

bool isCapture(std::string_view segment) {
  return segment.size() > 2 && segment.front() == '{' && segment.back() == '}';
}

}  // namespace

const std::string* HttpPluginRequest::pathParam(std::string_view name) const {
  for (const auto& [key, value] : pathParams) {
    if (key == name) return &value;
  }
  return nullptr;
}

const std::string* HttpPluginRequest::queryParam(std::string_view name) const {
  const std::string* found = nullptr;
  for (const auto& [key, value] : queryParams) {
    if (key == name) found = &value;
  }
  return found;
}

void HttpRouteRegistry::add(std::string method, std::string pattern, HttpHandler handler, bool mutating) {
  if (isFrozen) throw std::logic_error("http route registry is frozen; routes must be added during startup");
  constexpr std::array<std::string_view, 5> methods = {"GET", "POST", "PUT", "DELETE", "HEAD"};
  if (std::ranges::find(methods, std::string_view(method)) == methods.end())
    throw std::invalid_argument("http route '" + pattern + "': unsupported method '" + method + "'");
  if (!handler) throw std::invalid_argument("http route '" + method + " " + pattern + "' has no handler");
  auto fail = [&](std::string_view why) {
    throw std::invalid_argument("http route '" + pattern + "': " + std::string(why));
  };
  if (pattern.find('?') != std::string::npos || pattern.ends_with('/')) fail("not a path pattern");
  auto parts = splitPath(pattern);
  std::vector<std::string> segments;
  for (auto part : parts) {
    if (part.empty()) fail("empty path segment");
    if (part.find_first_of("{}") != std::string_view::npos && !isCapture(part)) fail("malformed capture");
    segments.emplace_back(part);
  }
  if (segments.size() >= 2 && segments[0] == "_plugins") {
    if (isCapture(segments[1])) fail("the segment after /_plugins/ must be literal");
  } else if (segments.size() >= 3 && segments[0] == "collections" && segments[1] == "{collection}") {
    constexpr std::array<std::string_view, 4> builtins = {"_search", "_update", "_schema", "_stats"};
    if (std::ranges::find(builtins, std::string_view(segments.back())) != builtins.end())
      fail("a built-in endpoint name cannot end a plugin route");
    if (std::ranges::find(segments, std::string("_snapshot")) != segments.end())
      fail("_snapshot paths are reserved");
    if (isCapture(segments[2])) fail("the segment after {collection} must be literal");
  } else {
    fail("plugin routes live under /_plugins/<name>/ or /collections/{collection}/<_endpoint>");
  }
  for (const auto& route : list) {
    if (route.pattern == pattern) {
      if (route.method == method) fail("duplicate route for " + method);
      continue;
    }
    // Two patterns overlap when some path could match both: the same segment
    // count and no position where both are literals that differ (a capture
    // matches anything). Matching would otherwise depend on registration order.
    if (route.segments.size() != segments.size()) continue;
    bool overlaps = true;
    for (size_t i = 0; i < segments.size() && overlaps; i++) {
      if (!isCapture(segments[i]) && !isCapture(route.segments[i]) && segments[i] != route.segments[i])
        overlaps = false;
    }
    if (overlaps) fail("overlaps the route '" + route.pattern + "'");
  }
  auto& allow = allowByPattern[pattern];
  if (!allow.empty()) allow += ", ";
  allow += method;
  list.push_back({std::move(method), std::move(pattern), std::move(segments), std::move(handler), mutating});
}

std::optional<HttpRouteRegistry::Match> HttpRouteRegistry::match(std::string_view path) const {
  if (list.empty()) return std::nullopt;
  auto parts = splitPath(path);
  std::optional<Match> found;
  for (const auto& route : list) {
    if (route.segments.size() != parts.size()) continue;
    std::vector<std::pair<std::string, std::string>> params;
    bool ok = true;
    for (size_t i = 0; i < parts.size() && ok; i++) {
      const auto& segment = route.segments[i];
      if (isCapture(segment)) {
        params.emplace_back(segment.substr(1, segment.size() - 2), parts[i]);
      } else {
        ok = segment == parts[i];
      }
    }
    if (!ok) continue;
    // Patterns never overlap (add rejects that), so every match shares one pattern.
    if (!found) {
      found.emplace();
      found->params = std::move(params);
      found->allow = allowByPattern.find(route.pattern)->second;
    }
    found->routes.push_back(&route);
  }
  return found;
}

}  // namespace luxir
