// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include <google/protobuf/arena.h>

#include "luxir/util/proto.h"
#include "luxir/reader/OrdColReader.h"
#include "luxir/reader/StrColReader.h"
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

inline BucketScalar integerAggregateOutput(BucketScalar value) {
  if (value.valid && value.type == BucketValueType::INT128
      && (value.intValue < INT64_MIN || value.intValue > INT64_MAX)) {
    return BucketScalar::failed(value.type, value.nature, AggregateFailure::INT64_OUTPUT_OVERFLOW);
  }
  return value;
}

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
  // Ordinals identify values only within this input's pinned reader dictionary.
  using AccumulateOrdBatch = void (*)(
      void* state, std::span<const int64_t> ords, int64_t numOrds,
      size_t maxSetBytes, std::span<const AggregateConstant> arguments);
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
  AccumulateOrdBatch accumulateOrdBatch = nullptr;
  // The aggregate needs only how many values each document holds. A bare
  // column input then binds to its shape (BoundValueCounts), and every raw
  // input (point, batch, keyed) is a per-document value count, not a value
  // (distinct values for an ARRAY terms column).
  bool countsValues = false;
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

  // Reads per-document value counts from a column shape (see countsValues).
  bool countsColumnValues() const {
    return resolved.state.countsValues && input->root().kind == ValueNodeKind::COLUMN;
  }
};

struct AggregateLeaf {
  uint32_t producer = 0;
  std::span<const AggregateConstant> arguments;
};

enum class AggregateNodeKind : uint8_t {
  CONSTANT,
  AGGREGATE,
  FUNCTION,
};

struct AggregateNode {
  AggregateNodeKind kind = AggregateNodeKind::CONSTANT;
  BucketValueType type = BucketValueType::INT128;
  ValueNature nature = ValueNature::NUMBER;
  const ValueFunction* function = nullptr;
  std::array<uint32_t, VALUE_MAX_ARITY> children{};
  uint8_t childCount = 0;
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
  void addOrdBatch(uint32_t producer, std::span<const int64_t> ords,
                   int64_t numOrds, size_t maxSetBytes);
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
  void addOrdBatch(uint32_t producer, std::span<const int64_t> ords,
                   int64_t numOrds, size_t maxSetBytes);
  void fail(AggregateFailure reason);
  bool failed() const;
  BucketScalar finish(AggregateEvalScratch& scratch) const;
  BucketScalar finish() const;

  BucketScalar finish(const AggregateProgram& readout, AggregateEvalScratch& scratch) const;
  static AggregateAccumulator* merge(AggregateAccumulator* target,
                                     AggregateAccumulator* source);
};

void writeAggregateValue(api::Val& target, const BucketScalar& value);

// How many values each document of a segment holds in a bare column, read
// from the column shape (docs with the field plus end value ranks) without
// decoding a value. A nested descendant column counts the values of every row
// the owner holds. Without either the segment has no such column.
//
// Like Elasticsearch value_count, a document counts each distinct value of a
// terms column (STRING, ID, IP) once, since Elasticsearch keeps those as
// sorted sets, and every value of an int64 column (numeric, date, boolean,
// geo point), since it keeps those as sorted lists. A SET column already holds
// a repeated value once, so only a multi-valued ARRAY terms column reads its
// values (ords, or raw bytes when not indexed) to count each document's
// distinct ones; every other column counts from the shape alone. A nested row
// is its own document.
struct BoundValueCounts {
  const ColumnShape* shape = nullptr;
  std::optional<ColumnShape::Cursor> cursor;
  BoundColumnRange* mapping = nullptr;
  // At most one is set, for an ARRAY terms column with a multi-valued shape.
  OrdColReader::BulkOrds* ords = nullptr;
  StrColReader::DocValues* strings = nullptr;

  bool distinct() const { return ords != nullptr || strings != nullptr; }

  int64_t count(int32_t docid) {
    if (mapping == nullptr && !distinct()) [[likely]] {
      return cursor && cursor->seek(docid) ? cursor->count() : 0;
    }
    return countSlow(docid);
  }

  // Values of every document of the segment.
  int64_t segmentValues() const;

  // Distinct values in the value rank range [first, end) of one document.
  int64_t distinctValues(int64_t first, int64_t end) const;

private:
  // A nested owner's rows, or a document whose repeats count once.
  int64_t countSlow(int32_t docid);
};

struct BoundAggregateInput {
  BoundValueProgram* values = nullptr;
  const AggregateProducer* producer = nullptr;
  OrdMap::SegToGlobal ordMapping;
  IntColReader::BulkIterator* columnIterator = nullptr;
  // Set for a countsColumnValues() producer, which is then a bareColumn whose
  // raw point is the document's value count.
  BoundValueCounts* valueCounts = nullptr;
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
      BoundStringColumn* column = values->nodes[producer->input->rootNode].stringColumn;
      if (column == nullptr || column->ords == nullptr) return;
      auto consumeRow = [&](int32_t row) {
        if (!column->cursor.seek(row)) return;
        for (int64_t rank = column->cursor.begin(); rank < column->cursor.end(); rank++) {
          int32_t stored = column->storedOrd(row, rank);
          assert(stored != 0);
          consume(ValueResult::integer(ordMapping.globalOrd(stored - 1)));
        }
      };
      auto* mapping = values->nodes[producer->input->rootNode].mapping;
      if (mapping) {
        auto family = mapping->owners.seek(docid);
        for (int32_t row = family.first; row < family.end; row++) consumeRow(row);
      } else consumeRow(docid);
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
    assert(bareColumn && (scalar() || valueCounts != nullptr));
    if (valueCounts != nullptr) {
      raw = valueCounts->count(docid);
      return raw != 0;
    }
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
    assert(scalar() || valueCounts != nullptr);
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
