// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "luxir/index/Inverter.h"
#include "luxir/schema/ValCoerce.h"
#include "luxir/util/ApiError.h"
#include "luxir/util/geo.h"

#include <fmt/format.h>

#include <array>
#include <optional>
#include <string_view>

namespace luxir::handler {

// Reads GEO_POINT document values. One point may be written as
//   [lon, lat]                          numeric array, GeoJSON order
//   {"lat": 40.7, "lon": -74.0}         object; numbers or numeric strings
//   {"type": "Point", "coordinates": [lon, lat]}   GeoJSON geometry
//   "40.7,-74.0"                        string, lat first
//   "POINT (-74.0 40.7)"                WKT, case-insensitive
//   "dr5regw3p"                         geohash, decoded to its cell centre
// The array, GeoJSON, "lat,lon" and WKT forms take an optional third (z)
// coordinate, which is rejected unless ignoreZValue is set; then it is
// dropped. Objects take no z key and geohashes have none.
//
// A field value is one point or an array of points. An array whose elements
// are all numbers is one point; any other array lists points, one per
// element. So [-74.0, 40.7] is one point, [[-74.0, 40.7], [-0.1, 51.5]] and
// ["40.7,-74.0", "gcpvj0"] are two.
struct GeoPointWire {
  std::string_view fieldName;
  bool ignoreMalformed = false;
  bool ignoreZValue = false;

  [[noreturn]] void fail(std::string_view detail) const {
    throw DocumentError(fmt::format(
        "field '{}': cannot read GEO_POINT value: {}", fieldName, detail));
  }

  // Calls fn(GeoPoint) for each point of a field value; nulls are skipped.
  // With ignoreMalformed a malformed point is skipped instead of failing the
  // document. Otherwise a malformed point throws, possibly after fn has seen
  // earlier points, so callers buffer the points and index them afterwards.
  template <typename F>
  void forEachPoint(const IndexVal& val, F&& fn) const {
    if (coerce::isNull(val)) return;
    if (const auto* strs = std::get_if<api::ArrStr>(&val.kind)) {
      for (std::string_view s : strs->v) visit(coerce::scalarVal(s), fn);
      return;
    }
    const auto* arr = std::get_if<api::ArrVal>(&val.kind);
    if (arr != nullptr && !allNumeric(*arr)) {
      for (const IndexVal& elem : arr->v) {
        if (!coerce::isNull(elem)) visit(elem, fn);
      }
      return;
    }
    if (coerce::isArray(val) && arraySize(val) == 0) return;
    visit(val, fn);
  }

  // Parses one point in any accepted form.
  GeoPoint parse(const IndexVal& val) const {
    if (const auto* s = std::get_if<std::string_view>(&val.kind)) {
      return parseString(*s);
    }
    if (const auto* m = std::get_if<api::Map>(&val.kind)) return parseObject(*m);
    if (coerce::isArray(val)) return parseCoordinates(val, "[lon, lat]");
    fail(fmt::format("expected [lon, lat], an object, or a string, got {}",
                     coerce::describe(val)));
  }

private:
  template <typename F>
  void visit(const IndexVal& val, F& fn) const {
    if (!ignoreMalformed) {
      fn(parse(val));
      return;
    }
    std::optional<GeoPoint> point;
    try {
      point = parse(val);
    } catch (const DocumentError&) {
      return;
    }
    fn(*point);
  }

  static std::optional<double> number(const IndexVal& val) {
    if (const auto* d = std::get_if<double>(&val.kind)) return *d;
    if (const auto* f = std::get_if<float>(&val.kind)) return (double)*f;
    if (const auto* i = std::get_if<int64_t>(&val.kind)) return (double)*i;
    return std::nullopt;
  }

  static bool allNumeric(const api::ArrVal& arr) {
    for (const IndexVal& elem : arr.v) {
      if (!number(elem)) return false;
    }
    return true;
  }

  static size_t arraySize(const IndexVal& val) {
    size_t n = 0;
    coerce::forEachElement(val, [&](const IndexVal&) { n++; });
    return n;
  }

  static bool space(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }

  static std::string_view trim(std::string_view s) {
    while (!s.empty() && space(s.front())) s.remove_prefix(1);
    while (!s.empty() && space(s.back())) s.remove_suffix(1);
    return s;
  }

  // A decimal number; a leading '+' is accepted, as JSON writers rarely emit
  // one but hand-written strings do.
  static std::optional<double> parseNumber(std::string_view s) {
    if (s.size() > 1 && s.front() == '+' && s[1] != '-' && s[1] != '+') s.remove_prefix(1);
    return coerce::parseDouble(s);
  }

  static bool startsWithNoCase(std::string_view s, std::string_view prefix) {
    if (s.size() < prefix.size()) return false;
    for (size_t i = 0; i < prefix.size(); i++) {
      char c = s[i];
      if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
      if (c != prefix[i]) return false;
    }
    return true;
  }

  // Validates and orders coordinates; `count` is 2 or 3 (z last).
  GeoPoint point(double longitude, double latitude, size_t count) const {
    if (count == 3 && !ignoreZValue) {
      fail("found a z (altitude) coordinate; set ignore_z_value: true on the "
           "field to accept and drop it");
    }
    if (!std::isfinite(latitude) || latitude < -90.0 || latitude > 90.0) {
      fail(fmt::format("latitude {} is not in [-90, 90]", latitude));
    }
    if (!std::isfinite(longitude) || longitude < -180.0 || longitude > 180.0) {
      fail(fmt::format("longitude {} is not in [-180, 180]", longitude));
    }
    return {latitude, longitude};
  }

  // A numeric coordinate array: [lon, lat] or [lon, lat, z].
  GeoPoint parseCoordinates(const IndexVal& val, std::string_view form) const {
    std::array<double, 3> c{};
    size_t n = 0;
    coerce::forEachElement(val, [&](const IndexVal& elem) {
      std::optional<double> v = number(elem);
      if (!v) {
        fail(fmt::format("{} element {} ({}) is not a number", form, n,
                         coerce::describe(elem)));
      }
      if (n < c.size()) c[n] = *v;
      n++;
    });
    if (n != 2 && n != 3) {
      fail(fmt::format("{} needs 2 coordinates (3 with z), got {}", form, n));
    }
    return point(c[0], c[1], n);
  }

  double objectNumber(const IndexVal& val, std::string_view key) const {
    if (std::optional<double> v = number(val)) return *v;
    if (const auto* s = std::get_if<std::string_view>(&val.kind)) {
      if (std::optional<double> v = parseNumber(trim(*s))) return *v;
    }
    fail(fmt::format("\"{}\" must be a number, got {}", key, coerce::describe(val)));
  }

  // {"lat": .., "lon": ..} or GeoJSON {"type": "Point", "coordinates": [..]}.
  GeoPoint parseObject(const api::Map& map) const {
    const IndexVal* lat = nullptr;
    const IndexVal* lon = nullptr;
    const IndexVal* type = nullptr;
    const IndexVal* coordinates = nullptr;
    // Duplicate keys resolve last-wins, as map_view::find does.
    for (const auto& [key, value] : map.fields) {
      if (key == "lat") lat = &*value;
      else if (key == "lon") lon = &*value;
      else if (key == "type") type = &*value;
      else if (key == "coordinates") coordinates = &*value;
      else fail(fmt::format("unknown key \"{}\" in a point object; use lat and lon, "
                            "or GeoJSON type and coordinates", key));
    }
    if (type != nullptr || coordinates != nullptr) {
      if (lat != nullptr || lon != nullptr) {
        fail("a point object takes lat and lon, or type and coordinates, not both");
      }
      const auto* name = type ? std::get_if<std::string_view>(&type->kind) : nullptr;
      if (name == nullptr || !startsWithNoCase(*name, "point") || name->size() != 5) {
        fail("a GeoJSON geo_point needs \"type\": \"Point\"");
      }
      if (coordinates == nullptr || !coerce::isArray(*coordinates)) {
        fail("a GeoJSON Point needs a \"coordinates\" array");
      }
      return parseCoordinates(*coordinates, "coordinates");
    }
    if (lat == nullptr || lon == nullptr) fail("a point object needs both lat and lon");
    return point(objectNumber(*lon, "lon"), objectNumber(*lat, "lat"), 2);
  }

  GeoPoint parseString(std::string_view raw) const {
    std::string_view s = trim(raw);
    if (startsWithNoCase(s, "point")
        && (s.size() == 5 || space(s[5]) || s[5] == '(')) {
      return parseWkt(s.substr(5), raw);
    }
    if (s.contains(',')) return parseLatLon(s, raw);
    if (std::optional<GeoPoint> p = decodeGeohash(s)) return *p;
    fail(fmt::format("'{}' is not \"lat,lon\", WKT POINT, or a geohash", raw));
  }

  // "lat,lon" or "lat,lon,z".
  GeoPoint parseLatLon(std::string_view s, std::string_view raw) const {
    std::array<double, 3> c{};
    size_t n = 0;
    while (true) {
      size_t comma = s.find(',');
      std::string_view part = trim(s.substr(0, comma));
      std::optional<double> v = parseNumber(part);
      if (!v || n == c.size()) {
        fail(fmt::format("'{}' is not \"lat,lon\" (2 numbers, 3 with z)", raw));
      }
      c[n++] = *v;
      if (comma == std::string_view::npos) break;
      s.remove_prefix(comma + 1);
    }
    if (n < 2) fail(fmt::format("'{}' is not \"lat,lon\" (2 numbers, 3 with z)", raw));
    return point(c[1], c[0], n);
  }

  // The text after "POINT": [Z] "(" lon lat [z] ")".
  GeoPoint parseWkt(std::string_view s, std::string_view raw) const {
    auto bad = [&]() {
      fail(fmt::format("'{}' is not WKT POINT (lon lat) or POINT Z (lon lat z)", raw));
    };
    s = trim(s);
    if (startsWithNoCase(s, "empty") && trim(s.substr(5)).empty()) {
      fail("POINT EMPTY has no coordinates");
    }
    bool zTag = false;
    if (!s.empty() && (s.front() == 'z' || s.front() == 'Z')) {
      zTag = true;
      s = trim(s.substr(1));
    }
    if (s.size() < 2 || s.front() != '(' || s.back() != ')') bad();
    s = s.substr(1, s.size() - 2);
    std::array<double, 3> c{};
    size_t n = 0;
    while (true) {
      while (!s.empty() && space(s.front())) s.remove_prefix(1);
      if (s.empty()) break;
      size_t end = s.find_first_of(" \t\n\r");
      std::optional<double> v = parseNumber(s.substr(0, end));
      if (!v || n == c.size()) bad();
      c[n++] = *v;
      if (end == std::string_view::npos) break;
      s.remove_prefix(end);
    }
    if (n < 2 || (zTag && n != 3)) bad();
    return point(c[0], c[1], n);
  }

public:
  // The centre of a geohash cell, or nullopt if `hash` is not a geohash of
  // 1 to 12 base-32 characters (either case).
  static std::optional<GeoPoint> decodeGeohash(std::string_view hash) {
    static constexpr std::string_view BASE32 = "0123456789bcdefghjkmnpqrstuvwxyz";
    if (hash.empty() || hash.size() > 12) return std::nullopt;
    double latLo = -90.0, latHi = 90.0, lonLo = -180.0, lonHi = 180.0;
    bool lonBit = true;  // bits alternate, longitude first
    for (char c : hash) {
      if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
      size_t v = BASE32.find(c);
      if (v == std::string_view::npos) return std::nullopt;
      for (int bit = 4; bit >= 0; bit--) {
        bool set = (v >> bit) & 1;
        double& lo = lonBit ? lonLo : latLo;
        double& hi = lonBit ? lonHi : latHi;
        double mid = (lo + hi) / 2.0;
        (set ? lo : hi) = mid;
        lonBit = !lonBit;
      }
    }
    return GeoPoint{(latLo + latHi) / 2.0, (lonLo + lonHi) / 2.0};
  }
};

} // namespace luxir::handler
