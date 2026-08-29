#pragma once

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "luxir/util/StrRef.h"
#include "luxir/util/encoding.h"

namespace luxir {

enum class FilterKeyScope : uint8_t {
  SEGMENT_STABLE = 0,
  CORE_STABLE = 1,
  READER_STABLE = 2,
  UNCACHEABLE = 3
};

inline FilterKeyScope strongestFilterKeyScope(FilterKeyScope a,
                                               FilterKeyScope b) {
  return (uint8_t)a >= (uint8_t)b ? a : b;
}

// In-memory keys cannot outlive their process-local encoder, so tags only need to be distinct within a build.
enum class FilterKeyTag : uint8_t {
  ALL = 1,
  NONE = 2,
  TERM = 3,
  PHRASE = 4,
  EXISTS = 5,
  NUMERIC_PREDICATE_RANGE = 6,
  PREFIX = 7,
  TERM_RANGE = 8,
  FUZZY = 9,
  GEO_BOX = 10,
  GEO_DISTANCE = 11,
  KNN = 12,
  BOOLEAN = 13,
  CONSTANT_SCORE = 14,
  BOOST = 15,
  FORCE_PREPARE = 16,
  BOOLEAN_MANDATORY = 17,
  BOOLEAN_OPTIONAL = 18,
  BOOLEAN_PROHIBITED = 19,
  BOOLEAN_FILTER = 20,
  WILDCARD = 21,
  REGEX = 22,
  ANY_OF = 23
};

// Correctness envelope: schemaGen and timeZone always, plus coreGen for CORE_STABLE scope.
struct FilterKeyContext {
  uint64_t schemaGen = 0;
  uint64_t coreGen = 0;
  int32_t fuzzyMaxExpansions = 10000;
  std::string_view timeZone;
};

class FilterKey {
  std::vector<std::byte> bytes_;
  uint64_t hash_ = 0;

  static uint64_t hashBytes(std::span<const std::byte> bytes) {
    return Hash::hash(bytes.data(), bytes.size());
  }

public:
  FilterKey() = default;

  explicit FilterKey(std::vector<std::byte> bytes)
    : bytes_(std::move(bytes)), hash_(hashBytes(bytes_)) {}

  explicit FilterKey(std::string_view bytes)
    : bytes_(std::as_bytes(std::span(bytes)).begin(),
             std::as_bytes(std::span(bytes)).end()),
      hash_(hashBytes(bytes_)) {}

  static FilterKey withHashForTest(std::string_view bytes, uint64_t hash) {
    FilterKey key(bytes);
    key.hash_ = hash;
    return key;
  }

  const std::vector<std::byte>& bytes() const { return bytes_; }
  uint64_t hash() const { return hash_; }

  friend bool operator==(const FilterKey& a, const FilterKey& b) {
    return a.bytes_ == b.bytes_;
  }
};

struct FilterKeyHash {
  size_t operator()(const FilterKey& key) const {
    return (size_t)key.hash();
  }
};

class FilterKeyBuilder {
  std::vector<std::byte> bytes;

  template <typename T>
  void appendFixed(T value) {
    static_assert(std::is_trivially_copyable_v<T>);
    size_t offset = bytes.size();
    bytes.resize(offset + sizeof(value));
    memcpy(bytes.data() + offset, &value, sizeof(value));
  }

  void appendBytes(std::span<const std::byte> value) {
    bytes.insert(bytes.end(), value.begin(), value.end());
  }

public:
  void appendTag(FilterKeyTag tag) {
    bytes.push_back((std::byte)tag);
  }

  void appendBool(bool value) {
    bytes.push_back(value ? std::byte{1} : std::byte{0});
  }

  void appendInt32(int32_t value) {
    appendInt64(value);
  }

  void appendUInt32(uint32_t value) {
    size_t offset = bytes.size();
    bytes.resize(offset + MAX_VINT_SIZE);
    char* start = (char*)bytes.data() + offset;
    char* end = writeVInt(start, value);
    bytes.resize(offset + (size_t)(end - start));
  }

  void appendInt64(int64_t value) {
    size_t offset = bytes.size();
    bytes.resize(offset + MAX_VLONG_SIZE);
    char* start = (char*)bytes.data() + offset;
    char* end = writeZLong(start, value);
    bytes.resize(offset + (size_t)(end - start));
  }

  void appendUInt64(uint64_t value) {
    size_t offset = bytes.size();
    bytes.resize(offset + MAX_VLONG_SIZE);
    char* start = (char*)bytes.data() + offset;
    char* end = writeVLong(start, value);
    bytes.resize(offset + (size_t)(end - start));
  }

  void appendFloat(float value) {
    appendFixed(value);
  }

  void appendDouble(double value) {
    appendFixed(value);
  }

  void appendSize(size_t value) {
    appendUInt64((uint64_t)value);
  }

  void appendString(std::string_view value) {
    appendSize(value.size());
    appendBytes(std::as_bytes(std::span(value)));
  }

  void appendTerm(std::string_view value) {
    assert(value.size() <= PackedTerm::MAX_LEN);
    bytes.push_back((std::byte)value.size());
    appendBytes(std::as_bytes(std::span(value)));
  }

  void appendOptionalTerm(const std::optional<std::string_view>& value) {
    appendBool(value.has_value());
    if (value) appendTerm(*value);
  }

  FilterKey finishStructural(FilterKeyScope scope,
                             const FilterKeyContext& ctx) && {
    FilterKeyBuilder envelope;
    envelope.appendUInt64(ctx.schemaGen);
    envelope.bytes.push_back((std::byte)scope);
    if (scope == FilterKeyScope::CORE_STABLE) {
      envelope.appendUInt64(ctx.coreGen);
    }
    // Coerced leaf values are authoritative, but retaining the timezone
    // identity makes cross-timezone reuse impossible even when two coercions
    // happen to produce the same value.
    envelope.appendString(ctx.timeZone);
    envelope.appendBytes(bytes);
    return FilterKey(std::move(envelope.bytes));
  }

  std::optional<FilterKey> finish(FilterKeyScope scope,
                                  const FilterKeyContext& ctx) && {
    if (scope == FilterKeyScope::UNCACHEABLE) return std::nullopt;
    return std::move(*this).finishStructural(scope, ctx);
  }
};

} // namespace luxir
