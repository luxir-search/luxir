// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include "luxir/value/AggregateFunctionRegistry.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <limits>
#include <stdexcept>

#include "luxir/util/NumericUtils.h"

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
void initState(void* target, std::span<const AggregateConstant> arguments) {
  unused(arguments);
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
    finishDenseFacetIntAvg};

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
          average ? finishIntAvg : finishIntSum};
}

AggregateStateOps doubleSumOps(bool average) {
  return {sizeof(DoubleSumState), initState<DoubleSumState>,
          average ? addDoubleSum<true> : addDoubleSum<false>,
          average ? addDoubleSumBatch<true> : addDoubleSumBatch<false>,
          average ? addDoubleSumRaw<true> : addDoubleSumRaw<false>,
          average ? addDoubleSumRawBatch<true>
                  : addDoubleSumRawBatch<false>,
          average ? mergeDoubleSum<true> : mergeDoubleSum<false>,
          average ? finishDoubleAvg : finishDoubleSum};
}

AggregateStateOps intExtremeOps(bool minimum) {
  return {sizeof(IntExtremeState), initState<IntExtremeState>,
          minimum ? addIntExtreme<true> : addIntExtreme<false>,
          minimum ? addIntExtremeBatch<true> : addIntExtremeBatch<false>,
          minimum ? addIntExtremeRaw<true> : addIntExtremeRaw<false>,
          minimum ? addIntExtremeRawBatch<true>
                  : addIntExtremeRawBatch<false>,
          minimum ? mergeIntExtreme<true> : mergeIntExtreme<false>,
          finishIntExtreme};
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
          finishDoubleExtreme};
}

ResolvedAggregate resolveAvg(
    const ValueNode* input,
    std::span<const AggregateConstant> trailingArguments) {
  assert(input != nullptr);
  assert(trailingArguments.empty());
  bool floating = input->type == ValueType::DOUBLE;
  return {BucketValueType::DOUBLE, input->nature,
          floating ? doubleSumOps(true) : intSumOps(true),
          floating ? nullptr : &DENSE_FACET_INT_AVG};
}

ResolvedAggregate resolveSum(
    const ValueNode* input,
    std::span<const AggregateConstant> trailingArguments) {
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
    std::span<const AggregateConstant> trailingArguments) {
  assert(input != nullptr);
  assert(trailingArguments.empty());
  bool floating = input->type == ValueType::DOUBLE;
  return {floating ? BucketValueType::DOUBLE : BucketValueType::INT128,
          input->nature,
          floating ? doubleExtremeOps(true) : intExtremeOps(true)};
}

ResolvedAggregate resolveMax(
    const ValueNode* input,
    std::span<const AggregateConstant> trailingArguments) {
  assert(input != nullptr);
  assert(trailingArguments.empty());
  bool floating = input->type == ValueType::DOUBLE;
  return {floating ? BucketValueType::DOUBLE : BucketValueType::INT128,
          input->nature,
          floating ? doubleExtremeOps(false) : intExtremeOps(false)};
}

constexpr AggregateFunction FUNCTIONS[] = {
    {"avg", 1, 1, resolveAvg},
    {"sum", 1, 1, resolveSum},
    {"min", 1, 1, resolveMin},
    {"max", 1, 1, resolveMax},
};

} // namespace

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
