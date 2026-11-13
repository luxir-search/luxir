// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

// Hash functions of the value language, for consistent bucketing and sampling:
// xxhash64(x), murmur64(x), hash_combine(a, b) and hash_bucket(x, n). The
// input is a number, hashed by its 64-bit pattern: an integer or DATE value by
// its two's complement bits, a double by its IEEE 754 bits (so -0.0 and 0.0
// hash differently). Results are int64 and may be negative.

#include <bit>

#include <xxhash.h>

#include "luxir/util/luxir_util.h"
#include "luxir/value/ValueFunctionSupport.h"

namespace luxir {
namespace {

using namespace valuefn;

constexpr ValueFunction::Operand RAW{ValueFunction::DateUnit::RAW};
constexpr ValueFunction::Operand CONSTANT_RAW{ValueFunction::DateUnit::RAW, false, true};

uint64_t bitsOf(const ValueResult& value) {
  return value.type == ValueType::DOUBLE ? std::bit_cast<uint64_t>(value.doubleValue)
                                         : (uint64_t)value.intValue;
}

ResolvedValue hashResolve(std::span<const ResolvedValue> args) {
  requireNumeric(args);
  requireScalars(args);
  return {ValueType::INT64, ValueNature::NUMBER};
}

int64_t constantInt(const ValueNode& operand, std::string_view what) {
  double value = numberOf(operand);
  if (value != std::floor(value) || !(value >= -0x1p63 && value < 0x1p63)) {
    throw std::runtime_error(fmt::format("{} must be an integer constant", what));
  }
  return (int64_t)value;
}

// node.intValue is the seed; hash_bucket also keeps the bucket count in
// intValue2 and the seed in intValue.
void prepareSeed(ValueNode& node, std::span<const ValueNode* const> args) {
  node.intValue = args.size() > 1 ? constantInt(*args[1], "seed") : 0;
}

void prepareBucket(ValueNode& node, std::span<const ValueNode* const> args) {
  int64_t buckets = constantInt(*args[1], "the bucket count");
  if (buckets <= 0) throw std::runtime_error("the bucket count must be positive");
  node.intValue2 = buckets;
  node.intValue = args.size() > 2 ? constantInt(*args[2], "seed") : 0;
}

uint64_t xxhash(uint64_t bits, int64_t seed) {
  return XXH64(&bits, sizeof bits, (XXH64_hash_t)seed);
}

uint64_t murmur(uint64_t bits, int64_t seed) {
  return Hash::MurmurHash64A(&bits, sizeof bits, (uint64_t)seed);
}

template <uint64_t (*Fn)(uint64_t, int64_t)>
ValueResult seededPoint(BoundValueProgram& program, const ValueNode& node,
                        int32_t docid, float score) {
  ValueResult input = program.evalNode(node.children[0], docid, score);
  if (!input.valid) return ValueResult::missing(node.type);
  return ValueResult::integer((int64_t)Fn(bitsOf(input), node.intValue));
}

ValueBounds hashBounds(const ValueNode& node, std::span<const ValueBounds> args) {
  return ValueBounds::unbounded(node.type, args[0].mayBeMissing, args[0].alwaysMissing);
}

ValueResult bucketPoint(BoundValueProgram& program, const ValueNode& node,
                        int32_t docid, float score) {
  ValueResult input = program.evalNode(node.children[0], docid, score);
  if (!input.valid) return ValueResult::missing(node.type);
  return ValueResult::integer((int64_t)(xxhash(bitsOf(input), node.intValue)
                                        % (uint64_t)node.intValue2));
}

ValueBounds bucketBounds(const ValueNode& node, std::span<const ValueBounds> args) {
  if (args[0].alwaysMissing) return ValueBounds::unbounded(node.type, true, true);
  ValueBounds out = ValueBounds::integer(0, node.intValue2 - 1, args[0].mayBeMissing);
  out.type = node.type;
  out.minAttained = out.maxAttained = false;
  return out;
}

ResolvedValue combineResolve(std::span<const ResolvedValue> args) {
  requireIntegers(args);
  return {ValueType::INT64, ValueNature::NUMBER};
}

// Boost's hash_combine on 64 bits.
ValueResult combinePoint(BoundValueProgram& program, const ValueNode& node,
                         int32_t docid, float score) {
  ValueResult a = program.evalNode(node.children[0], docid, score);
  ValueResult b = program.evalNode(node.children[1], docid, score);
  if (!a.valid || !b.valid) return ValueResult::missing(node.type);
  uint64_t seed = (uint64_t)a.intValue;
  seed ^= (uint64_t)b.intValue + UINT64_C(0x9e3779b97f4a7c15) + (seed << 6) + (seed >> 2);
  return ValueResult::integer((int64_t)seed);
}

ValueBounds combineBounds(const ValueNode& node, std::span<const ValueBounds> args) {
  return ValueBounds::unbounded(node.type, args[0].mayBeMissing || args[1].mayBeMissing,
                                args[0].alwaysMissing || args[1].alwaysMissing);
}

constexpr ValueFunction FUNCTIONS[] = {
    {ValueOpcode::NONE, "xxhash64", hashResolve, seededPoint<xxhash>, evalFunctionBatch,
     hashBounds, evalScalarOnlyElement, 1, 2, {RAW, CONSTANT_RAW}, nullptr, prepareSeed},
    {ValueOpcode::NONE, "murmur64", hashResolve, seededPoint<murmur>, evalFunctionBatch,
     hashBounds, evalScalarOnlyElement, 1, 2, {RAW, CONSTANT_RAW}, nullptr, prepareSeed},
    {ValueOpcode::NONE, "hash_bucket", hashResolve, bucketPoint, evalFunctionBatch,
     bucketBounds, evalScalarOnlyElement, 2, 3, {RAW, CONSTANT_RAW, CONSTANT_RAW}, nullptr,
     prepareBucket},
    {ValueOpcode::NONE, "hash_combine", combineResolve, combinePoint, evalFunctionBatch,
     combineBounds, evalScalarOnlyElement, 2, 2, {RAW, RAW}},
};

static_assert(badOperandFlags(FUNCTIONS).empty(), badOperandFlags(FUNCTIONS));

} // namespace

std::span<const ValueFunction> valueHashFunctions() {
  return FUNCTIONS;
}

} // namespace luxir
