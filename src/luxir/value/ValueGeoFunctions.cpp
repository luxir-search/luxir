// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

// Geo functions of the value language beyond geo_distance(): coordinates of a
// GEO_POINT column, distances and bearings between arbitrary points, a box
// test, and grid cell keys (geohash and geotile).

#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>

#include "luxir/util/geo.h"
#include "luxir/value/ValueFunctionSupport.h"

namespace luxir {
namespace {

using namespace valuefn;

constexpr ValueFunction::Operand RAW{ValueFunction::DateUnit::RAW};
constexpr ValueFunction::Operand GEO_POINT{
    .unit = ValueFunction::DateUnit::RAW, .kind = ValueFunction::Kind::GEO_POINT};
constexpr ValueFunction::Operand CONSTANT_RAW{ValueFunction::DateUnit::RAW, false, true};

constexpr double PI = std::numbers::pi;
constexpr double DEGREES_TO_RADIANS = 0.017453292519943295;

// ---------------------------------------------------------------------------
// geo_lat(point) and geo_lon(point): the decoded coordinates, in degrees. A
// multi-valued point gives an array.
ResolvedValue coordinateResolve(std::span<const ResolvedValue> args) {
  return {valueArray(args[0].type) ? ValueType::DOUBLE_ARRAY : ValueType::DOUBLE,
          ValueNature::NUMBER};
}

double latitudeOf(int64_t packed) { return geo::decodeLatitude(geo::unpackLatitude(packed)); }
double longitudeOf(int64_t packed) { return geo::decodeLongitude(geo::unpackLongitude(packed)); }

template <double (*Coordinate)(int64_t), int Limit>
struct Coordinate1 {
  static ValueResult apply(const ValueResult& input) {
    if (!input.valid) return ValueResult::missing(ValueType::DOUBLE);
    return ValueResult::floating(Coordinate(input.intValue));
  }

  static ValueResult point(BoundValueProgram& program, const ValueNode& node,
                           int32_t docid, float score) {
    ValueResult input = program.evalNode(node.children[0], docid, score);
    if (!input.valid) return ValueResult::missing(node.type);
    if (valueArray(input.type)) {
      return ValueResult::arrayValue(node.type, nodeIndex(program, node), docid, score,
                                     input.array.size);
    }
    return apply(input);
  }

  static ValueResult element(BoundValueProgram& program, const ValueNode& node,
                             const ValueArrayRef& array, int64_t index) {
    ValueResult points = program.evalNode(node.children[0], array.docid, array.score);
    return apply(program.evalArrayElement(points.array, index));
  }

  static ValueBounds bounds(const ValueNode& node, std::span<const ValueBounds> args) {
    if (args[0].alwaysMissing) return ValueBounds::unbounded(node.type, true, true);
    ValueBounds out = ValueBounds::floating(-Limit, Limit, args[0].mayBeMissing);
    out.type = node.type;
    return out;
  }
};

template <double (*Coordinate)(int64_t), int Limit>
constexpr ValueFunction coordinate(std::string_view name) {
  using F = Coordinate1<Coordinate, Limit>;
  return {ValueOpcode::NONE, name, coordinateResolve, F::point, evalFunctionBatch, F::bounds,
          F::element, 1, 1, {GEO_POINT}};
}

// ---------------------------------------------------------------------------
// Distances and bearing between two points given as latitude/longitude pairs
// in degrees (any numeric expressions, including geo_lat() and geo_lon()).
ResolvedValue pairResolve(std::span<const ResolvedValue> args) {
  requireNumeric(args);
  requireScalars(args);
  return {ValueType::DOUBLE, ValueNature::NUMBER};
}

constexpr double MAX_ARC_METERS = geo::EARTH_MEAN_RADIUS_METERS * PI;

double arcMeters(double lat1, double lon1, double lat2, double lon2) {
  return geo::haversinMeters(lat1, lon1, lat2, lon2);
}

double arcKilometers(double lat1, double lon1, double lat2, double lon2) {
  return geo::haversinMeters(lat1, lon1, lat2, lon2) / 1000.0;
}

// Equirectangular approximation: cheap, and accurate for short distances.
double planeMeters(double lat1, double lon1, double lat2, double lon2) {
  double x = (lon2 - lon1) * DEGREES_TO_RADIANS * std::cos((lat2 + lat1) / 2.0 * DEGREES_TO_RADIANS);
  double y = (lat2 - lat1) * DEGREES_TO_RADIANS;
  return std::sqrt(x * x + y * y) * geo::EARTH_MEAN_RADIUS_METERS;
}

// The initial bearing from the first point to the second, in degrees clockwise
// from north, in [0, 360).
double bearingDegrees(double lat1, double lon1, double lat2, double lon2) {
  double phi1 = lat1 * DEGREES_TO_RADIANS;
  double phi2 = lat2 * DEGREES_TO_RADIANS;
  double delta = (lon2 - lon1) * DEGREES_TO_RADIANS;
  double y = std::sin(delta) * std::cos(phi2);
  double x = std::cos(phi1) * std::sin(phi2) - std::sin(phi1) * std::cos(phi2) * std::cos(delta);
  double degrees = std::atan2(y, x) / DEGREES_TO_RADIANS;
  degrees = std::fmod(degrees + 360.0, 360.0);
  return degrees >= 360.0 ? 0.0 : degrees;
}

template <double (*Fn)(double, double, double, double), int64_t Max>
struct Pair4 {
  static ValueResult point(BoundValueProgram& program, const ValueNode& node,
                           int32_t docid, float score) {
    std::array<double, 4> v;
    for (size_t i = 0; i < 4; i++) {
      ValueResult arg = program.evalNode(node.children[i], docid, score);
      if (!arg.valid) return ValueResult::missing(node.type);
      v[i] = numberOf(arg);
    }
    double out = Fn(v[0], v[1], v[2], v[3]);
    if (!std::isfinite(out)) runtimeInvalid(node, docid, "produced NaN or infinity");
    return ValueResult::floating(out);
  }

  static ValueBounds bounds(const ValueNode& node, std::span<const ValueBounds> args) {
    bool mayBeMissing = false;
    for (const ValueBounds& arg : args) {
      if (arg.alwaysMissing) return ValueBounds::unbounded(node.type, true, true);
      mayBeMissing = mayBeMissing || arg.mayBeMissing;
    }
    if constexpr (Max > 0) return boundedResult(node, 0.0, (double)Max, mayBeMissing);
    return ValueBounds::unbounded(node.type, mayBeMissing);
  }
};

template <double (*Fn)(double, double, double, double), int64_t Max>
constexpr ValueFunction pair(std::string_view name) {
  using F = Pair4<Fn, Max>;
  return {ValueOpcode::NONE, name, pairResolve, F::point, evalFunctionBatch, F::bounds,
          evalScalarOnlyElement, 4, 4, {RAW, RAW, RAW, RAW}};
}

// ---------------------------------------------------------------------------
// geo_in_box(point, min_lat, min_lon, max_lat, max_lon): 1 when a point lies in
// the box, else 0. The box is tested in the encoded coordinate space, exactly
// as the geo_box query does, and wraps across the dateline when min_lon is
// greater than max_lon. A multi-valued point is inside when any value is.
ResolvedValue boxResolve(std::span<const ResolvedValue>) {
  return {ValueType::INT64, ValueNature::NUMBER};
}

// The encoded bounds live in the node: intValue packs (min lat, max lat) and
// intValue2 packs (min lon, max lon).
void prepareBox(ValueNode& node, std::span<const ValueNode* const> args) {
  double minLat = numberOf(*args[1]);
  double minLon = numberOf(*args[2]);
  double maxLat = numberOf(*args[3]);
  double maxLon = numberOf(*args[4]);
  geo::checkLatitude(minLat);
  geo::checkLatitude(maxLat);
  geo::checkLongitude(minLon);
  geo::checkLongitude(maxLon);
  if (minLat > maxLat) throw std::runtime_error("min_lat must not exceed max_lat");
  node.intValue = geo::pack(geo::encodeLatitudeCeil(minLat), geo::encodeLatitude(maxLat));
  node.intValue2 = geo::pack(geo::encodeLongitudeCeil(minLon), geo::encodeLongitude(maxLon));
}

bool insideBox(const ValueNode& node, int64_t packed) {
  int32_t minLat = geo::unpackLatitude(node.intValue);
  int32_t maxLat = geo::unpackLongitude(node.intValue);
  int32_t lat = geo::unpackLatitude(packed);
  int32_t lon = geo::unpackLongitude(packed);
  return lat >= minLat && lat <= maxLat
      && geo::longitudeInRange(lon, geo::unpackLatitude(node.intValue2),
                               geo::unpackLongitude(node.intValue2));
}

ValueResult boxPoint(BoundValueProgram& program, const ValueNode& node,
                     int32_t docid, float score) {
  ValueResult input = program.evalNode(node.children[0], docid, score);
  if (!input.valid) return ValueResult::missing(node.type);
  if (!valueArray(input.type)) return ValueResult::integer(insideBox(node, input.intValue) ? 1 : 0);
  for (int64_t i = 0; i < input.array.size; i++) {
    ValueResult item = program.evalArrayElement(input.array, i);
    if (item.valid && insideBox(node, item.intValue)) return ValueResult::integer(1);
  }
  return ValueResult::integer(0);
}

ValueBounds boxBounds(const ValueNode& node, std::span<const ValueBounds> args) {
  if (args[0].alwaysMissing) return ValueBounds::unbounded(node.type, true, true);
  ValueBounds out = ValueBounds::integer(0, 1, args[0].mayBeMissing);
  out.type = node.type;
  return out;
}

// ---------------------------------------------------------------------------
// Grid cell keys, so documents can be sorted or bucketed by cell.
//
// geohash(lat, lon, precision) is the standard base32 geohash of precision 1
// to 12 characters, as an int64 holding 5 bits per character, first character
// highest. Keys of one precision sort like the geohash strings do.
//
// geotile(lat, lon, zoom) is the web-mercator tile at zoom 0 to 29, packed as
// Elasticsearch does: (zoom << 58) | (x << 29) | y.
ResolvedValue cellResolve(std::span<const ResolvedValue> args) {
  requireNumeric(args);
  requireScalars(args);
  return {ValueType::INT64, ValueNature::NUMBER};
}

void checkCoordinates(const ValueNode& node, double lat, double lon, int32_t docid) {
  if (!geo::validLatitude(lat) || !geo::validLongitude(lon)) {
    runtimeInvalid(node, docid, "was given a coordinate outside [-90, 90] / [-180, 180]");
  }
}

void prepareCell(int64_t low, int64_t high, std::string_view what, ValueNode& node,
                 std::span<const ValueNode* const> args) {
  double value = numberOf(*args[2]);
  if (value != std::floor(value) || value < (double)low || value > (double)high) {
    throw std::runtime_error(fmt::format("{} must be an integer from {} to {}", what, low, high));
  }
  node.intValue = (int64_t)value;
}

void prepareGeohash(ValueNode& node, std::span<const ValueNode* const> args) {
  prepareCell(1, 12, "precision", node, args);
}

void prepareGeotile(ValueNode& node, std::span<const ValueNode* const> args) {
  prepareCell(0, 29, "zoom", node, args);
}

int64_t geohashOf(double lat, double lon, int precision) {
  double minLat = -90.0;
  double maxLat = 90.0;
  double minLon = -180.0;
  double maxLon = 180.0;
  uint64_t hash = 0;
  bool even = true;
  for (int bit = 0; bit < precision * 5; bit++) {
    double& low = even ? minLon : minLat;
    double& high = even ? maxLon : maxLat;
    double mid = (low + high) / 2.0;
    bool upper = (even ? lon : lat) >= mid;
    hash = (hash << 1) | (upper ? 1 : 0);
    (upper ? low : high) = mid;
    even = !even;
  }
  return (int64_t)hash;
}

constexpr double MAX_MERCATOR_LATITUDE = 85.0511287798066;

int64_t geotileOf(double lat, double lon, int zoom) {
  lat = std::clamp(lat, -MAX_MERCATOR_LATITUDE, MAX_MERCATOR_LATITUDE);
  int64_t tiles = (int64_t)1 << zoom;
  int64_t x = std::clamp((int64_t)std::floor((lon + 180.0) / 360.0 * (double)tiles),
                         (int64_t)0, tiles - 1);
  double sine = std::sin(lat * DEGREES_TO_RADIANS);
  double yTile = (0.5 - std::log((1.0 + sine) / (1.0 - sine)) / (4.0 * PI)) * (double)tiles;
  int64_t y = std::clamp((int64_t)std::floor(yTile), (int64_t)0, tiles - 1);
  return ((int64_t)zoom << 58) | (x << 29) | y;
}

template <int64_t (*Fn)(double, double, int)>
ValueResult cellPoint(BoundValueProgram& program, const ValueNode& node,
                      int32_t docid, float score) {
  ValueResult lat = program.evalNode(node.children[0], docid, score);
  ValueResult lon = program.evalNode(node.children[1], docid, score);
  if (!lat.valid || !lon.valid) return ValueResult::missing(node.type);
  checkCoordinates(node, numberOf(lat), numberOf(lon), docid);
  return ValueResult::integer(Fn(numberOf(lat), numberOf(lon), (int)node.intValue));
}

ValueBounds cellBounds(const ValueNode& node, std::span<const ValueBounds> args) {
  if (args[0].alwaysMissing || args[1].alwaysMissing) {
    return ValueBounds::unbounded(node.type, true, true);
  }
  return ValueBounds::unbounded(node.type, args[0].mayBeMissing || args[1].mayBeMissing);
}

constexpr ValueFunction FUNCTIONS[] = {
    coordinate<latitudeOf, 90>("geo_lat"),
    coordinate<longitudeOf, 180>("geo_lon"),
    // Lucene expressions' haversin(): kilometers.
    pair<arcKilometers, (int64_t)(MAX_ARC_METERS / 1000.0) + 1>("haversin"),
    pair<arcMeters, (int64_t)MAX_ARC_METERS + 1>("arc_distance"),
    pair<planeMeters, 0>("plane_distance"),
    pair<bearingDegrees, 360>("geo_bearing"),
    {ValueOpcode::NONE, "geo_in_box", boxResolve, boxPoint, evalFunctionBatch, boxBounds,
     evalScalarOnlyElement, 5, 5,
     {GEO_POINT, CONSTANT_RAW, CONSTANT_RAW, CONSTANT_RAW, CONSTANT_RAW}, nullptr, prepareBox},
    {ValueOpcode::NONE, "geohash", cellResolve, cellPoint<geohashOf>, evalFunctionBatch,
     cellBounds, evalScalarOnlyElement, 3, 3, {RAW, RAW, CONSTANT_RAW}, nullptr, prepareGeohash},
    {ValueOpcode::NONE, "geotile", cellResolve, cellPoint<geotileOf>, evalFunctionBatch,
     cellBounds, evalScalarOnlyElement, 3, 3, {RAW, RAW, CONSTANT_RAW}, nullptr, prepareGeotile},
};

static_assert(badOperandFlags(FUNCTIONS).empty(), badOperandFlags(FUNCTIONS));

} // namespace

std::span<const ValueFunction> valueGeoFunctions() {
  return FUNCTIONS;
}

} // namespace luxir
