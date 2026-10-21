// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include "luxir/value/AggregateFunctionRegistry.h"

#include <algorithm>
#include <cassert>
#include <bit>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <type_traits>

#include "luxir/schema/ValCoerce.h"
#include "luxir/util/NumericUtils.h"
#include "luxir/util/HyperLogLog.h"
#include "luxir/util/TDigest.h"

namespace luxir {
namespace {

template <class State>
State loadState(const void* source) {
  return loadUnaligned<State>(source);
}

template <class State>
void storeState(void* target, const State& state) {
  storeUnaligned<State>(target, state);
}

LUXIR_UNALIGNED_START
struct IntSumState {
  __int128 sum = 0;
  uint64_t count = 0;
  AggregateFailure failure = AggregateFailure::NONE;
} LUXIR_UNALIGNED_END;

LUXIR_UNALIGNED_START
struct DenseFacetIntAvgState {
  __int128 sum = 0;
  AggregateFailure failure = AggregateFailure::NONE;
} LUXIR_UNALIGNED_END;

LUXIR_UNALIGNED_START
struct DoubleSumState {
  double sum = 0.0;
  uint64_t count = 0;
  AggregateFailure failure = AggregateFailure::NONE;
} LUXIR_UNALIGNED_END;

LUXIR_UNALIGNED_START
struct IntExtremeState {
  int64_t value = 0;
  bool seen = false;
} LUXIR_UNALIGNED_END;

LUXIR_UNALIGNED_START
struct DoubleExtremeState {
  double value = 0.0;
  bool seen = false;
} LUXIR_UNALIGNED_END;

static_assert(sizeof(IntSumState) == 25);
static_assert(sizeof(DenseFacetIntAvgState) == 17);
static_assert(sizeof(DoubleSumState) == 17);
static_assert(sizeof(IntExtremeState) == 9);
static_assert(sizeof(DoubleExtremeState) == 9);

bool decodeRawDouble(FieldType::Type columnType, int64_t raw, double& value) {
  if (columnType == FieldType::FLOAT) {
    value = (double)sortableInt32ToFloat((int32_t)raw);
  } else {
    assert(columnType == FieldType::DOUBLE);
    value = sortableInt64ToDouble(raw);
  }
  return !std::isnan(value);
}

template <class State>
void initState(void* target, std::span<const AggregateConstant> arguments,
               std::pmr::memory_resource* resource) {
  unused(arguments);
  unused(resource);
  storeState(target, State{});
}

void addIntSumValue(void* target, int64_t value,
                    std::span<const AggregateConstant> arguments) {
  unused(arguments);
  IntSumState state = loadState<IntSumState>(target);
  if (state.failure != AggregateFailure::NONE) return;
  __int128 sum;
  if (__builtin_add_overflow(state.sum, (__int128)value, &sum)) {
    state.failure = AggregateFailure::INT128_OVERFLOW;
    storeState(target, state);
    return;
  }
  uint64_t count;
  if (__builtin_add_overflow(state.count, uint64_t{1}, &count)) {
    state.failure = AggregateFailure::COUNT_OVERFLOW;
    storeState(target, state);
    return;
  }
  state.sum = sum;
  state.count = count;
  storeState(target, state);
}

void addIntSum(void* target, const ValueResult& value,
               std::span<const AggregateConstant> arguments) {
  addIntSumValue(target, value.intValue, arguments);
}

void addIntSumRaw(void* target, int64_t raw, FieldType::Type columnType,
                  std::span<const AggregateConstant> arguments) {
  unused(columnType);
  unused(arguments);
  // A physical segment has at most INT32_MAX documents and a request cannot
  // traverse enough segments to overflow either lane here. Synthetic state
  // overflow remains checked by the generic and merge paths.
  auto* bytes = static_cast<std::byte*>(target);
  __int128 sum = loadUnaligned<__int128>(
      bytes + offsetof(IntSumState, sum));
  uint64_t count = loadUnaligned<uint64_t>(
      bytes + offsetof(IntSumState, count));
  storeUnaligned<__int128>(
      bytes + offsetof(IntSumState, sum), sum + (__int128)raw);
  storeUnaligned<uint64_t>(
      bytes + offsetof(IntSumState, count), count + 1);
}

void addIntSumRawBatch(void* target, std::span<const int64_t> values,
                       FieldType::Type columnType,
                       std::span<const AggregateConstant> arguments) {
  unused(columnType);
  unused(arguments);
  IntSumState state = loadState<IntSumState>(target);
  if (state.failure != AggregateFailure::NONE) return;
  for (int64_t value : values) {
    __int128 sum;
    if (__builtin_add_overflow(state.sum, (__int128)value, &sum)) {
      state.failure = AggregateFailure::INT128_OVERFLOW;
      break;
    }
    uint64_t count;
    if (__builtin_add_overflow(state.count, uint64_t{1}, &count)) {
      state.failure = AggregateFailure::COUNT_OVERFLOW;
      break;
    }
    state.sum = sum;
    state.count = count;
  }
  storeState(target, state);
}

void addIntSumBatch(void* target, std::span<const ScalarValueResult> values,
                    std::span<const AggregateConstant> arguments) {
  unused(arguments);
  IntSumState state = loadState<IntSumState>(target);
  if (state.failure != AggregateFailure::NONE) return;
  for (const ScalarValueResult& value : values) {
    if (!value.valid) continue;
    __int128 sum;
    if (__builtin_add_overflow(state.sum, (__int128)value.intValue, &sum)) {
      state.failure = AggregateFailure::INT128_OVERFLOW;
      break;
    }
    uint64_t count;
    if (__builtin_add_overflow(state.count, uint64_t{1}, &count)) {
      state.failure = AggregateFailure::COUNT_OVERFLOW;
      break;
    }
    state.sum = sum;
    state.count = count;
  }
  storeState(target, state);
}

void mergeIntSum(void* target, const void* source,
                 std::span<const AggregateConstant> arguments) {
  unused(arguments);
  IntSumState left = loadState<IntSumState>(target);
  IntSumState right = loadState<IntSumState>(source);
  if (left.failure != AggregateFailure::NONE) return;
  if (right.failure != AggregateFailure::NONE) {
    left.failure = right.failure;
    storeState(target, left);
    return;
  }
  __int128 sum;
  if (__builtin_add_overflow(left.sum, right.sum, &sum)) {
    left.failure = AggregateFailure::INT128_OVERFLOW;
    storeState(target, left);
    return;
  }
  uint64_t count;
  if (__builtin_add_overflow(left.count, right.count, &count)) {
    left.failure = AggregateFailure::COUNT_OVERFLOW;
    storeState(target, left);
    return;
  }
  left.sum = sum;
  left.count = count;
  storeState(target, left);
}

BucketScalar finishIntSum(
    const void* source, std::span<const AggregateConstant> arguments) {
  unused(arguments);
  IntSumState state = loadState<IntSumState>(source);
  if (state.failure != AggregateFailure::NONE) {
    return BucketScalar::failed(BucketValueType::INT128,
                                ValueNature::NUMBER, state.failure);
  }
  return state.count == 0 ? BucketScalar::missing(BucketValueType::INT128,
                                                  ValueNature::NUMBER)
                          : BucketScalar::integer(state.sum);
}

BucketScalar finishIntAvg(
    const void* source, std::span<const AggregateConstant> arguments) {
  unused(arguments);
  IntSumState state = loadState<IntSumState>(source);
  if (state.failure != AggregateFailure::NONE) {
    return BucketScalar::failed(BucketValueType::DOUBLE,
                                ValueNature::NUMBER, state.failure);
  }
  if (state.count == 0) {
    return BucketScalar::missing(BucketValueType::DOUBLE, ValueNature::NUMBER);
  }
  double value = (double)state.sum / (double)state.count;
  if (!std::isfinite(value)) {
    return BucketScalar::failed(BucketValueType::DOUBLE, ValueNature::NUMBER,
                                AggregateFailure::NON_FINITE_AVERAGE);
  }
  return BucketScalar::floating(value);
}

void initDenseFacetIntAvg(void* target) {
  storeState(target, DenseFacetIntAvgState{});
}

void addDenseFacetIntAvg(void* target, int64_t raw) {
  // One collector sees at most INT32_MAX documents, so one segment cannot
  // overflow int128 even if every value is an int64 extreme. Merge checks the
  // cross-segment sum below.
  auto* bytes = static_cast<std::byte*>(target);
  __int128 sum = loadUnaligned<__int128>(bytes);
  storeUnaligned<__int128>(bytes, sum + (__int128)raw);
}

// Calls add(state, sum, count) for the batch's values: once per run of equal
// consecutive keys when runs are long (a filtered or clustered domain), where
// summing a run in a register spares a store-and-reload of its state per
// value; otherwise once per value, as a run-detection branch would
// mispredict on interleaved keys.
template <typename Add>
void forEachKeyedRun(void* states, size_t stride,
                     std::span<const int64_t> keys,
                     std::span<const int64_t> raw, Add&& add) {
  auto* base = static_cast<std::byte*>(states);
  size_t n = raw.size();
  size_t changes = 0;
  for (size_t i = 1; i < n; i++) changes += keys[i] != keys[i - 1];
  if (4 * changes >= n) {
    for (size_t i = 0; i < n; i++) {
      add(base + (size_t)keys[i] * stride, (__int128)raw[i], uint64_t{1});
    }
    return;
  }
  size_t i = 0;
  while (i < n) {
    int64_t key = keys[i];
    __int128 sum = raw[i];
    size_t j = i + 1;
    for (; j < n && keys[j] == key; j++) sum += (__int128)raw[j];
    add(base + (size_t)key * stride, sum, (uint64_t)(j - i));
    i = j;
  }
}

void addDenseFacetIntAvgKeyed(void* states, size_t stride,
                              std::span<const int64_t> keys,
                              std::span<const int64_t> raw) {
  // As addDenseFacetIntAvg: one collector cannot overflow int128.
  forEachKeyedRun(states, stride, keys, raw,
      [](std::byte* bytes, __int128 sum, uint64_t count) LUXIR_INLINE {
        unused(count);
        storeUnaligned<__int128>(bytes, loadUnaligned<__int128>(bytes) + sum);
      });
}

void addIntSumKeyed(void* states, size_t stride,
                    std::span<const int64_t> keys,
                    std::span<const int64_t> raw, FieldType::Type columnType,
                    std::span<const AggregateConstant> arguments) {
  unused(columnType);
  unused(arguments);
  // As addIntSumRaw: one collector cannot overflow either lane.
  forEachKeyedRun(states, stride, keys, raw,
      [](std::byte* bytes, __int128 sum, uint64_t count) LUXIR_INLINE {
        std::byte* sumBytes = bytes + offsetof(IntSumState, sum);
        std::byte* countBytes = bytes + offsetof(IntSumState, count);
        storeUnaligned<__int128>(
            sumBytes, loadUnaligned<__int128>(sumBytes) + sum);
        storeUnaligned<uint64_t>(
            countBytes, loadUnaligned<uint64_t>(countBytes) + count);
      });
}

// The keyed form of a raw point accumulator, with the point folded into the
// loop.
template <auto RawPoint>
void addKeyedRaw(void* states, size_t stride, std::span<const int64_t> keys,
                 std::span<const int64_t> raw, FieldType::Type columnType,
                 std::span<const AggregateConstant> arguments) {
  auto* base = static_cast<std::byte*>(states);
  for (size_t i = 0; i < raw.size(); i++) {
    RawPoint(base + (size_t)keys[i] * stride, raw[i], columnType, arguments);
  }
}

void failDenseFacetIntAvg(void* target, AggregateFailure failure) {
  auto* bytes = static_cast<std::byte*>(target);
  AggregateFailure existing = loadUnaligned<AggregateFailure>(
      bytes + offsetof(DenseFacetIntAvgState, failure));
  if (existing == AggregateFailure::NONE) {
    storeUnaligned<AggregateFailure>(
        bytes + offsetof(DenseFacetIntAvgState, failure), failure);
  }
}

void mergeDenseFacetIntAvg(void* target, const void* source) {
  DenseFacetIntAvgState left = loadState<DenseFacetIntAvgState>(target);
  DenseFacetIntAvgState right = loadState<DenseFacetIntAvgState>(source);
  if (left.failure != AggregateFailure::NONE) return;
  if (right.failure != AggregateFailure::NONE) {
    left.failure = right.failure;
  } else if (__builtin_add_overflow(left.sum, right.sum, &left.sum)) {
    left.failure = AggregateFailure::INT128_OVERFLOW;
  }
  storeState(target, left);
}

BucketScalar finishDenseFacetIntAvg(const void* source, int64_t count) {
  DenseFacetIntAvgState state = loadState<DenseFacetIntAvgState>(source);
  if (state.failure != AggregateFailure::NONE) {
    return BucketScalar::failed(BucketValueType::DOUBLE,
                                ValueNature::NUMBER, state.failure);
  }
  assert(count > 0);
  double value = (double)state.sum / (double)count;
  if (!std::isfinite(value)) {
    return BucketScalar::failed(BucketValueType::DOUBLE, ValueNature::NUMBER,
                                AggregateFailure::NON_FINITE_AVERAGE);
  }
  return BucketScalar::floating(value);
}

constexpr DenseFacetStateOps DENSE_FACET_INT_AVG{
    sizeof(DenseFacetIntAvgState), initDenseFacetIntAvg,
    addDenseFacetIntAvg, failDenseFacetIntAvg, mergeDenseFacetIntAvg,
    finishDenseFacetIntAvg, addDenseFacetIntAvgKeyed};

template <bool Average>
void addDoubleSumValue(void* target, double value,
                       std::span<const AggregateConstant> arguments) {
  unused(arguments);
  DoubleSumState state = loadState<DoubleSumState>(target);
  if (state.failure != AggregateFailure::NONE) return;
  double sum = state.sum + value;
  if (!std::isfinite(sum)) {
    state.failure = Average ? AggregateFailure::NON_FINITE_AVERAGE
                            : AggregateFailure::NON_FINITE_SUM;
    storeState(target, state);
    return;
  }
  uint64_t count;
  if (__builtin_add_overflow(state.count, uint64_t{1}, &count)) {
    state.failure = AggregateFailure::COUNT_OVERFLOW;
    storeState(target, state);
    return;
  }
  state.sum = sum;
  state.count = count;
  storeState(target, state);
}

template <bool Average>
void addDoubleSum(void* target, const ValueResult& value,
                  std::span<const AggregateConstant> arguments) {
  addDoubleSumValue<Average>(target, value.doubleValue, arguments);
}

template <bool Average>
void addDoubleSumRaw(void* target, int64_t raw,
                     FieldType::Type columnType,
                     std::span<const AggregateConstant> arguments) {
  double value;
  if (!decodeRawDouble(columnType, raw, value)) return;
  unused(arguments);
  auto* bytes = static_cast<std::byte*>(target);
  if (loadUnaligned<AggregateFailure>(
          bytes + offsetof(DoubleSumState, failure))
      != AggregateFailure::NONE) return;
  double sum = loadUnaligned<double>(
      bytes + offsetof(DoubleSumState, sum)) + value;
  if (!std::isfinite(sum)) {
    storeUnaligned<AggregateFailure>(
        bytes + offsetof(DoubleSumState, failure),
        Average ? AggregateFailure::NON_FINITE_AVERAGE
                : AggregateFailure::NON_FINITE_SUM);
  } else {
    uint64_t count = loadUnaligned<uint64_t>(
        bytes + offsetof(DoubleSumState, count));
    storeUnaligned<double>(bytes + offsetof(DoubleSumState, sum), sum);
    storeUnaligned<uint64_t>(
        bytes + offsetof(DoubleSumState, count), count + 1);
  }
}

template <bool Average>
void addDoubleSumRawBatch(void* target, std::span<const int64_t> values,
                          FieldType::Type columnType,
                          std::span<const AggregateConstant> arguments) {
  unused(arguments);
  DoubleSumState state = loadState<DoubleSumState>(target);
  if (state.failure != AggregateFailure::NONE) return;
  for (int64_t raw : values) {
    double value;
    if (!decodeRawDouble(columnType, raw, value)) continue;
    double sum = state.sum + value;
    if (!std::isfinite(sum)) {
      state.failure = Average ? AggregateFailure::NON_FINITE_AVERAGE
                              : AggregateFailure::NON_FINITE_SUM;
      break;
    }
    uint64_t count;
    if (__builtin_add_overflow(state.count, uint64_t{1}, &count)) {
      state.failure = AggregateFailure::COUNT_OVERFLOW;
      break;
    }
    state.sum = sum;
    state.count = count;
  }
  storeState(target, state);
}

template <bool Average>
void addDoubleSumBatch(void* target,
                       std::span<const ScalarValueResult> values,
                       std::span<const AggregateConstant> arguments) {
  unused(arguments);
  DoubleSumState state = loadState<DoubleSumState>(target);
  if (state.failure != AggregateFailure::NONE) return;
  for (const ScalarValueResult& value : values) {
    if (!value.valid) continue;
    double sum = state.sum + value.doubleValue;
    if (!std::isfinite(sum)) {
      state.failure = Average ? AggregateFailure::NON_FINITE_AVERAGE
                              : AggregateFailure::NON_FINITE_SUM;
      break;
    }
    uint64_t count;
    if (__builtin_add_overflow(state.count, uint64_t{1}, &count)) {
      state.failure = AggregateFailure::COUNT_OVERFLOW;
      break;
    }
    state.sum = sum;
    state.count = count;
  }
  storeState(target, state);
}

template <bool Average>
void mergeDoubleSum(void* target, const void* source,
                    std::span<const AggregateConstant> arguments) {
  unused(arguments);
  DoubleSumState left = loadState<DoubleSumState>(target);
  DoubleSumState right = loadState<DoubleSumState>(source);
  if (left.failure != AggregateFailure::NONE) return;
  if (right.failure != AggregateFailure::NONE) {
    left.failure = right.failure;
    storeState(target, left);
    return;
  }
  double sum = left.sum + right.sum;
  if (!std::isfinite(sum)) {
    left.failure = Average ? AggregateFailure::NON_FINITE_AVERAGE
                           : AggregateFailure::NON_FINITE_SUM;
    storeState(target, left);
    return;
  }
  uint64_t count;
  if (__builtin_add_overflow(left.count, right.count, &count)) {
    left.failure = AggregateFailure::COUNT_OVERFLOW;
    storeState(target, left);
    return;
  }
  left.sum = sum;
  left.count = count;
  storeState(target, left);
}

BucketScalar finishDoubleSum(
    const void* source, std::span<const AggregateConstant> arguments) {
  unused(arguments);
  DoubleSumState state = loadState<DoubleSumState>(source);
  if (state.failure != AggregateFailure::NONE) {
    return BucketScalar::failed(BucketValueType::DOUBLE,
                                ValueNature::NUMBER, state.failure);
  }
  return state.count == 0 ? BucketScalar::missing(BucketValueType::DOUBLE,
                                                  ValueNature::NUMBER)
                          : BucketScalar::floating(state.sum);
}

BucketScalar finishDoubleAvg(
    const void* source, std::span<const AggregateConstant> arguments) {
  unused(arguments);
  DoubleSumState state = loadState<DoubleSumState>(source);
  if (state.failure != AggregateFailure::NONE) {
    return BucketScalar::failed(BucketValueType::DOUBLE,
                                ValueNature::NUMBER, state.failure);
  }
  if (state.count == 0) {
    return BucketScalar::missing(BucketValueType::DOUBLE, ValueNature::NUMBER);
  }
  double value = state.sum / (double)state.count;
  if (!std::isfinite(value)) {
    return BucketScalar::failed(BucketValueType::DOUBLE, ValueNature::NUMBER,
                                AggregateFailure::NON_FINITE_AVERAGE);
  }
  return BucketScalar::floating(value);
}

template <bool Minimum>
void addIntExtremeValue(void* target, int64_t value,
                        std::span<const AggregateConstant> arguments) {
  unused(arguments);
  IntExtremeState state = loadState<IntExtremeState>(target);
  if (!state.seen) {
    state.value = value;
    state.seen = true;
  } else if constexpr (Minimum) {
    state.value = std::min(state.value, value);
  } else {
    state.value = std::max(state.value, value);
  }
  storeState(target, state);
}

template <bool Minimum>
void addIntExtreme(void* target, const ValueResult& value,
                   std::span<const AggregateConstant> arguments) {
  addIntExtremeValue<Minimum>(target, value.intValue, arguments);
}

template <bool Minimum>
void addIntExtremeRaw(void* target, int64_t raw,
                      FieldType::Type columnType,
                      std::span<const AggregateConstant> arguments) {
  unused(columnType);
  unused(arguments);
  auto* bytes = static_cast<std::byte*>(target);
  bool seen = loadUnaligned<bool>(bytes + offsetof(IntExtremeState, seen));
  int64_t value = seen
      ? loadUnaligned<int64_t>(bytes + offsetof(IntExtremeState, value)) : raw;
  if (seen) {
    value = Minimum ? std::min(value, raw) : std::max(value, raw);
  }
  storeUnaligned<int64_t>(bytes + offsetof(IntExtremeState, value), value);
  if (!seen) {
    storeUnaligned<bool>(bytes + offsetof(IntExtremeState, seen), true);
  }
}

template <bool Minimum>
void addIntExtremeRawBatch(void* target, std::span<const int64_t> values,
                           FieldType::Type columnType,
                           std::span<const AggregateConstant> arguments) {
  unused(columnType);
  unused(arguments);
  IntExtremeState state = loadState<IntExtremeState>(target);
  for (int64_t value : values) {
    if (!state.seen) {
      state.value = value;
      state.seen = true;
    } else if constexpr (Minimum) {
      state.value = std::min(state.value, value);
    } else {
      state.value = std::max(state.value, value);
    }
  }
  storeState(target, state);
}

template <bool Minimum>
void addIntExtremeBatch(void* target,
                        std::span<const ScalarValueResult> values,
                        std::span<const AggregateConstant> arguments) {
  unused(arguments);
  IntExtremeState state = loadState<IntExtremeState>(target);
  for (const ScalarValueResult& value : values) {
    if (!value.valid) continue;
    if (!state.seen) {
      state.value = value.intValue;
      state.seen = true;
    } else if constexpr (Minimum) {
      state.value = std::min(state.value, value.intValue);
    } else {
      state.value = std::max(state.value, value.intValue);
    }
  }
  storeState(target, state);
}

template <bool Minimum>
void mergeIntExtreme(void* target, const void* source,
                     std::span<const AggregateConstant> arguments) {
  IntExtremeState right = loadState<IntExtremeState>(source);
  if (!right.seen) return;
  ValueResult value = ValueResult::integer(right.value);
  addIntExtreme<Minimum>(target, value, arguments);
}

BucketScalar finishIntExtreme(
    const void* source, std::span<const AggregateConstant> arguments) {
  unused(arguments);
  IntExtremeState state = loadState<IntExtremeState>(source);
  return state.seen ? BucketScalar::integer(state.value)
                    : BucketScalar::missing(BucketValueType::INT128,
                                            ValueNature::NUMBER);
}

template <bool Minimum>
void addDoubleExtremeValue(void* target, double value,
                           std::span<const AggregateConstant> arguments) {
  unused(arguments);
  DoubleExtremeState state = loadState<DoubleExtremeState>(target);
  if (!state.seen) {
    state.value = value;
    state.seen = true;
  } else if constexpr (Minimum) {
    state.value = std::min(state.value, value);
  } else {
    state.value = std::max(state.value, value);
  }
  storeState(target, state);
}

template <bool Minimum>
void addDoubleExtreme(void* target, const ValueResult& value,
                      std::span<const AggregateConstant> arguments) {
  addDoubleExtremeValue<Minimum>(target, value.doubleValue, arguments);
}

template <bool Minimum>
void addDoubleExtremeRaw(void* target, int64_t raw,
                         FieldType::Type columnType,
                         std::span<const AggregateConstant> arguments) {
  double value;
  if (!decodeRawDouble(columnType, raw, value)) return;
  unused(arguments);
  auto* bytes = static_cast<std::byte*>(target);
  bool seen = loadUnaligned<bool>(bytes + offsetof(DoubleExtremeState, seen));
  double extreme = seen
      ? loadUnaligned<double>(bytes + offsetof(DoubleExtremeState, value))
      : value;
  if (seen) {
    extreme = Minimum ? std::min(extreme, value) : std::max(extreme, value);
  }
  storeUnaligned<double>(
      bytes + offsetof(DoubleExtremeState, value), extreme);
  if (!seen) {
    storeUnaligned<bool>(bytes + offsetof(DoubleExtremeState, seen), true);
  }
}

template <bool Minimum>
void addDoubleExtremeRawBatch(void* target,
                              std::span<const int64_t> values,
                              FieldType::Type columnType,
                              std::span<const AggregateConstant> arguments) {
  unused(arguments);
  DoubleExtremeState state = loadState<DoubleExtremeState>(target);
  for (int64_t raw : values) {
    double value;
    if (!decodeRawDouble(columnType, raw, value)) continue;
    if (!state.seen) {
      state.value = value;
      state.seen = true;
    } else if constexpr (Minimum) {
      state.value = std::min(state.value, value);
    } else {
      state.value = std::max(state.value, value);
    }
  }
  storeState(target, state);
}

template <bool Minimum>
void addDoubleExtremeBatch(void* target,
                           std::span<const ScalarValueResult> values,
                           std::span<const AggregateConstant> arguments) {
  unused(arguments);
  DoubleExtremeState state = loadState<DoubleExtremeState>(target);
  for (const ScalarValueResult& value : values) {
    if (!value.valid) continue;
    if (!state.seen) {
      state.value = value.doubleValue;
      state.seen = true;
    } else if constexpr (Minimum) {
      state.value = std::min(state.value, value.doubleValue);
    } else {
      state.value = std::max(state.value, value.doubleValue);
    }
  }
  storeState(target, state);
}

template <bool Minimum>
void mergeDoubleExtreme(void* target, const void* source,
                        std::span<const AggregateConstant> arguments) {
  DoubleExtremeState right = loadState<DoubleExtremeState>(source);
  if (!right.seen) return;
  ValueResult value = ValueResult::floating(right.value);
  addDoubleExtreme<Minimum>(target, value, arguments);
}

BucketScalar finishDoubleExtreme(
    const void* source, std::span<const AggregateConstant> arguments) {
  unused(arguments);
  DoubleExtremeState state = loadState<DoubleExtremeState>(source);
  return state.seen ? BucketScalar::floating(state.value)
                    : BucketScalar::missing(BucketValueType::DOUBLE,
                                            ValueNature::NUMBER);
}

AggregateStateOps intSumOps(bool average) {
  return {sizeof(IntSumState), initState<IntSumState>, addIntSum,
          addIntSumBatch, addIntSumRaw, addIntSumRawBatch,
          mergeIntSum,
          average ? finishIntAvg : finishIntSum,
          addIntSumKeyed};
}

AggregateStateOps doubleSumOps(bool average) {
  return {sizeof(DoubleSumState), initState<DoubleSumState>,
          average ? addDoubleSum<true> : addDoubleSum<false>,
          average ? addDoubleSumBatch<true> : addDoubleSumBatch<false>,
          average ? addDoubleSumRaw<true> : addDoubleSumRaw<false>,
          average ? addDoubleSumRawBatch<true>
                  : addDoubleSumRawBatch<false>,
          average ? mergeDoubleSum<true> : mergeDoubleSum<false>,
          average ? finishDoubleAvg : finishDoubleSum,
          average ? addKeyedRaw<addDoubleSumRaw<true>>
                  : addKeyedRaw<addDoubleSumRaw<false>>};
}

AggregateStateOps intExtremeOps(bool minimum) {
  return {sizeof(IntExtremeState), initState<IntExtremeState>,
          minimum ? addIntExtreme<true> : addIntExtreme<false>,
          minimum ? addIntExtremeBatch<true> : addIntExtremeBatch<false>,
          minimum ? addIntExtremeRaw<true> : addIntExtremeRaw<false>,
          minimum ? addIntExtremeRawBatch<true>
                  : addIntExtremeRawBatch<false>,
          minimum ? mergeIntExtreme<true> : mergeIntExtreme<false>,
          finishIntExtreme,
          minimum ? addKeyedRaw<addIntExtremeRaw<true>>
                  : addKeyedRaw<addIntExtremeRaw<false>>};
}

AggregateStateOps doubleExtremeOps(bool minimum) {
  return {sizeof(DoubleExtremeState), initState<DoubleExtremeState>,
          minimum ? addDoubleExtreme<true> : addDoubleExtreme<false>,
          minimum ? addDoubleExtremeBatch<true>
                  : addDoubleExtremeBatch<false>,
          minimum ? addDoubleExtremeRaw<true>
                  : addDoubleExtremeRaw<false>,
          minimum ? addDoubleExtremeRawBatch<true>
                  : addDoubleExtremeRawBatch<false>,
          minimum ? mergeDoubleExtreme<true> : mergeDoubleExtreme<false>,
          finishDoubleExtreme,
          minimum ? addKeyedRaw<addDoubleExtremeRaw<true>>
                  : addKeyedRaw<addDoubleExtremeRaw<false>>};
}

ResolvedAggregate resolveAvg(
    const ValueNode* input,
    std::span<AggregateConstant> trailingArguments) {
  assert(input != nullptr);
  assert(trailingArguments.empty());
  bool floating = input->type == ValueType::DOUBLE;
  return {BucketValueType::DOUBLE, input->nature,
          floating ? doubleSumOps(true) : intSumOps(true),
          floating ? nullptr : &DENSE_FACET_INT_AVG};
}

ResolvedAggregate resolveSum(
    const ValueNode* input,
    std::span<AggregateConstant> trailingArguments) {
  assert(input != nullptr);
  assert(trailingArguments.empty());
  if (input->nature == ValueNature::DATE) {
    throw std::runtime_error("sum() cannot aggregate a DATE expression");
  }
  bool floating = input->type == ValueType::DOUBLE;
  return {floating ? BucketValueType::DOUBLE : BucketValueType::INT128,
          ValueNature::NUMBER,
          floating ? doubleSumOps(false) : intSumOps(false)};
}

ResolvedAggregate resolveMin(
    const ValueNode* input,
    std::span<AggregateConstant> trailingArguments) {
  assert(input != nullptr);
  assert(trailingArguments.empty());
  bool floating = input->type == ValueType::DOUBLE;
  return {floating ? BucketValueType::DOUBLE : BucketValueType::INT128,
          input->nature,
          floating ? doubleExtremeOps(true) : intExtremeOps(true)};
}

ResolvedAggregate resolveMax(
    const ValueNode* input,
    std::span<AggregateConstant> trailingArguments) {
  assert(input != nullptr);
  assert(trailingArguments.empty());
  bool floating = input->type == ValueType::DOUBLE;
  return {floating ? BucketValueType::DOUBLE : BucketValueType::INT128,
          input->nature,
          floating ? doubleExtremeOps(false) : intExtremeOps(false)};
}

int64_t cardinalityThreshold(std::span<const AggregateConstant> arguments) {
  if (arguments.empty()) return HyperLogLog::DEFAULT_PRECISION_THRESHOLD;
  const auto& value = arguments[0];
  if (value.type == ValueType::INT64) return value.intValue;
  return (int64_t)value.doubleValue;
}

template <class Sketch>
struct SketchState {
  std::pmr::memory_resource* resource;
  Sketch* sketch;
};

template <class Sketch>
void initSketch(void* target, std::span<const AggregateConstant> arguments,
                std::pmr::memory_resource* resource) {
  unused(arguments);
  storeState(target, SketchState<Sketch>{
      resource != nullptr ? resource : std::pmr::get_default_resource(), nullptr});
}

template <class Sketch, auto Parameter>
Sketch& getSketch(void* target, std::span<const AggregateConstant> arguments) {
  auto state = loadState<SketchState<Sketch>>(target);
  if (state.sketch == nullptr) {
    state.sketch = std::pmr::polymorphic_allocator<>(state.resource).new_object<Sketch>(
        Parameter(arguments), state.resource);
    storeState(target, state);
  }
  return *state.sketch;
}

template <class Sketch>
void destroySketch(void* target) noexcept {
  auto state = loadState<SketchState<Sketch>>(target);
  if (state.sketch != nullptr) {
    std::pmr::polymorphic_allocator<>(state.resource).delete_object(state.sketch);
  }
}

inline void addValue(HyperLogLog& sketch, int64_t value) {
  sketch.add(HyperLogLog::mix((uint64_t)value));
}

inline void addValue(HyperLogLog& sketch, double value) {
  if (!std::isfinite(value)) return;
  uint64_t bits = value == 0.0 ? 0 : std::bit_cast<uint64_t>(value);
  sketch.add(HyperLogLog::mix(bits));
}

inline void addValue(TDigest& sketch, int64_t value) {
  sketch.add((double)value);
}

inline void addValue(TDigest& sketch, double value) {
  if (std::isfinite(value)) sketch.add(value);
}

template <FieldType::Type Type, class F>
void forEachRawFloatingBatchValue(std::span<const int64_t> values, F&& consume) {
  for (int64_t raw : values) {
    double value;
    if (decodeRawDouble(Type, raw, value)) consume(value);
  }
}

template <class F>
void forEachRawBatchValue(std::span<const int64_t> values,
                          FieldType::Type columnType, F&& consume) {
  switch (columnType) {
    case FieldType::FLOAT:
      forEachRawFloatingBatchValue<FieldType::FLOAT>(values, consume);
      break;
    case FieldType::DOUBLE:
      forEachRawFloatingBatchValue<FieldType::DOUBLE>(values, consume);
      break;
    default:
      for (int64_t raw : values) consume(raw);
  }
}

template <class Sketch, auto Parameter>
void addSketch(void* target, const ValueResult& value,
               std::span<const AggregateConstant> arguments) {
  if (!value.valid) return;
  if (value.type == ValueType::DOUBLE && !std::isfinite(value.doubleValue)) return;
  auto& sketch = getSketch<Sketch, Parameter>(target, arguments);
  if (value.type == ValueType::DOUBLE) addValue(sketch, value.doubleValue);
  else addValue(sketch, value.intValue);
}

template <class Sketch, auto Parameter, bool Floating>
void addSketchBatch(void* target, std::span<const ScalarValueResult> values,
                    std::span<const AggregateConstant> arguments) {
  if (values.empty()) return;
  auto& sketch = getSketch<Sketch, Parameter>(target, arguments);
  for (const auto& value : values) {
    if (!value.valid) continue;
    if constexpr (Floating) addValue(sketch, value.doubleValue);
    else addValue(sketch, value.intValue);
  }
}

template <class Sketch, auto Parameter>
void addSketchRaw(void* target, int64_t raw, FieldType::Type columnType,
                  std::span<const AggregateConstant> arguments) {
  if (columnType == FieldType::FLOAT || columnType == FieldType::DOUBLE) {
    double value;
    if (decodeRawDouble(columnType, raw, value)) {
      addSketch<Sketch, Parameter>(target, ValueResult::floating(value), arguments);
    }
  } else {
    addSketch<Sketch, Parameter>(target, ValueResult::integer(raw), arguments);
  }
}

template <class Sketch, auto Parameter>
void addSketchRawBatch(void* target, std::span<const int64_t> values,
                       FieldType::Type columnType,
                       std::span<const AggregateConstant> arguments) {
  if (values.empty()) return;
  auto& sketch = getSketch<Sketch, Parameter>(target, arguments);
  forEachRawBatchValue(values, columnType, [&](auto value) { addValue(sketch, value); });
}

void mergeCardinality(void* target, const void* source,
                      std::span<const AggregateConstant> arguments) {
  auto state = loadState<SketchState<HyperLogLog>>(source);
  if (state.sketch != nullptr) {
    getSketch<HyperLogLog, cardinalityThreshold>(target, arguments).merge(*state.sketch);
  }
}

BucketScalar finishCardinality(const void* source,
                               std::span<const AggregateConstant> arguments) {
  unused(arguments);
  auto state = loadState<SketchState<HyperLogLog>>(source);
  return BucketScalar::integer(state.sketch == nullptr ? 0 : state.sketch->estimate());
}

ResolvedAggregate resolveCardinality(
    const ValueNode* input, std::span<AggregateConstant> arguments) {
  if (input->type == ValueType::COLUMN_ONLY && !input->columnIndexed) {
    throw std::runtime_error("cardinality() requires an indexed STRING or ID column");
  }
  if (!arguments.empty()) {
    const auto& value = arguments[0];
    double threshold = value.type == ValueType::INT64
        ? (double)value.intValue : value.doubleValue;
    if (!std::isfinite(threshold) || threshold < 0
        || threshold > HyperLogLog::MAX_PRECISION_THRESHOLD
        || threshold != std::floor(threshold)) {
      throw std::runtime_error("cardinality() threshold must be an integer in [0, 40000]");
    }
  }
  AggregateStateOps ops{sizeof(SketchState<HyperLogLog>), initSketch<HyperLogLog>,
      addSketch<HyperLogLog, cardinalityThreshold>,
      valueDouble(input->type) ? addSketchBatch<HyperLogLog, cardinalityThreshold, true>
                               : addSketchBatch<HyperLogLog, cardinalityThreshold, false>,
      addSketchRaw<HyperLogLog, cardinalityThreshold>,
      addSketchRawBatch<HyperLogLog, cardinalityThreshold>, mergeCardinality, finishCardinality};
  ops.destroy = destroySketch<HyperLogLog>;
  ops.inlineCapable = false;
  return {BucketValueType::INT128, ValueNature::NUMBER, ops};
}

LUXIR_UNALIGNED_START
struct PercentileRankState {
  uint64_t count = 0;
  uint64_t matched = 0;
  AggregateFailure failure = AggregateFailure::NONE;
} LUXIR_UNALIGNED_END;

// Compare without rounding an int64 to double or converting an out-of-range
// double to int64. This also keeps integer constants exact above 2^53.
bool rankMatches(const ValueResult& value, const AggregateConstant& threshold) {
  if (value.type == ValueType::INT64 && threshold.type == ValueType::INT64) {
    return value.intValue <= threshold.intValue;
  }
  if (value.type == ValueType::DOUBLE && threshold.type == ValueType::DOUBLE) {
    return value.doubleValue <= threshold.doubleValue;
  }
  if (value.type == ValueType::INT64) {
    double bound = threshold.doubleValue;
    if (bound < -0x1p63) return false;
    if (bound >= 0x1p63) return true;
    return value.intValue <= (int64_t)std::floor(bound);
  }
  double number = value.doubleValue;
  if (number < -0x1p63) return true;
  if (number >= 0x1p63) return false;
  return (int64_t)std::ceil(number) <= threshold.intValue;
}

void addRankValue(PercentileRankState& state, const ValueResult& value,
                  const AggregateConstant& threshold) {
  if (state.failure != AggregateFailure::NONE || !value.valid) return;
  if (value.type == ValueType::DOUBLE && !std::isfinite(value.doubleValue)) return;
  if (__builtin_add_overflow(state.count, uint64_t{1}, &state.count)) {
    state.failure = AggregateFailure::COUNT_OVERFLOW;
  } else if (rankMatches(value, threshold)) {
    state.matched++;
  }
}

void addRank(void* target, const ValueResult& value,
             std::span<const AggregateConstant> arguments) {
  auto state = loadState<PercentileRankState>(target);
  addRankValue(state, value, arguments[0]);
  storeState(target, state);
}

template <bool Floating>
void addRankBatch(void* target, std::span<const ScalarValueResult> values,
                  std::span<const AggregateConstant> arguments) {
  if (values.empty()) return;
  auto state = loadState<PercentileRankState>(target);
  for (const auto& value : values) {
    if (!value.valid) continue;
    addRankValue(state, Floating ? ValueResult::floating(value.doubleValue)
                                : ValueResult::integer(value.intValue), arguments[0]);
  }
  storeState(target, state);
}

void addRankRaw(void* target, int64_t raw, FieldType::Type columnType,
                std::span<const AggregateConstant> arguments) {
  if (columnType == FieldType::FLOAT || columnType == FieldType::DOUBLE) {
    double value;
    if (decodeRawDouble(columnType, raw, value)) {
      addRank(target, ValueResult::floating(value), arguments);
    }
  } else {
    addRank(target, ValueResult::integer(raw), arguments);
  }
}

void addRankRawBatch(void* target, std::span<const int64_t> values,
                     FieldType::Type columnType,
                     std::span<const AggregateConstant> arguments) {
  if (values.empty()) return;
  auto state = loadState<PercentileRankState>(target);
  forEachRawBatchValue(values, columnType, [&](auto value) {
    if constexpr (std::is_same_v<decltype(value), double>) {
      addRankValue(state, ValueResult::floating(value), arguments[0]);
    } else {
      addRankValue(state, ValueResult::integer(value), arguments[0]);
    }
  });
  storeState(target, state);
}

void mergeRank(void* target, const void* source,
                std::span<const AggregateConstant> arguments) {
  unused(arguments);
  auto left = loadState<PercentileRankState>(target);
  auto right = loadState<PercentileRankState>(source);
  if (left.failure != AggregateFailure::NONE) return;
  if (right.failure != AggregateFailure::NONE) {
    left.failure = right.failure;
  } else if (__builtin_add_overflow(left.count, right.count, &left.count)
             || __builtin_add_overflow(left.matched, right.matched, &left.matched)) {
    left.failure = AggregateFailure::COUNT_OVERFLOW;
  }
  storeState(target, left);
}

BucketScalar finishRank(const void* source,
                        std::span<const AggregateConstant> arguments) {
  unused(arguments);
  auto state = loadState<PercentileRankState>(source);
  if (state.failure != AggregateFailure::NONE) {
    return BucketScalar::failed(BucketValueType::DOUBLE, ValueNature::NUMBER, state.failure);
  }
  return state.count == 0
      ? BucketScalar::missing(BucketValueType::DOUBLE, ValueNature::NUMBER)
      : BucketScalar::floating(100.0 * ((double)state.matched / (double)state.count));
}

ResolvedAggregate resolveRank(
    const ValueNode* input, std::span<AggregateConstant> arguments) {
  if (arguments[0].type == ValueType::DOUBLE && !std::isfinite(arguments[0].doubleValue)) {
    throw std::runtime_error("percentile_rank() value must be finite");
  }
  if (input->kind == ValueNodeKind::COLUMN && input->columnType == FieldType::FLOAT) {
    auto& threshold = arguments[0];
    float value = threshold.type == ValueType::INT64
        ? coerce::toFloatScalar(threshold.intValue, input->text)
        : coerce::toFloatScalar(threshold.doubleValue, input->text);
    threshold = {ValueType::DOUBLE, 0, (double)value};
  }
  AggregateStateOps ops{sizeof(PercentileRankState), initState<PercentileRankState>, addRank,
      input->type == ValueType::DOUBLE ? addRankBatch<true> : addRankBatch<false>,
      addRankRaw, addRankRawBatch, mergeRank, finishRank, addKeyedRaw<addRankRaw>};
  return {BucketValueType::DOUBLE, ValueNature::NUMBER, ops};
}

double constantDouble(const AggregateConstant& value) {
  return value.type == ValueType::INT64 ? (double)value.intValue : value.doubleValue;
}

double percentileCompression(std::span<const AggregateConstant> arguments) {
  return arguments.size() == 2 ? constantDouble(arguments[1]) : TDigest::DEFAULT_COMPRESSION;
}

void mergePercentile(void* target, const void* source,
                      std::span<const AggregateConstant> arguments) {
  auto state = loadState<SketchState<TDigest>>(source);
  if (state.sketch != nullptr) {
    getSketch<TDigest, percentileCompression>(target, arguments).merge(*state.sketch);
  }
}

BucketScalar finishPercentile(const void* source,
                               std::span<const AggregateConstant> arguments) {
  auto state = loadState<SketchState<TDigest>>(source);
  if (state.sketch == nullptr || state.sketch->empty()) {
    return BucketScalar::missing(BucketValueType::DOUBLE, ValueNature::NUMBER);
  }
  state.sketch->finish();
  return BucketScalar::floating(state.sketch->quantile(constantDouble(arguments[0]) / 100.0));
}

ResolvedAggregate resolvePercentile(
    const ValueNode* input, std::span<AggregateConstant> arguments) {
  double percent = constantDouble(arguments[0]);
  if (!std::isfinite(percent) || percent < 0 || percent > 100) {
    throw std::runtime_error("percentile() percent must be finite and in [0, 100]");
  }
  double compression = percentileCompression(arguments);
  if (!std::isfinite(compression) || compression < TDigest::MIN_COMPRESSION
      || compression > TDigest::MAX_COMPRESSION) {
    throw std::runtime_error("percentile() compression must be finite and in [20, 1000]");
  }
  AggregateStateOps ops{sizeof(SketchState<TDigest>), initSketch<TDigest>,
      addSketch<TDigest, percentileCompression>,
      valueDouble(input->type) ? addSketchBatch<TDigest, percentileCompression, true>
                               : addSketchBatch<TDigest, percentileCompression, false>,
      addSketchRaw<TDigest, percentileCompression>,
      addSketchRawBatch<TDigest, percentileCompression>, mergePercentile, finishPercentile};
  ops.destroy = destroySketch<TDigest>;
  ops.inlineCapable = false;
  ops.orderSensitive = true;
  return {BucketValueType::DOUBLE, input->nature, ops};
}

AggregateConstant canonicalCardinalityArgument(
    std::span<const AggregateConstant> arguments, uint8_t index) {
  assert(index == 0);
  return {ValueType::INT64, cardinalityThreshold(arguments), 0};
}

AggregateConstant canonicalPercentileArgument(
    std::span<const AggregateConstant> arguments, uint8_t index) {
  assert(index == 1);
  return {ValueType::DOUBLE, 0, percentileCompression(arguments)};
}

constexpr AggregateFunction FUNCTIONS[] = {
    {"avg", 1, 1, resolveAvg},
    {"sum", 1, 1, resolveSum},
    {"min", 1, 1, resolveMin},
    {"max", 1, 1, resolveMax},
    {"cardinality", 1, 2, resolveCardinality, true, 0, canonicalCardinalityArgument},
    {"percentile_rank", 2, 2, resolveRank},
    {"percentile", 2, 3, resolvePercentile, false, 1, canonicalPercentileArgument},
};

} // namespace

bool AggregateFunction::sameStateArguments(
    std::span<const AggregateConstant> left,
    std::span<const AggregateConstant> right) const {
  if (canonicalArgument == nullptr && left.size() != right.size()) return false;
  size_t count = canonicalArgument ? (size_t)maxArguments - 1 : left.size();
  for (uint8_t i = 0; i < count; i++) {
    if ((readoutArguments & (uint32_t{1} << i)) != 0) continue;
    AggregateConstant a = canonicalArgument ? canonicalArgument(left, i) : left[i];
    AggregateConstant b = canonicalArgument ? canonicalArgument(right, i) : right[i];
    if (a.type != b.type) return false;
    if (a.type == ValueType::INT64) {
      if (a.intValue != b.intValue) return false;
    } else if (std::bit_cast<uint64_t>(a.doubleValue)
        != std::bit_cast<uint64_t>(b.doubleValue)) return false;
  }
  return true;
}

const AggregateFunction* AggregateFunctionRegistry::find(std::string_view name) {
  auto entry = std::ranges::find(FUNCTIONS, name, &AggregateFunction::name);
  return entry == std::end(FUNCTIONS) ? nullptr : &*entry;
}

std::span<const AggregateFunction> AggregateFunctionRegistry::entries() {
  return FUNCTIONS;
}

ExpressionFunctionLookup ExpressionFunctionRegistry::find(
    std::string_view name) {
  return {ValueFunctionRegistry::find(name),
          AggregateFunctionRegistry::find(name)};
}

} // namespace luxir
