#include <gtest/gtest.h>

#include <array>
#include <string>

#include "solux/schema/Schema.h"
#include "solux/util/proto.h"
#include "solux/value/ValueExprParser.h"
#include "test/SchemaBuilder.h"

using namespace solux;

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
    EXPECT_NE(nullptr, function.resolveType) << function.name;
    EXPECT_NE(nullptr, function.evalPoint) << function.name;
    EXPECT_NE(nullptr, function.evalBatch) << function.name;
    EXPECT_NE(nullptr, function.boundsPropagate) << function.name;
  }
}
