// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include "luxir/value/AggregateSketchState.h"

#include <algorithm>
#include <bit>
#include <cassert>
#include <cmath>
#include <stdexcept>
#include <string>

#include <xxhash.h>

#include "luxir/search/OrdMapStr.h"
#include "luxir/util/HyperLogLog.h"
#include "luxir/util/MemPool.h"
#include "luxir/util/NumericUtils.h"
#include "luxir/util/TDigest.h"

namespace luxir {
namespace {

// A sketch that keeps its serialized state for the finish that returns it.
template <class Sketch>
class EncodedSketch : public Sketch {
public:
  std::pmr::string encoded;

  template <class Parameter>
  EncodedSketch(Parameter parameter, std::pmr::memory_resource* resource)
      : Sketch(parameter, resource), encoded(resource) {}

  BucketScalar serialize() {
    encoded.clear();
    this->writeState(encoded);
    return BucketScalar::serialized(encoded);
  }
};

using PercentileState = EncodedSketch<TDigest>;
using CardinalityState = EncodedSketch<HyperLogLog>;

// The distinct global ordinals of one string column, kept exactly: ordinals
// are private to a reader, so the finish hashes each distinct term's bytes
// once and returns a state that merges across readers. An unsorted buffer is
// deduplicated as it doubles and becomes a bitset over the ordinal space once
// that is smaller.
class OrdinalTermSet {
  std::pmr::vector<int64_t> ords;
  std::pmr::vector<uint64_t> bits;
  int64_t numOrds = 0;
  size_t compactAt = 64;

  void set(int64_t ord) {
    assert(ord >= 0 && ord < numOrds);
    bits[(uint64_t)ord >> 6] |= uint64_t{1} << ((uint64_t)ord & 63);
  }

  void compact() {
    std::sort(ords.begin(), ords.end());
    ords.erase(std::unique(ords.begin(), ords.end()), ords.end());
    if (numOrds > 0 && ords.size() >= (size_t)numOrds / 64) {
      bits.assign(((size_t)numOrds + 63) / 64, 0);
      for (int64_t ord : ords) set(ord);
      std::pmr::vector<int64_t>(ords.get_allocator()).swap(ords);
      return;
    }
    compactAt = std::max<size_t>(64, ords.size() * 2);
  }

public:
  int64_t threshold;
  std::pmr::string encoded;

  OrdinalTermSet(int64_t threshold, std::pmr::memory_resource* resource)
      : ords(resource), bits(resource), threshold(threshold), encoded(resource) {}

  bool empty() const { return ords.empty() && bits.empty(); }

  void add(int64_t ord) {
    if (!bits.empty()) {
      set(ord);
      return;
    }
    ords.push_back(ord);
    if (ords.size() >= compactAt) compact();
  }

  void addOrds(std::span<const int64_t> values, int64_t count) {
    if (numOrds == 0) numOrds = count;
    assert(numOrds == count);
    for (int64_t ord : values) add(ord);
  }

  void merge(const OrdinalTermSet& other) {
    if (numOrds == 0) numOrds = other.numOrds;
    for (int64_t ord : other.ords) add(ord);
    for (size_t word = 0; word < other.bits.size(); word++) {
      for (uint64_t pending = other.bits[word]; pending != 0; pending &= pending - 1) {
        add((int64_t)(word * 64 + (size_t)std::countr_zero(pending)));
      }
    }
  }

  // Ascending distinct ordinals: terms are then read in dictionary order.
  template <class Consume>
  void forEachOrd(Consume&& consume) {
    if (bits.empty()) {
      compact();
      if (bits.empty()) {
        for (int64_t ord : ords) consume(ord);
        return;
      }
    }
    for (size_t word = 0; word < bits.size(); word++) {
      for (uint64_t pending = bits[word]; pending != 0; pending &= pending - 1) {
        consume((int64_t)(word * 64 + (size_t)std::countr_zero(pending)));
      }
    }
  }
};

// Value hashing and filtering match the registry's percentile and
// cardinality accumulators, so a state agrees with the scalar aggregate.
void addValue(TDigest& sketch, int64_t value) { sketch.add((double)value); }

void addValue(TDigest& sketch, double value) {
  if (std::isfinite(value)) sketch.add(value);
}

void addValue(HyperLogLog& sketch, int64_t value) {
  sketch.add(HyperLogLog::mix((uint64_t)value));
}

void addValue(HyperLogLog& sketch, double value) {
  if (!std::isfinite(value)) return;
  sketch.add(HyperLogLog::mix(value == 0.0 ? 0 : std::bit_cast<uint64_t>(value)));
}

void addValue(OrdinalTermSet& set, int64_t ord) { set.add(ord); }

void addValue(OrdinalTermSet& set, double value) {
  unused(set);
  unused(value);
  assert(false && "string ordinals are integers");
}

template <class Sketch>
struct SketchState {
  std::pmr::memory_resource* resource;
  Sketch* sketch;
};

template <class Sketch>
SketchState<Sketch> loadSketch(const void* source) {
  return loadUnaligned<SketchState<Sketch>>(source);
}

template <class Sketch>
void initSketch(void* target, std::span<const AggregateConstant> arguments,
                std::pmr::memory_resource* resource) {
  unused(arguments);
  storeUnaligned(target, SketchState<Sketch>{
      resource != nullptr ? resource : std::pmr::get_default_resource(), nullptr});
}

template <class Sketch, auto Parameter>
Sketch& getSketch(void* target, std::span<const AggregateConstant> arguments) {
  auto state = loadSketch<Sketch>(target);
  if (state.sketch == nullptr) {
    state.sketch = std::pmr::polymorphic_allocator<>(state.resource).new_object<Sketch>(
        Parameter(arguments), state.resource);
    storeUnaligned(target, state);
  }
  return *state.sketch;
}

template <class Sketch>
void destroySketch(void* target) noexcept {
  auto state = loadSketch<Sketch>(target);
  if (state.sketch != nullptr) {
    std::pmr::polymorphic_allocator<>(state.resource).delete_object(state.sketch);
  }
}

template <class Sketch, auto Parameter>
void mergeSketch(void* target, const void* source,
                 std::span<const AggregateConstant> arguments) {
  auto state = loadSketch<Sketch>(source);
  if (state.sketch != nullptr) {
    getSketch<Sketch, Parameter>(target, arguments).merge(*state.sketch);
  }
}

template <class Sketch, auto Parameter>
void addPoint(void* target, const ValueResult& value,
              std::span<const AggregateConstant> arguments) {
  if (!value.valid) return;
  auto& sketch = getSketch<Sketch, Parameter>(target, arguments);
  if (value.type == ValueType::DOUBLE) addValue(sketch, value.doubleValue);
  else addValue(sketch, value.intValue);
}

template <class Sketch, auto Parameter, bool Floating>
void addBatch(void* target, std::span<const ScalarValueResult> values,
              std::span<const AggregateConstant> arguments) {
  if (values.empty()) return;
  auto& sketch = getSketch<Sketch, Parameter>(target, arguments);
  for (const auto& value : values) {
    if (!value.valid) continue;
    if constexpr (Floating) addValue(sketch, value.doubleValue);
    else addValue(sketch, value.intValue);
  }
}

template <class Sketch>
void addRawValue(Sketch& sketch, int64_t raw, FieldType::Type columnType) {
  if (columnType == FieldType::FLOAT) {
    addValue(sketch, (double)sortableInt32ToFloat((int32_t)raw));
  } else if (columnType == FieldType::DOUBLE) {
    addValue(sketch, sortableInt64ToDouble(raw));
  } else {
    addValue(sketch, raw);
  }
}

template <class Sketch, auto Parameter>
void addRaw(void* target, int64_t raw, FieldType::Type columnType,
            std::span<const AggregateConstant> arguments) {
  addRawValue(getSketch<Sketch, Parameter>(target, arguments), raw, columnType);
}

template <class Sketch, auto Parameter>
void addRawBatch(void* target, std::span<const int64_t> values, FieldType::Type columnType,
                 std::span<const AggregateConstant> arguments) {
  if (values.empty()) return;
  auto& sketch = getSketch<Sketch, Parameter>(target, arguments);
  for (int64_t raw : values) addRawValue(sketch, raw, columnType);
}

template <class Sketch, auto Parameter>
AggregateStateOps sketchOps(BucketScalar (*finish)(const void*, std::span<const AggregateConstant>),
                            bool floating) {
  AggregateStateOps ops{sizeof(SketchState<Sketch>), initSketch<Sketch>,
      addPoint<Sketch, Parameter>,
      floating ? addBatch<Sketch, Parameter, true> : addBatch<Sketch, Parameter, false>,
      addRaw<Sketch, Parameter>, addRawBatch<Sketch, Parameter>,
      mergeSketch<Sketch, Parameter>, finish};
  ops.destroy = destroySketch<Sketch>;
  ops.inlineCapable = false;
  return ops;
}

double constantDouble(const AggregateConstant& value) {
  return value.type == ValueType::INT64 ? (double)value.intValue : value.doubleValue;
}

double percentileCompression(std::span<const AggregateConstant> arguments) {
  return arguments.empty() ? TDigest::DEFAULT_COMPRESSION : constantDouble(arguments[0]);
}

int64_t cardinalityThreshold(std::span<const AggregateConstant> arguments) {
  if (arguments.empty()) return HyperLogLog::DEFAULT_PRECISION_THRESHOLD;
  return arguments[0].type == ValueType::INT64 ? arguments[0].intValue
                                              : (int64_t)arguments[0].doubleValue;
}

BucketScalar missingState() {
  return BucketScalar::missing(BucketValueType::BYTES, ValueNature::NUMBER);
}

BucketScalar finishPercentileState(const void* source,
                                   std::span<const AggregateConstant> arguments) {
  unused(arguments);
  auto state = loadSketch<PercentileState>(source);
  if (state.sketch == nullptr || state.sketch->empty()) return missingState();
  return state.sketch->serialize();
}

BucketScalar finishCardinalityState(const void* source,
                                    std::span<const AggregateConstant> arguments) {
  unused(arguments);
  auto state = loadSketch<CardinalityState>(source);
  if (state.sketch == nullptr || (!state.sketch->dense() && state.sketch->exact.empty())) {
    return missingState();
  }
  return state.sketch->serialize();
}

void addOrdinalTerms(void* target, std::span<const int64_t> ords,
                     int64_t numOrds, size_t maxSetBytes,
                     std::span<const AggregateConstant> arguments) {
  unused(maxSetBytes);
  if (!ords.empty()) {
    getSketch<OrdinalTermSet, cardinalityThreshold>(target, arguments).addOrds(ords, numOrds);
  }
}

BucketScalar finishOrdinalTerms(const void* source,
                                std::span<const AggregateConstant> arguments,
                                const AggregateOrdinalSource& ordinals) {
  unused(arguments);
  auto state = loadSketch<OrdinalTermSet>(source);
  if (state.sketch == nullptr || state.sketch->empty()) return missingState();
  if (ordinals.reader == nullptr || ordinals.ords == nullptr) {
    throw std::logic_error("cardinality_state() ordinals were not bound");
  }
  OrdinalTermSet& set = *state.sketch;
  HyperLogLog sketch(set.threshold, state.resource);
  auto poolGuard = MemPool::threadLocalPoolGuard();
  OrdMapStr terms(poolGuard.pool(), ordinals.ords, *ordinals.reader,
                  ordinals.reader->field(ordinals.field));
  set.forEachOrd([&](int64_t ord) {
    std::string_view term = terms.ordToStr(ord);
    sketch.add(XXH3_64bits(term.data(), term.size()));
  });
  set.encoded.clear();
  sketch.writeState(set.encoded);
  return BucketScalar::serialized(set.encoded);
}

} // namespace

void requireNumericInput(const ValueNode* input, std::string_view function) {
  if (input->type == ValueType::COLUMN_ONLY) {
    throw std::runtime_error(std::string(function) + "() requires a numeric expression");
  }
}

void validateCardinality(const ValueNode* input, std::span<const AggregateConstant> arguments,
                         std::string_view function) {
  if (input->type == ValueType::COLUMN_ONLY && !input->columnIndexed) {
    throw std::runtime_error(std::string(function) + "() requires an indexed STRING or ID column");
  }
  if (!arguments.empty()) {
    double threshold = constantDouble(arguments[0]);
    if (!std::isfinite(threshold) || threshold < 0
        || threshold > HyperLogLog::MAX_PRECISION_THRESHOLD
        || threshold != std::floor(threshold)) {
      throw std::runtime_error(std::string(function)
          + "() threshold must be an integer in [0, 40000]");
    }
  }
}

ResolvedAggregate resolvePercentileState(
    const ValueNode* input, std::span<AggregateConstant> arguments) {
  requireNumericInput(input, "percentile_state");
  double compression = percentileCompression(arguments);
  if (!std::isfinite(compression) || compression < TDigest::MIN_COMPRESSION
      || compression > TDigest::MAX_COMPRESSION) {
    throw std::runtime_error("percentile_state() compression must be finite and in [20, 1000]");
  }
  AggregateStateOps ops = sketchOps<PercentileState, percentileCompression>(
      finishPercentileState, valueDouble(input->type));
  ops.orderSensitive = true;
  return {BucketValueType::BYTES, ValueNature::NUMBER, ops};
}

ResolvedAggregate resolveCardinalityState(
    const ValueNode* input, std::span<AggregateConstant> arguments) {
  validateCardinality(input, arguments, "cardinality_state");
  if (input->type != ValueType::COLUMN_ONLY) {
    return {BucketValueType::BYTES, ValueNature::NUMBER,
            sketchOps<CardinalityState, cardinalityThreshold>(
                finishCardinalityState, valueDouble(input->type))};
  }
  AggregateStateOps ops = sketchOps<OrdinalTermSet, cardinalityThreshold>(nullptr, false);
  ops.accumulateOrdBatch = addOrdinalTerms;
  ops.finishOrdinals = finishOrdinalTerms;
  return {BucketValueType::BYTES, ValueNature::NUMBER, ops};
}

AggregateConstant canonicalPercentileStateArgument(
    std::span<const AggregateConstant> arguments, uint8_t index) {
  assert(index == 0);
  unused(index);
  return {ValueType::DOUBLE, 0, percentileCompression(arguments)};
}

} // namespace luxir
