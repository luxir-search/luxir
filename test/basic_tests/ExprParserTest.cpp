// Parse tests for ExprParser: (string, schema, options) -> api::Query subtree,
// no engine.  Structural assertions on the emitted tree plus the rigorous
// parse-error contract (expr never degrades; errors carry byte offsets).
// Behaviors adapted from Lucene's classic TestQueryParser where the dialects
// overlap (precedence, ranges, decorations), with the deliberate deviations
// pinned: bare words error, +/- and AND/OR do not mix, positional specials
// instead of reserved-char sprawl.

#include <gtest/gtest.h>

#include <memory_resource>
#include <string>
#include <vector>

#include "solux/query/ExprParser.h"
#include "solux/schema/Schema.h"

#include "test/SoluxTest.h"

using namespace std;
using namespace solux;

using Operator = solux::api::Match_::Operator;

class ExprParserTest : public SoluxTest {
public:
  std::pmr::monotonic_buffer_resource arena;
  std::shared_ptr<Schema> schema = Schema::createDefaultSchema();
  std::vector<std::pair<std::string_view, ::hpp_proto::indirect_view<api::Val>>> varPairs;
  std::vector<api::Val> varVals;

  ExprParserTest() {
    // title/body analyzed text; status/url unanalyzed strings; count/rating/
    // created numeric column fields
    schema->fieldTypeMap["title"] = std::make_shared<TextFieldType>("title");
    schema->fieldTypeMap["body"] = std::make_shared<TextFieldType>("body");
    schema->fieldTypeMap["status"] = std::make_shared<StrFieldType>("status");
    schema->fieldTypeMap["url"] = std::make_shared<StrFieldType>("url");
    schema->fieldTypeMap["count"] = std::make_shared<IntFieldType>("count");
    schema->fieldTypeMap["rating"] = std::make_shared<FloatFieldType>("rating");
    schema->fieldTypeMap["created"] = std::make_shared<DateFieldType>("created");
    varVals.reserve(16);  // stable addresses for the map entries
  }

  void bindVar(std::string_view name, api::Val v) {
    varVals.push_back(v);
    varPairs.push_back({name, ::hpp_proto::indirect_view<api::Val>{&varVals.back()}});
  }

  const api::Query* parse(std::string_view q) {
    ExprOptions opts;
    opts.schema = schema.get();
    opts.vars = api::map_view<std::string_view, ::hpp_proto::indirect_view<api::Val>>(
        std::span(varPairs.data(), varPairs.size()));
    return parseExpr(q, opts, arena);
  }

  // parse expecting an error; returns the message
  std::string parseErr(std::string_view q) {
    try {
      parse(q);
    } catch (const std::exception& e) {
      return e.what();
    }
    ADD_FAILURE() << "expected a parse error for: " << q;
    return {};
  }

  static void expectContains(const std::string& msg, std::string_view needle) {
    EXPECT_NE(msg.find(needle), std::string::npos) << "message: " << msg;
  }

  // --- structural accessors (assert the arm, return it) ---
  static const api::Match& asMatch(const api::Query& q) {
    const auto* m = std::get_if<api::Match>(&q.kind);
    EXPECT_NE(nullptr, m);
    return *m;
  }
  static const api::BooleanQuery& asBool(const api::Query& q) {
    const auto* b = std::get_if<api::BooleanQuery>(&q.kind);
    EXPECT_NE(nullptr, b);
    return *b;
  }
  static const api::PhraseQuery& asPhrase(const api::Query& q) {
    const auto* p = std::get_if<api::PhraseQuery>(&q.kind);
    EXPECT_NE(nullptr, p);
    return *p;
  }
  static const api::PrefixQuery& asPrefix(const api::Query& q) {
    const auto* p = std::get_if<api::PrefixQuery>(&q.kind);
    EXPECT_NE(nullptr, p);
    return *p;
  }
  static const api::FuzzyQuery& asFuzzy(const api::Query& q) {
    const auto* f = std::get_if<api::FuzzyQuery>(&q.kind);
    EXPECT_NE(nullptr, f);
    return *f;
  }
  static const api::RangeQuery& asRange(const api::Query& q) {
    const auto* r = std::get_if<api::RangeQuery>(&q.kind);
    EXPECT_NE(nullptr, r);
    return *r;
  }
  static std::string_view matchVal(const api::Query& q) {
    const auto& m = asMatch(q);
    return m.val.has_value() ? m.val->asString() : std::string_view{};
  }
  static std::string_view valStr(const ::hpp_proto::optional_indirect_view<api::Val>& v) {
    return v.has_value() ? v->asString() : std::string_view{};
  }
};

// ---------- fielded terms and arm selection ----------

TEST_F(ExprParserTest, fieldedTerm) {
  const auto& m = asMatch(*parse("title:dune"));
  EXPECT_EQ("title", m.field);
  EXPECT_EQ("dune", valStr(m.val));
  EXPECT_EQ(Operator::OPERATOR_UNSPECIFIED, m.operator_);
}

TEST_F(ExprParserTest, numericFieldTermIsMatch) {
  // arm choice follows the SCHEMA, never the value's lexical shape
  EXPECT_EQ("10", matchVal(*parse("count:10")));
  EXPECT_EQ("-5", matchVal(*parse("count:-5")));  // '-' after ':' binds to the value
  EXPECT_EQ("3.5", matchVal(*parse("rating:3.5")));
}

TEST_F(ExprParserTest, unknownFieldErrors) {
  expectContains(parseErr("bogus:x"), "unknown field 'bogus'");
  expectContains(parseErr("bogus:x"), "byte 0");
}

TEST_F(ExprParserTest, bareWordErrorsAndNamesFixes) {
  auto msg = parseErr("dune");
  expectContains(msg, "unfielded term 'dune'");
  expectContains(msg, "simple_query");
}

TEST_F(ExprParserTest, phraseArmPerFieldType) {
  // analyzed TEXT: a positional phrase
  const auto& p = asPhrase(*parse("title:\"dune messiah\""));
  EXPECT_EQ("title", p.field);
  EXPECT_EQ("dune messiah", p.text);
  // unanalyzed STRING: one exact term
  EXPECT_EQ("in stock", matchVal(*parse("status:\"in stock\"")));
  // numeric: quotes only delimit
  EXPECT_EQ("10", matchVal(*parse("count:\"10\"")));
  // single quotes work the same (JSON friendliness)
  EXPECT_EQ("dune messiah", asPhrase(*parse("title:'dune messiah'")).text);
}

TEST_F(ExprParserTest, positionalSpecialsNeedNoEscaping) {
  // ':' splits only at the first colon; '*' and '~' only at token end
  EXPECT_EQ("https://x.com/a?b=1", matchVal(*parse("url:https://x.com/a?b=1")));
  EXPECT_EQ("12:30:00", matchVal(*parse("status:12:30:00")));
  EXPECT_EQ("a*b", matchVal(*parse("status:a*b")));
  EXPECT_EQ("a~b", matchVal(*parse("status:a~b")));
  EXPECT_EQ("a~2x", matchVal(*parse("status:a~2x")));
  EXPECT_EQ("a^b", matchVal(*parse("status:a^b")));
}

TEST_F(ExprParserTest, quotesArePositionalToo) {
  // a quote is special only where a value can begin; mid-word it is a byte,
  // so apostrophes in ordinary text do not need escaping
  EXPECT_EQ("don't", matchVal(*parse("title:don't")));
  EXPECT_EQ("say\"hi\"", matchVal(*parse("status:say\"hi\"")));

  const auto& b = asBool(*parse("title:(can't won't)"));
  ASSERT_EQ(2u, b.optional.size());
  EXPECT_EQ("can't", matchVal(b.optional[0]));
  EXPECT_EQ("won't", matchVal(b.optional[1]));

  // raw-text function arguments carry apostrophes through verbatim
  const auto& m = asMatch(*parse("match(don't stop, field=title)"));
  EXPECT_EQ("don't stop", valStr(m.val));
  // ...and a WHOLE-value quote still protects grammar characters
  EXPECT_EQ("a, don't", valStr(asMatch(*parse("match(\"a, don't\", field=title)")).val));

  // fuzzy suffix still binds after a quote byte
  EXPECT_EQ("don't", asFuzzy(*parse("title:don't~1")).term);
}

TEST_F(ExprParserTest, escapes) {
  EXPECT_EQ("a:b", matchVal(*parse("status:a\\:b")));   // escaped colon does not split
  EXPECT_EQ("ab*", matchVal(*parse("status:ab\\*")));   // escaped star is literal
  EXPECT_EQ("a b", matchVal(*parse("status:a\\ b")));   // escaped space joins
  EXPECT_EQ("say \"hi\"", matchVal(*parse("status:\"say \\\"hi\\\"\"")));
  expectContains(parseErr("status:a\\"), "dangling '\\'");
}

// ---------- boolean composition ----------

TEST_F(ExprParserTest, juxtapositionIsShould) {
  const auto& b = asBool(*parse("title:a title:b"));
  EXPECT_EQ(2u, b.optional.size());
  EXPECT_TRUE(b.required.empty());
  EXPECT_EQ("a", matchVal(b.optional[0]));
  EXPECT_EQ("b", matchVal(b.optional[1]));
}

TEST_F(ExprParserTest, plusMinusBuckets) {
  const auto& b = asBool(*parse("+title:a title:b -title:c"));
  ASSERT_EQ(1u, b.required.size());
  ASSERT_EQ(1u, b.optional.size());
  ASSERT_EQ(1u, b.prohibited.size());
  EXPECT_EQ("a", matchVal(b.required[0]));
  EXPECT_EQ("b", matchVal(b.optional[0]));
  EXPECT_EQ("c", matchVal(b.prohibited[0]));
}

TEST_F(ExprParserTest, singleClauseUnwraps) {
  EXPECT_EQ("a", matchVal(*parse("title:a")));
  EXPECT_EQ("a", matchVal(*parse("+title:a")));   // one required clause is itself
  EXPECT_EQ("a", matchVal(*parse("(title:a)")));  // group of one is itself
}

TEST_F(ExprParserTest, pureNegativeGetsMatchAll) {
  const auto& b = asBool(*parse("-title:a"));
  ASSERT_EQ(1u, b.optional.size());
  EXPECT_TRUE(std::holds_alternative<bool>(b.optional[0].kind));
  ASSERT_EQ(1u, b.prohibited.size());
  EXPECT_EQ("a", matchVal(b.prohibited[0]));
  // several negatives fold into ONE all-except level
  const auto& b2 = asBool(*parse("-title:a -title:b"));
  EXPECT_EQ(1u, b2.optional.size());
  EXPECT_EQ(2u, b2.prohibited.size());
}

TEST_F(ExprParserTest, andOrPrecedence) {
  // a OR b AND c == a OR (b AND c)
  const auto& b = asBool(*parse("title:a OR title:b AND title:c"));
  ASSERT_EQ(2u, b.optional.size());
  EXPECT_EQ("a", matchVal(b.optional[0]));
  const auto& andRun = asBool(b.optional[1]);
  EXPECT_EQ(2u, andRun.required.size());
}

TEST_F(ExprParserTest, notUnderAndMergesProhibited) {
  const auto& b = asBool(*parse("title:a AND NOT title:b"));
  ASSERT_EQ(1u, b.required.size());
  ASSERT_EQ(1u, b.prohibited.size());
  EXPECT_EQ("a", matchVal(b.required[0]));
  EXPECT_EQ("b", matchVal(b.prohibited[0]));
}

TEST_F(ExprParserTest, notAloneAndUnderOr) {
  const auto& b = asBool(*parse("NOT title:a"));
  EXPECT_EQ(1u, b.optional.size());  // match-all
  EXPECT_EQ(1u, b.prohibited.size());

  // a OR NOT b: the NOT leg is its own all-except node
  const auto& o = asBool(*parse("title:a OR NOT title:b"));
  ASSERT_EQ(2u, o.optional.size());
  const auto& leg = asBool(o.optional[1]);
  EXPECT_EQ(1u, leg.prohibited.size());
}

TEST_F(ExprParserTest, mixingErrors) {
  expectContains(parseErr("+title:a AND title:b"), "+/- prefixes cannot mix with AND/OR");
  expectContains(parseErr("title:a title:b AND title:c"),
                 "mix of AND/OR and juxtaposed clauses");
  expectContains(parseErr("AND title:a"), "cannot start with AND/OR");
  expectContains(parseErr("title:a AND"), "expected a clause after AND/OR");
  expectContains(parseErr("title:a AND OR title:b"), "cannot be used as a term");
  expectContains(parseErr("- title:a"), "immediately after '-'");
  expectContains(parseErr("NOT NOT title:a"), "at most one of +/-/NOT");
}

TEST_F(ExprParserTest, groupsNestAndKeepMode) {
  // groups let infix and prefix styles compose across levels
  const auto& b = asBool(*parse("+(title:a OR title:b) -status:x"));
  ASSERT_EQ(1u, b.required.size());
  EXPECT_EQ(2u, asBool(b.required[0]).optional.size());
  EXPECT_EQ(1u, b.prohibited.size());
  expectContains(parseErr("()"), "empty parentheses");
  expectContains(parseErr("(title:a"), "unmatched '('");
  expectContains(parseErr("title:a)"), "unexpected ')'");
}

// ---------- field groups (boolean scopes with distribution) ----------

TEST_F(ExprParserTest, fieldGroupDistributes) {
  const auto& b = asBool(*parse("title:(dune OR messiah)"));
  ASSERT_EQ(2u, b.optional.size());
  EXPECT_EQ("title", asMatch(b.optional[0]).field);
  EXPECT_EQ("dune", matchVal(b.optional[0]));
  EXPECT_EQ("title", asMatch(b.optional[1]).field);
  EXPECT_EQ("messiah", matchVal(b.optional[1]));
}

TEST_F(ExprParserTest, fieldGroupJuxtapositionAndDecorations) {
  const auto& b = asBool(*parse("title:(dune mess* fuzz~1 \"a b\")"));
  ASSERT_EQ(4u, b.optional.size());
  EXPECT_EQ("dune", matchVal(b.optional[0]));
  EXPECT_EQ("mess", asPrefix(b.optional[1]).prefix);
  EXPECT_EQ("fuzz", asFuzzy(b.optional[2]).term);
  EXPECT_EQ("a b", asPhrase(b.optional[3]).text);
}

TEST_F(ExprParserTest, numericScopeSignBindsToValue) {
  // in a numeric field's group, '-' before a number is the value's sign
  EXPECT_EQ("-5", matchVal(*parse("count:(-5)")));
  EXPECT_EQ("-.5", matchVal(*parse("rating:(-.5)")));

  const auto& b = asBool(*parse("count:(10 -5)"));
  ASSERT_EQ(2u, b.optional.size());
  EXPECT_EQ("10", matchVal(b.optional[0]));
  EXPECT_EQ("-5", matchVal(b.optional[1]));

  // exclusion in a numeric scope is spelled NOT
  const auto& n = asBool(*parse("count:(NOT -5)"));
  ASSERT_EQ(1u, n.prohibited.size());
  EXPECT_EQ("-5", matchVal(n.prohibited[0]));

  // '-' before a non-number, or in a non-numeric scope, is still an operator
  const auto& t = asBool(*parse("title:(-foo)"));
  ASSERT_EQ(1u, t.prohibited.size());
  EXPECT_EQ("foo", matchVal(t.prohibited[0]));
  const auto& a = asBool(*parse("count:(-abc)"));
  ASSERT_EQ(1u, a.prohibited.size());
}

TEST_F(ExprParserTest, fieldGroupInnerOverrideAndRanges) {
  const auto& b = asBool(*parse("title:(dune OR status:live)"));
  ASSERT_EQ(2u, b.optional.size());
  EXPECT_EQ("status", asMatch(b.optional[1]).field);

  const auto& r = asBool(*parse("count:(>=10 AND <20)"));
  ASSERT_EQ(2u, r.required.size());
  EXPECT_EQ("10", valStr(asRange(r.required[0]).gte));
  EXPECT_EQ("20", valStr(asRange(r.required[1]).lt));
}

// ---------- ranges and comparisons ----------

TEST_F(ExprParserTest, ranges) {
  const auto& r = asRange(*parse("count:[1 TO 10]"));
  EXPECT_EQ("count", r.field);
  EXPECT_EQ("1", valStr(r.gte));
  EXPECT_EQ("10", valStr(r.lte));
  EXPECT_FALSE(r.gt.has_value());

  const auto& ex = asRange(*parse("count:{1 TO 10}"));
  EXPECT_EQ("1", valStr(ex.gt));
  EXPECT_EQ("10", valStr(ex.lt));

  const auto& mixed = asRange(*parse("count:[1 TO 10}"));
  EXPECT_TRUE(mixed.gte.has_value());
  EXPECT_TRUE(mixed.lt.has_value());

  const auto& open = asRange(*parse("count:[* TO 10]"));
  EXPECT_FALSE(open.gte.has_value());
  EXPECT_FALSE(open.gt.has_value());
  EXPECT_EQ("10", valStr(open.lte));

  // date endpoints carry colons without escaping
  const auto& d = asRange(*parse("created:[2020-01-01T10:30:00Z TO *]"));
  EXPECT_EQ("2020-01-01T10:30:00Z", valStr(d.gte));
  EXPECT_FALSE(d.lte.has_value());
}

TEST_F(ExprParserTest, comparisons) {
  EXPECT_EQ("1990", valStr(asRange(*parse("count:>=1990")).gte));
  EXPECT_EQ("1990", valStr(asRange(*parse("count:>1990")).gt));
  EXPECT_EQ("1990", valStr(asRange(*parse("count:<=1990")).lte));
  EXPECT_EQ("1990", valStr(asRange(*parse("count:<1990")).lt));
}

TEST_F(ExprParserTest, juxtaposedSameFieldRangesError) {
  // count:(>5 <10) as OR matches nearly everything; make the writer pick
  expectContains(parseErr("count:(>5 <10)"), "combine as OR");
  expectContains(parseErr("count:[1 TO 5] count:[10 TO 20]"), "combine as OR");
  // explicit operators, required prefixes, and different fields are all fine
  EXPECT_EQ(2u, asBool(*parse("count:(>5 AND <10)")).required.size());
  EXPECT_EQ(2u, asBool(*parse("count:(>5 OR <10)")).optional.size());
  EXPECT_EQ(2u, asBool(*parse("count:(+>5 +<10)")).required.size());
  EXPECT_EQ(2u, asBool(*parse("count:* rating:*")).optional.size());
}

TEST_F(ExprParserTest, termRanges) {
  // term-backed fields take ranges too (byte order over the terms dictionary)
  const auto& r = asRange(*parse("status:[alpha TO mike]"));
  EXPECT_EQ("status", r.field);
  EXPECT_EQ("alpha", valStr(r.gte));
  EXPECT_EQ("mike", valStr(r.lte));
  EXPECT_EQ("m", valStr(asRange(*parse("status:>=m")).gte));
  EXPECT_EQ("m", valStr(asRange(*parse("title:{m TO *]")).gt));
}

TEST_F(ExprParserTest, rangeErrors) {
  expectContains(parseErr("count:[1 TO 2"), "expected ']' or '}'");
  expectContains(parseErr("count:[1 2]"), "expected TO");
  expectContains(parseErr("count:[TO 2]"), "expected a range endpoint");
  expectContains(parseErr("count:>="), "expected a range endpoint");
  expectContains(parseErr("[1 TO 2]"), "a range needs a field");
}

// ---------- decorations ----------

TEST_F(ExprParserTest, prefixAndFuzzy) {
  EXPECT_EQ("mess", asPrefix(*parse("title:mess*")).prefix);
  EXPECT_EQ("stat", asPrefix(*parse("status:stat*")).prefix);

  const auto& f = asFuzzy(*parse("title:dune~"));
  EXPECT_EQ("dune", f.term);
  EXPECT_FALSE(f.max_edits.has_value());  // bare ~ = AUTO, not Lucene's 2

  EXPECT_EQ(1, *asFuzzy(*parse("title:dune~1")).max_edits);
  EXPECT_EQ(2, *asFuzzy(*parse("title:dune~2")).max_edits);
  EXPECT_EQ("dune", matchVal(*parse("title:dune~0")));  // ~0 = exact
}

TEST_F(ExprParserTest, decorationErrors) {
  expectContains(parseErr("title:dune~3"), "exceeds the maximum of 2");
  // old Lucene float similarity: a teaching error, not a silent literal
  expectContains(parseErr("title:roam~0.8"), "whole number of edits");
  expectContains(parseErr("count:10~1"), "does not apply to numeric field");
  expectContains(parseErr("count:10*"), "does not apply to numeric field");
  expectContains(parseErr("title:ab*~1"), "cannot combine '*' and '~'");
  expectContains(parseErr("title:dune^2"), "boost (^) is reserved");
  expectContains(parseErr("title:dune^2.5"), "boost (^) is reserved");
  expectContains(parseErr("title:\"a b\"~2"), "phrase slop");
}

TEST_F(ExprParserTest, existsAndMatchAll) {
  EXPECT_TRUE(std::holds_alternative<bool>(parse("*:*")->kind));

  const auto& p = asPrefix(*parse("title:*"));  // term field: empty prefix
  EXPECT_EQ("title", p.field);
  EXPECT_TRUE(p.prefix.empty());

  const auto& r = asRange(*parse("count:*"));  // numeric column: unbounded range
  EXPECT_EQ("count", r.field);
  EXPECT_FALSE(r.gte.has_value() || r.gt.has_value() || r.lte.has_value() || r.lt.has_value());
}

// ---------- $var binding ----------

TEST_F(ExprParserTest, varBinding) {
  api::Val s;
  s.kind = std::string_view("dune messiah");
  bindVar("q", s);
  api::Val n;
  n.kind = (int64_t)42;
  bindVar("n", n);

  // bound as VALUES, never re-parsed: the whole string is one Match value
  EXPECT_EQ("dune messiah", matchVal(*parse("title:$q")));
  // the Val splices verbatim, arm and all
  const auto& m = asMatch(*parse("count:$n"));
  EXPECT_EQ(42, m.val->asInt());
  // range endpoints too
  EXPECT_EQ(42, asRange(*parse("count:>=$n")).gte->asInt());
}

TEST_F(ExprParserTest, varErrors) {
  expectContains(parseErr("title:$nope"), "undefined variable $nope");
  expectContains(parseErr("$x"), "a $variable is a value, not a clause");
  // '$' mid-token is a literal byte
  EXPECT_EQ("costs$5", matchVal(*parse("status:costs$5")));
}

// ---------- the function form ----------

TEST_F(ExprParserTest, functionMatch) {
  // raw-text positional: multi-word, unquoted; named args follow
  const auto& m = asMatch(*parse("match(dune messiah, field=title, operator=AND, min_match=2)"));
  EXPECT_EQ("title", m.field);
  EXPECT_EQ("dune messiah", valStr(m.val));
  EXPECT_EQ(Operator::AND, m.operator_);
  EXPECT_EQ(2, m.min_match);
  // quotes PROTECT, never MEAN: quoted and unquoted deliver the same bytes
  EXPECT_EQ("dune messiah", valStr(asMatch(*parse("match(\"dune messiah\", field=title)")).val));
  EXPECT_EQ("a, b", valStr(asMatch(*parse("match(\"a, b\", field=title)")).val));
}

TEST_F(ExprParserTest, functionVariety) {
  const auto& p = asPhrase(*parse("phrase(dune messiah, field=title)"));
  EXPECT_EQ("dune messiah", p.text);

  const auto& f = asFuzzy(*parse("fuzzy(smith, field=status, max_edits=2, prefix_length=0)"));
  EXPECT_EQ("smith", f.term);
  EXPECT_EQ(2, *f.max_edits);
  EXPECT_EQ(0, *f.prefix_length);

  const auto& pre = asPrefix(*parse("prefix(mess, field=title)"));
  EXPECT_EQ("mess", pre.prefix);

  const auto& r = asRange(*parse("range(field=count, gte=1990, lt=2000)"));
  EXPECT_EQ("1990", valStr(r.gte));
  EXPECT_EQ("2000", valStr(r.lt));

  EXPECT_TRUE(std::holds_alternative<bool>(parse("all()")->kind));

  const auto* cs = std::get_if<api::ConstantScoreQuery>(&parse("constant_score(status:live, score=2.5)")->kind);
  ASSERT_NE(nullptr, cs);
  EXPECT_FLOAT_EQ(2.5f, *cs->score);
  EXPECT_EQ("live", matchVal(*cs->query));
}

TEST_F(ExprParserTest, functionBooleanAndLists) {
  const auto* bq = std::get_if<api::BooleanQuery>(
      &parse("boolean(required=[status:live, title:dune], optional=[title:messiah], min_match=1)")->kind);
  ASSERT_NE(nullptr, bq);
  EXPECT_EQ(2u, bq->required.size());
  EXPECT_EQ(1u, bq->optional.size());
  EXPECT_EQ(1, bq->min_match);

  const auto* sq = std::get_if<api::SimpleQuery>(
      &parse("simple_query(dune messiah, fields=[title, body], operator=AND)")->kind);
  ASSERT_NE(nullptr, sq);
  EXPECT_EQ("dune messiah", sq->q);
  ASSERT_EQ(2u, sq->fields.size());
  EXPECT_EQ("title", sq->fields[0]);
  EXPECT_EQ(Operator::AND, sq->operator_);
}

TEST_F(ExprParserTest, functionVarArgs) {
  api::Val s;
  s.kind = std::string_view("+dune -messiah");
  bindVar("input", s);
  // end-user input rides through as a value, untouched
  const auto* sq =
      std::get_if<api::SimpleQuery>(&parse("simple_query($input, fields=[title])")->kind);
  ASSERT_NE(nullptr, sq);
  EXPECT_EQ("+dune -messiah", sq->q);
}

TEST_F(ExprParserTest, functionErrors) {
  expectContains(parseErr("bogus(x)"), "unknown query function 'bogus'");
  expectContains(parseErr("match(x, bogus=1)"), "unknown argument 'bogus' for match()");
  expectContains(parseErr("match(x, field=title, field=body)"), "duplicate argument");
  expectContains(parseErr("match(field=title, x)"), "positional argument after named");
  expectContains(parseErr("boolean(x)"), "takes only named arguments");
  expectContains(parseErr("all(x)"), "all() takes no arguments");
  expectContains(parseErr("expr(status:live)"), "not callable within expr");
  expectContains(parseErr("match(x, min_match=abc)"), "expects a number");
  expectContains(parseErr("match(x, operator=XOR)"), "expects AND or OR");
  expectContains(parseErr("knn(field=status, query=[1,2])"), "cannot express");
  expectContains(parseErr("match(a"), "unterminated match(...) call");
  expectContains(parseErr("simple_query(x, fields=title)"), "expects a list");
}

TEST_F(ExprParserTest, functionsIgnoreAmbientField) {
  // arguments are strictly node-local: the scope field does not leak in
  const auto& b = asBool(*parse("title:(dune OR match(x, field=body))"));
  ASSERT_EQ(2u, b.optional.size());
  EXPECT_EQ("body", asMatch(b.optional[1]).field);
}

// ---------- limits / robustness ----------

TEST_F(ExprParserTest, nestingBudget) {
  std::string deep;
  for (int i = 0; i < 200; i++) deep += "(";
  deep += "title:a";
  for (int i = 0; i < 200; i++) deep += ")";
  expectContains(parseErr(deep), "nesting exceeds the supported depth");

  // an explicit budget is honored (the request-shared counter)
  int budget = 8;
  ExprOptions opts;
  opts.schema = schema.get();
  opts.nestingBudget = &budget;
  EXPECT_THROW(parseExpr("((((((((title:a))))))))", opts, arena), std::runtime_error);
  EXPECT_EQ(8, budget);  // restored on unwind
}

TEST_F(ExprParserTest, emptyAndTrailing) {
  expectContains(parseErr(""), "empty expression");
  expectContains(parseErr("   "), "empty expression");
  expectContains(parseErr("title:"), "expected a value after 'title:'");
  expectContains(parseErr(":x"), "expected a field name before ':'");
}

TEST_F(ExprParserTest, unicodeAndBinarySafety) {
  // U+3000 is whitespace; other multi-byte sequences pass through opaquely
  const auto& b = asBool(*parse("title:a\xE3\x80\x80title:b"));
  EXPECT_EQ(2u, b.optional.size());
  EXPECT_EQ("caf\xC3\xA9", matchVal(*parse("title:caf\xC3\xA9")));

  // embedded NUL bytes are ordinary value bytes, not terminators
  std::string withNul = "status:a";
  withNul.push_back('\0');
  withNul += "b";
  auto val = matchVal(*parse(withNul));
  EXPECT_EQ(3u, val.size());

  // a lone high byte cannot make the parser read out of bounds
  EXPECT_EQ("\xE3", matchVal(*parse(std::string("status:\xE3"))));
}

TEST_F(ExprParserTest, keywordBoundaryIsByteExact) {
  // a keyword ends only at real whitespace/structure: the full U+3000
  // sequence delimits, but another 0xE3-lead character (hiragana) or an
  // embedded NUL byte is an ordinary token byte
  const auto& b = asBool(*parse("title:(a AND\xE3\x80\x80题)"));
  ASSERT_EQ(2u, b.required.size());  // U+3000: AND is the operator
  EXPECT_EQ("\xE9\xA2\x98", matchVal(b.required[1]));

  const auto& j = asBool(*parse("title:(a AND\xE3\x81\x82)"));
  ASSERT_EQ(2u, j.optional.size());  // hiragana: one juxtaposed term
  EXPECT_EQ("AND\xE3\x81\x82", matchVal(j.optional[1]));

  std::string withNul = "title:(a AND";
  withNul.push_back('\0');
  withNul += "b)";
  const auto& n = asBool(*parse(withNul));
  ASSERT_EQ(2u, n.optional.size());  // NUL: one juxtaposed term "AND\0b"
  EXPECT_EQ(5u, matchVal(n.optional[1]).size());
}
