// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include "luxir/util/ParamReader.h"

#include <algorithm>
#include <cmath>

namespace luxir {

void ParamReader::markSeen(std::string_view name) const {
  if (std::ranges::find(seen, name) == seen.end()) seen.push_back(name);
}

void ParamReader::fail(std::string_view name, std::string_view what) const {
  throw ParamError(owner + ": '" + std::string(name) + "' " + std::string(what));
}

const api::Val* ParamReader::get(std::string_view name) const {
  markSeen(name);
  const auto* found = entries.find(name);
  if (found == nullptr) return nullptr;
  const api::Val& val = **found;
  return val.isNull() ? nullptr : &val;
}

std::string_view ParamReader::getString(std::string_view name, std::string_view fallback) const {
  const api::Val* val = get(name);
  if (val == nullptr) return fallback;
  if (const auto* s = std::get_if<std::string_view>(&val->kind)) return *s;
  fail(name, "must be a string");
}

std::string_view ParamReader::requireString(std::string_view name) const {
  if (get(name) == nullptr) fail(name, "is required");
  std::string_view s = getString(name);
  if (s.empty()) fail(name, "must not be empty");
  return s;
}

std::optional<int64_t> ParamReader::getInt(std::string_view name) const {
  const api::Val* val = get(name);
  if (val == nullptr) return std::nullopt;
  if (const auto* i = std::get_if<std::int64_t>(&val->kind)) return *i;
  if (const auto* d = std::get_if<double>(&val->kind)) {
    if (std::isfinite(*d) && *d == std::trunc(*d) && std::abs(*d) < 9.0e15) return (int64_t)*d;
  }
  fail(name, "must be an integer");
}

int64_t ParamReader::getInt(std::string_view name, int64_t fallback) const {
  return getInt(name).value_or(fallback);
}

int64_t ParamReader::requireInt(std::string_view name) const {
  auto value = getInt(name);
  if (!value) fail(name, "is required");
  return *value;
}

std::optional<double> ParamReader::getDouble(std::string_view name) const {
  const api::Val* val = get(name);
  if (val == nullptr) return std::nullopt;
  if (const auto* d = std::get_if<double>(&val->kind)) return *d;
  if (const auto* i = std::get_if<std::int64_t>(&val->kind)) return (double)*i;
  if (const auto* f = std::get_if<float>(&val->kind)) return (double)*f;
  fail(name, "must be a number");
}

double ParamReader::getDouble(std::string_view name, double fallback) const {
  return getDouble(name).value_or(fallback);
}

bool ParamReader::getBool(std::string_view name, bool fallback) const {
  const api::Val* val = get(name);
  if (val == nullptr) return fallback;
  if (const auto* b = std::get_if<bool>(&val->kind)) return *b;
  fail(name, "must be true or false");
}

std::vector<std::string_view> ParamReader::getStringList(std::string_view name) const {
  const api::Val* val = get(name);
  std::vector<std::string_view> out;
  if (val == nullptr) return out;
  if (const auto* s = std::get_if<std::string_view>(&val->kind)) {
    out.push_back(*s);
  } else if (const auto* arr = std::get_if<api::ArrStr>(&val->kind)) {
    out.assign(arr->v.begin(), arr->v.end());
  } else if (const auto* vals = std::get_if<api::ArrVal>(&val->kind)) {
    for (const auto& element : vals->v) {
      const auto* s = std::get_if<std::string_view>(&element.kind);
      if (s == nullptr) fail(name, "must be a string or an array of strings");
      out.push_back(*s);
    }
  } else {
    fail(name, "must be a string or an array of strings");
  }
  return out;
}

const api::Map* ParamReader::getObject(std::string_view name) const {
  const api::Val* val = get(name);
  if (val == nullptr) return nullptr;
  if (const auto* map = std::get_if<api::Map>(&val->kind)) return map;
  fail(name, "must be an object");
}

std::vector<api::Val> ParamReader::getList(std::string_view name) const {
  const api::Val* val = get(name);
  std::vector<api::Val> out;
  if (val == nullptr) return out;
  auto push = [&](auto value) {
    api::Val element;
    element.kind = value;
    out.push_back(element);
  };
  if (const auto* vals = std::get_if<api::ArrVal>(&val->kind)) {
    out.assign(vals->v.begin(), vals->v.end());
  } else if (const auto* strings = std::get_if<api::ArrStr>(&val->kind)) {
    for (auto s : strings->v) push(s);
  } else if (const auto* ints = std::get_if<api::ArrInt>(&val->kind)) {
    for (auto i : ints->v) push(i);
  } else if (const auto* doubles = std::get_if<api::ArrDouble>(&val->kind)) {
    for (auto d : doubles->v) push(d);
  } else {
    out.push_back(*val);
  }
  return out;
}

void ParamReader::rejectUnknown() const {
  for (const auto& [key, val] : entries) {
    if (std::ranges::find(seen, key) != seen.end()) continue;
    std::string valid;
    for (std::string_view name : seen) {
      if (!valid.empty()) valid += ", ";
      valid += name;
    }
    throw ParamError(owner + ": unknown parameter '" + std::string(key) + "'"
                     + (valid.empty() ? "; it takes no parameters" : "; valid: " + valid));
  }
}

}  // namespace luxir
