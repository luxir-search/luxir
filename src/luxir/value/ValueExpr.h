// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <memory_resource>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include <google/protobuf/arena.h>

#include "luxir/reader/IntColReader.h"
#include "luxir/schema/FieldType.h"
#include "luxir/search/IndexReader.h"
#include "luxir/util/MemPool.h"
#include "luxir/util/proto.h"
#include "luxir/value/ValueFunctionRegistry.h"
#include "luxir/value/ValueTypes.h"

namespace luxir {

enum class ValueNodeKind : uint8_t {
  CONSTANT,
  VARIABLE,
  COLUMN,
  SCORE,
  DOCID,
  FUNCTION,
};

struct ValueNode {
  ValueNodeKind kind = ValueNodeKind::CONSTANT;
  ValueType type = ValueType::INT64;
  ValueNature nature = ValueNature::NUMBER;
  ValueOpcode opcode = ValueOpcode::NONE;
  const ValueFunction* function = nullptr;
  std::array<uint32_t, 2> children{};
  uint32_t arrayOffset = 0;
  uint32_t arraySize = 0;
  uint8_t childCount = 0;
  FieldType::Type columnType = FieldType::NONE;
  bool columnMultiValued = false;
  size_t sourcePos = 0;
  std::string_view text;
  int64_t intValue = 0;
  double doubleValue = 0.0;
};

class BoundValueProgram;

// Immutable after parsing. The object and all owned containers live on the
// request protobuf arena; segment bindings never write back into it.
class ValueProgram {
public:
  ArenaResource resource;
  std::pmr::vector<ValueNode> nodes;
  std::pmr::vector<int64_t> intArrays;
  std::pmr::vector<double> doubleArrays;
  std::optional<ValueResult> constantScalar;
  uint32_t rootNode = 0;
  bool needsScore = false;

  explicit ValueProgram(google::protobuf::Arena& arena)
      : resource(&arena), nodes(&resource), intArrays(&resource), doubleArrays(&resource) {}

  ValueNode& addNode(ValueNode node) {
    nodes.push_back(node);
    rootNode = (uint32_t)nodes.size() - 1;
    return nodes.back();
  }

  std::string_view copyString(std::string_view text) {
    char* dest = (char*)resource.allocate(text.size(), alignof(char));
    std::copy(text.begin(), text.end(), dest);
    return {dest, text.size()};
  }

  const ValueNode& root() const { return nodes[rootNode]; }
  BoundValueProgram* bind(MemPool& pool, IndexReader::Segment& segment) const;
};

struct BoundValueNode {
  std::optional<IntColReader> column;
  std::optional<IntColReader::SparseIterator> iterator;
  std::optional<IntColReader::BulkIterator> bulkIterator;
  ValueBounds bounds;
  int32_t cachedDoc = -1;
  int64_t valueStart = 0;
  int64_t valueEnd = 0;
  int32_t bulkLastDoc = -1;
  bool cachedPresent = false;
};

class BoundValueProgram {
  ValueBounds cachedScoreBounds;
  bool boundsCached = false;

public:
  const ValueProgram& program;
  IndexReader::Segment& segment;
  PostingsReader& postings;
  std::span<BoundValueNode> nodes;

  BoundValueProgram(MemPool& pool, const ValueProgram& program,
                    IndexReader::Segment& segment);

  ValueResult evalPoint(int32_t docid, float score);
  // Aggregate/facet scans call this with monotonically increasing document
  // IDs, allowing a bare column to keep its bulk decoder hot.
  ValueResult evalSequentialPoint(int32_t docid, float score);
  bool evalSequentialInt64(int32_t docid, int64_t& value);
  bool evalSequentialDouble(int32_t docid, double& value);
  ValueResult evalNode(uint32_t node, int32_t docid, float score);
  void evalBatch(std::span<const int32_t> docids, std::span<const float> scores,
                 std::span<ValueResult> results);
  void evalScalarBatch(std::span<const int32_t> docids,
                       std::span<ScalarValueResult> results);
  ValueResult evalArrayElement(const ValueArrayRef& array, int64_t index);
  const ValueBounds& bounds(uint32_t node) const { return nodes[node].bounds; }
  const ValueBounds& boundsForScore(const ValueBounds& scoreBounds);
  std::string_view firstUnboundedNode() const;

  ValueResult evalColumn(uint32_t node, int32_t docid, float score);
  ValueResult evalConstant(uint32_t node, int32_t docid, float score) const;
  int64_t arraySize(uint32_t node, int32_t docid, float score);

private:
  ValueResult evalColumnSequential(uint32_t node, int32_t docid,
                                   float score);
  bool readColumnSequential(uint32_t node, int32_t docid, int64_t& raw);
  void evalColumnBatch(uint32_t node, std::span<const int32_t> docids,
                       std::span<ValueResult> results);
  void propagateBounds(const ValueBounds& scoreBounds);
};

// A binding owns only segment-MemPool state. It also clears the caller's
// runtime pointer before destructing/rewinding, including exceptional unwind.
class BoundValueGuard {
public:
  MemPool::ScopeGuard scope;
  BoundValueProgram*& runtime;

  BoundValueGuard(MemPool& pool, const ValueProgram& program,
                  IndexReader::Segment& segment, BoundValueProgram*& runtime)
      : scope(pool), runtime(runtime) {
    runtime = program.bind(pool, segment);
  }

  ~BoundValueGuard() {
    runtime = nullptr;
  }

  BoundValueGuard(const BoundValueGuard&) = delete;
  BoundValueGuard& operator=(const BoundValueGuard&) = delete;
};

} // namespace luxir
