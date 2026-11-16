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

#include "luxir/reader/ColumnShape.h"
#include "luxir/reader/IntColReader.h"
#include "luxir/reader/OrdColReader.h"
#include "luxir/schema/FieldType.h"
#include "luxir/search/IndexReader.h"
#include "luxir/util/MemPool.h"
#include "luxir/util/geo.h"
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
  std::array<uint32_t, VALUE_MAX_ARITY> children{};
  uint32_t arrayOffset = 0;
  uint32_t arraySize = 0;
  uint8_t childCount = 0;
  FieldType::Type columnType = FieldType::NONE;
  bool columnMultiValued = false;
  bool columnIndexed = false;
  size_t sourcePos = 0;
  std::string_view text;
  int64_t intValue = 0;
  double doubleValue = 0.0;
};

using ValueFieldInfos = std::span<const IndexReader::FieldSegments>;

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

  ValueFieldInfos resolveFields(MemPool& pool, IndexReader& reader) const {
    auto fields = pool.make_span<IndexReader::FieldSegments>(nodes.size());
    resolveFields(reader, fields);
    return fields;
  }

  void resolveFields(IndexReader& reader, std::span<IndexReader::FieldSegments> fields) const {
    for (size_t i = 0; i < nodes.size(); i++) {
      fields[i] = nodes[i].kind == ValueNodeKind::COLUMN
          ? reader.field(nodes[i].text) : IndexReader::FieldSegments{};
    }
  }

  const ValueNode& root() const { return nodes[rootNode]; }
  ValueBounds constantBounds(uint32_t index) const;
  bool structurallyEqual(const ValueProgram& other) const;
  std::string describeDateSources(uint32_t node) const;
  uint32_t numericNode(uint32_t node);
  BoundValueProgram* bind(MemPool& pool, IndexReader::Segment& segment,
                          ValueFieldInfos fields) const;
};

// A STRING or ID column node. count() reads only its shape; a consumer that
// needs the stored ords (cardinality) attaches them.
struct BoundStringColumn {
  const SegFieldInfo& fieldInfo;
  ColumnShape shape;
  ColumnShape::Cursor cursor;
  OrdColReader* ords = nullptr;
  OrdColReader::BulkOrds* ordValues = nullptr;

  BoundStringColumn(PostingsReader& postings, const SegFieldInfo& fieldInfo)
      : fieldInfo(fieldInfo), shape(postings, fieldInfo), cursor(shape) {}

  // Only indexed STRING and ID columns store ords.
  void attachOrds(MemPool& pool, PostingsReader& postings) {
    ords = pool.make<OrdColReader>(postings, fieldInfo);
    ordValues = pool.make<OrdColReader::BulkOrds>(*ords);
  }

  // The stored ord (segment ord + 1) at value rank `rank` of `docid`, after
  // cursor.seek(docid) found it.
  int32_t storedOrd(int32_t docid, int64_t rank) {
    assert(ords != nullptr && rank >= cursor.begin() && rank < cursor.end());
    int64_t index = !ords->multiValued() && ords->docIdIndexed() ? docid : rank;
    return ordValues->valueAt(index);
  }
};

struct BoundValueNode {
  std::optional<IntColReader> column;
  std::optional<IntColReader::SparseIterator> iterator;
  std::optional<IntColReader::BulkIterator> bulkIterator;
  BoundStringColumn* stringColumn = nullptr;
  ValueBounds bounds;
  int32_t cachedDoc = -1;
  int64_t valueStart = 0;
  int64_t valueEnd = 0;
  int32_t bulkLastDoc = -1;
  bool cachedPresent = false;
};

// One step of a program's fused form (BoundValueProgram::prepareFused). Each
// step writes the register of its program node.
struct FusedValueStep {
  enum class Kind : uint8_t {
    COLUMN, DEF_COLUMN, SCORE, UNARY, BINARY, GEO_DISTANCE, GEO_DECAY, DECAY,
  };
  Kind kind = Kind::SCORE;
  ValueOpcode opcode = ValueOpcode::NONE;
  bool integerInput = false;  // DECAY: the input column holds integers
  uint32_t node = 0;
  uint32_t a = 0;  // operand register, or the column node
  uint32_t b = 0;
  geo::HaversinOrigin origin;
  ValueResult decayOrigin;
  double scale = 1.0;
  double offset = 0.0;
};

class BoundValueProgram {
  ValueBounds cachedScoreBounds;
  bool boundsCached = false;

public:
  const ValueProgram& program;
  IndexReader::Segment& segment;
  std::span<BoundValueNode> nodes;
  // The fused form (prepareFused); empty steps mean there is none.
  std::span<FusedValueStep> fusedSteps;
  std::span<double> fusedRegisters;
  std::span<int64_t> fusedRaw;
  bool fusedSequential = false;

  BoundValueProgram(MemPool& pool, const ValueProgram& program,
                    IndexReader::Segment& segment, ValueFieldInfos fields);

  ValueResult evalPoint(int32_t docid, float score);
  // Aggregate/facet scans call this with monotonically increasing document
  // IDs, allowing a bare column to keep its bulk decoder hot.
  ValueResult evalSequentialPoint(int32_t docid, float score);
  bool evalSequentialInt64(int32_t docid, int64_t& value);
  bool evalSequentialDouble(int32_t docid, double& value);
  ValueResult evalNode(uint32_t node, int32_t docid, float score);
  // Test seam: make prepareFused refuse every program.
  static inline bool disableFusedForTests = false;
  // Flattens a scalar numeric program into steps over double registers, with
  // the per-document dispatch and constant lookups gone. False when the
  // program has a node the steps do not cover. `sequential` selects the bulk
  // column decoder, for callers visiting a large share of the documents.
  bool prepareFused(MemPool& pool, bool sequential);
  bool fusedReady() const { return !fusedSteps.empty(); }
  // The program's value for a document, in ascending document order, computed
  // with the same arithmetic as evalPoint. False for anything but a plain
  // finite result (a missing value, a failed operation): evalPoint then
  // decides what the document gets.
  bool evalFused(int32_t docid, float score, double& out);
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
  bool compileFused(uint32_t node, std::span<uint8_t> seen, uint32_t& count);
  bool positionFused(uint32_t node, int32_t docid, int64_t& start, int64_t& end);
  int64_t fusedValueAt(uint32_t node, int64_t rank);
  ValueResult evalColumnSequential(uint32_t node, int32_t docid,
                                   float score);
  bool readColumnSequential(uint32_t node, int32_t docid, int64_t& raw);
  void evalColumnBatch(uint32_t node, std::span<const int32_t> docids,
                       std::span<ValueResult> results);
  void propagateBounds(const ValueBounds& scoreBounds);
};

} // namespace luxir
