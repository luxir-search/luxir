// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "luxir/api/luxir_types.hpp"
#include "luxir/util/ApiError.h"

#include <fmt/format.h>

#include <cassert>
#include <charconv>
#include <cmath>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace luxir::coerce {

// Single implementation of the Val -> field-native conversion rules.
// Index-time handlers and query-time building share these so both sides
// coerce identically: a doc ingested via one rule must be findable by
// querying the same literal.  FieldType::coerceColInt64 / coerceTerm are the
// per-field entry points (see FieldType.h); the functions here are the
// shared core those overrides call.

// Strict whole-token numeric parses: empty input, leading/trailing garbage,
// or overflow yields nullopt (no partial acceptance).
inline std::optional<int64_t> parseInt64(std::string_view s) {
  int64_t v;
  auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), v);
  if (ec != std::errc() || ptr != s.data() + s.size()) return std::nullopt;
  return v;
}

inline std::optional<double> parseDouble(std::string_view s) {
  double v;
  auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), v);
  if (ec != std::errc() || ptr != s.data() + s.size()) return std::nullopt;
  return v;
}

// Explicit null (JSON null) and an unset oneof both mean "no value".
inline bool isNull(const api::Val& val) {
  return std::holds_alternative<std::monostate>(val.kind)
      || std::holds_alternative<google::protobuf::NullValue>(val.kind);
}

inline bool isArray(const api::Val& val) {
  return std::holds_alternative<api::ArrVal>(val.kind)
      || std::holds_alternative<api::ArrStr>(val.kind)
      || std::holds_alternative<api::ArrInt>(val.kind)
      || std::holds_alternative<api::ArrFloat>(val.kind)
      || std::holds_alternative<api::ArrDouble>(val.kind)
      || std::holds_alternative<api::ArrBin>(val.kind);
}

// Short description of a Val for coercion error messages.
inline std::string describe(const api::Val& val) {
  if (auto s = std::get_if<std::string_view>(&val.kind)) return fmt::format("'{}'", *s);
  if (auto i = std::get_if<int64_t>(&val.kind)) return fmt::format("{}", *i);
  if (auto d = std::get_if<double>(&val.kind)) return fmt::format("{}", *d);
  if (auto f = std::get_if<float>(&val.kind)) return fmt::format("{}", *f);
  if (auto b = std::get_if<bool>(&val.kind)) return *b ? "true" : "false";
  if (isNull(val)) return "null";
  if (isArray(val)) return "an array value";
  if (std::holds_alternative<api::Vector>(val.kind)) return "a vector value";
  if (std::holds_alternative<api::ArrVector>(val.kind)) return "a vector array value";
  return "an unsupported value kind";
}

[[noreturn]] inline void throwCoerce(std::string_view fieldName, const api::Val& val,
                                     std::string_view target) {
  throw RequestError(fmt::format("field '{}': cannot coerce {} to {}",
                                       fieldName, describe(val), target), "invalid_value");
}

// int64 range bounds exactly representable as doubles: [-2^63, 2^63).
inline constexpr double INT64_LO = -9223372036854775808.0;
inline constexpr double INT64_HI = 9223372036854775808.0;

// Wrap a scalar as a Val for error reporting and for the mixed-value paths
// that deliberately route through FieldType's virtual coercion entry points.
inline api::Val scalarVal(auto value) {
  api::Val val;
  val.kind = value;
  return val;
}

// Typed scalar entry points let homogeneous array consumers select the Val arm
// once, outside their element loop. They intentionally mirror the Val coercion
// contract below; scalarVal is only called on a failing typed-coercion path.
template<typename T>
inline int64_t toInt64Scalar(T value, std::string_view fieldName) {
  using V = std::remove_cvref_t<T>;
  if constexpr (std::is_same_v<V, int64_t>) {
    return value;
  } else if constexpr (std::is_same_v<V, std::string_view>) {
    if (auto parsed = parseInt64(value)) return *parsed;
    if (auto parsed = parseDouble(value)) {
      if (*parsed >= INT64_LO && *parsed < INT64_HI
          && std::trunc(*parsed) == *parsed) {
        return (int64_t)*parsed;
      }
      throw RequestError(fmt::format(
          "field '{}': cannot use '{}' as an integer (value is not integral)",
          fieldName, value), "invalid_value");
    }
    throw RequestError(fmt::format(
        "field '{}': cannot parse '{}' as an integer", fieldName, value), "invalid_value");
  } else if constexpr (std::is_same_v<V, double>) {
    if (value >= INT64_LO && value < INT64_HI && std::trunc(value) == value) {
      return (int64_t)value;
    }
    api::Val val = scalarVal(value);
    throwCoerce(fieldName, val, "an integer (value is not integral)");
  } else if constexpr (std::is_same_v<V, float>) {
    double widened = (double)value;
    if (widened >= INT64_LO && widened < INT64_HI
        && std::trunc(widened) == widened) {
      return (int64_t)widened;
    }
    api::Val val = scalarVal(value);
    throwCoerce(fieldName, val, "an integer (value is not integral)");
  } else {
    api::Val val = scalarVal(value);
    throwCoerce(fieldName, val, "an integer");
  }
}

inline int64_t toInt64(const api::Val& val, std::string_view fieldName) {
  // expected arm first, coercions after
  if (auto i = std::get_if<int64_t>(&val.kind)) return toInt64Scalar(*i, fieldName);
  if (auto s = std::get_if<std::string_view>(&val.kind)) return toInt64Scalar(*s, fieldName);
  if (auto d = std::get_if<double>(&val.kind)) return toInt64Scalar(*d, fieldName);
  if (auto f = std::get_if<float>(&val.kind)) return toInt64Scalar(*f, fieldName);
  throwCoerce(fieldName, val, "an integer");
}

template<typename T>
inline double toDoubleScalar(T value, std::string_view fieldName) {
  using V = std::remove_cvref_t<T>;
  if constexpr (std::is_same_v<V, double>) {
    return value;
  } else if constexpr (std::is_same_v<V, float>) {
    return (double)value;
  } else if constexpr (std::is_same_v<V, int64_t>) {
    return (double)value;
  } else if constexpr (std::is_same_v<V, std::string_view>) {
    if (auto parsed = parseDouble(value)) return *parsed;
    throw RequestError(fmt::format(
        "field '{}': cannot parse '{}' as a number", fieldName, value), "invalid_value");
  } else {
    api::Val val = scalarVal(value);
    throwCoerce(fieldName, val, "a number");
  }
}

inline double toDouble(const api::Val& val, std::string_view fieldName) {
  if (auto d = std::get_if<double>(&val.kind)) return toDoubleScalar(*d, fieldName);
  if (auto f = std::get_if<float>(&val.kind)) return toDoubleScalar(*f, fieldName);
  if (auto i = std::get_if<int64_t>(&val.kind)) return toDoubleScalar(*i, fieldName);
  if (auto s = std::get_if<std::string_view>(&val.kind)) return toDoubleScalar(*s, fieldName);
  throwCoerce(fieldName, val, "a number");
}

template<typename T>
inline float toFloatScalar(T value, std::string_view fieldName) {
  using V = std::remove_cvref_t<T>;
  if constexpr (std::is_same_v<V, float>) return value;
  else return (float)toDoubleScalar(value, fieldName);
}

inline float toFloat(const api::Val& val, std::string_view fieldName) {
  if (auto f = std::get_if<float>(&val.kind)) return *f;
  return (float)toDouble(val, fieldName);
}

// Minimum buffer size for toText's numeric renderings (int64 needs 20 bytes,
// shortest-round-trip double up to 24).
inline constexpr size_t TEXT_BUF_SIZE = 32;

template<typename T>
inline std::string_view toTextScalar(T value, std::string_view fieldName,
                                     std::span<char> buf) {
  using V = std::remove_cvref_t<T>;
  if constexpr (std::is_same_v<V, std::string_view>) {
    return value;
  } else if constexpr (std::is_same_v<V, ::hpp_proto::bytes_view>) {
    return std::string_view((const char*)value.data(), value.size());
  } else if constexpr (std::is_same_v<V, bool>) {
    return value ? std::string_view("true") : std::string_view("false");
  } else if constexpr (std::is_same_v<V, int64_t>
                       || std::is_same_v<V, double>
                       || std::is_same_v<V, float>) {
    assert(buf.size() >= TEXT_BUF_SIZE);
    char* end = std::to_chars(buf.data(), buf.data() + buf.size(), value).ptr;
    return std::string_view(buf.data(), end - buf.data());
  } else {
    api::Val val = scalarVal(value);
    throwCoerce(fieldName, val, "text");
  }
}

// Term/text rendering (the lossless direction): numeric and bool arms render
// a canonical form into buf ("3" for int 3 AND double 3.0 - shortest
// round-trip to_chars) and return a view of it; string/bytes arms pass
// through as views of the original value.  Callers must consume the result
// before buf is reused or dies.
inline std::string_view toText(const api::Val& val, std::string_view fieldName,
                               std::span<char> buf) {
  if (auto s = std::get_if<std::string_view>(&val.kind)) return toTextScalar(*s, fieldName, buf);
  if (auto b = std::get_if<::hpp_proto::bytes_view>(&val.kind)) return toTextScalar(*b, fieldName, buf);
  if (auto b = std::get_if<bool>(&val.kind)) return toTextScalar(*b, fieldName, buf);
  if (auto i = std::get_if<int64_t>(&val.kind)) return toTextScalar(*i, fieldName, buf);
  if (auto d = std::get_if<double>(&val.kind)) return toTextScalar(*d, fieldName, buf);
  if (auto f = std::get_if<float>(&val.kind)) return toTextScalar(*f, fieldName, buf);
  throwCoerce(fieldName, val, "text");
}

// Visit the elements of an array-arm Val as scalar Vals, for element-wise
// coercion in multi-valued handlers.  Returns false if val is not an array.
template <typename F>
bool forEachElement(const api::Val& val, F&& fn) {
  if (auto a = std::get_if<api::ArrStr>(&val.kind)) {
    for (auto s : a->v) fn(scalarVal(s));
    return true;
  }
  if (auto a = std::get_if<api::ArrInt>(&val.kind)) {
    for (auto i : a->v) fn(scalarVal(i));
    return true;
  }
  if (auto a = std::get_if<api::ArrDouble>(&val.kind)) {
    for (auto d : a->v) fn(scalarVal(d));
    return true;
  }
  if (auto a = std::get_if<api::ArrFloat>(&val.kind)) {
    for (auto f : a->v) fn(scalarVal(f));
    return true;
  }
  if (auto a = std::get_if<api::ArrBin>(&val.kind)) {
    for (auto b : a->v) fn(scalarVal(b));
    return true;
  }
  if (auto a = std::get_if<api::ArrVal>(&val.kind)) {
    for (const auto& v : a->v) fn(v);
    return true;
  }
  return false;
}

// Append one dense vector to floats and return its element count.
inline int32_t appendVector(const api::Val& one, std::string_view fieldName,
                            int32_t ordinal, std::vector<float>& floats) {
  size_t start = floats.size();
  // Error-message prefix, formatted lazily so the success path does no formatting.
  auto prefix = [&]() { return ordinal < 0 ? std::string() : fmt::format("vector {}: ", ordinal); };
  auto appendDouble = [&](double d, size_t index) {
    float f = (float)d;
    if (!std::isfinite(f)) throw RequestError(fmt::format(
        "field '{}': {}element {} is outside finite float32 range", fieldName, prefix(), index), "invalid_value");
    floats.push_back(f);
  };
  if (const auto* vec = std::get_if<api::Vector>(&one.kind)) {
    if (!vec->f32.has_value()) throw RequestError(fmt::format(
        "field '{}': {}unsupported or unset vector encoding", fieldName, prefix()), "invalid_value");
    floats.insert(floats.end(), vec->f32->v.begin(), vec->f32->v.end());
  } else if (const auto* arr = std::get_if<api::ArrFloat>(&one.kind)) {
    floats.insert(floats.end(), arr->v.begin(), arr->v.end());
  } else if (const auto* arr = std::get_if<api::ArrDouble>(&one.kind)) {
    for (size_t i = 0; i < arr->v.size(); i++) appendDouble(arr->v[i], i);
  } else if (const auto* arr = std::get_if<api::ArrInt>(&one.kind)) {
    for (int64_t value : arr->v) floats.push_back((float)value);
  } else if (const auto* arr = std::get_if<api::ArrVal>(&one.kind)) {
    for (size_t i = 0; i < arr->v.size(); i++) {
      const auto& kind = arr->v[i].kind;
      if (const auto* value = std::get_if<float>(&kind)) floats.push_back(*value);
      else if (const auto* value = std::get_if<double>(&kind)) appendDouble(*value, i);
      else if (const auto* value = std::get_if<int64_t>(&kind)) floats.push_back((float)*value);
      else throw RequestError(fmt::format("field '{}': {}element {} is not a number",
                                                fieldName, prefix(), i), "invalid_value");
    }
  } else if (ordinal >= 0) {
    throw RequestError(fmt::format("field '{}': vector {} is not a vector",
                                         fieldName, ordinal), "invalid_value");
  } else {
    throwCoerce(fieldName, one, "a vector");
  }
  return (int32_t)(floats.size() - start);
}

// Append the dense vectors represented by val to floats, one contiguous run per
// vector, recording each vector's element count in lens. Returns true when val
// is a list and false when it is one vector.
inline bool toVectors(const api::Val& val, std::string_view fieldName,
                      std::vector<float>& floats, std::vector<int32_t>& lens) {
  floats.clear(); lens.clear();
  auto append = [&](const api::Val& one, int32_t ordinal) {
    lens.push_back(appendVector(one, fieldName, ordinal, floats));
  };
  if (const auto* arr = std::get_if<api::ArrVector>(&val.kind)) {
    for (size_t i = 0; i < arr->v.size(); i++) {
      api::Val one;
      one.kind = arr->v[i];
      append(one, (int32_t)i);
    }
    return true;
  }
  if (const auto* arr = std::get_if<api::ArrVal>(&val.kind)) {
    bool firstIsNumber = !arr->v.empty()
        && (std::holds_alternative<int64_t>(arr->v.front().kind)
            || std::holds_alternative<double>(arr->v.front().kind)
            || std::holds_alternative<float>(arr->v.front().kind));
    if (!firstIsNumber) {
      for (size_t i = 0; i < arr->v.size(); i++) append(arr->v[i], (int32_t)i);
      return true;
    }
  }
  append(val, -1);
  return false;
}

} // namespace luxir::coerce
