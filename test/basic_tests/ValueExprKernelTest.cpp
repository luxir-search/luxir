#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <limits>
#include <string>
#include <vector>

#include "luxir/value/ValueExprParser.h"
#include "test/CollectionHelper.h"
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

ValueProgram* parseValue(ArenaOwner& memory, Schema& schema, std::string_view expression) {
  ValueExprOptions options{&schema, {}};
  return ValueExprParser(options, *memory.arena).parse(expression);
}

} // namespace

class ValueExprKernelTest : public LuxirTest {};

TEST_F(ValueExprKernelTest, pointBatchMissingPrecisionReducersAndBounds) {
  CollectionHelper helper;
  constexpr int64_t BIG = 9007199254740993LL;
  helper.index(flatdoc("id_s", "a", "x_i", BIG, "f_f", -1.5f,
                       "values_is", vec_i(-3, 5), "den_i", -1,
                       "when_dt", int64_t{1000}), UpdateMessage::NO_COMMIT);
  helper.index(flatdoc("id_s", "b", "f_f", 4.25f,
                       "values_is", vec_i(2, 8, 4), "den_i", 0,
                       "when_dt", int64_t{3000}), UpdateMessage::NO_COMMIT);
  helper.index(flatdoc("id_s", "c", "x_i", -5, "f_f", 0.0f,
                       "den_i", 1), UpdateMessage::COMMIT);

  auto reader = helper.getIndexWriter()->getIndexReader();
  auto& segment = reader->segments()[0];
  auto schema = helper.collection().getSchema();
  ArenaOwner memory;
  MemPool pool;

  ValueProgram* program = parseValue(memory, *schema, "add(def(x_i,7),2)");
  auto bound = program->bind(pool, segment);
  std::array<int64_t, 3> expected{BIG + 2, 9, -3};
  for (int32_t doc = 0; doc < 3; doc++) {
    ValueResult result = bound->evalPoint(doc, 0.0f);
    ASSERT_TRUE(result.valid);
    EXPECT_EQ(ValueType::INT64, result.type);
    EXPECT_EQ(expected[(size_t)doc], result.intValue);
  }
  const ValueBounds& rootBounds = bound->bounds(program->rootNode);
  EXPECT_EQ(BoundsCertainty::BOUNDED, rootBounds.certainty);
  EXPECT_TRUE(rootBounds.mayBeMissing == false);
  EXPECT_LE(rootBounds.intMin, -3);
  EXPECT_GE(rootBounds.intMax, BIG + 2);

  std::array<int32_t, 3> docs{0, 1, 2};
  std::array<ValueResult, 3> batch;
  bound->evalBatch(docs, {}, batch);
  for (size_t i = 0; i < batch.size(); i++) {
    EXPECT_TRUE(batch[i].valid);
    EXPECT_EQ(expected[i], batch[i].intValue);
  }
  for (auto [expression, expectedValue] : {
           std::pair<std::string_view, double>{"min(values_is)", -3.0},
           {"max(values_is)", 5.0}, {"avg(values_is)", 1.0}}) {
    ValueProgram* reducer = parseValue(memory, *schema, expression);
    auto reducerBound = reducer->bind(pool, segment);
    ValueResult result = reducerBound->evalPoint(0, 0.0f);
    ASSERT_TRUE(result.valid) << expression;
    double actual = result.type == ValueType::DOUBLE
        ? result.doubleValue : (double)result.intValue;
    EXPECT_DOUBLE_EQ(expectedValue, actual) << expression;
    EXPECT_FALSE(reducerBound->evalPoint(2, 0.0f).valid) << expression;
  }

  ValueProgram* floating = parseValue(memory, *schema, "add(f_f,2.0)");
  auto floatingBound = floating->bind(pool, segment);
  const ValueBounds& floatBounds = floatingBound->bounds(floating->rootNode);
  ASSERT_EQ(BoundsCertainty::BOUNDED, floatBounds.certainty);
  EXPECT_DOUBLE_EQ(0.5, floatBounds.doubleMin);
  EXPECT_DOUBLE_EQ(6.25, floatBounds.doubleMax);

  ValueProgram* date = parseValue(memory, *schema, "when_dt");
  auto dateBound = date->bind(pool, segment);
  const ValueBounds& dateBounds = dateBound->bounds(date->rootNode);
  EXPECT_EQ(1000, dateBounds.intMin);
  EXPECT_EQ(3000, dateBounds.intMax);
  EXPECT_TRUE(dateBounds.mayBeMissing);

  ValueProgram* absent = parseValue(memory, *schema, "absent_i");
  auto absentBound = absent->bind(pool, segment);
  const ValueBounds& absentBounds = absentBound->bounds(absent->rootNode);
  EXPECT_EQ(BoundsCertainty::UNBOUNDED, absentBounds.certainty);
  EXPECT_TRUE(absentBounds.alwaysMissing);
  EXPECT_FALSE(absentBound->evalPoint(0, 0.0f).valid);
}

TEST_F(ValueExprKernelTest, negativeValuesAndRuntimeFiniteGuard) {
  CollectionHelper helper;
  helper.index(flatdoc("id_s", "neg", "x_i", -9, "den_i", -1),
               UpdateMessage::NO_COMMIT);
  helper.index(flatdoc("id_s", "zero", "x_i", 0, "den_i", 0),
               UpdateMessage::NO_COMMIT);
  helper.index(flatdoc("id_s", "pos", "x_i", 4, "den_i", 1),
               UpdateMessage::COMMIT);
  auto reader = helper.getIndexWriter()->getIndexReader();
  auto& segment = reader->segments()[0];
  auto schema = helper.collection().getSchema();
  ArenaOwner memory;
  MemPool pool;

  ValueProgram* abs = parseValue(memory, *schema, "abs(x_i)");
  auto absBound = abs->bind(pool, segment);
  EXPECT_EQ(9, absBound->evalPoint(0, 0.0f).intValue);
  EXPECT_EQ(0, absBound->evalPoint(1, 0.0f).intValue);

  ValueProgram* division = parseValue(memory, *schema, "div(1,den_i)");
  auto divisionBound = division->bind(pool, segment);
  EXPECT_EQ(BoundsCertainty::UNBOUNDED,
            divisionBound->bounds(division->rootNode).certainty);
  EXPECT_EQ(-1, divisionBound->evalPoint(0, 0.0f).intValue);
  EXPECT_THROW(divisionBound->evalPoint(1, 0.0f), std::runtime_error);
  EXPECT_EQ(1, divisionBound->evalPoint(2, 0.0f).intValue);
}

TEST_F(ValueExprKernelTest, provenInvalidityIsRejectedAtBind) {
  CollectionHelper helper;
  helper.index(flatdoc("id_s", "zero", "x_i", 0), UpdateMessage::NO_COMMIT);
  helper.index(flatdoc("id_s", "one", "x_i", 1), UpdateMessage::COMMIT);
  auto reader = helper.getIndexWriter()->getIndexReader();
  auto& segment = reader->segments()[0];
  auto schema = helper.collection().getSchema();
  ArenaOwner memory;
  MemPool pool;

  ValueProgram* logarithm = parseValue(memory, *schema, "log(x_i)");
  try {
    logarithm->bind(pool, segment);
    FAIL() << "expected log bounds failure";
  } catch (const std::runtime_error& error) {
    std::string message = error.what();
    EXPECT_NE(message.find("x_i"), std::string::npos) << message;
    EXPECT_NE(message.find("max("), std::string::npos) << message;
    EXPECT_NE(message.find("log1p"), std::string::npos) << message;
  }
}

TEST_F(ValueExprKernelTest, scoreBoundsStayUnknownAndPointUsesSuppliedScore) {
  CollectionHelper helper;
  helper.index(flatdoc("id_s", "a", "x_i", 2), UpdateMessage::COMMIT);
  auto reader = helper.getIndexWriter()->getIndexReader();
  auto& segment = reader->segments()[0];
  auto schema = helper.collection().getSchema();
  ArenaOwner memory;
  MemPool pool;

  ValueProgram* program = parseValue(memory, *schema, "add(score,x_i)");
  auto bound = program->bind(pool, segment);
  EXPECT_EQ(BoundsCertainty::UNBOUNDED, bound->bounds(program->rootNode).certainty);
  ValueBounds scoreInterval = ValueBounds::floating(-2.0, 3.0);
  scoreInterval.minAttained = false;
  scoreInterval.maxAttained = false;
  const ValueBounds& composed = bound->boundsForScore(scoreInterval);
  ASSERT_EQ(BoundsCertainty::BOUNDED, composed.certainty);
  EXPECT_DOUBLE_EQ(0.0, composed.doubleMin);
  EXPECT_DOUBLE_EQ(5.0, composed.doubleMax);
  EXPECT_EQ(&composed, &bound->boundsForScore(scoreInterval));
  ValueResult result = bound->evalPoint(0, 3.5f);
  ASSERT_TRUE(result.valid);
  EXPECT_DOUBLE_EQ(5.5, result.doubleValue);

  ValueProgram* squareRoot = parseValue(memory, *schema, "sqrt(score)");
  auto squareRootBound = squareRoot->bind(pool, segment);
  EXPECT_EQ(BoundsCertainty::UNBOUNDED,
            squareRootBound->bounds(squareRoot->rootNode).certainty);
  ValueBounds nonNegativeScore = ValueBounds::floating(0.0, 4.0);
  nonNegativeScore.minAttained = false;
  nonNegativeScore.maxAttained = false;
  const ValueBounds& squareRootBounds =
      squareRootBound->boundsForScore(nonNegativeScore);
  ASSERT_EQ(BoundsCertainty::BOUNDED, squareRootBounds.certainty);
  EXPECT_DOUBLE_EQ(0.0, squareRootBounds.doubleMin);
  EXPECT_DOUBLE_EQ(2.0, squareRootBounds.doubleMax);
  ValueBounds negativeScore = ValueBounds::floating(-1.0, -1.0);
  EXPECT_THROW(squareRootBound->boundsForScore(negativeScore),
               std::runtime_error);
  EXPECT_THROW(squareRootBound->evalPoint(0, -1.0f), std::runtime_error);

  ValueProgram* overflowing = parseValue(memory, *schema, "mul(score,1e308)");
  auto overflowingBound = overflowing->bind(pool, segment);
  EXPECT_THROW(overflowingBound->evalPoint(0, 2.0f), std::runtime_error);
}

TEST_F(ValueExprKernelTest, boundsFlagsSeparateUnknownInvalidAndMissing) {
  const ValueFunction* logarithm = ValueFunctionRegistry::find("log");
  const ValueFunction* multiply = ValueFunctionRegistry::find("mul");
  ASSERT_NE(nullptr, logarithm);
  ASSERT_NE(nullptr, multiply);

  ValueNode logNode;
  logNode.kind = ValueNodeKind::FUNCTION;
  logNode.type = ValueType::DOUBLE;
  logNode.opcode = ValueOpcode::LOG;
  logNode.text = "log";
  std::array<ValueBounds, 1> logInput{ValueBounds::integer(0, 10)};
  ValueBounds invalid = logarithm->boundsPropagate(logNode, logInput);
  EXPECT_EQ(BoundsCertainty::INVALID, invalid.certainty);
  EXPECT_EQ(BoundsInvalidity::DOMAIN, invalid.invalidity);

  logInput[0].minAttained = false;
  ValueBounds unknown = logarithm->boundsPropagate(logNode, logInput);
  EXPECT_EQ(BoundsCertainty::UNBOUNDED, unknown.certainty);
  EXPECT_EQ(BoundsInvalidity::NONE, unknown.invalidity);
  EXPECT_FALSE(unknown.alwaysMissing);

  logInput[0] = ValueBounds::unbounded(ValueType::DOUBLE, true, true);
  ValueBounds missing = logarithm->boundsPropagate(logNode, logInput);
  EXPECT_EQ(BoundsCertainty::UNBOUNDED, missing.certainty);
  EXPECT_TRUE(missing.mayBeMissing);
  EXPECT_TRUE(missing.alwaysMissing);

  ValueNode multiplyNode;
  multiplyNode.kind = ValueNodeKind::FUNCTION;
  multiplyNode.type = ValueType::DOUBLE;
  multiplyNode.opcode = ValueOpcode::MUL;
  multiplyNode.text = "mul";
  std::array<ValueBounds, 2> factors{
      ValueBounds::floating(1e308, 1e308), ValueBounds::floating(2.0, 2.0)};
  ValueBounds infinite = multiply->boundsPropagate(multiplyNode, factors);
  EXPECT_EQ(BoundsCertainty::INVALID, infinite.certainty);
  EXPECT_EQ(BoundsInvalidity::POSITIVE_INFINITY, infinite.invalidity);
}

TEST_F(ValueExprKernelTest, nonFiniteColumnEndpointIsRejectedAtBind) {
  CollectionHelper helper;
  helper.index(flatdoc("id_s", "finite", "weight_d", 1.0),
               UpdateMessage::NO_COMMIT);
  helper.index(flatdoc("id_s", "infinite", "weight_d",
                       std::numeric_limits<double>::infinity()),
               UpdateMessage::COMMIT);
  auto reader = helper.getIndexWriter()->getIndexReader();
  auto schema = helper.collection().getSchema();
  ArenaOwner memory;
  MemPool pool;
  ValueProgram* program = parseValue(memory, *schema, "add(weight_d,0.0)");
  EXPECT_THROW(program->bind(pool, reader->segments()[0]),
               std::runtime_error);
}
