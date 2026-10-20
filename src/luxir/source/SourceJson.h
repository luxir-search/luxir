// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

// Byte-level JSON helpers for capturing the documents of an update as submitted.
// They never build a value tree: a document is kept as its own bytes, so key
// order, duplicate keys and number spelling survive.

#include <string>
#include <string_view>
#include <vector>

namespace luxir::source {

inline bool jsonSpace(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }

// Appends `in` to `out` without the whitespace that lies outside strings.  The
// input is not validated: it has already been parsed, so only strings and their
// escapes need tracking.
inline void minifyJson(std::string_view in, std::string& out) {
  out.reserve(out.size() + in.size());
  size_t run = 0;  // start of the pending verbatim run
  bool inString = false;
  for (size_t i = 0; i < in.size(); i++) {
    char c = in[i];
    if (inString) {
      if (c == '\\') i++;
      else if (c == '"') inString = false;
    } else if (c == '"') {
      inString = true;
    } else if (jsonSpace(c)) {
      out.append(in.substr(run, i - run));
      run = i + 1;
    }
  }
  out.append(in.substr(run));
}

namespace detail {

inline void skipSpace(std::string_view s, size_t& i) {
  while (i < s.size() && jsonSpace(s[i])) i++;
}

// Advances past the string starting at s[i] == '"'.
inline bool skipString(std::string_view s, size_t& i) {
  for (i++; i < s.size(); i++) {
    if (s[i] == '\\') i++;
    else if (s[i] == '"') { i++; return true; }
  }
  return false;
}

// Advances past one JSON value starting at s[i] (no leading whitespace).  Only
// the structure is followed; scalars are skipped to their delimiter.
inline bool skipValue(std::string_view s, size_t& i) {
  if (i >= s.size()) return false;
  if (s[i] == '"') return skipString(s, i);
  if (s[i] != '{' && s[i] != '[') {
    while (i < s.size() && s[i] != ',' && s[i] != '}' && s[i] != ']' && !jsonSpace(s[i])) i++;
    return true;
  }
  size_t depth = 0;
  while (i < s.size()) {
    char c = s[i];
    if (c == '"') {
      if (!skipString(s, i)) return false;
      continue;
    }
    i++;
    if (c == '{' || c == '[') depth++;
    else if (c == '}' || c == ']') {
      if (--depth == 0) return true;
    }
  }
  return false;
}

}  // namespace detail

namespace detail {

// Collects the elements of the array whose '[' is at body[i].
inline bool arrayElements(std::string_view body, size_t& i, std::vector<std::string_view>& spans) {
  i++;
  for (;;) {
    skipSpace(body, i);
    if (i >= body.size()) return false;
    if (body[i] == ']') { i++; return true; }
    if (body[i] == ',') { i++; continue; }
    size_t start = i;
    if (!skipValue(body, i)) return false;
    spans.push_back(body.substr(start, i - start));
  }
}

inline bool updateDocSpans(std::string_view body, std::vector<std::string_view>& spans) {
  size_t i = 0;
  skipSpace(body, i);
  if (i >= body.size() || body[i] != '{') return false;
  i++;
  bool seen = false;
  for (;;) {
    skipSpace(body, i);
    if (i >= body.size()) return false;
    if (body[i] == '}') return true;
    if (body[i] == ',') { i++; continue; }
    if (body[i] != '"') return false;
    size_t keyStart = i + 1;
    if (!skipString(body, i)) return false;
    std::string_view key = body.substr(keyStart, i - 1 - keyStart);
    skipSpace(body, i);
    if (i >= body.size() || body[i] != ':') return false;
    i++;
    skipSpace(body, i);
    if (key == "docs") {
      if (seen || i >= body.size() || body[i] != '[') return false;
      seen = true;
      if (!arrayElements(body, i, spans)) return false;
    } else if (!skipValue(body, i)) {
      return false;
    }
  }
}

}  // namespace detail

// Finds the elements of the top-level "docs" array of an update request body and
// appends each element's bytes to `spans`.  False if the body is not shaped as
// expected (including a repeated "docs" key), in which case `spans` is cleared
// and the caller captures the documents another way.
inline bool findUpdateDocSpans(std::string_view body, std::vector<std::string_view>& spans) {
  spans.clear();
  if (detail::updateDocSpans(body, spans)) return true;
  spans.clear();
  return false;
}

}  // namespace luxir::source
