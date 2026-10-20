// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include "luxir/value/AggregateExpr.h"

#include <limits>
#include <stdexcept>

#include "luxir/api/luxir_types.hpp"
#include "luxir/util/NumericUtils.h"

namespace luxir {
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

AggregateFailure AggregateStateView::failure() const {
  return loadUnaligned<AggregateFailure>(storage);
}

void AggregateStateView::setFailure(AggregateFailure reason) {
  storeUnaligned<AggregateFailure>(storage, reason);
}

void* AggregateStateView::leafState(const AggregateLeaf& leaf) const {
  return storage + sizeof(AggregateFailure) + leaf.stateOffset;
}

void AggregateStateView::init(std::pmr::memory_resource* resource) {
  setFailure(AggregateFailure::NONE);
  size_t initialized = 0;
  try {
    for (const AggregateLeaf& leaf : program->leaves) {
      leaf.resolved.state.init(leafState(leaf), leaf.arguments, resource);
      initialized++;
    }
  } catch (...) {
    while (initialized != 0) {
      const auto& leaf = program->leaves[--initialized];
      if (leaf.resolved.state.destroy) leaf.resolved.state.destroy(leafState(leaf));
    }
    throw;
  }
}

void AggregateStateView::destroy() noexcept {
  for (const AggregateLeaf& leaf : program->leaves) {
    if (leaf.resolved.state.destroy) leaf.resolved.state.destroy(leafState(leaf));
  }
}

void AggregateStateView::add(uint32_t leafIndex, const ValueResult& value) {
  if (!value.valid || failed()) return;
  const AggregateLeaf& leaf = program->leaves[leafIndex];
  leaf.resolved.state.accumulate(leafState(leaf), value, leaf.arguments);
}

void AggregateStateView::addBatch(
    uint32_t leafIndex, std::span<const ScalarValueResult> values) {
  if (failed()) return;
  const AggregateLeaf& leaf = program->leaves[leafIndex];
  leaf.resolved.state.accumulateBatch(
      leafState(leaf), values, leaf.arguments);
}

void AggregateStateView::addRawBatch(
    uint32_t leafIndex, std::span<const int64_t> values,
    FieldType::Type columnType) {
  if (failed()) return;
  const AggregateLeaf& leaf = program->leaves[leafIndex];
  leaf.resolved.state.accumulateRawBatch(
      leafState(leaf), values, columnType, leaf.arguments);
}

void AggregateStateView::fail(AggregateFailure reason) {
  if (!failed()) setFailure(reason);
}

void AggregateStateView::merge(const AggregateStateView& source) {
  if (source.failed()) fail(source.failure());
  for (const AggregateLeaf& leaf : program->leaves) {
    leaf.resolved.state.merge(
        leafState(leaf), source.leafState(leaf), leaf.arguments);
  }
}

BucketScalar AggregateStateView::evaluate(AggregateEvalScratch& scratch) const {
  scratch.nodes.resize(program->nodes.size());
  for (size_t i = 0; i < program->nodes.size(); i++) {
    const AggregateNode& node = program->nodes[i];
    switch (node.kind) {
      case AggregateNodeKind::CONSTANT:
        scratch.nodes[i] = node.type == BucketValueType::DOUBLE
            ? BucketScalar::floating(node.doubleValue, node.nature)
            : BucketScalar::integer(node.intValue, node.nature);
        break;
      case AggregateNodeKind::AGGREGATE:
        scratch.nodes[i] = scratch.aggregates[node.aggregate];
        break;
      case AggregateNodeKind::UNARY: {
        assert(node.function != nullptr);
        std::array<BucketScalar, 1> args{
            scratch.nodes[node.children[0]]};
        scratch.nodes[i] = node.function->evalBucketScalar(
            args,
            node.type == BucketValueType::DOUBLE
                ? ValueType::DOUBLE : ValueType::INT64,
            node.nature);
        break;
      }
      case AggregateNodeKind::BINARY:
        assert(node.function != nullptr);
        std::array<BucketScalar, 2> args{
            scratch.nodes[node.children[0]],
            scratch.nodes[node.children[1]]};
        scratch.nodes[i] = node.function->evalBucketScalar(
            args,
            node.type == BucketValueType::DOUBLE
                ? ValueType::DOUBLE : ValueType::INT64,
            node.nature);
        break;
    }
  }
  return scratch.nodes[program->rootNode];
}

BucketScalar AggregateStateView::finish(AggregateEvalScratch& scratch) const {
  const AggregateNode& root = program->root();
  AggregateFailure stateFailure = failure();
  if (stateFailure != AggregateFailure::NONE) {
    return BucketScalar::failed(root.type, root.nature, stateFailure);
  }
  auto validateOutput = [](BucketScalar result) {
    if (result.valid && result.type == BucketValueType::INT128
        && (result.intValue < std::numeric_limits<int64_t>::min()
            || result.intValue > std::numeric_limits<int64_t>::max())) {
      return BucketScalar::failed(result.type, result.nature,
                                  AggregateFailure::INT64_OUTPUT_OVERFLOW);
    }
    return result;
  };
  if (root.kind == AggregateNodeKind::AGGREGATE) {
    const AggregateLeaf& leaf = program->leaves[root.aggregate];
    BucketScalar result = leaf.resolved.state.finish(
        leafState(leaf), leaf.arguments);
    result.nature = leaf.resolved.nature;
    return validateOutput(result);
  }
  scratch.aggregates.resize(program->leaves.size());
  for (size_t i = 0; i < program->leaves.size(); i++) {
    const AggregateLeaf& leaf = program->leaves[i];
    BucketScalar value = leaf.resolved.state.finish(
        leafState(leaf), leaf.arguments);
    value.nature = leaf.resolved.nature;
    scratch.aggregates[i] = value;
  }
  BucketScalar result = evaluate(scratch);
  return validateOutput(result);
}

BucketScalar AggregateStateView::finish() const {
  AggregateEvalScratch scratch;
  return finish(scratch);
}

AggregateAccumulator::AggregateAccumulator(const AggregateProgram& program,
                                           std::pmr::memory_resource* resource)
    : program(&program), states(AggregateStateView::bytes(program)) {
  AggregateStateView(program, states.data()).init(resource);
}

AggregateAccumulator::~AggregateAccumulator() {
  if (program != nullptr && program->hasIndirectStates) {
    AggregateStateView(*program, states.data()).destroy();
  }
}

void AggregateAccumulator::add(uint32_t leafIndex, const ValueResult& value) {
  AggregateStateView(*program, states.data()).add(leafIndex, value);
}

void AggregateAccumulator::addBatch(
    uint32_t leafIndex, std::span<const ScalarValueResult> values) {
  AggregateStateView(*program, states.data()).addBatch(leafIndex, values);
}

void AggregateAccumulator::addRawBatch(
    uint32_t leafIndex, std::span<const int64_t> values,
    FieldType::Type columnType) {
  AggregateStateView(*program, states.data()).addRawBatch(
      leafIndex, values, columnType);
}

void AggregateAccumulator::fail(AggregateFailure reason) {
  AggregateStateView(*program, states.data()).fail(reason);
}

bool AggregateAccumulator::failed() const {
  return AggregateStateView(
      *program, const_cast<std::byte*>(states.data())).failed();
}

BucketScalar AggregateAccumulator::finish(AggregateEvalScratch& scratch) const {
  return AggregateStateView(
      *program, const_cast<std::byte*>(states.data())).finish(scratch);
}

BucketScalar AggregateAccumulator::finish() const {
  AggregateEvalScratch scratch;
  return finish(scratch);
}

AggregateAccumulator* AggregateAccumulator::merge(
    AggregateAccumulator* target, AggregateAccumulator* source) {
  if (target->program != source->program) {
    throw std::runtime_error("cannot merge different aggregate programs");
  }
  AggregateStateView targetView(*target->program, target->states.data());
  AggregateStateView sourceView(*source->program, source->states.data());
  targetView.merge(sourceView);
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
  assert(value.intValue >= std::numeric_limits<int64_t>::min()
         && value.intValue <= std::numeric_limits<int64_t>::max());
  target.kind.emplace<int64_t>((int64_t)value.intValue);
}

std::span<const AggregateFieldInfos> resolveAggregateFields(
    const AggregateProgram& program, MemPool& pool, IndexReader& reader) {
  auto fields = pool.make_span<AggregateFieldInfos>(program.leaves.size());
  for (size_t i = 0; i < fields.size(); i++) {
    const ValueProgram& input = *program.leaves[i].input;
    fields[i].values = input.resolveFields(pool, reader);
    if (input.root().type == ValueType::COLUMN_ONLY) {
      fields[i].ords = reader.getOrdMap(input.root().text).get();
    }
  }
  return fields;
}

std::vector<BoundAggregateInput> bindAggregateInputs(
    const AggregateProgram& program, MemPool& pool,
    IndexReader::Segment& segment, std::span<const AggregateFieldInfos> fields) {
  std::vector<BoundAggregateInput> bindings;
  bindings.reserve(program.leaves.size());
  for (uint32_t index = 0; index < program.leaves.size(); index++) {
    const AggregateLeaf& leaf = program.leaves[index];
    const ValueNode& root = leaf.input->root();
    if (root.type == ValueType::COLUMN_ONLY) {
      BoundAggregateInput binding;
      binding.leaf = &leaf;
      binding.leafIndex = index;
      binding.leafStateOffset = (uint32_t)sizeof(AggregateFailure) + leaf.stateOffset;
      binding.columnType = root.columnType;
      binding.arguments = leaf.arguments;
      auto* info = fields[index].values[leaf.input->rootNode].at(segment);
      if (info != nullptr && fields[index].ords != nullptr) {
        binding.ordColumn = pool.make<OrdColReader>(segment.postingsReader(), *info);
        binding.ordIterator = pool.make<OrdColReader::Iterator>(*binding.ordColumn);
        binding.ordMapping = fields[index].ords->getSegToGlobal(segment.ord);
      }
      bindings.push_back(binding);
      continue;
    }
    BoundValueProgram* values = leaf.input->bind(pool, segment, fields[index].values);
    bool columnScan = root.kind == ValueNodeKind::COLUMN;
    bool bareColumn = columnScan && !valueArray(root.type);
    BoundValueNode& rootBinding = values->nodes[leaf.input->rootNode];
    bindings.push_back({
        .values = values,
        .leaf = &leaf,
        .ordMapping = {},
        .columnIterator = bareColumn && rootBinding.bulkIterator
            ? &*rootBinding.bulkIterator : nullptr,
        .accumulateRawPoint = leaf.resolved.state.accumulateRawPoint,
        .arguments = leaf.arguments,
        .leafStateOffset = (uint32_t)sizeof(AggregateFailure)
            + leaf.stateOffset,
        .leafIndex = index,
        .columnType = root.columnType,
        .columnScan = columnScan,
        .bareColumn = bareColumn
    });
  }
  return bindings;
}

} // namespace luxir
