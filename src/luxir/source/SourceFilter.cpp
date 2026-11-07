// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include "SourceFilter.h"

#include <new>
#include <stdexcept>

#include "SourceJson.h"
#include "luxir/api/build.h"

namespace luxir::source {

namespace {

// Whether `pattern` matches all of `s`, or with prefixOnly some string that
// starts with `s`. '*' matches any run of characters.
bool glob(std::string_view pattern, std::string_view s, bool prefixOnly) {
  size_t p = 0, i = 0, star = std::string_view::npos, mark = 0;
  while (i < s.size()) {
    if (p < pattern.size() && pattern[p] == '*') {
      star = p++;
      mark = i;
    } else if (p < pattern.size() && pattern[p] == s[i]) {
      p++;
      i++;
    } else if (star != std::string_view::npos) {
      p = star + 1;
      i = ++mark;
    } else {
      return false;
    }
  }
  if (prefixOnly) return true;
  while (p < pattern.size() && pattern[p] == '*') p++;
  return p == pattern.size();
}

void appendUtf8(uint32_t cp, std::string& out) {
  if (cp < 0x80) {
    out += (char)cp;
  } else if (cp < 0x800) {
    out += (char)(0xC0 | (cp >> 6));
    out += (char)(0x80 | (cp & 0x3F));
  } else if (cp < 0x10000) {
    out += (char)(0xE0 | (cp >> 12));
    out += (char)(0x80 | ((cp >> 6) & 0x3F));
    out += (char)(0x80 | (cp & 0x3F));
  } else {
    out += (char)(0xF0 | (cp >> 18));
    out += (char)(0x80 | ((cp >> 12) & 0x3F));
    out += (char)(0x80 | ((cp >> 6) & 0x3F));
    out += (char)(0x80 | (cp & 0x3F));
  }
}

bool hex4(std::string_view s, size_t i, uint32_t& cp) {
  if (i + 4 > s.size()) return false;
  cp = 0;
  for (size_t k = i; k < i + 4; k++) {
    char c = s[k];
    cp <<= 4;
    if (c >= '0' && c <= '9') cp |= (uint32_t)(c - '0');
    else if (c >= 'a' && c <= 'f') cp |= (uint32_t)(c - 'a' + 10);
    else if (c >= 'A' && c <= 'F') cp |= (uint32_t)(c - 'A' + 10);
    else return false;
  }
  return true;
}

// Appends the text of the JSON string body `raw` (between its quotes).
bool unescape(std::string_view raw, std::string& out) {
  for (size_t i = 0; i < raw.size(); i++) {
    if (raw[i] != '\\') {
      out += raw[i];
      continue;
    }
    if (++i >= raw.size()) return false;
    switch (raw[i]) {
      case 'b': out += '\b'; break;
      case 'f': out += '\f'; break;
      case 'n': out += '\n'; break;
      case 'r': out += '\r'; break;
      case 't': out += '\t'; break;
      case 'u': {
        uint32_t cp;
        if (!hex4(raw, i + 1, cp)) return false;
        i += 4;
        uint32_t low;
        if (cp >= 0xD800 && cp < 0xDC00 && i + 2 < raw.size() && raw[i + 1] == '\\' && raw[i + 2] == 'u' &&
            hex4(raw, i + 3, low) && low >= 0xDC00 && low < 0xE000) {
          cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
          i += 6;
        }
        appendUtf8(cp, out);
        break;
      }
      default: out += raw[i]; break;  // '"', '\\', '/'
    }
  }
  return true;
}

}  // namespace

SourceFilter::SourceFilter(std::span<const std::string_view> includes, std::span<const std::string_view> excludes) {
  if (!includes.empty() && !excludes.empty()) {
    throw std::invalid_argument("source includes and excludes are mutually exclusive");
  }
  for (auto lists : {includes, excludes}) {
    for (auto pattern : lists) {
      if (pattern.empty()) throw std::invalid_argument("source includes and excludes must not hold an empty pattern");
    }
  }
  this->includes.assign(includes.begin(), includes.end());
  this->excludes.assign(excludes.begin(), excludes.end());
}

SourceFilter::Decision SourceFilter::decide(std::string_view path) const {
  if (!excludes.empty()) {
    for (const auto& pattern : excludes) {
      if (glob(pattern, path, false)) return Decision::DROP;
    }
    return Decision::DESCEND;
  }
  for (const auto& pattern : includes) {
    if (glob(pattern, path, false)) return Decision::KEEP;
  }
  std::string below;
  below.reserve(path.size() + 1);
  below.append(path).push_back('.');
  for (const auto& pattern : includes) {
    if (glob(pattern, below, true)) return Decision::DESCEND;
  }
  return Decision::DROP;
}

bool SourceFilter::filterJson(std::string_view json, std::string& out) const {
  size_t i = 0;
  detail::skipSpace(json, i);
  if (i >= json.size() || json[i] != '{') return false;
  std::string path;
  size_t kept = 0;
  return filterObject(json, i, path, out, kept);
}

// s[i] is '{'. Appends the object with the members the filter keeps (even with
// none kept) and advances i past it; `kept` counts the members.
bool SourceFilter::filterObject(std::string_view s, size_t& i, std::string& path, std::string& out,
                                size_t& kept) const {
  out += '{';
  i++;
  std::string key;
  for (;;) {
    detail::skipSpace(s, i);
    if (i >= s.size()) return false;
    if (s[i] == '}') break;
    if (s[i] == ',') {
      i++;
      continue;
    }
    if (s[i] != '"') return false;
    size_t keyStart = i + 1;
    if (!detail::skipString(s, i)) return false;
    std::string_view rawKey = s.substr(keyStart, i - 1 - keyStart);
    detail::skipSpace(s, i);
    if (i >= s.size() || s[i] != ':') return false;
    i++;
    detail::skipSpace(s, i);
    if (i >= s.size()) return false;

    key.clear();
    if (!unescape(rawKey, key)) return false;
    size_t pathLen = path.size();
    if (!path.empty()) path += '.';
    path += key;
    size_t outLen = out.size();
    if (kept > 0) out += ',';
    out.append("\"").append(rawKey).append("\":");
    size_t valueStart = i;
    bool keep = false;
    switch (decide(path)) {
      case Decision::KEEP:
        if (!detail::skipValue(s, i)) return false;
        out.append(s.substr(valueStart, i - valueStart));
        keep = true;
        break;
      case Decision::DROP:
        if (!detail::skipValue(s, i)) return false;
        break;
      case Decision::DESCEND: {
        size_t inner = 0;
        if (s[i] == '{') {
          if (!filterObject(s, i, path, out, inner)) return false;
        } else if (s[i] == '[') {
          if (!filterArray(s, i, path, out, inner)) return false;
        } else {
          if (!detail::skipValue(s, i)) return false;
          // A scalar lies under no include; an exclude would have dropped it.
          if (!excludes.empty()) out.append(s.substr(valueStart, i - valueStart));
          inner = excludes.empty() ? 0 : 1;
        }
        keep = inner > 0 || !excludes.empty();
        break;
      }
    }
    path.resize(pathLen);
    if (keep) kept++;
    else out.resize(outLen);
  }
  i++;
  out += '}';
  return true;
}

// s[i] is '['. Elements sit at the array's path: objects and nested arrays are
// filtered, scalars stay only with excludes. With includes an emptied
// container element is dropped.
bool SourceFilter::filterArray(std::string_view s, size_t& i, std::string& path, std::string& out,
                               size_t& kept) const {
  out += '[';
  i++;
  for (;;) {
    detail::skipSpace(s, i);
    if (i >= s.size()) return false;
    if (s[i] == ']') break;
    if (s[i] == ',') {
      i++;
      continue;
    }
    size_t outLen = out.size();
    if (kept > 0) out += ',';
    size_t valueStart = i;
    size_t inner = 0;
    if (s[i] == '{') {
      if (!filterObject(s, i, path, out, inner)) return false;
    } else if (s[i] == '[') {
      if (!filterArray(s, i, path, out, inner)) return false;
    } else {
      if (!detail::skipValue(s, i)) return false;
      if (!excludes.empty()) out.append(s.substr(valueStart, i - valueStart));
      inner = excludes.empty() ? 0 : 1;
    }
    if (inner > 0 || !excludes.empty()) kept++;
    else out.resize(outLen);
  }
  i++;
  out += ']';
  return true;
}

api::Map SourceFilter::filterMap(const api::Map& doc, std::pmr::memory_resource& mr) const {
  std::string path;
  return filterMapAt(doc, path, mr);
}

api::Map SourceFilter::filterMapAt(const api::Map& map, std::string& path, std::pmr::memory_resource& mr) const {
  using IV = ::hpp_proto::indirect_view<api::Val>;
  using Pair = std::pair<std::string_view, IV>;
  api::Map result;
  if (map.fields.empty()) return result;
  Pair* kept = (Pair*)mr.allocate(sizeof(Pair) * map.fields.size(), alignof(Pair));
  size_t n = 0;
  for (const auto& [key, val] : map.fields) {
    size_t pathLen = path.size();
    if (!path.empty()) path += '.';
    path += key;
    const api::Val* out = nullptr;
    switch (decide(path)) {
      case Decision::KEEP: out = &*val; break;
      case Decision::DROP: break;
      case Decision::DESCEND: out = filterVal(*val, path, mr); break;
    }
    path.resize(pathLen);
    if (out) new (&kept[n++]) Pair(key, IV(const_cast<api::Val*>(out)));
  }
  result.fields = api::map_view<std::string_view, IV>(std::span<const Pair>(kept, n));
  return result;
}

// The filtered form of a value the filter descends into, or null when nothing
// of it is kept.
const api::Val* SourceFilter::filterVal(const api::Val& val, std::string& path, std::pmr::memory_resource& mr) const {
  if (const auto* map = std::get_if<api::Map>(&val.kind)) {
    api::Map filtered = filterMapAt(*map, path, mr);
    if (filtered.fields.empty() && excludes.empty()) return nullptr;
    auto* out = api::build::allocMessage<api::Val>(mr);
    out->kind = filtered;
    return out;
  }
  if (const auto* arr = std::get_if<api::ArrVal>(&val.kind)) {
    std::span<const api::Val> elements;
    api::Val* kept = api::build::allocArray(elements, arr->v.size(), mr);
    size_t n = 0;
    for (const auto& element : arr->v) {
      if (const auto* filtered = filterVal(element, path, mr)) kept[n++] = *filtered;
    }
    if (n == 0 && excludes.empty()) return nullptr;
    auto* out = api::build::allocMessage<api::Val>(mr);
    out->kind = api::ArrVal{elements.first(n)};
    return out;
  }
  // A scalar or typed array lies under no include; an exclude would have dropped it.
  return excludes.empty() ? nullptr : &val;
}

}  // namespace luxir::source
