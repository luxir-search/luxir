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
#include <utility>
#include <vector>

#include <google/protobuf/arena.h>

#include "luxir/reader/ColumnShape.h"
#include "luxir/reader/IntColReader.h"
#include "luxir/reader/OrdColReader.h"
#include "luxir/reader/OwnershipMapper.h"
#include "luxir/schema/FieldType.h"
#include "luxir/search/IndexReader.h"
#include "luxir/util/MemPool.h"
#include "luxir/util/proto.h"
#include "luxir/value/ValueFunctionRegistry.h"
#include "luxir/value/ValueTypes.h"

namespace luxir {

enum class ValueNodeKind : uint8_t {
  CONSTANT,
  MISSING,
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
  enum Flag : uint8_t { SHARED = 1, READS_SCORE = 2 };
  uint8_t flags = 0;
  size_t sourcePos = 0;
  std::string_view text;
  // A request-pinned descendant column; null reads the current view.
  IndexReader* source = nullptr;
  std::string_view ownerPath;
  int64_t intValue = 0;
  // A second integer parameter for functions that keep one (a box's longitudes,
  // a date's unit); prepare() sets it.
  int64_t intValue2 = 0;
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
          ? (nodes[i].source ? *nodes[i].source : reader).field(nodes[i].text) : IndexReader::FieldSegments{};
    }
  }

  // Nodes are appended after their dependencies. Walk reachable nodes once,
  // in child order, with caller-selected edges and a postorder visitor.
  template <class Children, class Visit>
  void walkNodes(uint32_t root, Children&& children, Visit&& visit) const {
    std::vector<bool> visited(nodes.size());
    auto walk = [&](auto&& self, uint32_t index) -> void {
      if (visited[index]) return;
      visited[index] = true;
      uint32_t mask = children(index);
      const ValueNode& node = nodes[index];
      for (uint8_t i = 0; i < node.childCount; i++) {
        if (mask & (1u << i)) self(self, node.children[i]);
      }
      visit(index);
    };
    walk(walk, root);
  }

  template <class Visit>
  void walkNodes(uint32_t root, Visit&& visit) const {
    walkNodes(root, [&](uint32_t index) {
      return (1u << nodes[index].childCount) - 1;
    }, std::forward<Visit>(visit));
  }

  // Reduce the program to the nodes the root needs, one per distinct value
  // and numbered in postorder from the root. A repeated subexpression becomes
  // one shared node, exactly like a let value, and structurallyEqual() is a
  // comparison of node vectors. Every program must pass through here.
  void retainRoot();

  const ValueNode& root() const { return nodes[rootNode]; }
  ValueBounds constantBounds(uint32_t index) const;
  bool structurallyEqual(const ValueProgram& other) const;
  std::string describeDateSources(uint32_t node) const;
  BoundValueProgram* bind(MemPool& pool, IndexReader::Segment& segment,
                          ValueFieldInfos fields) const;
};

// A STRING or ID column node. count() reads only its shape; a consumer that
// needs the stored ords (cardinality) attaches them.
struct BoundStringColumn {
  const DecodedFieldRecord& fieldInfo;
  ColumnShape shape;
  ColumnShape::Cursor cursor;
  OrdColReader* ords = nullptr;
  OrdColReader::BulkOrds* ordValues = nullptr;

  BoundStringColumn(PostingsReader& postings, const DecodedFieldRecord& fieldInfo)
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

struct ValueMemo {
  ValueResult result;
  int32_t docid = -1;
  uint32_t scoreBits = 0;
};

struct BoundColumnRange {
  OwnershipMapper::Cursor owners;
  ColumnShape::Cursor values;

  BoundColumnRange(MemPool& pool, const OwnershipMapper& mapper, const ColumnShape& shape)
      : owners(mapper, pool), values(shape) {}

  std::pair<int64_t, int64_t> range(int32_t owner) {
    auto family = owners.seek(owner);
    return values.range(family.first, family.end);
  }
};

struct BoundValueNode {
  // Allocated only for nodes with repeated consumers (including scalar
  // operands revisited by lazy array elements).
  ValueMemo* memo = nullptr;
  std::optional<IntColReader> column;
  std::optional<IntColReader::SparseIterator> iterator;
  std::optional<IntColReader::BulkIterator> bulkIterator;
  BoundStringColumn* stringColumn = nullptr;
  BoundColumnRange* mapping = nullptr;
  ValueBounds bounds;
  ValueBounds windowBounds;
  screaming::BitSet::Iterator* windowDocs = nullptr;
  screaming::BitSet::Selector* windowSelector = nullptr;
  int32_t cachedDoc = -1;
  int64_t valueStart = 0;
  int64_t valueEnd = 0;
  int32_t bulkLastDoc = -1;
  bool cachedPresent = false;
  bool hasWindowBounds = false;
};

class BoundValueProgram {
  MemPool* pool = nullptr;
  ValueBounds cachedScoreBounds;
  bool boundsCached = false;
  // Numeric single-valued column nodes, whose per-leaf stats can narrow bounds.
  std::span<uint32_t> windowColumns;
  std::span<ValueBounds> windowScratch;

public:
  const ValueProgram& program;
  IndexReader::Segment* segment;
  std::span<BoundValueNode> nodes;

  BoundValueProgram(MemPool& pool, const ValueProgram& program,
                    IndexReader::Segment& segment, ValueFieldInfos fields);

  // Used by parse-time constant analysis. Independence is established before
  // evaluation; document-dependent nodes are never evaluated through this ctor.
  explicit BoundValueProgram(const ValueProgram& program) : program(program), segment(nullptr) {}

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
  // Per-window column bounds, for callers that skip by score. Narrows every
  // eligible column to the leaf zone holding `doc` (see NumColumnFormat) and
  // returns the last doc that window covers; false from hasColumnWindows()
  // means there is nothing to narrow.
  bool hasColumnWindows() const { return !windowColumns.empty(); }
  int32_t columnWindowUpTo(int32_t doc);
  // Bounds of the program over the window columnWindowUpTo last computed.
  // Never throws: a proof of invalidity inside a window only makes it unbounded.
  const ValueBounds& boundsForColumnWindow(const ValueBounds& scoreBounds);
  std::string_view firstUnboundedNode() const;

  ValueResult evalColumn(uint32_t node, int32_t docid, float score);
  ValueResult evalConstant(uint32_t node, int32_t docid, float score) const;
  int64_t arraySize(uint32_t node, int32_t docid, float score);

private:
  ValueResult evalShared(uint32_t index, const ValueNode& node, int32_t docid, float score);
  ValueResult evalColumnSequential(uint32_t node, int32_t docid,
                                   float score);
  bool readColumnSequential(uint32_t node, int32_t docid, int64_t& raw);
  void evalColumnBatch(uint32_t node, std::span<const int32_t> docids,
                       std::span<ValueResult> results);
  void propagateBounds(const ValueBounds& scoreBounds);
  int32_t narrowColumnToWindow(uint32_t node, int32_t doc);
};

} // namespace luxir
