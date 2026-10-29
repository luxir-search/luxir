// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

#include <google/protobuf/arena.h>

#include "luxir/util/proto.h"
#include "luxir/reader/OrdColReader.h"
#include "luxir/search/OrdMap.h"
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

struct AggregateConstant {
  ValueType type = ValueType::INT64;
  int64_t intValue = 0;
  double doubleValue = 0.0;
};

struct AggregateStateOps {
  using Init = void (*)(void* state,
                        std::span<const AggregateConstant> arguments,
                        std::pmr::memory_resource* resource);
  using Destroy = void (*)(void* state) noexcept;
  using Accumulate = void (*)(void* state, const ValueResult& value,
                              std::span<const AggregateConstant> arguments);
  using AccumulateBatch = void (*)(
      void* state, std::span<const ScalarValueResult> values,
      std::span<const AggregateConstant> arguments);
  using AccumulateRawPoint = void (*)(
      void* state, int64_t raw, FieldType::Type columnType,
      std::span<const AggregateConstant> arguments);
  using AccumulateRawBatch = void (*)(
      void* state, std::span<const int64_t> raw,
      FieldType::Type columnType,
      std::span<const AggregateConstant> arguments);
  using Merge = void (*)(void* target, const void* source,
                         std::span<const AggregateConstant> arguments);
  using Finish = BucketScalar (*)(
      const void* state, std::span<const AggregateConstant> arguments);
  // accumulateRawPoint(states + keys[i] * stride, raw[i]) for every i: one
  // call folds a batch whose values belong to different keyed states.
  using AccumulateKeyedRawBatch = void (*)(
      void* states, size_t stride, std::span<const int64_t> keys,
      std::span<const int64_t> raw, FieldType::Type columnType,
      std::span<const AggregateConstant> arguments);

  uint32_t bytes = 0;
  Init init = nullptr;
  Accumulate accumulate = nullptr;
  AccumulateBatch accumulateBatch = nullptr;
  AccumulateRawPoint accumulateRawPoint = nullptr;
  AccumulateRawBatch accumulateRawBatch = nullptr;
  Merge merge = nullptr;
  Finish finish = nullptr;
  AccumulateKeyedRawBatch accumulateKeyedRawBatch = nullptr;
  Destroy destroy = nullptr;
  bool inlineCapable = true;
  bool replayCapable = true;
  bool orderSensitive = false;
};

// Optional packed state for a bare, single-valued column that is present on
// every document. The enclosing ordinal-entry count is then the aggregate value
// count, so avg need not duplicate that uint64_t in every metric entry.
struct DenseFacetStateOps {
  using Init = void (*)(void* state);
  using AccumulateRawPoint = void (*)(void* state, int64_t raw);
  // accumulateRawPoint(states + keys[i] * stride, raw[i]) for every i.
  using AccumulateKeyedRawBatch = void (*)(void* states, size_t stride,
                                           std::span<const int64_t> keys,
                                           std::span<const int64_t> raw);
  using Fail = void (*)(void* state, AggregateFailure failure);
  using Merge = void (*)(void* target, const void* source);
  using Finish = BucketScalar (*)(const void* state, int64_t count);

  uint32_t bytes = 0;
  Init init = nullptr;
  AccumulateRawPoint accumulateRawPoint = nullptr;
  Fail fail = nullptr;
  Merge merge = nullptr;
  Finish finish = nullptr;
  AccumulateKeyedRawBatch accumulateKeyedRawBatch = nullptr;
};

struct ResolvedAggregate {
  BucketValueType type = BucketValueType::INT128;
  ValueNature nature = ValueNature::NUMBER;
  AggregateStateOps state;
  const DenseFacetStateOps* denseFacetState = nullptr;
};

struct AggregateFunction;

struct AggregateProducer {
  const AggregateFunction* function = nullptr;
  ValueProgram* input = nullptr;
  ResolvedAggregate resolved;
  std::span<const AggregateConstant> arguments;
  uint32_t stateOffset = 0;

  bool sameState(const AggregateProducer& other) const;
};

struct AggregateLeaf {
  uint32_t producer = 0;
  std::span<const AggregateConstant> arguments;
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
  const ValueFunction* function = nullptr;
  std::array<uint32_t, 2> children{};
  uint32_t aggregate = 0;
  size_t sourcePos = 0;
  __int128 intValue = 0;
  double doubleValue = 0.0;
};

struct AggregateEvalScratch {
  std::vector<BucketScalar> aggregates;
  std::vector<BucketScalar> nodes;
};

class AggregateProgram {
public:
  ArenaResource resource;
  std::pmr::vector<AggregateNode> nodes;
  std::pmr::vector<AggregateProducer> producers;
  std::pmr::vector<AggregateLeaf> leaves;
  uint32_t rootNode = 0;
  uint32_t stateBytes = 0;
  bool hasIndirectStates = false;

  explicit AggregateProgram(google::protobuf::Arena& arena)
      : resource(&arena), nodes(&resource), producers(&resource), leaves(&resource) {}

  uint32_t addProducer(AggregateProducer producer, bool sharing = true);
  // Before binding or creating state; refresh resolved fields after adoption.
  // Source inputs/arguments must outlive this program; unequal sets are unchanged.
  bool adoptProducers(const AggregateProgram& source);

  uint32_t addNode(AggregateNode node) {
    nodes.push_back(node);
    rootNode = (uint32_t)nodes.size() - 1;
    return rootNode;
  }

  const AggregateNode& root() const { return nodes[rootNode]; }

  std::span<const AggregateConstant> copyConstants(
      std::span<const AggregateConstant> values) {
    if (values.empty()) return {};
    auto* target = (AggregateConstant*)resource.allocate(
        sizeof(AggregateConstant) * values.size(),
        alignof(AggregateConstant));
    std::uninitialized_copy(values.begin(), values.end(), target);
    return {target, values.size()};
  }
};

// A non-owning aggregate state stored in caller-provided bytes. Facet entries
// are packed without alignment, so every state callback and header access goes
// through the unaligned load/store helpers.
class AggregateStateView {
  const AggregateProgram* program;
  std::byte* storage;

  AggregateFailure failure() const;
  void setFailure(AggregateFailure reason);
  void* producerState(const AggregateProducer& producer) const;
  BucketScalar evaluate(AggregateEvalScratch& scratch) const;

public:
  AggregateStateView(const AggregateProgram& program, void* storage)
      : program(&program), storage(static_cast<std::byte*>(storage)) {}

  static uint32_t bytes(const AggregateProgram& program) {
    return (uint32_t)sizeof(AggregateFailure) + program.stateBytes;
  }

  // Init rolls back completed producers on failure; each init must itself
  // leave no resources behind when it throws.
  void init(std::pmr::memory_resource* resource = nullptr);
  void destroy() noexcept;
  void add(uint32_t producer, const ValueResult& value);
  void addBatch(uint32_t producer, std::span<const ScalarValueResult> values);
  void addRawBatch(uint32_t producer, std::span<const int64_t> values,
                   FieldType::Type columnType);
  void fail(AggregateFailure reason);
  bool failed() const { return failure() != AggregateFailure::NONE; }
  void merge(const AggregateStateView& source);
  BucketScalar finish(AggregateEvalScratch& scratch) const;
  BucketScalar finish() const;
};

class AggregateAccumulator {
  const AggregateProgram* program = nullptr;
  std::vector<std::byte> states;

public:
  int64_t releaseCount = 0;

  AggregateAccumulator() = default;
  explicit AggregateAccumulator(const AggregateProgram& program,
      std::pmr::memory_resource* resource = nullptr);
  ~AggregateAccumulator();
  AggregateAccumulator(const AggregateAccumulator&) = delete;
  AggregateAccumulator& operator=(const AggregateAccumulator&) = delete;

  void add(uint32_t producer, const ValueResult& value);
  void addBatch(uint32_t producer, std::span<const ScalarValueResult> values);
  void addRawBatch(uint32_t producer, std::span<const int64_t> values,
                   FieldType::Type columnType);
  void fail(AggregateFailure reason);
  bool failed() const;
  BucketScalar finish(AggregateEvalScratch& scratch) const;
  BucketScalar finish() const;

  BucketScalar finish(const AggregateProgram& readout, AggregateEvalScratch& scratch) const;
  static AggregateAccumulator* merge(AggregateAccumulator* target,
                                     AggregateAccumulator* source);
};

void writeAggregateValue(api::Val& target, const BucketScalar& value);
struct BoundAggregateInput {
  BoundValueProgram* values = nullptr;
  const AggregateProducer* producer = nullptr;
  OrdColReader* ordColumn = nullptr;
  OrdColReader::Iterator* ordIterator = nullptr;
  OrdMap::SegToGlobal ordMapping;
  IntColReader::BulkIterator* columnIterator = nullptr;
  AggregateStateOps::AccumulateRawPoint accumulateRawPoint = nullptr;
  std::span<const AggregateConstant> arguments;
  uint32_t producerStateOffset = 0;
  uint32_t producerIndex = 0;
  FieldType::Type columnType = FieldType::NONE;
  bool columnScan = false;
  bool bareColumn = false;

  bool scalar() const {
    return producer->input->root().type != ValueType::COLUMN_ONLY
        && !valueArray(producer->input->root().type);
  }

  template <typename Consume>
  void forEachValue(int32_t docid, Consume&& consume) {
    if (producer->input->root().type == ValueType::COLUMN_ONLY) {
      if (ordIterator == nullptr) return;
      if (ordIterator->docId() < docid) ordIterator->advance(docid);
      if (ordIterator->docId() != docid) return;
      auto add = [&](int32_t stored) {
        if (stored != 0) consume(ValueResult::integer(ordMapping.globalOrd(stored - 1)));
      };
      if (ordColumn->multiValued()) {
        auto [start, end] = ordColumn->getStartEndValueRank(ordIterator->rank());
        for (int64_t i = start; i < end; i++) add(ordIterator->values().valueAt(i));
      } else {
        add(ordIterator->value());
      }
      return;
    }
    ValueResult value = evalPoint(docid);
    if (!value.valid) return;
    if (valueArray(value.type)) {
      for (int64_t i = 0; i < value.array.size; i++) {
        consume(values->evalArrayElement(value.array, i));
      }
    } else {
      consume(value);
    }
  }

  bool readRawPoint(int32_t docid, int64_t& raw) {
    assert(bareColumn && scalar());
    if (columnIterator == nullptr) return false;
    IntColReader::BulkIterator& iterator = *columnIterator;
    if (iterator.docId() < docid) iterator.advance(docid);
    if (iterator.docId() != docid) return false;
    raw = iterator.value();
    return true;
  }

  ValueResult evalPoint(int32_t docid) {
    return values->evalSequentialPoint(docid, 0.0f);
  }

  void evalBatch(std::span<const int32_t> docids,
                 std::span<ScalarValueResult> results) {
    values->evalScalarBatch(docids, results);
  }

  void accumulatePoint(void* aggregateState, int32_t docid) {
    assert(scalar());
    void* producerState = static_cast<std::byte*>(aggregateState)
        + producerStateOffset;
    if (bareColumn) {
      int64_t raw;
      if (!readRawPoint(docid, raw)) return;
      accumulateRawPoint(producerState, raw, columnType, arguments);
      return;
    }
    ValueResult value = values->evalSequentialPoint(docid, 0.0f);
    if (value.valid) {
      producer->resolved.state.accumulate(
          producerState, value, arguments);
    }
  }

  void accumulateBatch(void* aggregateState,
                       std::span<const ScalarValueResult> batch) const {
    void* producerState = static_cast<std::byte*>(aggregateState)
        + producerStateOffset;
    producer->resolved.state.accumulateBatch(producerState, batch, arguments);
  }

};

struct AggregateFieldInfos {
  ValueFieldInfos values;
  // Borrowed from the reader's field cache, pinned by the request reader.
  const OrdMap* ords = nullptr;
};

std::span<const AggregateFieldInfos> resolveAggregateFields(
    const AggregateProgram& program, MemPool& pool, IndexReader& reader);

std::vector<BoundAggregateInput> bindAggregateInputs(
    const AggregateProgram& program, MemPool& pool,
    IndexReader::Segment& segment, std::span<const AggregateFieldInfos> fields);

} // namespace luxir
