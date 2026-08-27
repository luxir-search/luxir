#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <span>
#include <string_view>
#include <vector>

#include <google/protobuf/arena.h>

#include "luxir/util/proto.h"
#include "luxir/value/ValueExpr.h"

namespace luxir {

enum class BucketValueType : uint8_t {
  INT128,
  DOUBLE,
};

enum class AggregateFailure : uint8_t {
  NONE,
  VALUE_EVALUATION,
  INT128_OVERFLOW,
  COUNT_OVERFLOW,
  NON_FINITE_SUM,
  NON_FINITE_AVERAGE,
  NON_FINITE_EXPRESSION,
  INT64_OUTPUT_OVERFLOW,
};

std::string_view aggregateFailureReason(AggregateFailure failure);

struct BucketScalar {
  BucketValueType type = BucketValueType::INT128;
  ValueNature nature = ValueNature::NUMBER;
  AggregateFailure failure = AggregateFailure::NONE;
  bool valid = false;
  __int128 intValue = 0;
  double doubleValue = 0.0;

  static BucketScalar missing(BucketValueType type, ValueNature nature) {
    BucketScalar value;
    value.type = type;
    value.nature = nature;
    return value;
  }

  static BucketScalar integer(__int128 number,
                              ValueNature nature = ValueNature::NUMBER) {
    BucketScalar value;
    value.valid = true;
    value.intValue = number;
    value.nature = nature;
    return value;
  }

  static BucketScalar failed(BucketValueType type, ValueNature nature,
                             AggregateFailure failure) {
    BucketScalar value = missing(type, nature);
    value.failure = failure;
    return value;
  }

  static BucketScalar floating(double number,
                               ValueNature nature = ValueNature::NUMBER) {
    BucketScalar value;
    value.type = BucketValueType::DOUBLE;
    value.valid = true;
    value.doubleValue = number;
    value.nature = nature;
    return value;
  }
};

struct AggregateStateOps {
  using Init = void (*)(void* state);
  using Accumulate = void (*)(void* state, const ValueResult& value);
  using Merge = void (*)(void* target, const void* source);
  using Finish = BucketScalar (*)(const void* state);

  uint32_t bytes = 0;
  Init init = nullptr;
  Accumulate accumulate = nullptr;
  Merge merge = nullptr;
  Finish finish = nullptr;

  // A batch accumulate entry point is deliberately deferred until facet state
  // layout is repacked; that round determines the useful batch memory shape.
};

struct ResolvedAggregate {
  BucketValueType type = BucketValueType::INT128;
  ValueNature nature = ValueNature::NUMBER;
  AggregateStateOps state;
};

struct AggregateLeaf {
  ValueProgram* input = nullptr;
  ResolvedAggregate resolved;
  uint32_t stateOffset = 0;
};

enum class AggregateNodeKind : uint8_t {
  CONSTANT,
  AGGREGATE,
  UNARY,
  BINARY,
};

struct AggregateNode {
  AggregateNodeKind kind = AggregateNodeKind::CONSTANT;
  BucketValueType type = BucketValueType::INT128;
  ValueNature nature = ValueNature::NUMBER;
  ValueOpcode opcode = ValueOpcode::NONE;
  std::array<uint32_t, 2> children{};
  uint32_t aggregate = 0;
  size_t sourcePos = 0;
  __int128 intValue = 0;
  double doubleValue = 0.0;
};

class AggregateProgram {
public:
  ArenaResource resource;
  std::pmr::vector<AggregateNode> nodes;
  std::pmr::vector<AggregateLeaf> leaves;
  uint32_t rootNode = 0;
  uint32_t stateBytes = 0;

  explicit AggregateProgram(google::protobuf::Arena& arena)
      : resource(&arena), nodes(&resource), leaves(&resource) {}

  uint32_t addNode(AggregateNode node) {
    nodes.push_back(node);
    rootNode = (uint32_t)nodes.size() - 1;
    return rootNode;
  }

  const AggregateNode& root() const { return nodes[rootNode]; }
};

class AggregateAccumulator {
  const AggregateProgram* program = nullptr;
  std::vector<std::byte> states;
  AggregateFailure failure = AggregateFailure::NONE;

  BucketScalar evaluate(std::span<const BucketScalar> aggregates) const;

public:
  int64_t releaseCount = 0;

  AggregateAccumulator() = default;
  explicit AggregateAccumulator(const AggregateProgram& program);

  void add(uint32_t leaf, const ValueResult& value);
  void fail(AggregateFailure reason);
  bool failed() const { return failure != AggregateFailure::NONE; }
  BucketScalar finish() const;

  static AggregateAccumulator* merge(AggregateAccumulator* target,
                                     AggregateAccumulator* source);
};

void writeAggregateValue(api::Val& target, const BucketScalar& value);

} // namespace luxir
