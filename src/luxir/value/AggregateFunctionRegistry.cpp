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

struct IntSumState {
  __int128 sum = 0;
  uint64_t count = 0;
  AggregateFailure failure = AggregateFailure::NONE;
};

struct DoubleSumState {
  double sum = 0.0;
  uint64_t count = 0;
  AggregateFailure failure = AggregateFailure::NONE;
};

struct IntExtremeState {
  int64_t value = 0;
  bool seen = false;
};

struct DoubleExtremeState {
  double value = 0.0;
  AggregateFailure failure = AggregateFailure::NONE;
  bool seen = false;
};

template <class State>
void initState(void* target) {
  storeState(target, State{});
}

void addIntSum(void* target, const ValueResult& value) {
  IntSumState state = loadState<IntSumState>(target);
  if (state.failure != AggregateFailure::NONE) return;
  __int128 sum;
  if (__builtin_add_overflow(state.sum, (__int128)value.intValue, &sum)) {
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

void mergeIntSum(void* target, const void* source) {
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

BucketScalar finishIntSum(const void* source) {
  IntSumState state = loadState<IntSumState>(source);
  if (state.failure != AggregateFailure::NONE) {
    return BucketScalar::failed(BucketValueType::INT128,
                                ValueNature::NUMBER, state.failure);
  }
  return state.count == 0 ? BucketScalar::missing(BucketValueType::INT128,
                                                  ValueNature::NUMBER)
                          : BucketScalar::integer(state.sum);
}

BucketScalar finishIntAvg(const void* source) {
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

template <bool Average>
void addDoubleSum(void* target, const ValueResult& value) {
  DoubleSumState state = loadState<DoubleSumState>(target);
  if (state.failure != AggregateFailure::NONE) return;
  double sum = state.sum + value.doubleValue;
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
void mergeDoubleSum(void* target, const void* source) {
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

BucketScalar finishDoubleSum(const void* source) {
  DoubleSumState state = loadState<DoubleSumState>(source);
  if (state.failure != AggregateFailure::NONE) {
    return BucketScalar::failed(BucketValueType::DOUBLE,
                                ValueNature::NUMBER, state.failure);
  }
  return state.count == 0 ? BucketScalar::missing(BucketValueType::DOUBLE,
                                                  ValueNature::NUMBER)
                          : BucketScalar::floating(state.sum);
}

BucketScalar finishDoubleAvg(const void* source) {
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
void addIntExtreme(void* target, const ValueResult& value) {
  IntExtremeState state = loadState<IntExtremeState>(target);
  if (!state.seen) {
    state.value = value.intValue;
    state.seen = true;
  } else if constexpr (Minimum) {
    state.value = std::min(state.value, value.intValue);
  } else {
    state.value = std::max(state.value, value.intValue);
  }
  storeState(target, state);
}

template <bool Minimum>
void mergeIntExtreme(void* target, const void* source) {
  IntExtremeState right = loadState<IntExtremeState>(source);
  if (!right.seen) return;
  ValueResult value = ValueResult::integer(right.value);
  addIntExtreme<Minimum>(target, value);
}

BucketScalar finishIntExtreme(const void* source) {
  IntExtremeState state = loadState<IntExtremeState>(source);
  return state.seen ? BucketScalar::integer(state.value)
                    : BucketScalar::missing(BucketValueType::INT128,
                                            ValueNature::NUMBER);
}

template <bool Minimum>
void addDoubleExtreme(void* target, const ValueResult& value) {
  DoubleExtremeState state = loadState<DoubleExtremeState>(target);
  if (state.failure != AggregateFailure::NONE) return;
  if (!std::isfinite(value.doubleValue)) {
    state.failure = AggregateFailure::NON_FINITE_EXPRESSION;
    storeState(target, state);
    return;
  }
  if (!state.seen) {
    state.value = value.doubleValue;
    state.seen = true;
  } else if constexpr (Minimum) {
    state.value = std::min(state.value, value.doubleValue);
  } else {
    state.value = std::max(state.value, value.doubleValue);
  }
  storeState(target, state);
}

template <bool Minimum>
void mergeDoubleExtreme(void* target, const void* source) {
  DoubleExtremeState right = loadState<DoubleExtremeState>(source);
  if (right.failure != AggregateFailure::NONE) {
    DoubleExtremeState left = loadState<DoubleExtremeState>(target);
    if (left.failure == AggregateFailure::NONE) {
      left.failure = right.failure;
      storeState(target, left);
    }
    return;
  }
  if (!right.seen) return;
  ValueResult value = ValueResult::floating(right.value);
  addDoubleExtreme<Minimum>(target, value);
}

BucketScalar finishDoubleExtreme(const void* source) {
  DoubleExtremeState state = loadState<DoubleExtremeState>(source);
  if (state.failure != AggregateFailure::NONE) {
    return BucketScalar::failed(BucketValueType::DOUBLE,
                                ValueNature::NUMBER, state.failure);
  }
  return state.seen ? BucketScalar::floating(state.value)
                    : BucketScalar::missing(BucketValueType::DOUBLE,
                                            ValueNature::NUMBER);
}

AggregateStateOps intSumOps(bool average) {
  return {sizeof(IntSumState), initState<IntSumState>, addIntSum, mergeIntSum,
          average ? finishIntAvg : finishIntSum};
}

AggregateStateOps doubleSumOps(bool average) {
  return {sizeof(DoubleSumState), initState<DoubleSumState>,
          average ? addDoubleSum<true> : addDoubleSum<false>,
          average ? mergeDoubleSum<true> : mergeDoubleSum<false>,
          average ? finishDoubleAvg : finishDoubleSum};
}

AggregateStateOps intExtremeOps(bool minimum) {
  return {sizeof(IntExtremeState), initState<IntExtremeState>,
          minimum ? addIntExtreme<true> : addIntExtreme<false>,
          minimum ? mergeIntExtreme<true> : mergeIntExtreme<false>,
          finishIntExtreme};
}

AggregateStateOps doubleExtremeOps(bool minimum) {
  return {sizeof(DoubleExtremeState), initState<DoubleExtremeState>,
          minimum ? addDoubleExtreme<true> : addDoubleExtreme<false>,
          minimum ? mergeDoubleExtreme<true> : mergeDoubleExtreme<false>,
          finishDoubleExtreme};
}

ResolvedAggregate resolveAvg(const ValueNode* input) {
  assert(input != nullptr);
  bool floating = input->type == ValueType::DOUBLE;
  return {BucketValueType::DOUBLE, input->nature,
          floating ? doubleSumOps(true) : intSumOps(true)};
}

ResolvedAggregate resolveSum(const ValueNode* input) {
  assert(input != nullptr);
  if (input->nature == ValueNature::DATE) {
    throw std::runtime_error("sum() cannot aggregate a DATE expression");
  }
  bool floating = input->type == ValueType::DOUBLE;
  return {floating ? BucketValueType::DOUBLE : BucketValueType::INT128,
          ValueNature::NUMBER,
          floating ? doubleSumOps(false) : intSumOps(false)};
}

ResolvedAggregate resolveMin(const ValueNode* input) {
  assert(input != nullptr);
  bool floating = input->type == ValueType::DOUBLE;
  return {floating ? BucketValueType::DOUBLE : BucketValueType::INT128,
          input->nature,
          floating ? doubleExtremeOps(true) : intExtremeOps(true)};
}

ResolvedAggregate resolveMax(const ValueNode* input) {
  assert(input != nullptr);
  bool floating = input->type == ValueType::DOUBLE;
  return {floating ? BucketValueType::DOUBLE : BucketValueType::INT128,
          input->nature,
          floating ? doubleExtremeOps(false) : intExtremeOps(false)};
}

constexpr AggregateFunction FUNCTIONS[] = {
    {AggregateOpcode::AVG, "avg", 1, 1, resolveAvg},
    {AggregateOpcode::SUM, "sum", 1, 1, resolveSum},
    {AggregateOpcode::MIN, "min", 1, 1, resolveMin},
    {AggregateOpcode::MAX, "max", 1, 1, resolveMax},
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
