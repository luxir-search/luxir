#include <gtest/gtest.h>

#include <array>
#include <limits>
#include <string>
#include <thread>
#include <vector>

#include "luxir/search/ops/ExprStatsOp.h"
#include "luxir/value/AggregateExprParser.h"
#include "test/CollectionHelper.h"
#include "test/LocalReq.h"
#include "test/LuxirTest.h"
#include "test/TestUtils.h"

using namespace luxir;
using namespace luxir::test;

namespace {

class ArenaOwner {
public:
  google::protobuf::Arena* arena = createArena();
  ~ArenaOwner() { releaseArena(arena); }
};

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
  std::vector<BoundValueProgram*> bindings;
  for (const AggregateLeaf& leaf : program.leaves) {
    bindings.push_back(leaf.input->bind(pool, segment));
  }
  int32_t maxDoc = segment.postingsReader().maxDoc();
  for (int32_t doc = 0; doc < maxDoc; doc++) {
    for (size_t leaf = 0; leaf < bindings.size(); leaf++) {
      accumulator.add((uint32_t)leaf, bindings[leaf]->evalPoint(doc, 0.0f));
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
  EXPECT_EQ(ValueOpcode::ADD, expression->root().opcode);
  ASSERT_EQ(2u, expression->leaves.size());
  EXPECT_EQ(ValueOpcode::MUL, expression->leaves[0].input->root().opcode);

  AggregateProgram* functionForms = parseAggregate(
      memory, *schema, "add(sum(price_i), div(max(foo_i), 2))");
  EXPECT_EQ(ValueOpcode::ADD, functionForms->root().opcode);
  EXPECT_EQ(ValueOpcode::DIV,
            functionForms->nodes[functionForms->root().children[1]].opcode);
  EXPECT_EQ(ValueOpcode::ADD,
            parseAggregate(memory, *schema, "add(1, sum(price_i))")
                ->root().opcode);
  EXPECT_EQ(ValueOpcode::ABS,
            parseAggregate(memory, *schema, "abs(sum(price_i))")
                ->root().opcode);
  EXPECT_EQ(ValueOpcode::FLOOR,
            parseAggregate(memory, *schema,
                           "floor(sum(price_i) / sum(foo_i))")
                ->root().opcode);

  AggregateProgram* nested =
      parseAggregate(memory, *schema, "avg(avg(prices_is))");
  ASSERT_EQ(1u, nested->leaves.size());
  EXPECT_EQ(ValueOpcode::AVG, nested->leaves[0].input->root().opcode);

  expectParseError(memory, *schema, "avg(prices_is)",
                   "requires one scalar per document");
  expectParseError(memory, *schema, "avg(prices_is)",
                   "avg(...), min(...), or max(...)");
  expectParseError(memory, *schema, "price_i", "must be an aggregate call");
  expectParseError(memory, *schema, "sum()", "expects exactly 1 argument");
  expectParseError(memory, *schema, "sum(price_i + * 2)", "byte 14");
  expectParseError(memory, *schema, "min(sum(price_i), 2)",
                   "sum() is a bucket aggregate");
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

  api::Val rawWideOutput;
  writeAggregateValue(rawWideOutput, BucketScalar::integer(
      (__int128)std::numeric_limits<int64_t>::max() + 1));
  EXPECT_TRUE(rawWideOutput.isNull());
}

TEST_F(AggregateExprTest, registryEntriesHaveResolvedStateLifecycles) {
  auto schema = Schema::createDefaultSchema();
  ArenaOwner memory;
  for (const AggregateFunction& function : AggregateFunctionRegistry::entries()) {
    EXPECT_NE(nullptr, function.resolve) << function.name;
    EXPECT_TRUE(function.supports(FunctionCapability::BUCKET_AGGREGATE))
        << function.name;
    EXPECT_EQ(1, function.minArguments) << function.name;
    EXPECT_EQ(1, function.maxArguments) << function.name;
    AggregateProgram* program = parseAggregate(
        memory, *schema, std::string(function.name) + "(price_i)");
    ASSERT_EQ(1u, program->leaves.size());
    const AggregateStateOps& state = program->leaves[0].resolved.state;
    EXPECT_GT(state.bytes, 0u) << function.name;
    EXPECT_NE(nullptr, state.init) << function.name;
    EXPECT_NE(nullptr, state.accumulate) << function.name;
    EXPECT_NE(nullptr, state.merge) << function.name;
    EXPECT_NE(nullptr, state.finish) << function.name;
  }
}

TEST_F(AggregateExprTest, dataFailuresMergeAndFinishAsNull) {
  auto schema = Schema::createDefaultSchema();
  ArenaOwner memory;

  AggregateProgram* doubleSum =
      parseAggregate(memory, *schema, "sum(price_d)");
  AggregateAccumulator left(*doubleSum);
  AggregateAccumulator right(*doubleSum);
  left.add(0, ValueResult::floating(std::numeric_limits<double>::max()));
  right.add(0, ValueResult::floating(std::numeric_limits<double>::max()));
  EXPECT_NO_THROW(AggregateAccumulator::merge(&left, &right));
  BucketScalar nonFinite = left.finish();
  EXPECT_EQ(AggregateFailure::NON_FINITE_SUM, nonFinite.failure);

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
