// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include <gtest/gtest.h>

#include <array>
#include <latch>
#include <limits>
#include <map>
#include <random>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "luxir/search/ops/ExprStatsOp.h"
#include "luxir/search/SearchOverrides.h"
#include "luxir/value/AggregateExprParser.h"
#include "luxir/util/HyperLogLog.h"
#include "luxir/util/TDigest.h"
#include "luxir/server/JsonRequest.h"
#include "test/CollectionHelper.h"
#include "test/LocalReq.h"
#include "test/LuxirTest.h"
#include "test/QueryBuild.h"
#include "test/SchemaBuilder.h"
#include "test/TestUtils.h"

using namespace luxir;
using namespace luxir::test;

namespace {

class ArenaOwner {
public:
  google::protobuf::Arena* arena = createArena();
  ~ArenaOwner() { releaseArena(arena); }
};

auto facetAggregateOverrides() {
  return SearchOverridesGuard(
      forcedFacetFeedStrategy, forcedFacetSubOpInline,
      forcedRequestMemoryMaxBytes,
      facetAggregateStateReservationCounterForTests,
      aggregateExecutionStatsForTests,
      inlineAggregateStatsForTests,
      inlineFacetEntryStatsForTests,
      disableDenseFacetStateForTests, disableAggregateSharingForTests,
      forcedInlineFacetEntryMode,
      forcedRangeFacetBucketDomainByteBudget,
      forcedRangeFacetBindingStateChunkBytes,
      facetBucketBlockCounter);
}

AggregateProgram* parseAggregate(ArenaOwner& memory, Schema& schema,
                                 std::string_view expression,
                                 std::string_view opName = "metric") {
  AggregateExprOptions options{&schema, {}, opName};
  options.shareProducers = !disableAggregateSharingForTests;
  return AggregateExprParser(options, *memory.arena).parse(expression);
}

void expectParseError(ArenaOwner& memory, Schema& schema,
                      std::string_view expression, std::string_view expected,
                      std::string_view opName = "metric") {
  try {
    parseAggregate(memory, schema, expression, opName);
    FAIL() << "expected parse failure for " << expression;
  } catch (const std::runtime_error& error) {
    EXPECT_NE(std::string(error.what()).find(expected), std::string::npos)
        << error.what();
  }
}

BucketScalar evaluate(AggregateProgram& program, IndexReader& reader, IndexReader::Segment& segment) {
  MemPool pool;
  AggregateAccumulator accumulator(program);
  std::vector<BoundAggregateInput> bindings =
      bindAggregateInputs(program, pool, segment, resolveAggregateFields(program, pool, reader));
  int32_t maxDoc = segment.postingsReader().maxDoc();
  for (int32_t doc = 0; doc < maxDoc; doc++) {
    for (size_t producer = 0; producer < bindings.size(); producer++) {
      accumulator.add((uint32_t)producer, bindings[producer].evalPoint(doc));
    }
  }
  return accumulator.finish();
}

std::vector<double> indexPercentileData(CollectionHelper& helper, int segments) {
  helper.getIndexWriter()->mergePolicy->setMergeFactor(100);
  std::mt19937_64 random(29);
  std::lognormal_distribution<double> distribution(3.0, 1.4);
  std::vector<double> values;
  for (int segment = 0; segment < segments; segment++) {
    std::vector<Doc> docs;
    for (int i = 0; i < 1200; i++) {
      double x = distribution(random);
      values.push_back(x);
      docs.push_back(flatdoc("id", std::to_string(values.size()), "x_d", x,
                            "cat_s", i % 2 == 0 ? "a" : "b"));
    }
    helper.indexAll(docs, UpdateMessage::COMMIT);
  }
  std::sort(values.begin(), values.end());
  return values;
}

void indexSharedSketchData(CollectionHelper& helper) {
  helper.getIndexWriter()->mergePolicy->setMergeFactor(100);
  for (int segment = 0; segment < 3; segment++) {
    std::vector<Doc> docs;
    for (int i = 0; i < 600; i++) {
      double x = std::exp((double)((i * 7 + segment) % 101) / 10) * (i % 2 ? 100 : 1);
      docs.push_back(flatdoc("id", std::to_string(segment * 600 + i), "x_d", x,
          "y_d", x * 2, "bucket_i", i % 2, "cat_s", i % 2 ? "b" : "a"));
    }
    helper.indexAll(docs, UpdateMessage::COMMIT);
  }
}

constexpr std::string_view SHARED_SKETCH_OPS = R"json({
  "p50":"percentile(x_d,50)","p95":"percentile(x_d,95)",
  "p99":"percentile(x_d,99)","spread":"percentile(x_d,99)-percentile(x_d,50)"})json";

using SketchResults = std::map<std::string, std::vector<uint64_t>>;

void collectSketchResults(const api::Val& value, const std::string& path, SketchResults& result) {
  auto ops = [&](const auto& children, std::string_view suffix = {}) {
    for (auto& [name, child] : children) {
      collectSketchResults(*child, path + std::string(suffix) + "/" + std::string(name), result);
    }
  };
  if (const auto* number = std::get_if<double>(&value.kind)) {
    result[path] = {(uint64_t)value.kind.index(), std::bit_cast<uint64_t>(*number)};
  } else if (const auto* number = std::get_if<int64_t>(&value.kind)) {
    result[path] = {(uint64_t)value.kind.index(), std::bit_cast<uint64_t>(*number)};
  } else if (value.isNull()) {
    result[path] = {(uint64_t)value.kind.index()};
  } else if (const auto* facet = std::get_if<api::FacetResult>(&value.kind)) {
    ops(facet->ops);
  } else if (const auto* list = std::get_if<api::DocList>(&value.kind)) {
    ops(list->ops);
    if (list->groups) ops(list->groups->ops, "/groups");
  } else if (const auto* array = std::get_if<api::ArrVal>(&value.kind)) {
    result[path] = {(uint64_t)value.kind.index(), array->v.size()};
    for (size_t i = 0; i < array->v.size(); i++) {
      collectSketchResults(array->v[i], path + "/" + std::to_string(i), result);
    }
  } else ADD_FAILURE() << "unexpected metric result at " << path;
}

struct SketchRun {
  SketchResults values;
  size_t peakBytes;
};

SketchRun runSharedSketchRequest(CollectionHelper& helper, std::string_view json,
                                 AggregateExecutionStats& stats, int parallel = 1) {
  SearchOverridesGuard guard(aggregateExecutionStatsForTests);
  aggregateExecutionStatsForTests = &stats;
  auto req = localReq(helper.getSearchEngine());
  parseQueryRequest(json, req->rawRequest(), req->mr);
  req->collection("main");
  req->execute(parallel);
  EXPECT_TRUE(req->ok()) << req->errorMsg();
  SketchResults result;
  if (req->ok()) for (auto& [name, value] : req->responses.back()->proto.ops) {
    collectSketchResults(*value, "/" + std::string(name), result);
  }
  EXPECT_EQ(0u, stats.retainedStateBytes.load());
  return {std::move(result), req->memoryTracker.peak()};
}

} // namespace

class AggregateExprTest : public LuxirTest {};

TEST_F(AggregateExprTest, sharedGrammarLevelsOffsetsAndScoreRejection) {
  auto schema = Schema::createDefaultSchema();
  ArenaOwner memory;

  AggregateProgram* expression =
      parseAggregate(memory, *schema, "sum(price_i * 2) + max(foo_i)");
  ASSERT_NE(nullptr, expression->root().function);
  EXPECT_EQ(ValueOpcode::ADD, expression->root().function->opcode);
  ASSERT_EQ(2u, expression->leaves.size());
  EXPECT_EQ(ValueOpcode::MUL, expression->producers[0].input->root().opcode);

  AggregateProgram* functionForms = parseAggregate(
      memory, *schema, "add(sum(price_i), div(max(foo_i), 2))");
  ASSERT_NE(nullptr, functionForms->root().function);
  EXPECT_EQ(ValueOpcode::ADD, functionForms->root().function->opcode);
  EXPECT_EQ(ValueOpcode::DIV,
            functionForms->nodes[functionForms->root().children[1]]
                .function->opcode);
  EXPECT_EQ(ValueOpcode::ADD,
            parseAggregate(memory, *schema, "add(1, sum(price_i))")
                ->root().function->opcode);
  EXPECT_EQ(ValueOpcode::ABS,
            parseAggregate(memory, *schema, "abs(sum(price_i))")
                ->root().function->opcode);
  EXPECT_EQ(ValueOpcode::FLOOR,
            parseAggregate(memory, *schema,
                           "floor(sum(price_i) / sum(foo_i))")
                ->root().function->opcode);

  AggregateProgram* nested =
      parseAggregate(memory, *schema, "avg(avg(prices_is))");
  ASSERT_EQ(1u, nested->leaves.size());
  EXPECT_EQ(ValueOpcode::AVG, nested->producers[0].input->root().opcode);

  expectParseError(memory, *schema, "avg(prices_is)",
                   "requires one scalar per document");
  expectParseError(memory, *schema, "avg(prices_is)",
                   ValueFunctionRegistry::arrayReducerNames());
  expectParseError(memory, *schema, "price_i", "must be an aggregate call");
  expectParseError(memory, *schema, "1",
                   "expression contains no aggregate function");
  api::Val constantVar;
  constantVar.kind = int64_t{2};
  using ConstantPair = std::pair<
      std::string_view, ::hpp_proto::indirect_view<api::Val>>;
  std::array<ConstantPair, 1> constantPairs{{{"scale", {&constantVar}}}};
  AggregateExprOptions constantOptions{
      schema.get(),
      api::map_view<std::string_view,
                    ::hpp_proto::indirect_view<api::Val>>{constantPairs},
      "constant"};
  try {
    AggregateExprParser(constantOptions, *memory.arena).parse("$scale");
    FAIL() << "expected aggregate-free variable expression to fail";
  } catch (const std::runtime_error& error) {
    EXPECT_NE(std::string(error.what()).find(
                  "expression contains no aggregate function"),
              std::string::npos) << error.what();
  }
  expectParseError(memory, *schema, "sum()", "expects exactly 1 argument");
  expectParseError(memory, *schema, "sum(price_i 2)",
                   "byte 12: expected ')'");
  expectParseError(memory, *schema, "sum(price_i",
                   "byte 11: expected ')'");
  expectParseError(memory, *schema, "sum(price_i, 2)",
                   "expects exactly 1 argument");
  expectParseError(memory, *schema, "sum(price_i + * 2)", "byte 14");
  AggregateProgram* bucketMinimum =
      parseAggregate(memory, *schema, "min(sum(price_i), 2)");
  ASSERT_NE(nullptr, bucketMinimum->root().function);
  EXPECT_EQ(ValueOpcode::MIN, bucketMinimum->root().function->opcode);
  expectParseError(memory, *schema, "def(sum(price_i), 0)",
                   "def() is a per-document value function");
  expectParseError(memory, *schema, "avg(score)",
                   "op 'scored': scores are not available",
                   "scored");

  api::Val scale;
  scale.kind = int64_t{2};
  api::Val bias;
  bias.kind = double{0.5};
  using Pair = std::pair<std::string_view,
                         ::hpp_proto::indirect_view<api::Val>>;
  std::array<Pair, 2> pairs{{{"scale", {&scale}}, {"bias", {&bias}}}};
  AggregateExprOptions options{
      schema.get(),
      api::map_view<std::string_view,
                    ::hpp_proto::indirect_view<api::Val>>{pairs},
      "vars"};
  AggregateProgram* variables = AggregateExprParser(options, *memory.arena).parse(
      "sum(price_i * $scale) + $bias");
  EXPECT_EQ(BucketValueType::DOUBLE, variables->root().type);
  EXPECT_EQ(ValueNodeKind::VARIABLE,
            variables->producers[0].input->nodes[
                variables->producers[0].input->root().children[1]].kind);
}

TEST_F(AggregateExprTest, bitDomainWalkIncludesLastDocument) {
  constexpr int32_t MAX_DOC = 65;
  RAMBitDocSet domain(MAX_DOC);
  for (int32_t doc : {0, 31, MAX_DOC - 1}) domain.mutableBits().set(doc);

  std::vector<int32_t> docs;
  forEachDomainDoc(&domain, MAX_DOC,
                   [&](int32_t doc) { docs.push_back(doc); });

  EXPECT_EQ((std::vector<int32_t>{0, 31, MAX_DOC - 1}), docs);
}

TEST_F(AggregateExprTest, dateRulesAreResolvedAcrossBothLevels) {
  auto schema = Schema::createDefaultSchema();
  ArenaOwner memory;

  EXPECT_EQ(ValueNature::DATE_MS,
            parseAggregate(memory, *schema, "avg(when_dt)")->root().nature);
  EXPECT_EQ(ValueNature::DATE_MS,
            parseAggregate(memory, *schema, "min(when_dt)")->root().nature);
  EXPECT_EQ(ValueNature::DATE_MS,
            parseAggregate(memory, *schema, "max(when_dt)")->root().nature);
  EXPECT_EQ(ValueNature::NUMBER,
            parseAggregate(memory, *schema,
                           "max(when_dt) - min(when_dt)")->root().nature);
  EXPECT_EQ(ValueNature::NUMBER,
            parseAggregate(memory, *schema, "sum(when_dt * 2)")
                ->root().nature);
  EXPECT_EQ(ValueNature::NUMBER,
            parseAggregate(memory, *schema,
                           "div(avg(when_dt), 1000)")->root().nature);

  expectParseError(memory, *schema, "sum(when_dt)",
                   "cannot aggregate a DATE expression");
  expectParseError(memory, *schema, "max(when_dt) + min(when_dt)",
                   "cannot add two DATE");
}

TEST_F(AggregateExprTest, exactSumsMissingDefAndPerDocReducers) {
  CollectionHelper helper;
  constexpr int64_t TWO_TO_53 = int64_t{1} << 53;
  helper.index(flatdoc("id_s", "a", "x_i", TWO_TO_53,
                       "den_i", 2, "zero_i", 2,
                       "values_is", vec_i(1, 3)),
               UpdateMessage::NO_COMMIT);
  helper.index(flatdoc("id_s", "b", "x_i", 1, "den_i", 3,
                       "zero_i", 0),
               UpdateMessage::NO_COMMIT);
  helper.index(flatdoc("id_s", "c", "x_i", -TWO_TO_53,
                       "den_i", 5, "zero_i", 4,
                       "values_is", vec_i(5)),
               UpdateMessage::NO_COMMIT);
  helper.index(flatdoc("id_s", "d", "den_i", 10),
               UpdateMessage::COMMIT);

  auto reader = helper.getIndexWriter()->snapshots.readers.getReader();
  ASSERT_EQ(1u, reader->segments().size());
  auto schema = helper.collection().getSchema();
  ArenaOwner memory;
  auto eval = [&](std::string_view expression) {
    return evaluate(*parseAggregate(memory, *schema, expression),
                    *reader, reader->segments()[0]);
  };

  BucketScalar exact = eval("sum(x_i)");
  ASSERT_TRUE(exact.valid);
  EXPECT_EQ(BucketValueType::INT128, exact.type);
  EXPECT_EQ((__int128)1, exact.intValue);
  api::Val exactOutput;
  writeAggregateValue(exactOutput, exact);
  EXPECT_EQ(1, exactOutput.asInt());

  AggregateProgram* sumProgram =
      parseAggregate(memory, *schema, "sum(x_i)", "merge");
  AggregateAccumulator left(*sumProgram);
  AggregateAccumulator right(*sumProgram);
  left.add(0, ValueResult::integer(TWO_TO_53));
  right.add(0, ValueResult::integer(1));
  right.add(0, ValueResult::integer(-TWO_TO_53));
  AggregateAccumulator::merge(&left, &right);
  EXPECT_EQ((__int128)1, left.finish().intValue);

  BucketScalar average = eval("avg(x_i)");
  ASSERT_TRUE(average.valid);
  EXPECT_DOUBLE_EQ(1.0 / 3.0, average.doubleValue);

  BucketScalar withDefault = eval("avg(def(x_i,0))");
  ASSERT_TRUE(withDefault.valid);
  EXPECT_DOUBLE_EQ(0.25, withDefault.doubleValue);

  BucketScalar ratio = eval("sum(x_i) / sum(den_i)");
  ASSERT_TRUE(ratio.valid);
  EXPECT_EQ(BucketValueType::DOUBLE, ratio.type);
  EXPECT_DOUBLE_EQ(0.05, ratio.doubleValue);
  EXPECT_DOUBLE_EQ(ratio.doubleValue,
                   eval("div(sum(x_i), sum(den_i))").doubleValue);

  BucketScalar perDocDivision = eval("sum(1 / zero_i)");
  ASSERT_TRUE(perDocDivision.valid);
  EXPECT_DOUBLE_EQ(0.75, perDocDivision.doubleValue);
  EXPECT_DOUBLE_EQ(20.75, eval("sum(def(1 / zero_i, 10))").doubleValue);

  BucketScalar bucketDivision = eval("sum(den_i) / 0");
  EXPECT_FALSE(bucketDivision.valid);
  api::Val bucketDivisionOutput;
  writeAggregateValue(bucketDivisionOutput, bucketDivision);
  EXPECT_TRUE(bucketDivisionOutput.isNull());

  BucketScalar reduced = eval("avg(avg(values_is))");
  ASSERT_TRUE(reduced.valid);
  EXPECT_DOUBLE_EQ(3.5, reduced.doubleValue);

  BucketScalar pooledSum = eval("sum(sum(values_is))");
  ASSERT_TRUE(pooledSum.valid);
  EXPECT_EQ(BucketValueType::INT128, pooledSum.type);
  EXPECT_EQ((__int128)9, pooledSum.intValue);
  BucketScalar pooledAverage =
      eval("sum(sum(values_is)) / sum(count(values_is))");
  ASSERT_TRUE(pooledAverage.valid);
  EXPECT_DOUBLE_EQ(3.0, pooledAverage.doubleValue);
  EXPECT_NE(reduced.doubleValue, pooledAverage.doubleValue);

  BucketScalar missing = eval("sum(absent_i) + 1");
  EXPECT_FALSE(missing.valid);
  api::Val output;
  writeAggregateValue(output, missing);
  EXPECT_TRUE(output.isNull());
}

TEST_F(AggregateExprTest, outputOverflowBecomesContainedFailure) {
  CollectionHelper helper;
  helper.index(flatdoc("id_s", "a", "x_i",
                       std::numeric_limits<int64_t>::max()),
               UpdateMessage::NO_COMMIT);
  helper.index(flatdoc("id_s", "b", "x_i",
                       std::numeric_limits<int64_t>::max()),
               UpdateMessage::COMMIT);

  auto reader = helper.getIndexWriter()->snapshots.readers.getReader();
  auto schema = helper.collection().getSchema();
  ArenaOwner memory;
  AggregateProgram* program = parseAggregate(memory, *schema, "sum(x_i)", "wide");
  BucketScalar value = evaluate(*program, *reader, reader->segments()[0]);
  EXPECT_FALSE(value.valid);
  EXPECT_EQ(AggregateFailure::INT64_OUTPUT_OVERFLOW, value.failure);

  api::Val output;
  EXPECT_NO_THROW(writeAggregateValue(output, value));
  EXPECT_TRUE(output.isNull());

}

TEST_F(AggregateExprTest, registryEntriesHaveResolvedStateLifecycles) {
  auto schema = Schema::createDefaultSchema();
  ArenaOwner memory;
  for (const AggregateFunction& function : AggregateFunctionRegistry::entries()) {
    EXPECT_NE(nullptr, function.resolve) << function.name;
    EXPECT_LE(1, function.minArguments) << function.name;
    EXPECT_GE(function.maxArguments, function.minArguments) << function.name;
    AggregateProgram* program = parseAggregate(
        memory, *schema, std::string(function.name)
            + (function.minArguments == 2 ? "(price_i, 5)" : "(price_i)"));
    ASSERT_EQ(1u, program->leaves.size());
    const AggregateStateOps& state = program->producers[0].resolved.state;
    EXPECT_GT(state.bytes, 0u) << function.name;
    EXPECT_NE(nullptr, state.init) << function.name;
    EXPECT_NE(nullptr, state.accumulate) << function.name;
    EXPECT_NE(nullptr, state.accumulateBatch) << function.name;
    EXPECT_NE(nullptr, state.accumulateRawPoint) << function.name;
    EXPECT_NE(nullptr, state.accumulateRawBatch) << function.name;
    EXPECT_NE(nullptr, state.merge) << function.name;
    EXPECT_NE(nullptr, state.finish) << function.name;
  }
  for (const ValueFunction& function : ValueFunctionRegistry::entries()) {
    EXPECT_NE(nullptr, function.evalPoint) << function.name;
  }
  for (std::string_view name : {
           "add", "sub", "mul", "div", "neg", "abs", "sqrt", "log",
           "log1p", "floor", "min", "max"}) {
    const ValueFunction* function = ValueFunctionRegistry::find(name);
    ASSERT_NE(nullptr, function) << name;
    EXPECT_NE(nullptr, function->evalBucketScalar) << name;
  }
  for (std::string_view name : {
           "def", "avg", "sum", "count"}) {
    const ValueFunction* function = ValueFunctionRegistry::find(name);
    ASSERT_NE(nullptr, function) << name;
    EXPECT_EQ(nullptr, function->evalBucketScalar) << name;
  }
}

TEST_F(AggregateExprTest, cardinalityFlattensValuesAndMapsGlobalOrds) {
  CollectionHelper helper;
  helper.getIndexWriter()->mergePolicy->setMergeFactor(100);
  helper.indexAll({
      flatdoc("id", "a", "s_ss", vecs("a", "c"), "n_is", vec_i(1, 2), "when_dt", int64_t{10},
              "s_s", "a", "n_i", 1, "n_d", 1.5),
      flatdoc("id", "b", "s_ss", vecs("c"), "n_is", vec_i(2, 3),
              "s_s", "b", "n_i", 2)}, UpdateMessage::COMMIT);
  helper.indexAll({
      flatdoc("id", "c", "s_ss", vecs("b", "c"), "n_is", vec_i(1, 4), "when_dt", int64_t{20},
              "s_s", "a", "n_i", 1, "n_d", 2.5),
      flatdoc("id", "d")}, UpdateMessage::COMMIT);
  helper.indexAll({flatdoc("id", "e", "s_ss", vecs("a"), "n_is", vec_i(2),
                           "s_s", "c", "n_i", 3, "n_d", 1.5)}, UpdateMessage::COMMIT);
  auto req = localReq(helper.getSearchEngine());
  auto& root = req->collection("main");
  root.expr("strings", "cardinality(s_ss)");
  root.expr("ids", "cardinality(id)");
  root.expr("numbers", "cardinality(n_is)");
  root.expr("computed", "cardinality(n_is * 0)");
  root.expr("dates", "cardinality(when_dt)");
  root.expr("rank", "percentile_rank(when_dt, 10)");
  root.expr("threshold", "cardinality(n_is, $t)", "t", int64_t{40000});
  root.expr("single_string", "cardinality(s_s)");
  root.expr("single_int", "cardinality(n_i)");
  root.expr("double_values", "cardinality(n_d,10)");
  auto& selected = root.topDocs("selected").exprQuery("n_i:1").limit(0);
  selected.expr("strings", "cardinality(s_s)");
  selected.expr("numbers", "cardinality(n_is)");
  req->execute(-1);
  ASSERT_TRUE(req->ok()) << req->errorMsg();
  EXPECT_EQ(3, req->scalar<int64_t>("strings"));
  EXPECT_EQ(5, req->scalar<int64_t>("ids"));
  EXPECT_EQ(4, req->scalar<int64_t>("numbers"));
  EXPECT_EQ(1, req->scalar<int64_t>("computed"));
  EXPECT_EQ(2, req->scalar<int64_t>("dates"));
  EXPECT_DOUBLE_EQ(50, req->scalar<double>("rank"));
  EXPECT_EQ(4, req->scalar<int64_t>("threshold"));
  EXPECT_EQ(3, req->scalar<int64_t>("single_string"));
  EXPECT_EQ(3, req->scalar<int64_t>("single_int"));
  EXPECT_EQ(2, req->scalar<int64_t>("double_values"));
  EXPECT_EQ(1, req->docList("selected")->ops.at("strings")->asInt());
  EXPECT_EQ(3, req->docList("selected")->ops.at("numbers")->asInt());
  EXPECT_EQ(0u, req->memoryTracker.bytes());
  EXPECT_GT(req->memoryTracker.peak(), 0u);
}

TEST_F(AggregateExprTest, cardinalityEstimatesMatchExactRandomSets) {
  CollectionHelper helper;
  helper.getIndexWriter()->mergePolicy->setMergeFactor(100);
  std::mt19937_64 random(5);
  std::set<int64_t> users, values;
  for (int segment = 0; segment < 4; segment++) {
    std::vector<Doc> docs;
    for (int i = 0; i < 6000; i++) {
      int64_t user = (int64_t)(random() % 20000);
      int64_t value = (int64_t)(random() % 50000);
      users.insert(user);
      values.insert(value);
      docs.push_back(flatdoc("id", std::to_string(segment * 6000 + i),
          "user_s", "u" + std::to_string(user), "value_i", value));
    }
    helper.indexAll(docs, UpdateMessage::COMMIT);
  }
  auto req = localReq(helper.getSearchEngine());
  auto& root = req->collection("main");
  root.expr("users", "cardinality(user_s)");
  root.expr("numbers", "cardinality(value_i)");
  root.expr("users_exact", "cardinality(user_s,40000)");
  root.expr("numbers_exact", "cardinality(value_i,40000)");
  req->execute(-1);
  ASSERT_TRUE(req->ok()) << req->errorMsg();
  EXPECT_NEAR((double)users.size(), (double)req->scalar<int64_t>("users"), users.size() * 0.065);
  EXPECT_NEAR((double)values.size(), (double)req->scalar<int64_t>("numbers"), values.size() * 0.065);
  EXPECT_EQ((int64_t)users.size(), req->scalar<int64_t>("users_exact"));
  EXPECT_EQ((int64_t)values.size(), req->scalar<int64_t>("numbers_exact"));
  EXPECT_EQ(0u, req->memoryTracker.bytes());
}

TEST_F(AggregateExprTest, sketchRowsSkipNonFiniteValuesAndCanonicalizeZero) {
  CollectionHelper helper;
  double nan = std::numeric_limits<double>::quiet_NaN();
  double inf = std::numeric_limits<double>::infinity();
  for (double x : {-0.0, 0.0, 7.0, nan, inf, -inf}) {
    helper.index(flatdoc("id", std::to_string(std::bit_cast<uint64_t>(x)),
                         "x_d", x, "x_f", (float)x), UpdateMessage::NO_COMMIT);
  }
  helper.commit();
  auto req = localReq(helper.getSearchEngine());
  auto& root = req->collection("main");
  root.expr("d", "cardinality(x_d)");
  root.expr("f", "cardinality(x_f)");
  root.expr("r", "percentile_rank(x_d, 0)");
  root.expr("rf", "percentile_rank(x_f, 0)");
  root.expr("expr", "cardinality(def(other_d, x_d))");
  root.expr("pd", "percentile(x_d, 50)");
  root.expr("pf", "percentile(x_f, 50)");
  req->execute(false);
  ASSERT_TRUE(req->ok()) << req->errorMsg();
  EXPECT_EQ(2, req->scalar<int64_t>("d"));
  EXPECT_EQ(2, req->scalar<int64_t>("f"));
  EXPECT_EQ(2, req->scalar<int64_t>("expr"));
  EXPECT_DOUBLE_EQ(100.0 * (2.0 / 3.0), req->scalar<double>("r"));
  EXPECT_DOUBLE_EQ(100.0 * (2.0 / 3.0), req->scalar<double>("rf"));
  EXPECT_DOUBLE_EQ(0, req->scalar<double>("pd"));
  EXPECT_DOUBLE_EQ(0, req->scalar<double>("pf"));
  EXPECT_TRUE(req->respWarnings().empty());
}

TEST_F(AggregateExprTest, exactRanksKeepMixedNumericComparisonsExact) {
  auto schema = Schema::createDefaultSchema();
  ArenaOwner memory;
  struct Example { std::string_view expression; ValueResult value; double expected; };
  for (const auto& example : {
      Example{"percentile_rank(x_i, 9007199254740992.0)", ValueResult::integer(9007199254740993LL), 0},
      Example{"percentile_rank(x_i, 9007199254740993)", ValueResult::integer(9007199254740993LL), 100},
      Example{"percentile_rank(x_i, 9223372036854775808.0)", ValueResult::integer(INT64_MAX), 100},
      Example{"percentile_rank(x_i, -9223372036854775808.0)", ValueResult::integer(INT64_MIN), 100},
      Example{"percentile_rank(x_i, -1.5)", ValueResult::integer(-1), 0},
      Example{"percentile_rank(x_d, 9223372036854775807)", ValueResult::floating(0x1p63), 0},
      Example{"percentile_rank(x_d, -9223372036854775808)", ValueResult::floating(-0x1p63), 100},
      Example{"percentile_rank(x_d, 9007199254740993)", ValueResult::floating(9007199254740992.0), 100},
      Example{"percentile_rank(x_f, 0.1)", ValueResult::floating((double)0.1f), 100},
      Example{"percentile_rank(x_f + 0, 0.1)", ValueResult::floating((double)0.1f), 0}}) {
    auto* program = parseAggregate(memory, *schema, example.expression);
    AggregateAccumulator accumulator(*program);
    accumulator.add(0, example.value);
    EXPECT_DOUBLE_EQ(example.expected, accumulator.finish().doubleValue) << example.expression;
  }
  auto* program = parseAggregate(memory, *schema, "percentile_rank(x_i, 5)");
  AggregateAccumulator overflow(*program);
  overflow.add(0, ValueResult::integer(5));
  for (int i = 0; i < 64; i++) AggregateAccumulator::merge(&overflow, &overflow);
  EXPECT_EQ(AggregateFailure::COUNT_OVERFLOW, overflow.finish().failure);
  std::vector<int64_t> raw;
  std::vector<ScalarValueResult> values;
  for (int i = 0; i < 2000; i++) {
    raw.push_back(i % 10);
    values.push_back(ScalarValueResult::integer(i % 10));
  }
  AggregateAccumulator batch(*program), column(*program);
  batch.addBatch(0, values);
  column.addRawBatch(0, raw, FieldType::INT);
  AggregateAccumulator::merge(&batch, &column);
  EXPECT_DOUBLE_EQ(60, batch.finish().doubleValue);
}

TEST_F(AggregateExprTest, floatRankThresholdMatchesRangeCoercion) {
  CollectionHelper helper;
  helper.indexAll({flatdoc("id", "1", "x_f", 0.1f),
                   flatdoc("id", "2", "x_f", 0.2f)}, UpdateMessage::COMMIT);
  auto req = localReq(helper.getSearchEngine());
  auto& root = req->collection("main");
  root.topDocs("q").exprQuery("x_f:[* TO 0.1]").countLimit().limit(0);
  root.expr("bare", "percentile_rank(x_f, 0.1)");
  root.expr("variable", "percentile_rank(x_f, $v)", "v", 0.1);
  root.expr("computed", "percentile_rank(x_f + 0, 0.1)");
  req->execute(false);
  ASSERT_TRUE(req->ok()) << req->errorMsg();
  EXPECT_EQ(1, req->getMatchCount());
  EXPECT_DOUBLE_EQ(50, req->scalar<double>("bare"));
  EXPECT_DOUBLE_EQ(50, req->scalar<double>("variable"));
  EXPECT_DOUBLE_EQ(0, req->scalar<double>("computed"));
}

TEST_F(AggregateExprTest, podProgramsKeepUnchargedAccumulatorStorage) {
  CollectionHelper helper;
  helper.indexAll({flatdoc("id", "1", "x_i", 10),
                   flatdoc("id", "2", "x_i", 20)}, UpdateMessage::COMMIT);
  auto guard = facetAggregateOverrides();
  forcedRequestMemoryMaxBytes = 1;
  auto req = localReq(helper.getSearchEngine());
  auto& root = req->collection("main");
  root.expr("existing", "sum(x_i) + avg(x_i) + min(x_i) + max(x_i)");
  root.expr("rank", "percentile_rank(x_i, 10)");
  req->execute(false);
  ASSERT_TRUE(req->ok()) << req->errorMsg();
  EXPECT_DOUBLE_EQ(75, req->scalar<double>("existing"));
  EXPECT_DOUBLE_EQ(50, req->scalar<double>("rank"));
  EXPECT_EQ(0u, req->memoryTracker.peak());
}

TEST_F(AggregateExprTest, cardinalityAndRanksPreserveInt64ColumnValues) {
  CollectionHelper helper;
  helper.indexAll({flatdoc("id", "1", "x_i", 9007199254740992LL),
                   flatdoc("id", "2", "x_i", 9007199254740993LL),
                   flatdoc("id", "3", "x_i", INT64_MAX)}, UpdateMessage::COMMIT);
  auto req = localReq(helper.getSearchEngine());
  auto& root = req->collection("main");
  root.expr("c", "cardinality(x_i)");
  root.expr("r", "percentile_rank(x_i,9007199254740992.0)");
  root.expr("computed", "percentile_rank(x_i + 0,9007199254740993)");
  req->execute(false);
  ASSERT_TRUE(req->ok()) << req->errorMsg();
  EXPECT_EQ(3, req->scalar<int64_t>("c"));
  EXPECT_DOUBLE_EQ(100.0 / 3, req->scalar<double>("r"));
  EXPECT_DOUBLE_EQ(100.0 * (2.0 / 3), req->scalar<double>("computed"));
}

TEST_F(AggregateExprTest, sketchRowsValidateArgumentsCapabilitiesAndEmptyResults) {
  auto schema = Schema::createDefaultSchema();
  ArenaOwner memory;
  for (std::string_view expression : {"cardinality(x_i, -1)", "cardinality(x_i, 40001)",
                                     "cardinality(x_i, 1.5)"}) {
    expectParseError(memory, *schema, expression, "integer in [0, 40000]");
  }
  expectParseError(memory, *schema, "cardinality(x_sc)", "indexed STRING or ID");
  expectParseError(memory, *schema, "cardinality(x_t)", "TEXT");
  expectParseError(memory, *schema, "cardinality(x_i, x_i)", "numeric constants");
  expectParseError(memory, *schema, "percentile_rank(x_is, 1)", "one scalar per document");
  expectParseError(memory, *schema, "percentile_rank(x_s, 1)", "numeric expression");
  expectParseError(memory, *schema, "percentile_rank(x_i, 1, 2)", "arguments");
  auto* cardinality = parseAggregate(memory, *schema, "cardinality(x_i, 0)");
  EXPECT_FALSE(cardinality->producers[0].resolved.state.inlineCapable);
  EXPECT_NE(nullptr, cardinality->producers[0].resolved.state.destroy);
  EXPECT_EQ(0, (int64_t)AggregateAccumulator(*cardinality).finish().intValue);
  auto* rank = parseAggregate(memory, *schema, "percentile_rank(x_i, 1)");
  EXPECT_TRUE(rank->producers[0].resolved.state.inlineCapable);
  EXPECT_FALSE(AggregateAccumulator(*rank).finish().valid);
  EXPECT_FALSE(parseAggregate(memory, *schema, "sum(x_d)")->producers[0].resolved.state.orderSensitive);
  EXPECT_FALSE(parseAggregate(memory, *schema, "sum(x_i)")->producers[0].resolved.state.orderSensitive);
}

TEST_F(AggregateExprTest, indirectStatesReleaseFailedAddsAndMerges) {
  auto schema = Schema::createDefaultSchema();
  ArenaOwner memory;
  auto* two = parseAggregate(memory, *schema, "cardinality(x_i) + cardinality(y_i)");
  RequestMemTracker small(sizeof(HyperLogLog));
  RequestMemResource initResource(small, "test aggregates");
  {
    AggregateAccumulator accumulator(*two, &initResource);
    EXPECT_EQ(0u, small.bytes());
    EXPECT_THROW(accumulator.add(0, ValueResult::integer(1)), ResourceExhaustedError);
    EXPECT_EQ(sizeof(HyperLogLog), small.bytes());
    EXPECT_THROW(accumulator.add(1, ValueResult::integer(1)), ResourceExhaustedError);
  }
  EXPECT_EQ(0u, small.bytes());
  auto* program = parseAggregate(memory, *schema, "cardinality(x_i, 1)");
  RequestMemTracker tracker(4096);
  RequestMemResource resource(tracker, "test aggregates");
  {
    AggregateAccumulator left(*program, &resource), right(*program, &resource);
    left.add(0, ValueResult::integer(1));
    right.add(0, ValueResult::integer(2));
    size_t reserved = tracker.ceiling() - tracker.bytes();
    tracker.charge(reserved, "test aggregates", "other live state");
    EXPECT_THROW(AggregateAccumulator::merge(&left, &right), ResourceExhaustedError);
    tracker.release(reserved);
    left.fail(AggregateFailure::VALUE_EVALUATION);
  }
  EXPECT_EQ(0u, tracker.bytes());
}

TEST_F(AggregateExprTest, emptyCardinalityStatesDoNotAllocate) {
  auto schema = Schema::createDefaultSchema();
  ArenaOwner memory;
  auto* program = parseAggregate(memory, *schema, "cardinality(x_i)");
  RequestMemTracker tracker(0);
  RequestMemResource resource(tracker, "cardinality");
  {
    AggregateAccumulator empty(*program, &resource), other(*program, &resource);
    empty.addBatch(0, {});
    empty.addRawBatch(0, {}, FieldType::INT);
    empty.add(0, ValueResult::floating(std::numeric_limits<double>::quiet_NaN()));
    empty.add(0, ValueResult::floating(std::numeric_limits<double>::infinity()));
    AggregateAccumulator::merge(&empty, &other);
    EXPECT_EQ(0, (int64_t)empty.finish().intValue);
    EXPECT_EQ(0u, tracker.peak());
    other.add(0, ValueResult::integer(7));
    AggregateAccumulator::merge(&other, &empty);
    EXPECT_EQ(1, (int64_t)other.finish().intValue);
    AggregateAccumulator::merge(&empty, &other);
    EXPECT_EQ(1, (int64_t)empty.finish().intValue);
    other.add(0, ValueResult::integer(8));
    EXPECT_EQ(1, (int64_t)empty.finish().intValue);
  }
  EXPECT_EQ(0u, tracker.bytes());
}

TEST_F(AggregateExprTest, cardinalityEstimatorChargesExactAndDenseStorage) {
  auto schema = Schema::createDefaultSchema();
  ArenaOwner memory;
  for (int threshold : {0, 16, 3000, 40000}) {
    auto* program = parseAggregate(memory, *schema,
        "cardinality(x_i," + std::to_string(threshold) + ")");
    RequestMemTracker tracker(0);
    RequestMemResource resource(tracker, "cardinality");
    {
      AggregateAccumulator accumulator(*program, &resource);
      int count = std::max(100, threshold * 2);
      for (int i = 0; i < count; i++) accumulator.add(0, ValueResult::integer(i));
      double estimate = (double)accumulator.finish().intValue;
      double registers = (double)(uint64_t{1} << HyperLogLog::precisionForThreshold(threshold));
      EXPECT_NEAR(count, estimate, count * 4.16 / std::sqrt(registers));
    }
    EXPECT_GT(tracker.peak(), (size_t)threshold * sizeof(uint64_t));
    EXPECT_EQ(0u, tracker.bytes());
  }
}

TEST_F(AggregateExprTest, sketchRowsHandleEmptyDomainsAndEmptyFacetBuckets) {
  CollectionHelper helper;
  helper.indexAll({flatdoc("id", "1", "x_i", 10, "s_s", "a"),
                   flatdoc("id", "2", "x_i", 30, "s_s", "b")}, UpdateMessage::COMMIT);
  auto req = localReq(helper.getSearchEngine());
  auto& root = req->collection("main");
  root.expr("c", "cardinality(other_s)");
  root.expr("r", "percentile_rank(other_i, 20)");
  auto& facet = root.rangeFacet("f", "x_i").range(0, 60, 20).mincount(0);
  facet.expr("c", "cardinality(s_s)");
  facet.expr("r", "percentile_rank(x_i,20)");
  req->execute(false);
  ASSERT_TRUE(req->ok()) << req->errorMsg();
  EXPECT_EQ(0, req->scalar<int64_t>("c"));
  EXPECT_TRUE(req->responses[0]->proto.ops.at("r")->isNull());
  const auto& result = *req->responses[0]->proto.ops.at("f")->facetResult();
  const auto& counts = std::get<api::ArrVal>(result.ops.at("c")->kind).v;
  const auto& ranks = std::get<api::ArrVal>(result.ops.at("r")->kind).v;
  ASSERT_EQ(3u, counts.size());
  EXPECT_EQ(1, counts[0].asInt());
  EXPECT_EQ(1, counts[1].asInt());
  EXPECT_EQ(0, counts[2].asInt());
  EXPECT_DOUBLE_EQ(100, ranks[0].asDouble());
  EXPECT_DOUBLE_EQ(0, ranks[1].asDouble());
  EXPECT_TRUE(ranks[2].isNull());

  auto query = localReq(helper.getSearchEngine());
  parseQueryRequest(R"json({"ops":{"f":{"query_facet":{"buckets":{"empty":"id:none"},
      "ops":{"c":"cardinality(s_s)","r":"percentile_rank(x_i,20)"}}}}})json",
      query->rawRequest(), query->mr);
  query->collection("main");
  query->execute(false);
  ASSERT_TRUE(query->ok()) << query->errorMsg();
  const auto& empty = *query->responses[0]->proto.ops.at("f")->facetResult();
  EXPECT_EQ(0, std::get<api::ArrVal>(empty.ops.at("c")->kind).v[0].asInt());
  EXPECT_TRUE(std::get<api::ArrVal>(empty.ops.at("r")->kind).v[0].isNull());
}

TEST_F(AggregateExprTest, sketchConstantsMustBeFiniteAndCardinalityCannotSortInline) {
  CollectionHelper helper;
  helper.indexAll({flatdoc("id", "1", "x_i", 1, "cat_s", "a")}, UpdateMessage::COMMIT);
  for (std::string_view expression : {"cardinality(x_i,$v)", "percentile_rank(x_i,$v)",
                                     "percentile(x_i,$v)", "percentile(x_i,50,$v)"}) {
    auto req = localReq(helper.getSearchEngine());
    req->collection("main").expr("bad", expression, "v", std::numeric_limits<double>::infinity());
    req->execute(false);
    EXPECT_FALSE(req->ok());
    EXPECT_NE(std::string::npos, req->errorMsg().find("NaN or infinity"));
  }
  auto req = localReq(helper.getSearchEngine());
  auto& facet = req->collection("main").facet("f", "cat_s");
  facet.expr("keys", "cardinality(x_i)");
  qb::sort(facet, "keys", qb::DESC);
  req->execute(false);
  EXPECT_FALSE(req->ok());
  EXPECT_NE(std::string::npos, req->errorMsg().find("without inline support"));
}

TEST_F(AggregateExprTest, cardinalityReplayMatchesDomainsAndRankSortsInline) {
  CollectionHelper helper;
  helper.indexAll({
      flatdoc("id", "1", "parent_ss", vecs("a", "b"), "tags_ss", vecs("x", "y"), "x_i", 10, "n_is", vec_i(1, 2)),
      flatdoc("id", "2", "parent_ss", vecs("a"), "tags_ss", vecs("x"), "x_i", 20, "n_is", vec_i(2, 3))}, UpdateMessage::COMMIT);
  helper.indexAll({
      flatdoc("id", "3", "parent_ss", vecs("b", "c"), "tags_ss", vecs("w", "y"), "x_i", 30, "n_is", vec_i(3, 4)),
      flatdoc("id", "4", "parent_ss", vecs("c"), "x_i", 40)}, UpdateMessage::COMMIT);
  auto guard = facetAggregateOverrides();
  forcedFacetSubOpInline = FacetSubOpInlineMode::ALL;
  auto run = [&](FacetFeedStrategy feed) {
    forcedFacetFeedStrategy = feed;
    auto req = localReq(helper.getSearchEngine());
    auto& facet = req->collection("main").facet("f", "parent_ss").limit(-1);
    facet.expr("pct", "percentile_rank(x_i, 20)");
    facet.expr("keys", "cardinality(tags_ss)");
    facet.expr("numbers", "cardinality(n_is + 1)");
    facet.expr("mixed", "cardinality(tags_ss) + percentile_rank(x_i, 20)");
    qb::sort(facet, "pct", qb::DESC);
    req->execute(-1);
    EXPECT_TRUE(req->ok()) << req->errorMsg();
    EXPECT_EQ(0u, req->memoryTracker.bytes());
    std::vector<std::byte> encoded;
    if (req->ok()) {
      const auto& result = *req->responses[0]->proto.ops.at("f")->facetResult();
      EXPECT_EQ("a", std::get<api::ColStr>(result.bucket_ids->kind).v[0]);
      const auto& keys = std::get<api::ArrVal>(result.ops.at("keys")->kind).v;
      EXPECT_EQ(2, keys[0].asInt());
      EXPECT_EQ(3, keys[1].asInt());
      EXPECT_EQ(2, keys[2].asInt());
      const auto& mixed = std::get<api::ArrVal>(result.ops.at("mixed")->kind).v;
      EXPECT_DOUBLE_EQ(102, mixed[0].asDouble());
      EXPECT_TRUE(api::encode(result, encoded));
    }
    return encoded;
  };
  auto domains = run(FacetFeedStrategy::BUCKET_DOMAINS);
  EXPECT_EQ(domains, run(FacetFeedStrategy::STRING_COLUMN_REPLAY));
}

TEST_F(AggregateExprTest, indirectReplayAndDomainFailuresReleaseAllState) {
  CollectionHelper helper;
  std::vector<Doc> docs;
  for (int i = 0; i < 100; i++) docs.push_back(flatdoc("id", std::to_string(i),
      "cat_s", "c" + std::to_string(i % 4), "x_i", i));
  helper.indexAll(docs, UpdateMessage::COMMIT);
  auto guard = facetAggregateOverrides();
  forcedFacetSubOpInline = FacetSubOpInlineMode::SORT_KEY_ONLY;
  for (auto feed : {FacetFeedStrategy::BUCKET_DOMAINS, FacetFeedStrategy::STRING_COLUMN_REPLAY}) {
    forcedFacetFeedStrategy = feed;
    for (size_t ceiling : {32u, 256u, 512u}) {
      forcedRequestMemoryMaxBytes = ceiling;
      auto req = localReq(helper.getSearchEngine());
      req->collection("main").facet("f", "cat_s").limit(-1)
          .expr("keys", "cardinality(x_i) + cardinality(id)");
      req->execute(false);
      EXPECT_FALSE(req->ok()) << ceiling;
      EXPECT_EQ(0u, req->memoryTracker.bytes()) << ceiling;
    }
  }
}

TEST_F(AggregateExprTest, manySmallCardinalityBucketsRespectRequestBudget) {
  CollectionHelper helper;
  std::vector<Doc> docs;
  for (int i = 0; i < 1024; i++) docs.push_back(flatdoc("id", std::to_string(i),
      "cat_s", "c" + std::to_string(i % 64), "key_s", "k" + std::to_string(i)));
  helper.indexAll(docs, UpdateMessage::COMMIT);
  auto guard = facetAggregateOverrides();
  forcedRequestMemoryMaxBytes = 64 * 1024;
  auto req = localReq(helper.getSearchEngine());
  req->collection("main").facet("f", "cat_s").limit(-1).expr("keys", "cardinality(key_s)");
  req->execute(false);
  ASSERT_TRUE(req->ok()) << req->errorMsg();
  const auto& result = *req->responses[0]->proto.ops.at("f")->facetResult();
  const auto& values = std::get<api::ArrVal>(result.ops.at("keys")->kind).v;
  ASSERT_EQ(64u, values.size());
  for (const auto& value : values) EXPECT_EQ(16, value.asInt());
  EXPECT_LE(req->memoryTracker.peak(), 64u * 1024);
  EXPECT_EQ(0u, req->memoryTracker.bytes());
}

TEST_F(AggregateExprTest, cardinalityBucketBlocksUseObservedStateBytes) {
  CollectionHelper helper;
  for (int segment = 0; segment < 2; segment++) {
    std::vector<Doc> docs;
    for (int i = 0; i < 70; i++) docs.push_back(flatdoc(
        "id", std::to_string(segment * 70 + i), "bucket_i", i,
        "x_is", vec_i(i, segment * 70 + i)));
    helper.indexAll(docs, UpdateMessage::COMMIT);
  }
  size_t blocks = 0;
  auto guard = facetAggregateOverrides();
  forcedRangeFacetBucketDomainByteBudget = 1;
  facetBucketBlockCounter = &blocks;
  for (size_t budget : {4u * 1024, 256u * 1024}) {
    forcedRangeFacetBindingStateChunkBytes = budget;
    blocks = 0;
    auto req = localReq(helper.getSearchEngine());
    req->collection("main").rangeFacet("f", "bucket_i").range(0, 70, 1)
        .expr("keys", "cardinality(x_is)");
    req->execute(false);
    ASSERT_TRUE(req->ok()) << req->errorMsg();
    const auto& result = *req->responses[0]->proto.ops.at("f")->facetResult();
    const auto& values = std::get<api::ArrVal>(result.ops.at("keys")->kind).v;
    ASSERT_EQ(70u, values.size());
    for (const auto& value : values) EXPECT_EQ(2, value.asInt());
    if (budget == 4u * 1024) {
      EXPECT_GT(blocks, 2u);
      EXPECT_LT(blocks, 70u);
    } else {
      EXPECT_EQ(2u, blocks);
    }
    EXPECT_EQ(0u, req->memoryTracker.bytes());
  }
}

TEST_F(AggregateExprTest, exactRankOrdersCollapsedGroups) {
  CollectionHelper helper;
  helper.indexAll({flatdoc("id", "1", "cat_s", "a", "x_i", 10),
                   flatdoc("id", "2", "cat_s", "b", "x_i", 30)}, UpdateMessage::COMMIT);
  helper.indexAll({flatdoc("id", "3", "cat_s", "a", "x_i", 20),
                   flatdoc("id", "4", "cat_s", "b", "x_i", 10)}, UpdateMessage::COMMIT);
  auto req = localReq(helper.getSearchEngine());
  parseQueryRequest(R"json({"limit":-1,"get_scores":true,"collapse":{"field":"cat_s","head_sort":"x_i asc",
      "sort":"pct desc","ops":{"pct":"percentile_rank(x_i,20)"}}})json",
      req->rawRequest(), req->mr);
  req->collection("main");
  req->execute(-1);
  ASSERT_TRUE(req->ok()) << req->errorMsg();
  const auto* list = req->docList();
  ASSERT_EQ(2u, list->groups->ranks.size());
  EXPECT_DOUBLE_EQ(100, list->groups->ranks[0].v[0].asDouble());
  EXPECT_DOUBLE_EQ(50, list->groups->ranks[1].v[0].asDouble());
}

TEST_F(AggregateExprTest, percentileValidationNatureAndExactInputs) {
  auto schema = Schema::createDefaultSchema();
  ArenaOwner memory;
  for (std::string_view expr : {"percentile(x_i,-1)", "percentile(x_i,101)"}) {
    expectParseError(memory, *schema, expr, "in [0, 100]");
  }
  for (std::string_view expr : {"percentile(x_i,50,19)", "percentile(x_i,50,1001)"}) {
    expectParseError(memory, *schema, expr, "in [20, 1000]");
  }
  expectParseError(memory, *schema, "percentile(x_is,50)", "one scalar per document");
  expectParseError(memory, *schema, "percentile(x_s,50)", "numeric expression");
  expectParseError(memory, *schema, "percentile(x_i)", "arguments");
  expectParseError(memory, *schema, "percentile(x_i, x_i)", "numeric constants");
  EXPECT_NO_THROW(parseAggregate(memory, *schema, "percentile(x_i,100,1000)"));
  auto* program = parseAggregate(memory, *schema, "percentile(when_dt,25,20.5)");
  EXPECT_EQ(ValueNature::DATE_MS, program->root().nature);
  EXPECT_EQ(BucketValueType::DOUBLE, program->root().type);
  EXPECT_TRUE(program->producers[0].resolved.state.orderSensitive);
  EXPECT_FALSE(program->producers[0].resolved.state.inlineCapable);
  RequestMemTracker tracker(0);
  RequestMemResource resource(tracker, "percentile");
  {
    AggregateAccumulator accumulator(*program, &resource);
    accumulator.addBatch(0, {});
    accumulator.addRawBatch(0, {}, FieldType::INT);
    EXPECT_FALSE(accumulator.finish().valid);
    EXPECT_EQ(0u, tracker.bytes());
    for (int i : {30, 10, 40, 20}) accumulator.add(0, ValueResult::integer(i));
    accumulator.add(0, ValueResult::floating(std::numeric_limits<double>::quiet_NaN()));
    accumulator.add(0, ValueResult::floating(std::numeric_limits<double>::infinity()));
    auto result = accumulator.finish();
    EXPECT_DOUBLE_EQ(17.5, result.doubleValue);
    EXPECT_EQ(ValueNature::DATE_MS, result.nature);
  }
  EXPECT_EQ(0u, tracker.bytes());
}

TEST_F(AggregateExprTest, internedPercentilesShareDigestMemoryAndBits) {
  auto schema = Schema::createDefaultSchema();
  ArenaOwner memory;
  SearchOverridesGuard guard(disableAggregateSharingForTests);
  std::array<size_t, 2> bytes{};
  std::array<uint64_t, 2> bits{};
  for (int separate : {0, 1}) {
    disableAggregateSharingForTests = separate != 0;
    auto* program = parseAggregate(memory, *schema,
        "percentile(x_d,99) - percentile(x_d,50,100)");
    EXPECT_EQ(2u, program->leaves.size());
    ASSERT_EQ(separate ? 2u : 1u, program->producers.size());
    RequestMemTracker tracker(0);
    RequestMemResource resource(tracker, "interned percentile");
    {
      AggregateAccumulator accumulator(*program, &resource);
      EXPECT_FALSE(accumulator.finish().valid);
      EXPECT_EQ(0u, tracker.bytes());
      for (int value : {3, 1, 2}) {
        for (uint32_t producer = 0; producer < program->producers.size(); producer++) {
          accumulator.add(producer, ValueResult::floating((double)value));
        }
      }
      EXPECT_DOUBLE_EQ(0.98, accumulator.finish().doubleValue);
      for (int i = 0; i < 2400; i++) {
        for (uint32_t producer = 0; producer < program->producers.size(); producer++) {
          accumulator.add(producer, ValueResult::floating(std::exp((double)(i % 101) / 10)));
        }
      }
      auto result = accumulator.finish();
      ASSERT_TRUE(result.valid);
      bits[separate] = std::bit_cast<uint64_t>(result.doubleValue);
      bytes[separate] = resource.bytes();
      EXPECT_EQ(bytes[separate], tracker.bytes());
    }
    EXPECT_EQ(0u, tracker.bytes());
  }
  EXPECT_EQ(bits[0], bits[1]);
  EXPECT_GT(bytes[0], sizeof(TDigest));
  EXPECT_EQ(bytes[0] * 2, bytes[1]);
}

TEST_F(AggregateExprTest, producerKeysUseFamilyInputAndStateArguments) {
  auto schema = Schema::createDefaultSchema();
  ArenaOwner memory;
  for (auto [expression, count] : std::initializer_list<std::pair<std::string_view, size_t>>{
      {"sum(x_i) + sum(col('x_i'))", 1},
      {"avg(x_i) + avg(x_i)", 1},
      {"min(x_i) + min(x_i)", 1},
      {"max(x_i) + max(x_i)", 1},
      {"sum(x_i) + avg(x_i)", 2},
      {"percentile(x_d,99,100.0) - percentile(x_d,50)", 1},
      {"percentile(x_d,99,101) - percentile(x_d,50)", 2},
      {"cardinality(x_i) + cardinality(x_i,3000.0)", 1},
      {"cardinality(x_i,3000) + cardinality(x_i)", 1},
      {"cardinality(x_i,3001) + cardinality(x_i)", 2},
      {"percentile_rank(x_i,10) + percentile_rank(x_i,20)", 2},
      {"percentile_rank(f_f,0.1) + percentile_rank(f_f,0.10000000149011612)", 1},
      {"sum(x_i) + sum(y_i)", 2},
      {"sum(x_i) + sum(x_i + 0)", 2},
      {"sum(x_i + 1) + sum(1 + x_i)", 2},
      {"sum(9007199254740992) + sum(9007199254740993)", 2}}) {
    EXPECT_EQ(count, parseAggregate(memory, *schema, expression)->producers.size()) << expression;
  }
}

TEST_F(AggregateExprTest, producerInputsCompareResolvedConstantsStructurally) {
  auto schema = Schema::createDefaultSchema();
  ArenaOwner memory, otherMemory;
  std::array<int64_t, 2> integers{9007199254740992LL, 9007199254740993LL};
  auto otherIntegers = integers;
  std::array<double, 2> doubles{0.5, 1.5};
  auto otherDoubles = doubles;
  api::Val scalar, array, copy, floating, floatingCopy;
  scalar.kind = int64_t{9007199254740993LL};
  array.kind = api::ArrInt{integers};
  copy.kind = api::ArrInt{otherIntegers};
  floating.kind = api::ArrDouble{doubles};
  floatingCopy.kind = api::ArrDouble{otherDoubles};
  using Pair = std::pair<std::string_view, ::hpp_proto::indirect_view<api::Val>>;
  std::array<Pair, 5> pairs{{{"n", {&scalar}}, {"a", {&array}},
                            {"b", {&copy}}, {"d", {&floating}}, {"e", {&floatingCopy}}}};
  AggregateExprOptions options{schema.get(), {pairs}, "constants"};
  auto parse = [&](std::string_view expression) {
    return AggregateExprParser(options, *memory.arena).parse(expression);
  };
  EXPECT_EQ(1u, parse("sum($n) + sum(9007199254740993)")->producers.size());
  EXPECT_EQ(2u, parse("sum($n) + sum(9007199254740992)")->producers.size());
  auto* arrays = parse("cardinality($a) + cardinality($b)");
  EXPECT_EQ(1u, arrays->producers.size());
  EXPECT_EQ(1u, parse("cardinality($d) + cardinality($e)")->producers.size());
  EXPECT_EQ(2u, parse("cardinality($a) + cardinality($d)")->producers.size());
  auto* shifted = AggregateExprParser(options, *otherMemory.arena).parse("cardinality($b)");
  auto& arrayInput = *shifted->producers[0].input;
  arrayInput.intArrays.insert(arrayInput.intArrays.begin(), 7);
  arrayInput.nodes[arrayInput.rootNode].arrayOffset++;
  EXPECT_TRUE(arrays->producers[0].sameState(shifted->producers[0]));
  auto* left = parse("sum(x_i + $n)");
  auto* right = parseAggregate(otherMemory, *schema, "sum( x_i + 9007199254740993 )");
  auto& input = *right->producers[0].input;
  input.nodes.insert(input.nodes.begin(), ValueNode{});
  for (auto& node : input.nodes) {
    for (uint8_t i = 0; i < node.childCount; i++) node.children[i]++;
  }
  input.rootNode++;
  EXPECT_TRUE(left->producers[0].sameState(right->producers[0]));
  otherIntegers[1]--;
  EXPECT_EQ(2u, parse("cardinality($a) + cardinality($b)")->producers.size());
  otherDoubles[1]++;
  EXPECT_EQ(2u, parse("cardinality($d) + cardinality($e)")->producers.size());
}

TEST_F(AggregateExprTest, programsAdoptProducerLayoutForSharedStateReadouts) {
  auto schema = Schema::createDefaultSchema();
  ArenaOwner memory;
  auto* source = parseAggregate(memory, *schema,
      "cardinality(id) + percentile(x_d,99)");
  auto* target = parseAggregate(memory, *schema,
      "percentile(x_d,50,100) + cardinality(id,3000)");
  auto* reference = parseAggregate(memory, *schema,
      "percentile(x_d,50,100) + cardinality(id,3000)");
  auto* different = parseAggregate(memory, *schema,
      "percentile(x_d,50,101) + cardinality(id)");
  EXPECT_FALSE(different->adoptProducers(*source));
  EXPECT_EQ(ValueType::DOUBLE, different->producers[0].input->root().type);
  EXPECT_TRUE(source->adoptProducers(*source));
  ASSERT_TRUE(target->adoptProducers(*source));
  EXPECT_EQ(source->stateBytes, target->stateBytes);
  EXPECT_EQ(1u, target->leaves[0].producer);
  EXPECT_EQ(0u, target->leaves[1].producer);
  for (size_t i = 0; i < source->producers.size(); i++) {
    EXPECT_EQ(source->producers[i].stateOffset, target->producers[i].stateOffset);
    EXPECT_EQ(source->producers[i].arguments.data(), target->producers[i].arguments.data());
  }
  RequestMemTracker tracker(0);
  RequestMemResource resource(tracker, "shared layout");
  {
    std::vector<std::byte> storage(AggregateStateView::bytes(*source));
    AggregateStateView state(*source, storage.data()), readout(*target, storage.data());
    state.init(&resource);
    AggregateAccumulator expected(*reference, &resource);
    for (int i = 0; i < 1200; i++) {
      state.add(0, ValueResult::integer(i % 29));
      expected.add(1, ValueResult::integer(i % 29));
      auto value = ValueResult::floating(std::exp((double)(i % 101) / 10));
      state.add(1, value);
      expected.add(0, value);
    }
    EXPECT_EQ(std::bit_cast<uint64_t>(expected.finish().doubleValue),
              std::bit_cast<uint64_t>(readout.finish().doubleValue));
    EXPECT_GT(state.finish().doubleValue, readout.finish().doubleValue);
    state.destroy();
  }
  EXPECT_EQ(0u, tracker.bytes());
}

TEST_F(AggregateExprTest, internedInputsAgreeAcrossBucketInlineAndReplayBindings) {
  CollectionHelper helper;
  indexPercentileData(helper, 3);
  auto guard = facetAggregateOverrides();
  forcedFacetSubOpInline = FacetSubOpInlineMode::ALL;
  for (auto feed : {FacetFeedStrategy::BUCKET_DOMAINS, FacetFeedStrategy::STRING_COLUMN_REPLAY}) {
    forcedFacetFeedStrategy = feed;
    std::vector<uint64_t> expected;
    for (bool separate : {false, true}) {
      disableAggregateSharingForTests = separate;
      auto req = localReq(helper.getSearchEngine());
      auto& root = req->collection("main");
      root.expr("spread", "percentile(x_d,99) - percentile(x_d,50)");
      root.expr("expression", "percentile(x_d*2,99) - percentile(x_d*2,50)");
      root.expr("ords", "cardinality(cat_s) + cardinality(cat_s)");
      auto& facet = root.facet("f", "cat_s").limit(-1);
      facet.expr("spread", "percentile(x_d,99) - percentile(x_d,50)");
      facet.expr("twice", "sum(x_d) + sum(x_d)");
      facet.expr("pct", "percentile_rank(x_d,100) + percentile_rank(x_d,100)");
      req->execute(false);
      ASSERT_TRUE(req->ok()) << req->errorMsg();
      EXPECT_EQ(4, req->scalar<int64_t>("ords"));
      std::vector<uint64_t> bits;
      for (std::string_view name : {"spread", "expression"}) {
        bits.push_back(std::bit_cast<uint64_t>(req->scalar<double>(name)));
      }
      auto& result = *req->responses[0]->proto.ops.at("f")->facetResult();
      for (std::string_view name : {"spread", "twice", "pct"}) {
        for (const auto& value : std::get<api::ArrVal>(result.ops.at(name)->kind).v) {
          bits.push_back(std::bit_cast<uint64_t>(value.asDouble()));
        }
      }
      if (!separate) expected = bits;
      else EXPECT_EQ(expected, bits);
      EXPECT_EQ(0u, req->memoryTracker.bytes());
    }
  }
}

TEST_F(AggregateExprTest, internedEmptyAndFailingInputsPreserveNulls) {
  CollectionHelper helper;
  helper.index(flatdoc("id", "1", "x_i", -1), UpdateMessage::COMMIT);
  SearchOverridesGuard guard(disableAggregateSharingForTests);
  for (bool separate : {false, true}) {
    disableAggregateSharingForTests = separate;
    auto req = localReq(helper.getSearchEngine());
    auto& root = req->collection("main");
    root.expr("empty", "percentile(missing_d,99) - percentile(missing_d,50)");
    root.expr("bad", "percentile(sqrt(x_i),99) - percentile(sqrt(x_i),50)");
    root.expr("zero", "cardinality(missing_d) + cardinality(missing_d)");
    req->execute(false);
    ASSERT_TRUE(req->ok()) << req->errorMsg();
    EXPECT_TRUE(req->responses[0]->proto.ops.at("empty")->isNull());
    EXPECT_TRUE(req->responses[0]->proto.ops.at("bad")->isNull());
    EXPECT_EQ(0, req->scalar<int64_t>("zero"));
    EXPECT_EQ(0u, req->memoryTracker.bytes());
  }
}

TEST_F(AggregateExprTest, siblingPercentilesShareRootStateAndRemainDeterministic) {
  CollectionHelper helper;
  indexSharedSketchData(helper);
  auto guard = facetAggregateOverrides();
  std::string json = "{\"ops\":" + std::string(SHARED_SKETCH_OPS) + "}";
  AggregateExecutionStats baselineStats, sharedStats;
  disableAggregateSharingForTests = true;
  auto baseline = runSharedSketchRequest(helper, json, baselineStats);
  disableAggregateSharingForTests = false;
  auto shared = runSharedSketchRequest(helper, json, sharedStats);
  EXPECT_EQ(baseline.values, shared.values);
  EXPECT_EQ(4u, shared.values.size());
  EXPECT_EQ(3u, sharedStats.followers.load());
  EXPECT_EQ(3u, sharedStats.producerStates.load());
  EXPECT_EQ(15u, baselineStats.producerStates.load());
  EXPECT_GT(sharedStats.finishedStateBytes.load(), sizeof(TDigest));
  EXPECT_EQ(sharedStats.finishedStateBytes.load() * 5, baselineStats.finishedStateBytes.load());
  EXPECT_GT(baseline.peakBytes, shared.peakBytes);
  for (int run = 0; run < 3; run++) {
    AggregateExecutionStats stats;
    EXPECT_EQ(baseline.values, runSharedSketchRequest(helper, json, stats, -1).values);
  }
}

TEST_F(AggregateExprTest, siblingCardinalitiesShareReusableState) {
  CollectionHelper helper;
  indexSharedSketchData(helper);
  auto guard = facetAggregateOverrides();
  std::string_view json = R"json({"ops":{"a":"cardinality(x_d)",
    "z":"cardinality(x_d)+cardinality(x_d)"}})json";
  AggregateExecutionStats baselineStats, sharedStats;
  disableAggregateSharingForTests = true;
  auto baseline = runSharedSketchRequest(helper, json, baselineStats);
  disableAggregateSharingForTests = false;
  auto shared = runSharedSketchRequest(helper, json, sharedStats);
  EXPECT_EQ(baseline.values, shared.values);
  EXPECT_EQ(1u, sharedStats.followers.load());
  EXPECT_EQ(1u, sharedStats.producerStates.load());
  EXPECT_EQ(3u, baselineStats.producerStates.load());
  EXPECT_EQ(sharedStats.finishedStateBytes.load() * 3, baselineStats.finishedStateBytes.load());
}

TEST_F(AggregateExprTest, siblingSketchesShareFacetBucketsAndDeclineReplay) {
  CollectionHelper helper;
  indexSharedSketchData(helper);
  auto guard = facetAggregateOverrides();
  SearchOverridesGuard blockGuard(forcedFacetBucketBlockBuckets);
  forcedFacetBucketBlockBuckets = 1;
  std::string ops(SHARED_SKETCH_OPS);
  std::array<std::pair<std::string, size_t>, 4> cases{{
    {R"json({"ops":{"f":{"field_facet":{"field":"cat_s","limit":-1,"ops":)json" + ops + "}}}}", 2},
    {R"json({"ops":{"f":{"field_facet":{"field":"bucket_i","limit":-1,"ops":)json" + ops + "}}}}", 2},
    {R"json({"ops":{"f":{"range_facet":{"field":"bucket_i","start":0,"end":3,"gap":1,"mincount":0,"ops":)json" + ops + "}}}}", 3},
    {R"json({"ops":{"f":{"query_facet":{"buckets":{"a":"cat_s:a","b":"cat_s:b","empty":"id:absent"},"ops":)json" + ops + "}}}}", 3}
  }};
  for (auto feed : {FacetFeedStrategy::BUCKET_DOMAINS, FacetFeedStrategy::STRING_COLUMN_REPLAY}) {
    forcedFacetFeedStrategy = feed;
    for (auto& [json, buckets] : cases) {
      SCOPED_TRACE(json);
      AggregateExecutionStats baselineStats, sharedStats;
      disableAggregateSharingForTests = true;
      auto baseline = runSharedSketchRequest(helper, json, baselineStats);
      disableAggregateSharingForTests = false;
      auto shared = runSharedSketchRequest(helper, json, sharedStats);
      EXPECT_EQ(baseline.values, shared.values);
      EXPECT_FALSE(shared.values.empty());
      EXPECT_EQ(3u, sharedStats.followers.load());
      EXPECT_EQ(buckets * 3, sharedStats.producerStates.load());
      EXPECT_EQ(buckets * 15, baselineStats.producerStates.load());
      EXPECT_EQ(sharedStats.finishedStateBytes.load() * 5, baselineStats.finishedStateBytes.load());
    }
  }
}

TEST_F(AggregateExprTest, siblingSketchesRouteListGroupAndFusionResults) {
  CollectionHelper helper;
  indexSharedSketchData(helper);
  auto guard = facetAggregateOverrides();
  std::string ops(SHARED_SKETCH_OPS);
  std::array<std::pair<std::string, size_t>, 4> cases{{
    {R"json({"ops":{"q":{"top_docs":{"limit":0,"ops":)json" + ops + "}}}}", 3},
    {R"json({"ops":{"q":{"top_docs":{"limit":2,"collapse":{"field":"cat_s","head_sort":"id asc","ops":)json"
        + ops + R"json(},"ops":)json" + ops + "}}}}", 6},
    {R"json({"ops":{"f":{"fusion":{"rrf":{},"sources":{"all":{"limit":-1}},"limit":-1,"ops":)json"
        + ops + "}}}}", 3},
    {R"json({"ops":{"f":{"fusion":{"rrf":{},"sources":{"all":{"limit":-1}},"limit":2,"collapse":{"field":"cat_s","head_sort":"id asc","ops":)json"
        + ops + R"json(},"ops":)json" + ops + "}}}}", 6}
  }};
  for (auto& [json, followers] : cases) {
    SCOPED_TRACE(json);
    AggregateExecutionStats baselineStats, sharedStats;
    disableAggregateSharingForTests = true;
    auto baseline = runSharedSketchRequest(helper, json, baselineStats);
    disableAggregateSharingForTests = false;
    auto shared = runSharedSketchRequest(helper, json, sharedStats, -1);
    EXPECT_EQ(baseline.values, shared.values);
    EXPECT_FALSE(shared.values.empty());
    EXPECT_EQ(followers, sharedStats.followers.load());
  }
}

TEST_F(AggregateExprTest, siblingSketchGroupsRequireEqualIndirectProducerSets) {
  CollectionHelper helper;
  indexSharedSketchData(helper);
  auto guard = facetAggregateOverrides();
  for (std::string_view ops : {
      R"json({"a":"percentile(x_d,50)","b":"percentile(x_d,99,101)"})json",
      R"json({"a":"percentile(x_d,50)","b":"percentile(y_d,99)"})json",
      R"json({"a":"percentile(x_d,50)","b":"percentile(x_d,99)+sum(x_d)"})json",
      R"json({"a":"cardinality(x_d)","b":"percentile(x_d,99)"})json",
      R"json({"a":"percentile(x_d,50)","b":"percentile(x_d,99)+cardinality(cat_s)"})json"}) {
    SCOPED_TRACE(ops);
    std::string json = "{\"ops\":" + std::string(ops) + "}";
    AggregateExecutionStats baselineStats, sharedStats;
    disableAggregateSharingForTests = true;
    auto baseline = runSharedSketchRequest(helper, json, baselineStats);
    disableAggregateSharingForTests = false;
    auto shared = runSharedSketchRequest(helper, json, sharedStats);
    EXPECT_EQ(baseline.values, shared.values);
    EXPECT_EQ(2u, shared.values.size());
    EXPECT_EQ(0u, sharedStats.followers.load());
    EXPECT_EQ(baselineStats.producerStates.load(), sharedStats.producerStates.load());
  }
}

TEST_F(AggregateExprTest, siblingSketchGroupsRespectRoutesAndNormalizeDefault) {
  CollectionHelper helper;
  indexSharedSketchData(helper);
  auto guard = facetAggregateOverrides();
  for (std::string json : {
      R"json({"ops":{"q":{"top_docs":{"limit":0,"filter":[{"query":"bucket_i:0","except_ops":["p50"]}],"ops":)json"
          + std::string(SHARED_SKETCH_OPS) + "}}}}",
      std::string(R"json({"ops":{"f":{"field_facet":{"field":"cat_s","limit":-1,"ops":{
        "a":"percentile(x_d,50)","z":{"expr_op":"percentile(x_d,99)","domain":{}}}}}}})json")}) {
    SCOPED_TRACE(json);
    AggregateExecutionStats baselineStats, sharedStats;
    disableAggregateSharingForTests = true;
    auto baseline = runSharedSketchRequest(helper, json, baselineStats);
    disableAggregateSharingForTests = false;
    auto shared = runSharedSketchRequest(helper, json, sharedStats);
    EXPECT_EQ(baseline.values, shared.values);
    EXPECT_FALSE(shared.values.empty());
    EXPECT_EQ(json.find("except_ops") != std::string::npos ? 2u : 1u,
              sharedStats.followers.load());
    EXPECT_EQ(6u, sharedStats.producerStates.load());
  }
}

TEST_F(AggregateExprTest, siblingSketchGroupsRefreshFieldsAfterReorderingProducers) {
  CollectionHelper helper;
  indexSharedSketchData(helper);
  auto guard = facetAggregateOverrides();
  for (std::string_view json : {
      R"json({"ops":{"a":"percentile(x_d,50)+percentile(y_d+1,99)",
                   "z":"percentile(y_d+1,50)+percentile(x_d,99)"}})json",
      R"json({"ops":{"a":"cardinality(cat_s)+percentile(x_d,50)",
                   "z":"percentile(x_d,99)+cardinality(cat_s)"}})json"}) {
    AggregateExecutionStats baselineStats, sharedStats;
    disableAggregateSharingForTests = true;
    auto baseline = runSharedSketchRequest(helper, json, baselineStats);
    disableAggregateSharingForTests = false;
    auto shared = runSharedSketchRequest(helper, json, sharedStats);
    EXPECT_EQ(baseline.values, shared.values);
    EXPECT_EQ(1u, sharedStats.followers.load());
    EXPECT_EQ(6u, sharedStats.producerStates.load());
    EXPECT_EQ(sharedStats.finishedStateBytes.load() * 2, baselineStats.finishedStateBytes.load());
  }
}

TEST_F(AggregateExprTest, siblingSketchesPreserveEmptyAndFailingInputs) {
  CollectionHelper helper;
  auto guard = facetAggregateOverrides();
  std::string ops(SHARED_SKETCH_OPS);
  for (const std::string& json : {"{\"ops\":" + ops + "}",
      R"json({"ops":{"f":{"query_facet":{"buckets":{"empty":"id:absent"},"ops":)json" + ops + "}}}}"}) {
    AggregateExecutionStats baselineStats, sharedStats;
    disableAggregateSharingForTests = true;
    auto baseline = runSharedSketchRequest(helper, json, baselineStats);
    disableAggregateSharingForTests = false;
    auto shared = runSharedSketchRequest(helper, json, sharedStats);
    EXPECT_EQ(baseline.values, shared.values);
    EXPECT_FALSE(shared.values.empty());
    EXPECT_EQ(3u, sharedStats.followers.load());
    EXPECT_EQ(1u, sharedStats.producerStates.load());
    EXPECT_EQ(0u, sharedStats.finishedStateBytes.load());
  }
  helper.index(flatdoc("id", "bad", "x_d", -1.0), UpdateMessage::COMMIT);
  std::string json = R"json({"ops":{"a":"percentile(sqrt(x_d),50)",
                              "z":"percentile(sqrt(x_d),99)"}})json";
  AggregateExecutionStats baselineStats, sharedStats;
  disableAggregateSharingForTests = true;
  auto baseline = runSharedSketchRequest(helper, json, baselineStats);
  disableAggregateSharingForTests = false;
  auto shared = runSharedSketchRequest(helper, json, sharedStats);
  EXPECT_EQ(baseline.values, shared.values);
  ASSERT_EQ(2u, shared.values.size());
  EXPECT_EQ(1u, shared.values.at("/a").size());
  EXPECT_EQ(1u, shared.values.at("/z").size());
  EXPECT_EQ(1u, sharedStats.followers.load());
}

TEST_F(AggregateExprTest, siblingSketchReadoutFailureUsesMemberIdentity) {
  CollectionHelper helper;
  helper.indexAll({flatdoc("id", "1", "x_d", 1.0), flatdoc("id", "2", "x_d", 4.0)},
                  UpdateMessage::COMMIT);
  SearchOverridesGuard guard(disableAggregateSharingForTests);
  uint64_t expected = 0;
  for (bool separate : {true, false}) {
    disableAggregateSharingForTests = separate;
    auto req = localReq(helper.getSearchEngine());
    parseQueryRequest(R"json({"ops":{"a":"percentile(x_d,50)",
      "z":"sqrt(percentile(x_d,50)-percentile(x_d,99))"}})json",
      req->rawRequest(), req->mr);
    req->collection("main");
    req->execute(false);
    ASSERT_TRUE(req->ok()) << req->errorMsg();
    auto bits = std::bit_cast<uint64_t>(req->scalar<double>("a"));
    if (separate) expected = bits;
    else EXPECT_EQ(expected, bits);
    EXPECT_TRUE(req->responses.back()->proto.ops.at("z")->isNull());
    ASSERT_EQ(1u, req->respWarnings().size());
    EXPECT_NE(std::string_view::npos, req->respWarnings()[0].message.find("op 'z'"));
  }
}

TEST_F(AggregateExprTest, percentileFieldRangeAndQueryBuckets) {
  CollectionHelper helper;
  helper.indexAll({flatdoc("id", "1", "x_i", 10, "when_dt", int64_t{10}, "bucket_i", 0, "cat_s", "a"),
                   flatdoc("id", "2", "x_i", 30, "when_dt", int64_t{30}, "bucket_i", 1, "cat_s", "b")}, UpdateMessage::COMMIT);
  helper.indexAll({flatdoc("id", "3", "x_i", 20, "when_dt", int64_t{20}, "bucket_i", 0, "cat_s", "a"),
                   flatdoc("id", "4", "x_i", 40, "when_dt", int64_t{40}, "bucket_i", 1, "cat_s", "b")}, UpdateMessage::COMMIT);
  auto req = localReq(helper.getSearchEngine());
  parseQueryRequest(R"json({"ops":{"queries":{"query_facet":{"buckets":{
      "a":"cat_s:a","empty":"id:none"},"ops":{"pct":"percentile(x_i,50)"}}}}})json",
      req->rawRequest(), req->mr);
  auto& root = req->collection("main");
  root.expr("exact", "percentile(x_i,25)");
  root.expr("dates", "percentile(when_dt,50)");
  root.expr("missing", "percentile(other_i,50)");
  root.facet("field", "cat_s").limit(-1).expr("pct", "percentile(x_i,50)");
  root.rangeFacet("ranges", "bucket_i").range(0,3,1).mincount(0)
      .expr("pct", "percentile(x_i,50)");
  req->execute(-1);
  ASSERT_TRUE(req->ok()) << req->errorMsg();
  EXPECT_DOUBLE_EQ(17.5, req->scalar<double>("exact"));
  EXPECT_DOUBLE_EQ(25, req->scalar<double>("dates"));
  EXPECT_TRUE(req->responses[0]->proto.ops.at("missing")->isNull());
  for (std::string_view name : {"field", "ranges", "queries"}) {
    auto& facet = *req->responses[0]->proto.ops.at(name)->facetResult();
    const auto& values = std::get<api::ArrVal>(facet.ops.at("pct")->kind).v;
    ASSERT_GE(values.size(), 2u);
    EXPECT_DOUBLE_EQ(15, values[0].asDouble()) << name;
    if (name == "queries") EXPECT_TRUE(values[1].isNull());
    else EXPECT_DOUBLE_EQ(35, values[1].asDouble()) << name;
    if (name == "ranges") {
      ASSERT_EQ(3u, values.size());
      EXPECT_TRUE(values[2].isNull());
    }
  }
  EXPECT_EQ(0u, req->memoryTracker.bytes());
}

TEST_F(AggregateExprTest, percentileRankAccuracyAndParallelDeterminism) {
  CollectionHelper helper;
  auto values = indexPercentileData(helper, 12);
  auto reader = helper.getIndexWriter()->snapshots.readers.getReader();
  ASSERT_GE(reader->segments().size(), 8u);
  const std::array<double, 7> percents{0, 0.1, 1, 50, 95, 99.9, 100};
  std::vector<uint64_t> expected;
  for (int run = 0; run < 7; run++) {
    auto req = localReq(helper.getSearchEngine());
    auto& root = req->collection("main");
    for (size_t i = 0; i < percents.size(); i++) {
      root.expr("p" + std::to_string(i), "percentile(x_d,$p)", "p", percents[i]);
    }
    req->execute(run == 0 ? 1 : -1);
    ASSERT_TRUE(req->ok()) << req->errorMsg();
    for (size_t i = 0; i < percents.size(); i++) {
      double x = req->scalar<double>("p" + std::to_string(i));
      if (run == 0) expected.push_back(std::bit_cast<uint64_t>(x));
      else EXPECT_EQ(expected[i], std::bit_cast<uint64_t>(x)) << run << " p=" << percents[i];
      double rank = (double)(std::lower_bound(values.begin(), values.end(), x) - values.begin())
          / (double)values.size();
      double q = percents[i] / 100.0;
      EXPECT_NEAR(q, rank, std::min(0.005, 0.002 + 3 * std::sqrt(q * (1 - q)) / 100.0));
    }
    EXPECT_DOUBLE_EQ(values.front(), req->scalar<double>("p0"));
    EXPECT_DOUBLE_EQ(values.back(), req->scalar<double>("p6"));
    EXPECT_EQ(0u, req->memoryTracker.bytes());
  }
}

TEST_F(AggregateExprTest, percentileReplayMatchesOrderedBucketDomains) {
  CollectionHelper helper;
  helper.index(flatdoc("id", "empty", "cat_s", "a"), UpdateMessage::COMMIT);
  indexPercentileData(helper, 6);
  auto guard = facetAggregateOverrides();
  forcedFacetSubOpInline = FacetSubOpInlineMode::SORT_KEY_ONLY;
  std::vector<std::byte> expected;
  for (auto feed : {FacetFeedStrategy::BUCKET_DOMAINS, FacetFeedStrategy::STRING_COLUMN_REPLAY}) {
    forcedFacetFeedStrategy = feed;
    auto req = localReq(helper.getSearchEngine());
    auto& facet = req->collection("main").facet("f", "cat_s").limit(-1);
    facet.expr("pct", "percentile(x_d,99.9)");
    facet.expr("spread", "percentile(x_d,99) - percentile(x_d,50)");
    facet.expr("mixed", "percentile(x_d,95) + cardinality(id)");
    req->execute(-1);
    ASSERT_TRUE(req->ok()) << req->errorMsg();
    std::vector<std::byte> encoded;
    ASSERT_TRUE(api::encode(*req->responses[0]->proto.ops.at("f")->facetResult(), encoded));
    if (feed == FacetFeedStrategy::BUCKET_DOMAINS) expected = encoded;
    else EXPECT_EQ(expected, encoded);
    EXPECT_EQ(0u, req->memoryTracker.bytes());
  }
}

TEST_F(AggregateExprTest, orderedDriverFoldsPermutedSegmentCompletions) {
  auto schema = Schema::createDefaultSchema();
  ArenaOwner memory;
  auto* program = parseAggregate(memory, *schema, "percentile(x_d,99.9)");
  std::array<int, 4> order{0,1,2,3};
  uint64_t expected = 0;
  do {
    RequestMemTracker tracker(0);
    RequestMemResource resource(tracker, "ordered percentile");
    int created = 0, completed = 0;
    size_t firstBytes = 0;
    {
      SegmentMergeDriver<AggregateAccumulator> driver(4, [&](auto result) {
        EXPECT_EQ(4, result->releaseCount);
        uint64_t bits = std::bit_cast<uint64_t>(result->finish().doubleValue);
        if (expected == 0) expected = bits;
        EXPECT_EQ(expected, bits);
        completed++;
      });
      driver.setCreator([&] { created++; return new AggregateAccumulator(*program, &resource); });
      driver.setOrdered(&resource);
      for (int segment : order) {
        driver.contribute((size_t)segment, [&](auto& accumulator) {
          EXPECT_FALSE(accumulator.finish().valid);
          for (int i = 0; i < 1200; i++) {
            accumulator.add(0, ValueResult::floating(std::exp((double)((i * 7 + segment) % 100) / 10)));
          }
        });
        EXPECT_EQ(order.back() == segment ? 1 : 0, completed);
        if (order == std::array<int,4>{0,1,2,3}) {
          if (segment == 0) firstBytes = resource.bytes();
          EXPECT_LE(resource.bytes(), firstBytes * 2);
        }
      }
      EXPECT_EQ(4, created);
    }
    EXPECT_EQ(0u, tracker.bytes());
  } while (std::next_permutation(order.begin(), order.end()));
}

TEST_F(AggregateExprTest, percentileReplayFailureReleasesSegmentAndRunningStates) {
  CollectionHelper helper;
  indexPercentileData(helper, 3);
  auto guard = facetAggregateOverrides();
  forcedFacetFeedStrategy = FacetFeedStrategy::STRING_COLUMN_REPLAY;
  for (size_t ceiling : {10000u, 20000u}) {
    forcedRequestMemoryMaxBytes = ceiling;
    auto req = localReq(helper.getSearchEngine());
    req->collection("main").facet("f", "cat_s").limit(-1).expr("pct", "percentile(x_d,95)");
    req->execute(false);
    EXPECT_FALSE(req->ok()) << ceiling;
    EXPECT_EQ(0u, req->memoryTracker.bytes());
  }
}

TEST_F(AggregateExprTest, orderedPendingDigestsReleaseOnAllocationFailure) {
  auto schema = Schema::createDefaultSchema();
  ArenaOwner memory;
  auto* program = parseAggregate(memory, *schema, "percentile(x_i,50)");
  RequestMemTracker tracker(1024 * 1024);
  RequestMemResource resource(tracker, "ordered percentile");
  {
    SegmentMergeDriver<AggregateAccumulator> driver(4, [](auto) { ADD_FAILURE(); });
    driver.setCreator([&] { return new AggregateAccumulator(*program, &resource); });
    driver.setOrdered(&resource);
    driver.contribute(3, [&](auto& accumulator) {
      for (int i = 0; i < 1200; i++) accumulator.add(0, ValueResult::integer(i));
    });
    size_t reserved = tracker.ceiling() - tracker.bytes();
    tracker.charge(reserved, "ordered percentile", "other live state");
    EXPECT_THROW(driver.contribute(2, [&](auto& accumulator) {
      accumulator.add(0, ValueResult::integer(1));
    }), ResourceExhaustedError);
    tracker.release(reserved);
    EXPECT_GT(tracker.bytes(), 0u);
  }
  EXPECT_EQ(0u, tracker.bytes());
}

TEST_F(AggregateExprTest, orderedMergeFailureReleasesFreshAndRunningDigests) {
  auto schema = Schema::createDefaultSchema();
  ArenaOwner memory;
  auto* program = parseAggregate(memory, *schema, "percentile(x_i,50,20)");
  RequestMemTracker tracker(16384);
  RequestMemResource resource(tracker, "ordered percentile");
  size_t reserved = 0;
  {
    SegmentMergeDriver<AggregateAccumulator> driver(2, [](auto) { ADD_FAILURE(); });
    driver.setCreator([&] { return new AggregateAccumulator(*program, &resource); });
    driver.setOrdered(&resource);
    driver.contribute(0, [&](auto& accumulator) { accumulator.add(0, ValueResult::integer(0)); });
    EXPECT_THROW(driver.contribute(1, [&](auto& accumulator) {
      for (int i = 0; i < 100; i++) accumulator.add(0, ValueResult::integer(i));
      reserved = tracker.ceiling() - tracker.bytes();
      tracker.charge(reserved, "ordered percentile", "other live state");
    }), ResourceExhaustedError);
    tracker.release(reserved);
  }
  EXPECT_EQ(0u, tracker.bytes());
}

TEST_F(AggregateExprTest, dataFailuresMergeAndFinishAsNull) {
  auto schema = Schema::createDefaultSchema();
  ArenaOwner memory;

  AggregateProgram* doubleSum =
      parseAggregate(memory, *schema, "sum(price_d)");

  AggregateProgram* intSum =
      parseAggregate(memory, *schema, "sum(price_i)");
  AggregateAccumulator batched(*intSum);
  std::array<ScalarValueResult, 4> integers{
      ScalarValueResult::integer(std::numeric_limits<int64_t>::max()),
      ScalarValueResult{},
      ScalarValueResult::integer(-std::numeric_limits<int64_t>::max()),
      ScalarValueResult::integer(1)};
  batched.addBatch(0, integers);
  ASSERT_TRUE(batched.finish().valid);
  EXPECT_EQ((__int128)1, batched.finish().intValue);

  AggregateAccumulator left(*doubleSum);
  AggregateAccumulator right(*doubleSum);
  left.add(0, ValueResult::floating(std::numeric_limits<double>::max()));
  right.add(0, ValueResult::floating(std::numeric_limits<double>::max()));
  EXPECT_NO_THROW(AggregateAccumulator::merge(&left, &right));
  BucketScalar nonFinite = left.finish();
  EXPECT_EQ(AggregateFailure::NON_FINITE_SUM, nonFinite.failure);

  AggregateAccumulator nonFiniteBatch(*doubleSum);
  std::array<ScalarValueResult, 2> floating{
      ScalarValueResult::floating(std::numeric_limits<double>::max()),
      ScalarValueResult::floating(std::numeric_limits<double>::max())};
  nonFiniteBatch.addBatch(0, floating);
  EXPECT_EQ(AggregateFailure::NON_FINITE_SUM,
            nonFiniteBatch.finish().failure);

  AggregateProgram* average =
      parseAggregate(memory, *schema, "avg(price_d)");
  AggregateAccumulator nonFiniteAverage(*average);
  nonFiniteAverage.add(
      0, ValueResult::floating(std::numeric_limits<double>::max()));
  nonFiniteAverage.add(
      0, ValueResult::floating(std::numeric_limits<double>::max()));
  EXPECT_EQ(AggregateFailure::NON_FINITE_AVERAGE,
            nonFiniteAverage.finish().failure);

  AggregateProgram* product = parseAggregate(
      memory, *schema, "sum(x_i) * sum(x_i)");
  AggregateAccumulator overflow(*product);
  for (uint32_t producer = 0; producer < product->producers.size(); producer++) {
    overflow.add(producer, ValueResult::integer(
        std::numeric_limits<int64_t>::max()));
    overflow.add(producer, ValueResult::integer(
        std::numeric_limits<int64_t>::max()));
  }
  BucketScalar tooWide = overflow.finish();
  EXPECT_EQ(AggregateFailure::INT128_OVERFLOW, tooWide.failure);

  AggregateProgram* countProgram =
      parseAggregate(memory, *schema, "sum(x_i)");
  AggregateAccumulator countOverflow(*countProgram);
  countOverflow.add(0, ValueResult::integer(0));
  for (int i = 0; i < 64; i++) {
    AggregateAccumulator::merge(&countOverflow, &countOverflow);
  }
  EXPECT_EQ(AggregateFailure::COUNT_OVERFLOW,
            countOverflow.finish().failure);

  AggregateProgram* floatingProduct = parseAggregate(
      memory, *schema, "sum(price_d) * sum(price_d)");
  AggregateAccumulator expressionOverflow(*floatingProduct);
  expressionOverflow.add(
      0, ValueResult::floating(std::numeric_limits<double>::max()));
  EXPECT_EQ(AggregateFailure::NON_FINITE_EXPRESSION,
            expressionOverflow.finish().failure);

  AggregateProgram* squareRoot =
      parseAggregate(memory, *schema, "sqrt(sum(x_i))");
  AggregateAccumulator domainFailure(*squareRoot);
  domainFailure.add(0, ValueResult::integer(-1));
  EXPECT_EQ(AggregateFailure::NON_FINITE_EXPRESSION,
            domainFailure.finish().failure);

  AggregateAccumulator valueFailure(*doubleSum);
  valueFailure.fail(AggregateFailure::VALUE_EVALUATION);
  EXPECT_EQ(AggregateFailure::VALUE_EVALUATION,
            valueFailure.finish().failure);

  api::Val output;
  writeAggregateValue(output, nonFinite);
  EXPECT_TRUE(output.isNull());
}

TEST_F(AggregateExprTest, runtimeWarningsAreThreadSafeAndDeduplicated) {
  auto req = localReq(luxirNode->getSearchEngine());
  std::string message = "aggregate op 'wide' emitted null: ";
  message += aggregateFailureReason(AggregateFailure::INT64_OUTPUT_OVERFLOW);
  std::array<std::thread, 8> writers;
  for (std::thread& writer : writers) {
    writer = std::thread([&] {
      req->warnOnce("aggregate_eval_failed", message);
    });
  }
  for (std::thread& writer : writers) writer.join();
  req->collection("main").topDocs("q").allQuery().limit(1);
  req->execute(false);

  ASSERT_TRUE(req->ok()) << req->errorMsg();
  ASSERT_EQ(1u, req->respWarnings().size());
  EXPECT_EQ("aggregate_eval_failed", req->respWarnings()[0].code);
  EXPECT_EQ(message, req->respWarnings()[0].message);
}

TEST_F(AggregateExprTest, exprOpExecutesOverRootDomain) {
  CollectionHelper helper;
  helper.index(flatdoc("id", "a", "price_i", 10), UpdateMessage::NO_COMMIT);
  helper.index(flatdoc("id", "b", "price_i", 20), UpdateMessage::COMMIT);

  auto req = localReq(helper.getSearchEngine());
  req->collection("main").expr(
      "revenue", "sum(price_i * $scale)", "scale", int64_t{2});
  req->execute(false);

  ASSERT_TRUE(req->ok()) << req->errorMsg();
  EXPECT_EQ(60, req->scalar<int64_t>("revenue"));
}

TEST_F(AggregateExprTest, columnNaNsAreMissingAndInfinitiesReachAggregates) {
  CollectionHelper helper;
  helper.index(flatdoc(
      "id", "a", "low_d", -std::numeric_limits<double>::infinity(),
      "nan_d", std::numeric_limits<double>::quiet_NaN(),
      "total_d", 1.0), UpdateMessage::NO_COMMIT);
  helper.index(flatdoc(
      "id", "b", "low_d", 5.0, "nan_d", 7.0,
      "total_d", std::numeric_limits<double>::infinity()),
      UpdateMessage::COMMIT);

  auto req = localReq(helper.getSearchEngine());
  auto& collection = req->collection("main");
  collection.expr("minimum", "min(low_d)");
  collection.expr("bucket_minimum", "min(min(low_d), 0)");
  collection.expr("without_nan", "min(nan_d)");
  collection.expr("total", "sum(total_d)");
  req->execute(false);

  ASSERT_TRUE(req->ok()) << req->errorMsg();
  EXPECT_EQ(-std::numeric_limits<double>::infinity(),
            req->scalar<double>("minimum"));
  EXPECT_EQ(-std::numeric_limits<double>::infinity(),
            req->scalar<double>("bucket_minimum"));
  EXPECT_DOUBLE_EQ(7.0, req->scalar<double>("without_nan"));
  const auto* total = req->responses[0]->proto.ops.find("total");
  ASSERT_NE(nullptr, total);
  EXPECT_TRUE((**total).isNull());
  ASSERT_EQ(1u, req->respWarnings().size());
  EXPECT_NE(req->respWarnings()[0].message.find("aggregate op 'total'"),
            std::string_view::npos);
  EXPECT_NE(req->respWarnings()[0].message.find("sum produced"),
            std::string_view::npos);
}

TEST_F(AggregateExprTest, stringFacetInlineSortsExprMetricByOpName) {
  CollectionHelper helper;
  helper.index(flatdoc("id", "1", "cat_s", "a", "price_i", 10,
                       "qty_i", 2), UpdateMessage::NO_COMMIT);
  helper.index(flatdoc("id", "2", "cat_s", "b", "price_i", 30,
                       "qty_i", 1), UpdateMessage::COMMIT);
  helper.index(flatdoc("id", "3", "cat_s", "a", "price_i", 20,
                       "qty_i", 1), UpdateMessage::NO_COMMIT);
  helper.index(flatdoc("id", "4", "cat_s", "c", "price_i", 99),
               UpdateMessage::COMMIT);

  auto req = localReq(helper.getSearchEngine());
  auto& facet = req->collection("main").facet("categories", "cat_s").limit(3);
  facet.expr("weighted", "sum(price_i * qty_i) / sum(qty_i)");
  qb::sort(facet, "weighted", qb::DESC);
  req->execute(false);

  ASSERT_TRUE(req->ok()) << req->errorMsg();
  const auto* result = req->responses[0]->proto.ops.at("categories")->facetResult();
  ASSERT_NE(nullptr, result);
  const auto& ids = std::get<api::ColStr>(result->bucket_ids->kind).v;
  ASSERT_EQ(3u, ids.size());
  EXPECT_EQ("b", ids[0]);
  EXPECT_EQ("a", ids[1]);
  EXPECT_EQ("c", ids[2]);
  const auto& values = std::get<api::ArrVal>(result->ops.at("weighted")->kind).v;
  ASSERT_EQ(3u, values.size());
  EXPECT_DOUBLE_EQ(30.0, values[0].asDouble());
  EXPECT_DOUBLE_EQ(40.0 / 3.0, values[1].asDouble());
  EXPECT_TRUE(values[2].isNull());
  EXPECT_EQ(0u, req->memoryTracker.bytes());

  auto asc = localReq(helper.getSearchEngine());
  auto& ascFacet = asc->collection("main")
      .facet("categories", "cat_s").limit(3);
  ascFacet.expr("weighted", "sum(price_i * qty_i) / sum(qty_i)");
  qb::sort(ascFacet, "weighted", qb::ASC);
  asc->execute(false);
  ASSERT_TRUE(asc->ok()) << asc->errorMsg();
  const auto* ascResult =
      asc->responses[0]->proto.ops.at("categories")->facetResult();
  ASSERT_NE(nullptr, ascResult);
  const auto& ascIds = std::get<api::ColStr>(ascResult->bucket_ids->kind).v;
  ASSERT_EQ(3u, ascIds.size());
  EXPECT_EQ("a", ascIds[0]);
  EXPECT_EQ("b", ascIds[1]);
  EXPECT_EQ("c", ascIds[2]);
}

TEST_F(AggregateExprTest, stringFacetInlineEmitsNullAndWarningOnFailure) {
  CollectionHelper helper;
  helper.index(flatdoc("id", "1", "cat_s", "bad", "x_i", -1),
               UpdateMessage::NO_COMMIT);
  helper.index(flatdoc("id", "2", "cat_s", "ok", "x_i", 4),
               UpdateMessage::COMMIT);

  auto guard = facetAggregateOverrides();
  forcedFacetSubOpInline = FacetSubOpInlineMode::ALL;
  auto req = localReq(helper.getSearchEngine());
  auto& facet = req->collection("main").facet("f", "cat_s").limit(2);
  facet.expr("root", "sqrt(sum(x_i))");
  facet.expr("absent", "sum(absent_i)");
  req->execute(false);

  ASSERT_TRUE(req->ok()) << req->errorMsg();
  const auto* result = req->responses[0]->proto.ops.at("f")->facetResult();
  ASSERT_NE(nullptr, result);
  const auto& roots = std::get<api::ArrVal>(result->ops.at("root")->kind).v;
  const auto& missing = std::get<api::ArrVal>(result->ops.at("absent")->kind).v;
  ASSERT_EQ(2u, roots.size());
  EXPECT_TRUE(roots[0].isNull());
  EXPECT_DOUBLE_EQ(2.0, roots[1].asDouble());
  EXPECT_TRUE(missing[0].isNull());
  EXPECT_TRUE(missing[1].isNull());
  ASSERT_EQ(1u, req->respWarnings().size());
  EXPECT_EQ("aggregate_eval_failed", req->respWarnings()[0].code);
  EXPECT_NE(std::string(req->respWarnings()[0].message).find("aggregate op 'root'"),
            std::string::npos);
}

TEST_F(AggregateExprTest,
       denseAndSparseInlineEntriesMatchOnRemappedMultiValueSegments) {
  CollectionHelper helper;
  ASSERT_TRUE(helper.indexAll({
      flatdoc("id", "1", "cat_ss", vecs("a", "c"), "x_i", 1),
      flatdoc("id", "2", "cat_ss", vecs("b"), "x_i", 10),
  }, UpdateMessage::COMMIT).success);
  ASSERT_TRUE(helper.indexAll({
      flatdoc("id", "3", "x_i", 100),
  }, UpdateMessage::COMMIT).success);
  ASSERT_TRUE(helper.indexAll({
      flatdoc("id", "4", "cat_ss", vecs("b", "c"), "x_i", 20),
      flatdoc("id", "5", "cat_ss", vecs("d"), "x_i", 5),
  }, UpdateMessage::COMMIT).success);

  auto guard = facetAggregateOverrides();
  forcedFacetSubOpInline = FacetSubOpInlineMode::ALL;
  auto run = [&](InlineFacetEntryMode mode) {
    forcedInlineFacetEntryMode = mode;
    auto req = localReq(helper.getSearchEngine());
    auto& facet = req->collection("main").facet("f", "cat_ss").limit(4);
    facet.expr("metric", "avg(x_i)");
    qb::sort(facet, "metric", qb::DESC);
    req->execute(false);
    EXPECT_TRUE(req->ok()) << req->errorMsg();
    EXPECT_EQ(0u, req->memoryTracker.bytes());
    std::vector<std::byte> encoded;
    const auto* result = req->responses[0]->proto.ops.at("f")->facetResult();
    EXPECT_NE(nullptr, result);
    if (result != nullptr) {
      EXPECT_TRUE(api::encode(*result, encoded));
    }
    return encoded;
  };

  EXPECT_EQ(run(InlineFacetEntryMode::FORCE_DENSE),
            run(InlineFacetEntryMode::FORCE_SPARSE));
}

TEST_F(AggregateExprTest, deniedDenseInlineEntriesFallBackToSparse) {
  CollectionHelper helper;
  std::vector<Doc> docs;
  for (int32_t i = 0; i < 100; i++) {
    docs.push_back(flatdoc(
        "id", std::to_string(i), "cat_s", "bucket-" + std::to_string(i),
        "pick_s", i == 0 ? "yes" : "no", "x_i", i + 1));
  }
  ASSERT_TRUE(helper.indexAll(docs, UpdateMessage::COMMIT).success);

  InlineFacetEntryStats stats;
  auto guard = facetAggregateOverrides();
  forcedFacetSubOpInline = FacetSubOpInlineMode::ALL;
  forcedInlineFacetEntryMode = InlineFacetEntryMode::FORCE_DENSE;
  inlineFacetEntryStatsForTests = &stats;
  forcedRequestMemoryMaxBytes = 512;
  auto req = localReq(helper.getSearchEngine());
  auto& top = req->topDocs("q");
  top.countLimit().matchQuery("pick_s", "yes");
  auto& facet = top.facet("f", "cat_s").limit(1);
  facet.expr("metric", "sum(x_i)");
  qb::sort(facet, "metric", qb::DESC);
  req->execute(false);

  ASSERT_TRUE(req->ok()) << req->errorMsg();
  EXPECT_EQ(0u, stats.denseTables.load());
  EXPECT_EQ(1u, stats.sparseTables.load());
  EXPECT_EQ(1u, stats.denseFallbacks.load());
  const auto* result = req->docList("q")->ops.at("f")->facetResult();
  ASSERT_NE(nullptr, result);
  ASSERT_EQ(1u, result->counts.size());
  EXPECT_EQ(1, result->counts[0]);
  const auto& values =
      std::get<api::ArrVal>(result->ops.at("metric")->kind).v;
  ASSERT_EQ(1u, values.size());
  EXPECT_EQ(1, values[0].asInt());
  EXPECT_EQ(0u, req->memoryTracker.bytes());
}

TEST_F(AggregateExprTest, denseInlineMergePropagatesAggregateFailure) {
  CollectionHelper helper;
  constexpr int32_t SEGMENTS = 8;
  constexpr int32_t DOCS_PER_SEGMENT = 1000;
  for (int32_t segment = 0; segment < SEGMENTS; segment++) {
    std::vector<Doc> docs;
    docs.reserve(DOCS_PER_SEGMENT);
    for (int32_t doc = 0; doc < DOCS_PER_SEGMENT; doc++) {
      docs.push_back(flatdoc(
          "id", std::to_string(segment * DOCS_PER_SEGMENT + doc),
          "cat_s", "a", "x_i", segment == 3 ? -1 : 4));
    }
    ASSERT_TRUE(helper.indexAll(docs, UpdateMessage::COMMIT).success);
  }

  InlineFacetEntryStats stats;
  auto guard = facetAggregateOverrides();
  forcedFacetSubOpInline = FacetSubOpInlineMode::ALL;
  forcedInlineFacetEntryMode = InlineFacetEntryMode::FORCE_DENSE;
  inlineFacetEntryStatsForTests = &stats;
  auto req = localReq(helper.getSearchEngine());
  auto& facet = req->collection("main").facet("f", "cat_s").limit(1);
  facet.expr("metric", "sum(sqrt(x_i))");
  qb::sort(facet, "metric", qb::DESC);
  req->execute(true);

  ASSERT_TRUE(req->ok()) << req->errorMsg();
  EXPECT_GT(stats.denseMerges.load(), 0u);
  const auto* result = req->responses[0]->proto.ops.at("f")->facetResult();
  const auto& values =
      std::get<api::ArrVal>(result->ops.at("metric")->kind).v;
  ASSERT_EQ(1u, values.size());
  EXPECT_TRUE(values[0].isNull());
  ASSERT_EQ(1u, req->respWarnings().size());
  EXPECT_EQ("aggregate_eval_failed", req->respWarnings()[0].code);
}

TEST_F(AggregateExprTest, budgetedInlineFacetTableMergesSparseIntoDense) {
  RequestMemTracker tracker(1200);
  InlineFacetEntryStats stats;
  {
    FacetEntryTable dense;
    FacetEntryTable sparse;
    std::span<SearchOp::InlineCalculator*> calculators;
    dense.configure(calculators, tracker, 100, "dense", &stats);
    sparse.configure(calculators, tracker, 100, "sparse", &stats);
    dense.initialize(1, true, false);
    EXPECT_EQ(100u * sizeof(int64_t) + sizeof(int64_t), tracker.bytes());
    sparse.initialize(1, true, false);

    EXPECT_EQ(1u, stats.denseTables.load());
    EXPECT_EQ(1u, stats.sparseTables.load());
    EXPECT_EQ(1u, stats.denseFallbacks.load());
    dense.add(7, 0);
    sparse.add(7, 1);
    sparse.add(9, 2);
    dense.merge(sparse);

    EXPECT_EQ(1u, stats.mixedMerges.load());
    ASSERT_EQ(2u, dense.size());
    int64_t count7 = -1;
    int64_t count9 = -1;
    dense.forEachEntry([&](int64_t ord, char* entry) {
      if (ord == 7) count7 = loadUnaligned<int64_t>(entry);
      if (ord == 9) count9 = loadUnaligned<int64_t>(entry);
    });
    EXPECT_EQ(2, count7);
    EXPECT_EQ(1, count9);
    EXPECT_GT(tracker.bytes(), 0u);
  }
  EXPECT_EQ(0u, tracker.bytes());
}

TEST_F(AggregateExprTest, inlineMincountZeroRemainsTouchedOnly) {
  CollectionHelper helper;
  ASSERT_TRUE(helper.indexAll({
      flatdoc("id", "1", "cat_s", "x", "pick_s", "yes", "x_i", 3),
      flatdoc("id", "2", "cat_s", "y", "pick_s", "no", "x_i", 5),
      flatdoc("id", "3", "cat_s", "z", "pick_s", "no", "x_i", 7),
  }, UpdateMessage::COMMIT).success);

  auto guard = facetAggregateOverrides();
  forcedFacetSubOpInline = FacetSubOpInlineMode::ALL;
  forcedInlineFacetEntryMode = InlineFacetEntryMode::FORCE_DENSE;
  auto req = localReq(helper.getSearchEngine());
  auto& top = req->topDocs("q");
  top.countLimit().matchQuery("pick_s", "yes");
  auto& facet = top.facet("f", "cat_s").limit(-1).mincount(0);
  facet.expr("metric", "sum(x_i)");
  qb::sort(facet, "metric", qb::DESC);
  req->execute(false);

  ASSERT_TRUE(req->ok()) << req->errorMsg();
  const auto* result = req->docList("q")->ops.at("f")->facetResult();
  const auto& ids = std::get<api::ColStr>(result->bucket_ids->kind).v;
  ASSERT_EQ(1u, ids.size());
  EXPECT_EQ("x", ids[0]);
  EXPECT_EQ(1, result->counts[0]);
}

TEST_F(AggregateExprTest, selectedStringBucketDomainsUseOrdinaryCalculator) {
  CollectionHelper helper;
  helper.index(flatdoc("id", "1", "cat_s", "a", "x_i", 10),
               UpdateMessage::NO_COMMIT);
  helper.index(flatdoc("id", "2", "cat_s", "a", "x_i", 20),
               UpdateMessage::COMMIT);
  helper.index(flatdoc("id", "3", "cat_s", "b", "x_i", 30),
               UpdateMessage::NO_COMMIT);
  helper.index(flatdoc("id", "4", "cat_s", "c", "x_i", 100),
               UpdateMessage::COMMIT);

  auto guard = facetAggregateOverrides();
  forcedFacetSubOpInline = FacetSubOpInlineMode::SORT_KEY_ONLY;
  forcedFacetFeedStrategy = FacetFeedStrategy::BUCKET_DOMAINS;
  auto req = localReq(helper.getSearchEngine());
  auto& facet = req->collection("main").facet("f", "cat_s").limit(2);
  facet.expr("metric", "avg(x_i)");
  req->execute(false);

  ASSERT_TRUE(req->ok()) << req->errorMsg();
  const auto* result = req->responses[0]->proto.ops.at("f")->facetResult();
  ASSERT_NE(nullptr, result);
  const auto& ids = std::get<api::ColStr>(result->bucket_ids->kind).v;
  const auto& values = std::get<api::ArrVal>(result->ops.at("metric")->kind).v;
  ASSERT_EQ(2u, ids.size());
  EXPECT_EQ("a", ids[0]);
  EXPECT_EQ("b", ids[1]);
  EXPECT_DOUBLE_EQ(15.0, values[0].asDouble());
  EXPECT_DOUBLE_EQ(30.0, values[1].asDouble());
}

TEST_F(AggregateExprTest, forcedStringReplayMatchesBucketDomains) {
  CollectionHelper helper;
  helper.index(flatdoc("id", "1", "parent_ss", vecs("a", "b"),
                       "x_i", 10, "qty_i", 2), UpdateMessage::NO_COMMIT);
  helper.index(flatdoc("id", "2", "parent_ss", vecs("a"),
                       "x_i", 20, "qty_i", 1), UpdateMessage::COMMIT);
  helper.index(flatdoc("id", "3", "parent_ss", vecs("b", "c"),
                       "x_i", 30, "qty_i", 3), UpdateMessage::NO_COMMIT);
  helper.index(flatdoc("id", "4", "parent_ss", vecs("c"),
                       "x_i", 40, "qty_i", 4), UpdateMessage::COMMIT);

  auto guard = facetAggregateOverrides();
  forcedFacetSubOpInline = FacetSubOpInlineMode::SORT_KEY_ONLY;
  auto run = [&](FacetFeedStrategy feed) {
    forcedFacetFeedStrategy = feed;
    auto req = localReq(helper.getSearchEngine());
    auto& facet = req->collection("main").facet("f", "parent_ss").limit(2);
    facet.expr("ratio", "sum(x_i) / sum(qty_i)");
    req->execute(false);
    EXPECT_TRUE(req->ok()) << req->errorMsg();
    EXPECT_EQ(0u, req->memoryTracker.bytes());
    std::vector<std::byte> encoded;
    const auto* result = req->responses[0]->proto.ops.at("f")->facetResult();
    EXPECT_NE(nullptr, result);
    if (result != nullptr) {
      EXPECT_TRUE(api::encode(*result, encoded));
    }
    return encoded;
  };

  auto domains = run(FacetFeedStrategy::BUCKET_DOMAINS);
  EXPECT_EQ(domains, run(FacetFeedStrategy::STRING_COLUMN_REPLAY));
}

TEST_F(AggregateExprTest, requestMemoryBreakerCoversInlineAndReplay) {
  CollectionHelper helper;
  helper.index(flatdoc("id", "1", "cat_s", "a", "x_i", 10),
               UpdateMessage::NO_COMMIT);
  helper.index(flatdoc("id", "2", "cat_s", "b", "x_i", 20),
               UpdateMessage::COMMIT);

  auto guard = facetAggregateOverrides();
  forcedRequestMemoryMaxBytes = 1;
  auto run = [&](bool replay) {
    forcedFacetSubOpInline = replay ? FacetSubOpInlineMode::SORT_KEY_ONLY
                                    : FacetSubOpInlineMode::ALL;
    forcedFacetFeedStrategy = replay
        ? FacetFeedStrategy::STRING_COLUMN_REPLAY
        : FacetFeedStrategy::BUCKET_DOMAINS;
    auto req = localReq(helper.getSearchEngine());
    auto& facet = req->collection("main").facet("limited", "cat_s").limit(2);
    facet.expr("metric", "sum(x_i)");
    if (!replay) qb::sort(facet, "metric", qb::DESC);
    req->execute(false);
    EXPECT_FALSE(req->ok());
    EXPECT_NE(req->errorMsg().find(
                  "request memory breaker 'facet aggregate state'"),
              std::string::npos);
    EXPECT_NE(req->errorMsg().find("facet 'limited' metric 'metric'"),
              std::string::npos);
    EXPECT_NE(req->errorMsg().find("attempted total"), std::string::npos);
    EXPECT_NE(req->errorMsg().find("ceiling of 1 bytes"),
              std::string::npos);
  };
  run(false);
  run(true);
}

TEST_F(AggregateExprTest, inlineFacetUsesPackedTableStrideAndReservation) {
  constexpr int32_t BUCKETS = 5000;
  CollectionHelper helper;
  std::vector<Doc> docs;
  docs.reserve(BUCKETS);
  for (int32_t i = 0; i < BUCKETS; i++) {
    docs.push_back(flatdoc("id", std::to_string(i),
                           "cat_s", "bucket-" + std::to_string(i),
                           "metric_d", (double)i));
  }
  ASSERT_TRUE(helper.indexAll(docs, UpdateMessage::COMMIT).success);

  ArenaOwner memory;
  auto schema = helper.collection().getSchema();
  AggregateProgram* program =
      parseAggregate(memory, *schema, "min(metric_d)");
  constexpr size_t EXPECTED_STRIDE = 10;
  ASSERT_EQ(EXPECTED_STRIDE, AggregateStateView::bytes(*program));

  InlineFacetEntryStats stats;
  auto guard = facetAggregateOverrides();
  forcedFacetSubOpInline = FacetSubOpInlineMode::ALL;
  forcedRequestMemoryMaxBytes = 256 * 1024;
  inlineFacetEntryStatsForTests = &stats;
  auto req = localReq(helper.getSearchEngine());
  auto& facet = req->collection("main").facet("f", "cat_s").limit(1);
  facet.expr("metric", "min(metric_d)");
  qb::sort(facet, "metric", qb::DESC);
  req->execute(false);

  ASSERT_TRUE(req->ok()) << req->errorMsg();
  EXPECT_EQ(sizeof(int64_t) + EXPECTED_STRIDE, stats.entryStride.load());
  EXPECT_EQ(1u, stats.denseTables.load());
  EXPECT_EQ(0u, req->memoryTracker.bytes());
}

TEST_F(AggregateExprTest, denseIntAvgInlineUsesFacetCountState) {
  CollectionHelper helper;
  ASSERT_TRUE(helper.indexAll({
      flatdoc("id", "1", "cat_s", "a", "metric_i", 10),
      flatdoc("id", "2", "cat_s", "a", "metric_i", 20),
      flatdoc("id", "3", "cat_s", "b", "metric_i", 100),
  }, UpdateMessage::COMMIT).success);

  InlineAggregateStats stats;
  auto guard = facetAggregateOverrides();
  inlineAggregateStatsForTests = &stats;
  forcedFacetSubOpInline = FacetSubOpInlineMode::ALL;
  forcedInlineFacetEntryMode = InlineFacetEntryMode::FORCE_DENSE;
  auto req = localReq(helper.getSearchEngine());
  auto& facet = req->collection("main").facet("f", "cat_s").limit(2);
  facet.expr("metric", "avg(metric_i)");
  qb::sort(facet, "metric", qb::DESC);
  req->execute(false);

  ASSERT_TRUE(req->ok()) << req->errorMsg();
  const auto* result = req->responses[0]->proto.ops.at("f")->facetResult();
  const auto& ids = std::get<api::ColStr>(result->bucket_ids->kind).v;
  const auto& values =
      std::get<api::ArrVal>(result->ops.at("metric")->kind).v;
  ASSERT_EQ(2u, ids.size());
  EXPECT_EQ("b", ids[0]);
  EXPECT_EQ("a", ids[1]);
  EXPECT_DOUBLE_EQ(100.0, values[0].asDouble());
  EXPECT_DOUBLE_EQ(15.0, values[1].asDouble());
  EXPECT_EQ(17u, stats.stateBytesPerBucket.load());
  EXPECT_EQ(0u, stats.finalizedBytes.load());
  EXPECT_EQ(24u, stats.peakFinalizedBytes.load());
}

TEST_F(AggregateExprTest, missingIntColumnKeepsExplicitAvgCount) {
  CollectionHelper helper;
  ASSERT_TRUE(helper.indexAll({
      flatdoc("id", "1", "cat_s", "a", "metric_i", 10),
      flatdoc("id", "2", "cat_s", "a"),
  }, UpdateMessage::COMMIT).success);

  InlineAggregateStats stats;
  auto guard = facetAggregateOverrides();
  inlineAggregateStatsForTests = &stats;
  forcedFacetSubOpInline = FacetSubOpInlineMode::ALL;
  auto req = localReq(helper.getSearchEngine());
  auto& facet = req->collection("main").facet("f", "cat_s").limit(1);
  facet.expr("metric", "avg(metric_i)");
  qb::sort(facet, "metric", qb::DESC);
  req->execute(false);

  ASSERT_TRUE(req->ok()) << req->errorMsg();
  const auto* result = req->responses[0]->proto.ops.at("f")->facetResult();
  const auto& values =
      std::get<api::ArrVal>(result->ops.at("metric")->kind).v;
  ASSERT_EQ(1u, values.size());
  EXPECT_DOUBLE_EQ(10.0, values[0].asDouble());
  EXPECT_EQ(26u, stats.stateBytesPerBucket.load());
}

TEST_F(AggregateExprTest, defaultBudgetCoversHighCardinalityParallelExtremes) {
  constexpr size_t BUCKETS = 278741;
  constexpr size_t STRIDE = 10;
  constexpr size_t LIVE_PIECES = 32 + 1;
  constexpr size_t CHUNK_BYTES = 64 * 1024;
  constexpr size_t REQUIRED =
      BUCKETS * STRIDE * LIVE_PIECES + LIVE_PIECES * CHUNK_BYTES;
  static_assert(REQUIRED == 94147218);
  // The breaker defaults to 0 (track, never reject); the documented sizing
  // guidance for operators enabling it must still cover this workload.
  EXPECT_EQ(0u, SearchConfig{}.request_memory_max_bytes);
  EXPECT_GE(160ULL * 1024 * 1024, REQUIRED);

  constexpr size_t STATE_BYTES = BUCKETS * STRIDE;
  constexpr size_t RESERVED_PER_PIECE =
      ((STATE_BYTES + CHUNK_BYTES - 1) / CHUNK_BYTES) * CHUNK_BYTES;
  SearchOverridesGuard guard(forcedRequestMemoryMaxBytes);
  forcedRequestMemoryMaxBytes = 0;
  auto req = localReq(luxirNode->getSearchEngine());
  std::latch reserved(LIVE_PIECES);
  std::latch release(1);
  std::array<std::exception_ptr, LIVE_PIECES> errors;
  std::array<std::thread, LIVE_PIECES> workers;
  for (size_t i = 0; i < workers.size(); i++) {
    workers[i] = std::thread([&, i] {
      size_t bytes = 0;
      try {
        bytes = req->memoryTracker.chargeUpTo(
            RESERVED_PER_PIECE, RESERVED_PER_PIECE,
            "facet aggregate state", "facet 'f' metric 'minimum'");
      } catch (...) {
        errors[i] = std::current_exception();
      }
      reserved.count_down();
      release.wait();
      if (bytes != 0) req->memoryTracker.release(bytes);
    });
  }
  reserved.wait();
  EXPECT_EQ(RESERVED_PER_PIECE * LIVE_PIECES,
            req->memoryTracker.bytes());
  release.count_down();
  for (std::thread& worker : workers) worker.join();
  for (const std::exception_ptr& error : errors) {
    if (error == nullptr) continue;
    try {
      std::rethrow_exception(error);
    } catch (const std::exception& exception) {
      ADD_FAILURE() << exception.what();
    }
  }
  EXPECT_EQ(0u, req->memoryTracker.bytes());
}

TEST_F(AggregateExprTest, requestMemoryBreakerComesFromNodeConfig) {
  LuxirConfig config;
  config.search.request_memory_max_bytes = 1;
  LuxirNode node(config);
  CollectionHelper helper(node);
  helper.index(flatdoc("id", "1", "cat_s", "a", "x_i", 10),
               UpdateMessage::COMMIT);

  auto guard = facetAggregateOverrides();
  forcedRequestMemoryMaxBytes = 0;
  forcedFacetSubOpInline = FacetSubOpInlineMode::ALL;
  auto req = localReq(helper.getSearchEngine());
  auto& facet = req->collection("main").facet("configured", "cat_s").limit(1);
  facet.expr("metric", "sum(x_i)");
  req->execute(false);

  EXPECT_FALSE(req->ok());
  EXPECT_NE(req->errorMsg().find(
                "request memory breaker 'facet aggregate state'"),
            std::string::npos);
  EXPECT_NE(req->errorMsg().find("facet 'configured' metric 'metric'"),
            std::string::npos);
  EXPECT_NE(req->errorMsg().find("ceiling of 1 bytes"),
            std::string::npos);
}

TEST_F(AggregateExprTest, rangeFacetFeedsExprChildrenInBindingBlocks) {
  CollectionHelper helper;
  for (int64_t i = 0; i < 70; i++) {
    helper.index(flatdoc("id", std::to_string(i), "bucket_i", i,
                         "x_i", i + 1),
                 i == 34 || i == 69 ? UpdateMessage::COMMIT
                                    : UpdateMessage::NO_COMMIT);
  }

  size_t blocks = 0;
  auto guard = facetAggregateOverrides();
  forcedRangeFacetBucketDomainByteBudget = 1;
  forcedRangeFacetBindingStateChunkBytes = 64 * 1024;
  facetBucketBlockCounter = &blocks;
  auto run = [&](std::string_view expression, int64_t multiplier) {
    auto req = localReq(helper.getSearchEngine());
    auto& facet = req->collection("main").rangeFacet("ranges", "bucket_i")
        .range(0, 70, 1).mincount(0);
    facet.expr("metric", expression);
    req->execute(false);

    EXPECT_TRUE(req->ok()) << req->errorMsg();
    const auto* result =
        req->responses[0]->proto.ops.at("ranges")->facetResult();
    EXPECT_NE(nullptr, result);
    if (result != nullptr) {
      const auto& values =
          std::get<api::ArrVal>(result->ops.at("metric")->kind).v;
      EXPECT_EQ(70u, values.size());
      for (size_t i = 0; i < values.size(); i++) {
        EXPECT_EQ(((int64_t)i + 1) * multiplier, values[i].asInt());
      }
    }
  };

  run("sum(x_i)", 1);
  EXPECT_EQ(1u, blocks);

  std::string heavyExpression;
  for (int i = 0; i < 100; i++) {
    if (!heavyExpression.empty()) heavyExpression += "+";
    heavyExpression += "sum(x_i)";
  }
  blocks = 0;
  run(heavyExpression, 100);
  EXPECT_EQ(1u, blocks);
  disableAggregateSharingForTests = true;
  blocks = 0;
  run(heavyExpression, 100);
  EXPECT_GT(blocks, 1u);
}

TEST_F(AggregateExprTest, denseTableHugePageBoundaryAndReuse) {
  RequestMemTracker tracker(0);
  for (size_t bytes : {bigBufferFloor - 8, bigBufferFloor, bigBufferFloor + 8}) {
    for (int reuse = 0; reuse < 2; ++reuse) {
      {
        FacetEntryTable table;
        int64_t keys = (int64_t)(bytes / sizeof(int64_t));
        table.configure({}, tracker, keys, "dense boundary", nullptr);
        table.initialize(3, true, false);
        EXPECT_EQ(tracker.bytes(), bigBufferStorage(bytes) + 3 * sizeof(int64_t));
        for (int64_t key : {int64_t{0}, keys / 2, keys - 1}) {
          auto* entry = table.resolveEntry(key);
          EXPECT_EQ(loadUnaligned<int64_t>(entry), 0);
          table.touchEntry(entry, 0);
          table.touchEntry(entry, 1);
          EXPECT_EQ(loadUnaligned<int64_t>(entry), 2);
        }
      }
      EXPECT_EQ(tracker.bytes(), 0);
    }
  }
}

TEST_F(AggregateExprTest, dateUnitsSurviveAggregateAndBucketResults) {
  CollectionHelper helper;
  SchemaBuilder builder;
  for (auto name : {"stamp", "absent"}) {
    auto& field = builder.field(name);
    field.type = api::FieldDef::FieldClass::DATE;
    field.unit = api::FieldDef::DateUnit::NS;
  }
  builder.set(helper.collection());
  ASSERT_TRUE(helper.indexAll({
      flatdoc("id", "a", "stamp", 1, "when_dt", 1, "cat_s", "a"),
      flatdoc("id", "b", "stamp", 4, "when_dt", 2, "cat_s", "a")},
      UpdateMessage::COMMIT).success);
  auto schema = helper.collection().getSchema();
  auto reader = helper.getIndexWriter()->snapshots.readers.getReader();
  ArenaOwner memory;
  struct Example { const char* expression; double expected; bool floating; ValueNature nature; };
  for (const auto& example : {
      Example{"min(stamp)", 1, false, ValueNature::DATE_NS},
      {"max(stamp)", 4, false, ValueNature::DATE_NS},
      {"avg(stamp)", 2.5, true, ValueNature::DATE_NS},
      {"percentile(stamp,50)", 2.5, true, ValueNature::DATE_NS},
      {"avg(stamp)+1", 3.5, true, ValueNature::DATE_NS},
      {"avg(stamp+1)", 3.5, true, ValueNature::DATE_NS},
      {"min(min(stamp),max(stamp))", 1, false, ValueNature::DATE_NS},
      {"max(min(stamp),max(stamp))", 4, false, ValueNature::DATE_NS},
      {"min(def(absent,stamp))", 1, false, ValueNature::DATE_NS},
      {"min(def(absent,7))", 7, false, ValueNature::DATE_NS},
      {"max(stamp)-min(stamp)", 3, false, ValueNature::NUMBER},
      {"avg(stamp)-min(stamp)", 1.5, true, ValueNature::NUMBER},
      {"min(stamp)-max(when_dt)", -1999999, false, ValueNature::NUMBER},
      {"max(when_dt)-max(stamp)", 1999996, false, ValueNature::NUMBER},
      {"avg(when_dt)-avg(stamp)", 1499997.5, true, ValueNature::NUMBER},
      {"avg(def(absent,when_dt))", 1500000, true, ValueNature::DATE_NS},
      {"avg(def(when_dt,stamp))", 1500000, true, ValueNature::DATE_NS},
      {"min(min(stamp,when_dt))", 1, false, ValueNature::DATE_NS},
      {"max(max(when_dt,stamp))", 2000000, false, ValueNature::DATE_NS},
      {"min(min(stamp),min(when_dt))", 1, false, ValueNature::DATE_NS},
      {"max(avg(when_dt),avg(stamp))", 1500000, true, ValueNature::DATE_NS}}) {
    SCOPED_TRACE(example.expression);
    auto* program = parseAggregate(memory, *schema, example.expression);
    EXPECT_EQ(example.nature, program->root().nature);
    BucketScalar result = evaluate(*program, *reader, reader->segments()[0]);
    ASSERT_TRUE(result.valid);
    EXPECT_EQ(example.nature, result.nature);
    EXPECT_EQ(example.floating ? BucketValueType::DOUBLE : BucketValueType::INT128, result.type);
    EXPECT_DOUBLE_EQ(example.expected, example.floating ? result.doubleValue : (double)result.intValue);
  }
  expectParseError(memory, *schema, "sum(stamp)", "cannot aggregate a DATE");
  expectParseError(memory, *schema, "max(stamp)+min(when_dt)", "cannot add two DATE");

  // The same native ns values must survive dense, keyed, replay and shared
  // aggregate execution, including a DOUBLE date with a fractional tick.
  auto guard = facetAggregateOverrides();
  for (auto feed : {FacetFeedStrategy::BUCKET_DOMAINS, FacetFeedStrategy::STRING_COLUMN_REPLAY}) {
    forcedFacetFeedStrategy = feed;
    for (bool separate : {false, true}) {
      disableAggregateSharingForTests = separate;
      disableDenseFacetStateForTests = separate;
      auto req = localReq(helper.getSearchEngine());
      auto& root = req->collection("main");
      root.expr("mid", "avg(stamp)");
      root.expr("pct", "percentile(stamp,50)");
      auto& facet = root.facet("f", "cat_s").limit(-1);
      facet.expr("mid", "avg(stamp)");
      facet.expr("lo", "min(stamp)");
      facet.expr("hi", "max(stamp)");
      req->execute();
      ASSERT_OK(req);
      EXPECT_DOUBLE_EQ(2.5, req->scalar<double>("mid"));
      EXPECT_DOUBLE_EQ(2.5, req->scalar<double>("pct"));
      auto& result = *req->responses[0]->proto.ops.at("f")->facetResult();
      EXPECT_DOUBLE_EQ(2.5, std::get<api::ArrVal>(result.ops.at("mid")->kind).v[0].asDouble());
      EXPECT_EQ(1, std::get<api::ArrVal>(result.ops.at("lo")->kind).v[0].asInt());
      EXPECT_EQ(4, std::get<api::ArrVal>(result.ops.at("hi")->kind).v[0].asInt());
    }
  }
}

TEST_F(AggregateExprTest, dateUnitIsPartOfProducerIdentityAndSameUnitTreesAreUnconverted) {
  ArenaOwner memory;
  auto base = Schema::createDefaultSchema();
  AggregateProgram* millis = nullptr;
  AggregateProgram combined(*memory.arena);
  for (bool nanos : {false, true}) {
    SchemaBuilder builder;
    auto& field = builder.field("stamp");
    field.type = api::FieldDef::FieldClass::DATE;
    field.unit = nanos ? api::FieldDef::DateUnit::NS : api::FieldDef::DateUnit::MS;
    auto schema = builder.build(base.get());
    auto* program = parseAggregate(memory, *schema, "max(stamp)-min(stamp)");
    ASSERT_EQ(3u, program->nodes.size());
    EXPECT_EQ(AggregateNodeKind::BINARY, program->root().kind);
    EXPECT_EQ(ValueFunctionRegistry::find("sub"), program->root().function);
    EXPECT_EQ((std::array<uint32_t, 2>{0, 1}), program->root().children);
    for (int i = 0; i < 2; i++) {
      EXPECT_EQ(AggregateNodeKind::AGGREGATE, program->nodes[i].kind);
      EXPECT_EQ(nanos ? ValueNature::DATE_NS : ValueNature::DATE_MS, program->nodes[i].nature);
      EXPECT_EQ(1u, program->producers[i].input->nodes.size());
    }
    EXPECT_EQ(ValueNature::NUMBER, program->root().nature);
    const auto& producer = program->producers[0];
    uint32_t index = combined.addProducer(producer);
    EXPECT_EQ(nanos ? 1u : 0u, index);
    EXPECT_EQ(index, combined.addProducer(producer));
    if (!nanos) millis = program;
    else {
      EXPECT_FALSE(producer.sameState(millis->producers[0]));
      // Even with matching output metadata, input identity includes the unit.
      AggregateProducer differentInput = producer;
      differentInput.resolved.nature = millis->producers[0].resolved.nature;
      EXPECT_FALSE(differentInput.sameState(millis->producers[0]));
      AggregateProducer differentOutput = millis->producers[0];
      differentOutput.resolved.nature = producer.resolved.nature;
      EXPECT_FALSE(differentOutput.sameState(millis->producers[0]));
    }
  }
}
