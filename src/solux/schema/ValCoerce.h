#pragma once

#include "solux/api/solux_types.hpp"

#include <fmt/format.h>

#include <cassert>
#include <charconv>
#include <cmath>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>

namespace solux::coerce {

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
  return "an unsupported value kind";
}

[[noreturn]] inline void throwCoerce(std::string_view fieldName, const api::Val& val,
                                     std::string_view target) {
  throw std::runtime_error(fmt::format("field '{}': cannot coerce {} to {}",
                                       fieldName, describe(val), target));
}

// int64 range bounds exactly representable as doubles: [-2^63, 2^63).
inline constexpr double INT64_LO = -9223372036854775808.0;
inline constexpr double INT64_HI = 9223372036854775808.0;

inline int64_t toInt64(const api::Val& val, std::string_view fieldName) {
  // expected arm first, coercions after
  if (auto i = std::get_if<int64_t>(&val.kind)) return *i;
  if (auto s = std::get_if<std::string_view>(&val.kind)) {
    if (auto v = parseInt64(*s)) return *v;
    throw std::runtime_error(fmt::format(
        "field '{}': cannot parse '{}' as an integer", fieldName, *s));
  }
  // narrowing double -> int only when integral (NaN/inf/out-of-range fail the checks)
  if (auto d = std::get_if<double>(&val.kind)) {
    if (*d >= INT64_LO && *d < INT64_HI && std::trunc(*d) == *d) return (int64_t)*d;
    throwCoerce(fieldName, val, "an integer (value is not integral)");
  }
  if (auto f = std::get_if<float>(&val.kind)) {
    double d = (double)*f;
    if (d >= INT64_LO && d < INT64_HI && std::trunc(d) == d) return (int64_t)d;
    throwCoerce(fieldName, val, "an integer (value is not integral)");
  }
  throwCoerce(fieldName, val, "an integer");
}

inline double toDouble(const api::Val& val, std::string_view fieldName) {
  if (auto d = std::get_if<double>(&val.kind)) return *d;
  if (auto f = std::get_if<float>(&val.kind)) return (double)*f;
  // widening int -> double accepted (inexact above 2^53, like the proto/JSON ecosystem)
  if (auto i = std::get_if<int64_t>(&val.kind)) return (double)*i;
  if (auto s = std::get_if<std::string_view>(&val.kind)) {
    if (auto v = parseDouble(*s)) return *v;
    throw std::runtime_error(fmt::format(
        "field '{}': cannot parse '{}' as a number", fieldName, *s));
  }
  throwCoerce(fieldName, val, "a number");
}

inline float toFloat(const api::Val& val, std::string_view fieldName) {
  if (auto f = std::get_if<float>(&val.kind)) return *f;
  return (float)toDouble(val, fieldName);
}

// Minimum buffer size for toText's numeric renderings (int64 needs 20 bytes,
// shortest-round-trip double up to 24).
inline constexpr size_t TEXT_BUF_SIZE = 32;

// Term/text rendering (the lossless direction): numeric and bool arms render
// a canonical form into buf ("3" for int 3 AND double 3.0 - shortest
// round-trip to_chars) and return a view of it; string/bytes arms pass
// through as views of the original value.  Callers must consume the result
// before buf is reused or dies.
inline std::string_view toText(const api::Val& val, std::string_view fieldName,
                               std::span<char> buf) {
  if (auto s = std::get_if<std::string_view>(&val.kind)) return *s;
  if (auto b = std::get_if<::hpp_proto::bytes_view>(&val.kind)) {
    return std::string_view((const char*)b->data(), b->size());
  }
  if (auto b = std::get_if<bool>(&val.kind)) {
    return *b ? std::string_view("true") : std::string_view("false");
  }
  assert(buf.size() >= TEXT_BUF_SIZE);
  char* end;
  if (auto i = std::get_if<int64_t>(&val.kind)) {
    end = std::to_chars(buf.data(), buf.data() + buf.size(), *i).ptr;
  } else if (auto d = std::get_if<double>(&val.kind)) {
    end = std::to_chars(buf.data(), buf.data() + buf.size(), *d).ptr;
  } else if (auto f = std::get_if<float>(&val.kind)) {
    end = std::to_chars(buf.data(), buf.data() + buf.size(), *f).ptr;
  } else {
    throwCoerce(fieldName, val, "text");
  }
  return std::string_view(buf.data(), end - buf.data());
}

// Wrap a scalar as a Val (for routing typed values through the coercion
// virtuals and for element-wise array handling).
inline api::Val scalarVal(auto x) {
  api::Val v;
  v.kind = x;
  return v;
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

} // namespace solux::coerce
