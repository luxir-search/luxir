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

#include "solux/reader/IntColReader.h"
#include "solux/schema/FieldType.h"
#include "solux/util/MemPool.h"
#include "solux/util/proto.h"
#include "solux/value/ValueFunctionRegistry.h"
#include "solux/value/ValueTypes.h"

namespace solux {

class PostingsReader;

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
  u_ptr<BoundValueProgram> bind(MemPool& pool, PostingsReader& postings) const;
};

struct BoundValueNode {
  std::optional<IntColReader> column;
  std::optional<IntColReader::SparseIterator> iterator;
  ValueBounds bounds;
  int32_t cachedDoc = -1;
  int64_t valueStart = 0;
  int64_t valueEnd = 0;
  bool cachedPresent = false;
};

class BoundValueProgram {
public:
  const ValueProgram& program;
  PostingsReader& postings;
  std::pmr::vector<BoundValueNode> nodes;

  BoundValueProgram(MemPool& pool, const ValueProgram& program, PostingsReader& postings);

  ValueResult evalPoint(int32_t docid, float score);
  ValueResult evalNode(uint32_t node, int32_t docid, float score);
  void evalBatch(std::span<const int32_t> docids, std::span<const float> scores,
                 std::span<ValueResult> results);
  ValueResult evalArrayElement(const ValueArrayRef& array, int64_t index);
  const ValueBounds& bounds(uint32_t node) const { return nodes[node].bounds; }

  ValueResult evalColumn(uint32_t node, int32_t docid, float score);
  ValueResult evalConstant(uint32_t node, int32_t docid, float score) const;
  int64_t arraySize(uint32_t node, int32_t docid, float score);
};

// A binding owns only segment-MemPool state. It also clears the caller's
// runtime pointer before destructing/rewinding, including exceptional unwind.
class BoundValueGuard {
public:
  MemPool::ScopeGuard scope;
  BoundValueProgram*& runtime;
  u_ptr<BoundValueProgram> owned;

  BoundValueGuard(MemPool& pool, const ValueProgram& program,
                  PostingsReader& postings, BoundValueProgram*& runtime)
      : scope(pool), runtime(runtime), owned(program.bind(pool, postings)) {
    runtime = owned.get();
  }

  ~BoundValueGuard() {
    runtime = nullptr;
    owned.reset();
  }

  BoundValueGuard(const BoundValueGuard&) = delete;
  BoundValueGuard& operator=(const BoundValueGuard&) = delete;
};

} // namespace solux
