#include "luxir/value/AggregateExpr.h"

#include <cmath>
#include <limits>
#include <stdexcept>

#include "luxir/api/luxir_types.hpp"

namespace luxir {
namespace {

double asDouble(const BucketScalar& value) {
  return value.type == BucketValueType::DOUBLE
      ? value.doubleValue : (double)value.intValue;
}

BucketScalar evalUnary(const AggregateNode& node, const BucketScalar& child) {
  if (child.failure != AggregateFailure::NONE) {
    return BucketScalar::failed(node.type, node.nature, child.failure);
  }
  if (!child.valid) return BucketScalar::missing(node.type, node.nature);
  if (node.type == BucketValueType::DOUBLE) {
    double input = asDouble(child);
    double result;
    switch (node.opcode) {
      case ValueOpcode::NEG: result = -input; break;
      case ValueOpcode::ABS: result = std::abs(input); break;
      case ValueOpcode::SQRT: result = std::sqrt(input); break;
      case ValueOpcode::LOG: result = std::log(input); break;
      case ValueOpcode::LOG1P: result = std::log1p(input); break;
      case ValueOpcode::FLOOR: result = std::floor(input); break;
      default: std::unreachable();
    }
    if (!std::isfinite(result)) {
      return BucketScalar::failed(node.type, node.nature,
                                  AggregateFailure::NON_FINITE_EXPRESSION);
    }
    return BucketScalar::floating(result, node.nature);
  }
  __int128 result;
  if (node.opcode == ValueOpcode::ABS && child.intValue >= 0) {
    return BucketScalar::integer(child.intValue, node.nature);
  }
  if (__builtin_sub_overflow((__int128)0, child.intValue, &result)) {
    return BucketScalar::failed(node.type, node.nature,
                                AggregateFailure::INT128_OVERFLOW);
  }
  return BucketScalar::integer(result, node.nature);
}

BucketScalar evalBinary(const AggregateNode& node, const BucketScalar& left,
                        const BucketScalar& right) {
  if (left.failure != AggregateFailure::NONE) {
    return BucketScalar::failed(node.type, node.nature, left.failure);
  }
  if (right.failure != AggregateFailure::NONE) {
    return BucketScalar::failed(node.type, node.nature, right.failure);
  }
  if (!left.valid || !right.valid) {
    return BucketScalar::missing(node.type, node.nature);
  }
  if (node.type == BucketValueType::DOUBLE) {
    double a = asDouble(left);
    double b = asDouble(right);
    if (node.opcode == ValueOpcode::DIV && b == 0.0) {
      return BucketScalar::missing(node.type, node.nature);
    }
    double result;
    switch (node.opcode) {
      case ValueOpcode::ADD: result = a + b; break;
      case ValueOpcode::SUB: result = a - b; break;
      case ValueOpcode::MUL: result = a * b; break;
      case ValueOpcode::DIV: result = a / b; break;
      default: std::unreachable();
    }
    if (!std::isfinite(result)) {
      return BucketScalar::failed(node.type, node.nature,
                                  AggregateFailure::NON_FINITE_EXPRESSION);
    }
    return BucketScalar::floating(result, node.nature);
  }

  __int128 result = 0;
  bool overflow = false;
  switch (node.opcode) {
    case ValueOpcode::ADD:
      overflow = __builtin_add_overflow(left.intValue, right.intValue, &result);
      break;
    case ValueOpcode::SUB:
      overflow = __builtin_sub_overflow(left.intValue, right.intValue, &result);
      break;
    case ValueOpcode::MUL:
      overflow = __builtin_mul_overflow(left.intValue, right.intValue, &result);
      break;
    default:
      std::unreachable();
  }
  if (overflow) {
    return BucketScalar::failed(node.type, node.nature,
                                AggregateFailure::INT128_OVERFLOW);
  }
  return BucketScalar::integer(result, node.nature);
}

} // namespace

std::string_view aggregateFailureReason(AggregateFailure failure) {
  switch (failure) {
    case AggregateFailure::NONE: return {};
    case AggregateFailure::VALUE_EVALUATION:
      return "per-document expression produced an invalid numeric value";
    case AggregateFailure::INT128_OVERFLOW:
      return "integer aggregation overflowed int128";
    case AggregateFailure::COUNT_OVERFLOW:
      return "aggregate value count overflowed uint64";
    case AggregateFailure::NON_FINITE_SUM:
      return "floating-point sum produced NaN or infinity";
    case AggregateFailure::NON_FINITE_AVERAGE:
      return "average produced NaN or infinity";
    case AggregateFailure::NON_FINITE_EXPRESSION:
      return "bucket expression produced NaN or infinity";
    case AggregateFailure::INT64_OUTPUT_OVERFLOW:
      return "integer result does not fit the int64 response type";
  }
  return "aggregate evaluation failed";
}

AggregateAccumulator::AggregateAccumulator(const AggregateProgram& program)
    : program(&program), states(program.stateBytes) {
  for (const AggregateLeaf& leaf : program.leaves) {
    leaf.resolved.state.init(states.data() + leaf.stateOffset);
  }
}

void AggregateAccumulator::add(uint32_t leafIndex, const ValueResult& value) {
  if (!value.valid) return;
  if (failed()) return;
  const AggregateLeaf& leaf = program->leaves[leafIndex];
  leaf.resolved.state.accumulate(states.data() + leaf.stateOffset, value);
}

void AggregateAccumulator::fail(AggregateFailure reason) {
  if (failure == AggregateFailure::NONE) failure = reason;
}

BucketScalar AggregateAccumulator::evaluate(
    std::span<const BucketScalar> aggregates) const {
  std::vector<BucketScalar> values(program->nodes.size());
  for (size_t i = 0; i < program->nodes.size(); i++) {
    const AggregateNode& node = program->nodes[i];
    switch (node.kind) {
      case AggregateNodeKind::CONSTANT:
        values[i] = node.type == BucketValueType::DOUBLE
            ? BucketScalar::floating(node.doubleValue, node.nature)
            : BucketScalar::integer(node.intValue, node.nature);
        break;
      case AggregateNodeKind::AGGREGATE:
        values[i] = aggregates[node.aggregate];
        break;
      case AggregateNodeKind::UNARY:
        values[i] = evalUnary(node, values[node.children[0]]);
        break;
      case AggregateNodeKind::BINARY:
        values[i] = evalBinary(node, values[node.children[0]],
                               values[node.children[1]]);
        break;
    }
  }
  return values[program->rootNode];
}

BucketScalar AggregateAccumulator::finish() const {
  const AggregateNode& root = program->root();
  if (failure != AggregateFailure::NONE) {
    return BucketScalar::failed(root.type, root.nature, failure);
  }
  std::vector<BucketScalar> aggregates(program->leaves.size());
  for (size_t i = 0; i < program->leaves.size(); i++) {
    const AggregateLeaf& leaf = program->leaves[i];
    BucketScalar value = leaf.resolved.state.finish(
        states.data() + leaf.stateOffset);
    value.nature = leaf.resolved.nature;
    aggregates[i] = value;
  }
  BucketScalar result = evaluate(aggregates);
  if (result.valid && result.type == BucketValueType::INT128
      && (result.intValue < std::numeric_limits<int64_t>::min()
          || result.intValue > std::numeric_limits<int64_t>::max())) {
    return BucketScalar::failed(result.type, result.nature,
                                AggregateFailure::INT64_OUTPUT_OVERFLOW);
  }
  return result;
}

AggregateAccumulator* AggregateAccumulator::merge(
    AggregateAccumulator* target, AggregateAccumulator* source) {
  if (target->program != source->program) {
    throw std::runtime_error("cannot merge different aggregate programs");
  }
  if (target->failure == AggregateFailure::NONE) {
    target->failure = source->failure;
  }
  for (const AggregateLeaf& leaf : target->program->leaves) {
    leaf.resolved.state.merge(target->states.data() + leaf.stateOffset,
                              source->states.data() + leaf.stateOffset);
  }
  return target;
}

void writeAggregateValue(api::Val& target, const BucketScalar& value) {
  if (!value.valid) {
    target.kind.emplace<google::protobuf::NullValue>(
        google::protobuf::NullValue::NULL_VALUE);
    return;
  }
  if (value.type == BucketValueType::DOUBLE) {
    target.kind.emplace<double>(value.doubleValue);
    return;
  }
  if (value.intValue < std::numeric_limits<int64_t>::min()
      || value.intValue > std::numeric_limits<int64_t>::max()) {
    target.kind.emplace<google::protobuf::NullValue>(
        google::protobuf::NullValue::NULL_VALUE);
    return;
  }
  target.kind.emplace<int64_t>((int64_t)value.intValue);
}

} // namespace luxir
