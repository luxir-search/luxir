#include <gtest/gtest.h>

#include <array>
#include <latch>
#include <limits>
#include <string>
#include <thread>
#include <vector>

#include "luxir/search/ops/ExprStatsOp.h"
#include "luxir/search/SearchOverrides.h"
#include "luxir/value/AggregateExprParser.h"
#include "test/CollectionHelper.h"
#include "test/LocalReq.h"
#include "test/LuxirTest.h"
#include "test/QueryBuild.h"
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
      inlineAggregateStatsForTests,
      disableDenseFacetStateForTests,
      enableInlineFacetEntryCache,
      forcedRangeFacetBucketDomainByteBudget,
      forcedRangeFacetBindingStateChunkBytes,
      rangeFacetBindingBlockCounter);
}

AggregateProgram* parseAggregate(ArenaOwner& memory, Schema& schema,
                                 std::string_view expression,
                                 std::string_view opName = "metric") {
  AggregateExprOptions options{&schema, {}, opName};
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

BucketScalar evaluate(AggregateProgram& program, IndexReader::Segment& segment) {
  MemPool pool;
  AggregateAccumulator accumulator(program);
  std::vector<BoundAggregateInput> bindings =
      bindAggregateInputs(program, pool, segment);
  int32_t maxDoc = segment.postingsReader().maxDoc();
  for (int32_t doc = 0; doc < maxDoc; doc++) {
    for (size_t leaf = 0; leaf < bindings.size(); leaf++) {
      accumulator.add((uint32_t)leaf, bindings[leaf].evalPoint(doc));
    }
  }
  return accumulator.finish();
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
  EXPECT_EQ(ValueOpcode::MUL, expression->leaves[0].input->root().opcode);

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
  EXPECT_EQ(ValueOpcode::AVG, nested->leaves[0].input->root().opcode);

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
            variables->leaves[0].input->nodes[
                variables->leaves[0].input->root().children[1]].kind);
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

  EXPECT_EQ(ValueNature::DATE,
            parseAggregate(memory, *schema, "avg(when_dt)")->root().nature);
  EXPECT_EQ(ValueNature::DATE,
            parseAggregate(memory, *schema, "min(when_dt)")->root().nature);
  EXPECT_EQ(ValueNature::DATE,
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

  auto reader = helper.getIndexWriter()->getIndexReader();
  ASSERT_EQ(1u, reader->segments().size());
  auto schema = helper.collection().getSchema();
  ArenaOwner memory;
  auto eval = [&](std::string_view expression) {
    return evaluate(*parseAggregate(memory, *schema, expression),
                    reader->segments()[0]);
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

  auto reader = helper.getIndexWriter()->getIndexReader();
  auto schema = helper.collection().getSchema();
  ArenaOwner memory;
  AggregateProgram* program = parseAggregate(memory, *schema, "sum(x_i)", "wide");
  BucketScalar value = evaluate(*program, reader->segments()[0]);
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
    EXPECT_EQ(1, function.minArguments) << function.name;
    EXPECT_EQ(1, function.maxArguments) << function.name;
    AggregateProgram* program = parseAggregate(
        memory, *schema, std::string(function.name) + "(price_i)");
    ASSERT_EQ(1u, program->leaves.size());
    const AggregateStateOps& state = program->leaves[0].resolved.state;
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
  for (uint32_t leaf = 0; leaf < 2; leaf++) {
    overflow.add(leaf, ValueResult::integer(
        std::numeric_limits<int64_t>::max()));
    overflow.add(leaf, ValueResult::integer(
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
  expressionOverflow.add(
      1, ValueResult::floating(std::numeric_limits<double>::max()));
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
  facet.expr("missing", "sum(absent_i)");
  req->execute(false);

  ASSERT_TRUE(req->ok()) << req->errorMsg();
  const auto* result = req->responses[0]->proto.ops.at("f")->facetResult();
  ASSERT_NE(nullptr, result);
  const auto& roots = std::get<api::ArrVal>(result->ops.at("root")->kind).v;
  const auto& missing = std::get<api::ArrVal>(result->ops.at("missing")->kind).v;
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

TEST_F(AggregateExprTest, inlineFacetUsesTrueStrideAndChunkedReservations) {
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

  std::atomic<size_t> reservations{0};
  auto guard = facetAggregateOverrides();
  forcedFacetSubOpInline = FacetSubOpInlineMode::ALL;
  forcedRequestMemoryMaxBytes = 160 * 1024;
  facetAggregateStateReservationCounterForTests = &reservations;
  auto req = localReq(helper.getSearchEngine());
  auto& facet = req->collection("main").facet("f", "cat_s").limit(1);
  facet.expr("metric", "min(metric_d)");
  qb::sort(facet, "metric", qb::DESC);
  req->execute(false);

  ASSERT_TRUE(req->ok()) << req->errorMsg();
  EXPECT_LE(reservations.load(), 3u);
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
  enableInlineFacetEntryCache = true;
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
  rangeFacetBindingBlockCounter = &blocks;
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
  EXPECT_GT(blocks, 1u);
}
