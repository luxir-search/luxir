#include <gtest/gtest.h>

#include <array>
#include <string>

#include "luxir/schema/Schema.h"
#include "luxir/util/proto.h"
#include "luxir/value/ValueExprParser.h"
#include "test/SchemaBuilder.h"

using namespace luxir;

namespace {

class TestArena {
public:
  google::protobuf::Arena* arena = createArena();
  ~TestArena() { releaseArena(arena); }
};

void expectParseError(ValueExprParser& parser, std::string_view expression,
                      std::string_view expected) {
  try {
    parser.parse(expression);
    FAIL() << "expected parse failure for " << expression;
  } catch (const std::runtime_error& error) {
    EXPECT_NE(std::string(error.what()).find(expected), std::string::npos) << error.what();
  }
}

} // namespace

TEST(ValueExprParserTest, grammarTypesAndReservedLeaves) {
  auto base = Schema::createDefaultSchema();
  SchemaBuilder schemaBuilder;
  auto& reservedColumn = schemaBuilder.field("score");
  reservedColumn.type = api::FieldDef_::FieldClass::INT;
  reservedColumn.column = true;
  auto schema = schemaBuilder.build(base.get());
  TestArena memory;
  ValueExprOptions options{schema.get(), {}};
  ValueExprParser parser(options, *memory.arena);

  ValueProgram* arithmetic = parser.parse("add(price_i,mul(2,3.5))");
  EXPECT_EQ(ValueType::DOUBLE, arithmetic->root().type);
  EXPECT_EQ(ValueNodeKind::FUNCTION, arithmetic->root().kind);

  ValueProgram* integer = parser.parse("sub(9223372036854775806,1)");
  EXPECT_EQ(ValueType::INT64, integer->root().type);

  ValueProgram* explicitColumn = parser.parse("col('price_i')");
  EXPECT_EQ(ValueNodeKind::COLUMN, explicitColumn->root().kind);
  EXPECT_EQ("price_i", explicitColumn->root().text);

  ValueProgram* score = parser.parse("score");
  EXPECT_EQ(ValueNodeKind::SCORE, score->root().kind);
  EXPECT_TRUE(score->needsScore);
  ValueProgram* scoreColumn = parser.parse("col(\"score\")");
  EXPECT_EQ(ValueNodeKind::COLUMN, scoreColumn->root().kind);
  EXPECT_EQ(ValueType::INT64, scoreColumn->root().type);
  EXPECT_EQ(ValueNodeKind::SCORE, parser.parse("_score_")->root().kind);
  EXPECT_EQ(ValueNodeKind::DOCID, parser.parse("_docid_")->root().kind);
}

TEST(ValueExprParserTest, infixPrecedenceParenthesesAndUnary) {
  auto schema = Schema::createDefaultSchema();
  TestArena memory;
  ValueExprOptions options{schema.get(), {}};
  ValueExprParser parser(options, *memory.arena);

  ValueProgram* precedence = parser.parse("price_i + 2 * 3");
  EXPECT_EQ(ValueOpcode::ADD, precedence->root().opcode);
  EXPECT_EQ(ValueOpcode::MUL,
            precedence->nodes[precedence->root().children[1]].opcode);

  ValueProgram* parentheses = parser.parse("(price_i + 2) * 3");
  EXPECT_EQ(ValueOpcode::MUL, parentheses->root().opcode);
  EXPECT_EQ(ValueOpcode::ADD,
            parentheses->nodes[parentheses->root().children[0]].opcode);

  ValueProgram* unary = parser.parse("-price_i + +2");
  EXPECT_EQ(ValueOpcode::ADD, unary->root().opcode);
  EXPECT_EQ(ValueOpcode::NEG, unary->nodes[unary->root().children[0]].opcode);

  ValueProgram* functionArg = parser.parse("add(price_i * 2, 1)");
  EXPECT_EQ(ValueOpcode::ADD, functionArg->root().opcode);
  EXPECT_EQ(ValueOpcode::MUL,
            functionArg->nodes[functionArg->root().children[0]].opcode);

  ValueProgram* constant = parser.parse("1 + 2 * 3");
  ASSERT_TRUE(constant->constantScalar.has_value());
  EXPECT_EQ(7, constant->constantScalar->intValue);

  ValueProgram* division = parser.parse("5 / 2");
  EXPECT_EQ(ValueType::DOUBLE, division->root().type);
  ASSERT_TRUE(division->constantScalar.has_value());
  EXPECT_DOUBLE_EQ(2.5, division->constantScalar->doubleValue);
  EXPECT_EQ(ValueType::DOUBLE, parser.parse("div(5,2)")->root().type);
}

TEST(ValueExprParserTest, infixErrorsReportOperatorOffsets) {
  auto schema = Schema::createDefaultSchema();
  TestArena memory;
  ValueExprOptions options{schema.get(), {}};
  ValueExprParser parser(options, *memory.arena);

  expectParseError(parser, "price_i + * 2", "byte 10");
  expectParseError(parser, "(price_i + 1", "byte 12");
}

TEST(ValueExprParserTest, flatInfixChainsConsumeAndRestoreNestingBudget) {
  auto schema = Schema::createDefaultSchema();
  TestArena memory;
  auto chain = [](int terms) {
    std::string expression = "1";
    for (int i = 1; i < terms; i++) expression += "+1";
    return expression;
  };

  int budget = 8;
  ValueExprOptions options{schema.get(), {}, &budget};
  ValueExprParser parser(options, *memory.arena);
  expectParseError(parser, chain(10), "nesting exceeds");
  EXPECT_EQ(8, budget);
  EXPECT_NO_THROW(parser.parse(chain(8)));
  EXPECT_EQ(8, budget);

  int siblingBudget = 4;
  ValueExprOptions siblingOptions{schema.get(), {}, &siblingBudget};
  ValueExprParser siblingParser(siblingOptions, *memory.arena);
  EXPECT_NO_THROW(siblingParser.parse("add(1+2+3+4,5+6+7+8)"));
  EXPECT_EQ(4, siblingBudget);
}

TEST(ValueExprParserTest, logicalDateRules) {
  auto schema = Schema::createDefaultSchema();
  TestArena memory;
  ValueExprOptions options{schema.get(), {}};
  ValueExprParser parser(options, *memory.arena);

  EXPECT_EQ(ValueNature::DATE, parser.parse("when_dt")->root().nature);
  EXPECT_EQ(ValueNature::DATE,
            parser.parse("when_dt + 1000")->root().nature);
  EXPECT_EQ(ValueNature::NUMBER,
            parser.parse("when_dt - when_dt")->root().nature);
  EXPECT_EQ(ValueNature::DATE,
            parser.parse("avg(stamps_dts)")->root().nature);
  EXPECT_EQ(ValueNature::DATE,
            parser.parse("def(when_dt,0)")->root().nature);
  EXPECT_EQ(ValueNature::NUMBER,
            parser.parse("2 * when_dt")->root().nature);
  EXPECT_EQ(ValueNature::NUMBER,
            parser.parse("when_dt / 86400000")->root().nature);
  EXPECT_EQ(ValueNature::NUMBER,
            parser.parse("-when_dt")->root().nature);
  EXPECT_EQ(ValueNature::NUMBER,
            parser.parse("floor(when_dt / 86400000)")->root().nature);
  EXPECT_EQ(ValueNature::NUMBER,
            parser.parse("div(avg(stamps_dts), 1000)")->root().nature);

  expectParseError(parser, "when_dt + when_dt", "cannot add two DATE");
}

TEST(ValueExprParserTest, variablesAndArrayReducers) {
  auto schema = Schema::createDefaultSchema();
  TestArena memory;
  api::Val scalar;
  scalar.kind = int64_t{7};
  std::array<int64_t, 3> numbers{9, -2, 4};
  api::Val array;
  array.kind = api::ArrInt{numbers};
  using Pair = std::pair<std::string_view, ::hpp_proto::indirect_view<api::Val>>;
  std::array<Pair, 2> pairs{{{"n", {&scalar}}, {"values", {&array}}}};
  ValueExprOptions options{schema.get(), api::map_view<std::string_view,
      ::hpp_proto::indirect_view<api::Val>>{pairs}};
  ValueExprParser parser(options, *memory.arena);

  EXPECT_EQ(ValueNodeKind::VARIABLE, parser.parse("$n")->root().kind);
  EXPECT_EQ(ValueType::INT64, parser.parse("add($n,1)")->root().type);
  EXPECT_EQ(ValueType::INT64, parser.parse("min($values)")->root().type);
  EXPECT_EQ(ValueType::DOUBLE, parser.parse("avg($values)")->root().type);
  EXPECT_EQ(ValueType::INT64_ARRAY, parser.parse("def($values,$values)")->root().type);
  EXPECT_EQ(ValueType::DOUBLE_ARRAY, parser.parse("$values / 2")->root().type);
}

TEST(ValueExprParserTest, reportsArityTypeVariableAndLiteralErrors) {
  auto schema = Schema::createDefaultSchema();
  TestArena memory;
  ValueExprOptions options{schema.get(), {}};

  {
    ValueExprParser parser(options, *memory.arena);
    expectParseError(parser, "add(1)", "expects 2 arguments");
  }
  {
    ValueExprParser parser(options, *memory.arena);
    expectParseError(parser, "avg(price_i)", "requires a numeric array");
  }
  {
    ValueExprParser parser(options, *memory.arena);
    expectParseError(parser, "add(name_s,1)", "non-numeric column");
  }
  {
    ValueExprParser parser(options, *memory.arena);
    expectParseError(parser, "$missing", "undefined variable");
  }
  {
    ValueExprParser parser(options, *memory.arena);
    expectParseError(parser, "'price_i'", "only valid as the argument of col");
  }
  {
    ValueExprParser parser(options, *memory.arena);
    expectParseError(parser, "1e", "invalid numeric literal");
  }
}

TEST(ValueExprParserTest, rejectsNonFiniteVariablesAndExcessiveDepth) {
  auto schema = Schema::createDefaultSchema();
  TestArena memory;
  api::Val infinite;
  infinite.kind = std::numeric_limits<double>::infinity();
  using Pair = std::pair<std::string_view, ::hpp_proto::indirect_view<api::Val>>;
  std::array<Pair, 1> pairs{{{"bad", {&infinite}}}};
  ValueExprOptions options{schema.get(), api::map_view<std::string_view,
      ::hpp_proto::indirect_view<api::Val>>{pairs}};
  {
    ValueExprParser parser(options, *memory.arena);
    expectParseError(parser, "$bad", "NaN or infinity");
  }

  std::string deep;
  for (int i = 0; i < 129; i++) deep += "neg(";
  deep += "1";
  for (int i = 0; i < 129; i++) deep += ")";
  ValueExprOptions depthOptions{schema.get(), {}};
  ValueExprParser parser(depthOptions, *memory.arena);
  expectParseError(parser, deep, "nesting exceeds");
}

TEST(ValueExprParserTest, registryEntriesAreComplete) {
  auto entries = ValueFunctionRegistry::entries();
  ASSERT_FALSE(entries.empty());
  for (const ValueFunction& function : entries) {
    EXPECT_NE(nullptr, function.resolve) << function.name;
    EXPECT_TRUE(function.supports(FunctionCapability::DOCUMENT_VALUE))
        << function.name;
    EXPECT_NE(nullptr, function.evalPoint) << function.name;
    EXPECT_NE(nullptr, function.evalBatch) << function.name;
    EXPECT_NE(nullptr, function.boundsPropagate) << function.name;
  }
}
