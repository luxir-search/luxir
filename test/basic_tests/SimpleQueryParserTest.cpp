// Parse tests for SimpleQueryParser: (string, schema, options) -> api::Query
// subtree, no engine.  Structural assertions on the emitted tree.  Behaviors
// adapted from Lucene's TestSimpleQueryParser where the dialects overlap
// (operators, negation, precedence, never-fails edge cases), plus the Solux
// fielded-term extension and FieldType-directed arm selection.

#include <gtest/gtest.h>

#include <memory_resource>

#include "solux/query/SimpleQueryParser.h"
#include "solux/schema/Schema.h"

#include "test/SoluxTest.h"

using namespace std;
using namespace solux;

using Operator = solux::api::Match_::Operator;

class SimpleQueryParserTest : public SoluxTest {
public:
  std::pmr::monotonic_buffer_resource arena;
  std::shared_ptr<Schema> schema = Schema::createDefaultSchema();
  std::vector<std::string_view> fields{"body"};
  std::vector<std::string_view> allowed;  // empty = every queryable field
  Operator op = Operator::OPERATOR_UNSPECIFIED;
  int32_t minMatch = 0;

  SimpleQueryParserTest() {
    // title/body analyzed text; status/url unanalyzed strings; count/rating/
    // created numeric column fields (exact-match arm only)
    schema->fieldTypeMap["title"] = std::make_shared<TextFieldType>("title");
    schema->fieldTypeMap["body"] = std::make_shared<TextFieldType>("body");
    schema->fieldTypeMap["status"] = std::make_shared<StrFieldType>("status");
    schema->fieldTypeMap["url"] = std::make_shared<StrFieldType>("url");
    schema->fieldTypeMap["count"] = std::make_shared<IntFieldType>("count");
    schema->fieldTypeMap["rating"] = std::make_shared<FloatFieldType>("rating");
    schema->fieldTypeMap["created"] = std::make_shared<DateFieldType>("created");
  }

  SimpleQueryResult parse(std::string_view q) {
    SimpleQueryOptions opts;
    opts.fields = std::span<const std::string_view>(fields.data(), fields.size());
    opts.schema = schema.get();
    opts.allowed_fields = std::span<const std::string_view>(allowed.data(), allowed.size());
    opts.operator_ = op;
    opts.min_match = minMatch;
    return parseSimpleQuery(q, opts, arena);
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
  static std::string_view matchVal(const api::Query& q) {
    const auto& m = asMatch(q);
    return m.val.has_value() ? m.val->asString() : std::string_view{};
  }
  static bool hasWarning(const SimpleQueryResult& r, std::string_view code) {
    for (const auto& w : r.warnings) {
      if (w.code == code) return true;
    }
    return false;
  }
};

TEST_F(SimpleQueryParserTest, singleTerm) {
  auto r = parse("foo");
  ASSERT_NE(nullptr, r.root);
  const auto& m = asMatch(*r.root);
  EXPECT_EQ("body", m.field);
  EXPECT_EQ("foo", matchVal(*r.root));
  EXPECT_TRUE(r.warnings.empty());
}

TEST_F(SimpleQueryParserTest, juxtapositionIsOptionalByDefault) {
  auto r = parse("foo bar");
  const auto& b = asBool(*r.root);
  ASSERT_EQ(2u, b.optional.size());
  EXPECT_TRUE(b.required.empty());
  EXPECT_EQ("foo", matchVal(b.optional[0]));
  EXPECT_EQ("bar", matchVal(b.optional[1]));
}

TEST_F(SimpleQueryParserTest, plusIsUnaryRequired) {
  // '+' is a unary modifier on the next clause, not an infix operator: the
  // default operator applies only to the bare clause (classic QueryParser /
  // Google precedence).  '+foo bar' = require foo, bar optional.
  auto r = parse("+foo bar");
  const auto& b = asBool(*r.root);
  ASSERT_EQ(1u, b.required.size());
  EXPECT_EQ("foo", matchVal(b.required[0]));
  ASSERT_EQ(1u, b.optional.size());
  EXPECT_EQ("bar", matchVal(b.optional[0]));

  // all-'+' is a pure conjunction (how the benchmark corpus expresses AND)
  const auto& b2 = asBool(*parse("+foo +bar").root);
  ASSERT_EQ(2u, b2.required.size());
  EXPECT_TRUE(b2.optional.empty());

  // a '+' glued to a term is literal (operators are only operators at a
  // clause boundary): "foo+" is the single term "foo+"
  EXPECT_EQ("foo+", matchVal(*parse("foo+").root));
}

TEST_F(SimpleQueryParserTest, minusIsUnaryProhibitedAndRestricts) {
  // '-foo bar' restricts: prohibit foo, bar optional (not the old widening
  // "all-except-foo OR bar")
  auto r = parse("-foo bar");
  const auto& b = asBool(*r.root);
  ASSERT_EQ(1u, b.prohibited.size());
  EXPECT_EQ("foo", matchVal(b.prohibited[0]));
  ASSERT_EQ(1u, b.optional.size());
  EXPECT_EQ("bar", matchVal(b.optional[0]));
  EXPECT_TRUE(b.required.empty());
}

TEST_F(SimpleQueryParserTest, plusBindsOnlyWhenAdjacent) {
  // whitespace between '+' and its clause drops it, mirroring the '-' rule
  auto r = parse("+ foo");
  EXPECT_EQ("foo", matchVal(*r.root));
}

TEST_F(SimpleQueryParserTest, defaultOperatorAnd) {
  op = Operator::AND;
  auto r = parse("foo bar");
  const auto& b = asBool(*r.root);
  ASSERT_EQ(2u, b.required.size());
  // the operator also drives multi-term combination within an analyzed token
  EXPECT_EQ(Operator::AND, asMatch(b.required[0]).operator_);
}

TEST_F(SimpleQueryParserTest, orConjunction) {
  // under the default OR, '|' is a no-op: both clauses are already optional
  const auto& b = asBool(*parse("foo | bar").root);
  ASSERT_EQ(2u, b.optional.size());
  EXPECT_TRUE(b.required.empty());

  // under the default AND, '|' keeps its two operands optional (classic
  // QueryParser: an OR conjunction demotes the preceding clause); a bare
  // neighbor stays required
  op = Operator::AND;
  const auto& b2 = asBool(*parse("foo | bar baz").root);
  ASSERT_EQ(2u, b2.optional.size());  // foo, bar
  ASSERT_EQ(1u, b2.required.size());  // baz
  EXPECT_EQ("baz", matchVal(b2.required[0]));
}

TEST_F(SimpleQueryParserTest, negation) {
  auto r = parse("-foo");
  const auto& b = asBool(*r.root);
  ASSERT_EQ(1u, b.prohibited.size());
  ASSERT_EQ(1u, b.optional.size());  // the match-all companion
  EXPECT_TRUE(std::holds_alternative<bool>(b.optional[0].kind));
  EXPECT_EQ("foo", matchVal(b.prohibited[0]));
}

TEST_F(SimpleQueryParserTest, operatorsAreLiteralMidToken) {
  // '+' '|' are operators ONLY at a clause boundary; glued into a term they
  // are ordinary bytes and pass through to analysis (the distinguishing rule)
  EXPECT_EQ("c++", matchVal(*parse("c++").root));
  EXPECT_EQ("a+b", matchVal(*parse("a+b").root));
  EXPECT_EQ("a|b", matchVal(*parse("a|b").root));

  // the rule extends into fielded values too
  EXPECT_EQ("+foo", matchVal(*parse("status:+foo").root));
  EXPECT_EQ("a|b", matchVal(*parse("status:a|b").root));

  // space-separated, the same characters are operators again
  EXPECT_EQ(2u, asBool(*parse("a | b").root).optional.size());
  const auto& b = asBool(*parse("a +b").root);
  ASSERT_EQ(1u, b.required.size());
  EXPECT_EQ("b", matchVal(b.required[0]));
  ASSERT_EQ(1u, b.optional.size());
  EXPECT_EQ("a", matchVal(b.optional[0]));
}

TEST_F(SimpleQueryParserTest, stackedLeadingSignsFirstWins) {
  // only the FIRST sign at a boundary is a modifier; the rest are literal
  // term bytes.  '+-foo' = require the term "-foo" (single clause, unwrapped)
  EXPECT_EQ("-foo", matchVal(*parse("+-foo").root));

  // '-+foo' = prohibit the term "+foo"; a pure-negative level gets a match-all
  // companion so it means "everything except"
  const auto& b = asBool(*parse("-+foo").root);
  ASSERT_EQ(1u, b.prohibited.size());
  EXPECT_EQ("+foo", matchVal(b.prohibited[0]));
  ASSERT_EQ(1u, b.optional.size());
  EXPECT_TRUE(std::holds_alternative<bool>(b.optional[0].kind));

  // '--foo' no longer cancels: '-' modifier + literal term "-foo"
  const auto& b2 = asBool(*parse("--foo").root);
  ASSERT_EQ(1u, b2.prohibited.size());
  EXPECT_EQ("-foo", matchVal(b2.prohibited[0]));

  // '-|foo' = prohibit the term "|foo" (the '|' is literal after the sign)
  const auto& b3 = asBool(*parse("-|foo").root);
  ASSERT_EQ(1u, b3.prohibited.size());
  EXPECT_EQ("|foo", matchVal(b3.prohibited[0]));
}

TEST_F(SimpleQueryParserTest, extraneousParenNeutralForBoundary) {
  // an unmatched '(' is extraneous and leaves the clause boundary untouched,
  // so a following sign is still a modifier (as it is in a matched group)
  const auto& b = asBool(*parse("(-foo").root);
  ASSERT_EQ(1u, b.prohibited.size());
  EXPECT_EQ("foo", matchVal(b.prohibited[0]));

  EXPECT_EQ("foo", matchVal(*parse("(+foo").root));  // required single -> leaf
  EXPECT_EQ("foo", matchVal(*parse(")+foo").root));  // a stray ')' is neutral too
}

TEST_F(SimpleQueryParserTest, whitespaceBreaksNegation) {
  // '- foo' is not a negation: the not resets at whitespace (Lucene behavior)
  auto r = parse("- foo");
  EXPECT_EQ("foo", matchVal(*r.root));
}

TEST_F(SimpleQueryParserTest, precedenceGroups) {
  // '+' applies to the group; the bare word stays optional (default OR)
  auto r = parse("foo +(bar | baz)");
  const auto& outer = asBool(*r.root);
  ASSERT_EQ(1u, outer.required.size());  // the +(...) group
  ASSERT_EQ(1u, outer.optional.size());  // bare foo
  EXPECT_EQ("foo", matchVal(outer.optional[0]));
  const auto& inner = asBool(outer.required[0]);
  ASSERT_EQ(2u, inner.optional.size());
}

TEST_F(SimpleQueryParserTest, neverFailsEdges) {
  EXPECT_EQ(nullptr, parse("").root);
  EXPECT_EQ(nullptr, parse("   ").root);
  EXPECT_EQ(nullptr, parse("-").root);
  EXPECT_EQ(nullptr, parse("+ | -").root);
  EXPECT_EQ(nullptr, parse("()").root);

  // unbalanced constructs are extraneous characters, not errors
  EXPECT_EQ("foo", matchVal(*parse("((foo").root));
  EXPECT_EQ("foo", matchVal(*parse("foo)").root));
  EXPECT_EQ("foo", matchVal(*parse("\"foo").root));  // unterminated quote reparses
}

TEST_F(SimpleQueryParserTest, wholeInputStarMatchesAll) {
  auto r = parse(" * ");
  ASSERT_NE(nullptr, r.root);
  EXPECT_TRUE(std::holds_alternative<bool>(r.root->kind));
}

TEST_F(SimpleQueryParserTest, escapes) {
  EXPECT_EQ("+foo", matchVal(*parse("\\+foo").root));
  EXPECT_EQ("foo\"bar", matchVal(*parse("foo\\\"bar").root));
  EXPECT_EQ("a:b", matchVal(*parse("a\\:b").root));  // escaped colon is never structural
  EXPECT_EQ("foo", matchVal(*parse("foo\\").root));  // trailing escape dropped
  EXPECT_EQ(nullptr, parse("\\").root);
}

TEST_F(SimpleQueryParserTest, ideographicSpaceSeparates) {
  auto r = parse("foo\xE3\x80\x80"
                 "bar");
  const auto& b = asBool(*r.root);
  EXPECT_EQ(2u, b.optional.size());
}

TEST_F(SimpleQueryParserTest, prefixOperator) {
  auto r = parse("abc*");
  const auto* p = std::get_if<api::PrefixQuery>(&r.root->kind);
  ASSERT_NE(nullptr, p);
  EXPECT_EQ("abc", p->prefix);
  EXPECT_EQ("body", p->field);

  // '*' is only special at token end
  EXPECT_EQ("ab*c", matchVal(*parse("ab*c").root));
  // escaped star is literal
  EXPECT_EQ("abc*", matchVal(*parse("abc\\*").root));
}

TEST_F(SimpleQueryParserTest, fuzzyOperator) {
  auto r = parse("abc~1");
  const auto* f = std::get_if<api::FuzzyQuery>(&r.root->kind);
  ASSERT_NE(nullptr, f);
  EXPECT_EQ("abc", f->term);
  EXPECT_EQ(1, f->max_edits.value());

  // bare ~ = AUTO (unset), the engine picks by term length
  const auto* fAuto = std::get_if<api::FuzzyQuery>(&parse("abc~").root->kind);
  ASSERT_NE(nullptr, fAuto);
  EXPECT_FALSE(fAuto->max_edits.has_value());

  // ~0 is a plain term
  EXPECT_EQ("abc", matchVal(*parse("abc~0").root));

  // a '~' is a fuzzy operator only as a clean trailing suffix (digits then a
  // token boundary); mangled fuzzy syntax is a literal byte, so the whole
  // thing reads as one term (like '*' anywhere but the token end)
  EXPECT_EQ("abc~xyz", matchVal(*parse("abc~xyz").root));
  EXPECT_EQ("abc~2+d", matchVal(*parse("abc~2+d").root));
}

TEST_F(SimpleQueryParserTest, fuzzyClampDeclared) {
  auto r = parse("abc~7");
  const auto* f = std::get_if<api::FuzzyQuery>(&r.root->kind);
  ASSERT_NE(nullptr, f);
  EXPECT_EQ(2, f->max_edits.value());
  EXPECT_TRUE(hasWarning(r, "fuzzy_clamped"));
}

TEST_F(SimpleQueryParserTest, phrase) {
  auto r = parse("\"foo bar\"");
  const auto* p = std::get_if<api::PhraseQuery>(&r.root->kind);
  ASSERT_NE(nullptr, p);
  EXPECT_EQ("foo bar", p->text);
  EXPECT_EQ("body", p->field);
}

TEST_F(SimpleQueryParserTest, phraseSlopDeclaredIgnored) {
  auto r = parse("\"foo bar\"~2");
  ASSERT_NE(nullptr, std::get_if<api::PhraseQuery>(&r.root->kind));
  EXPECT_TRUE(hasWarning(r, "phrase_slop_ignored"));

  // explicit ~0 changes nothing and warns nothing
  auto r0 = parse("\"foo bar\"~0");
  EXPECT_TRUE(r0.warnings.empty());
}

TEST_F(SimpleQueryParserTest, multiFieldExpansionSums) {
  fields = {"title", "body"};
  auto r = parse("foo");
  const auto& b = asBool(*r.root);
  ASSERT_EQ(2u, b.optional.size());
  EXPECT_EQ("title", asMatch(b.optional[0]).field);
  EXPECT_EQ("body", asMatch(b.optional[1]).field);
  EXPECT_EQ("foo", matchVal(b.optional[0]));
}

TEST_F(SimpleQueryParserTest, fieldedTerm) {
  auto r = parse("status:in_stock");
  const auto& m = asMatch(*r.root);
  EXPECT_EQ("status", m.field);
  EXPECT_EQ("in_stock", matchVal(*r.root));
}

TEST_F(SimpleQueryParserTest, fieldedDecorations) {
  // '-' immediately after 'field:' binds to the value
  EXPECT_EQ("-10", matchVal(*parse("status:-10").root));

  const auto* p = std::get_if<api::PrefixQuery>(&parse("title:ab*").root->kind);
  ASSERT_NE(nullptr, p);
  EXPECT_EQ("title", p->field);
  EXPECT_EQ("ab", p->prefix);

  const auto* f = std::get_if<api::FuzzyQuery>(&parse("title:abc~1").root->kind);
  ASSERT_NE(nullptr, f);
  EXPECT_EQ("title", f->field);
  EXPECT_EQ("abc", f->term);

  // field:* = documents having the field (empty prefix)
  const auto* has = std::get_if<api::PrefixQuery>(&parse("title:*").root->kind);
  ASSERT_NE(nullptr, has);
  EXPECT_EQ("", has->prefix);
}

TEST_F(SimpleQueryParserTest, fieldedPhrase) {
  auto r = parse("title:\"foo bar\"");
  const auto* p = std::get_if<api::PhraseQuery>(&r.root->kind);
  ASSERT_NE(nullptr, p);
  EXPECT_EQ("title", p->field);
  EXPECT_EQ("foo bar", p->text);
}

TEST_F(SimpleQueryParserTest, unknownFieldDegradesToText) {
  // the Gmail rule: a colon token with an unknown head is literal text
  auto r = parse("price:10");
  const auto& m = asMatch(*r.root);
  EXPECT_EQ("body", m.field);
  EXPECT_EQ("price:10", matchVal(*r.root));
  EXPECT_TRUE(r.warnings.empty());  // unknown names are normal text, not declared

  // "re: your email" - empty value never blows up
  auto r2 = parse("re: your email");
  const auto& b = asBool(*r2.root);
  ASSERT_EQ(3u, b.optional.size());
  EXPECT_EQ("re:", matchVal(b.optional[0]));
  EXPECT_EQ("your", matchVal(b.optional[1]));
}

TEST_F(SimpleQueryParserTest, onlyFirstColonIsStructural) {
  auto r = parse("url:https://x");
  const auto& m = asMatch(*r.root);
  EXPECT_EQ("url", m.field);
  EXPECT_EQ("https://x", matchVal(*r.root));
}

TEST_F(SimpleQueryParserTest, narrowedFieldDegradesWithWarning) {
  allowed = {"title", "body", "url"};
  auto r = parse("status:x");
  EXPECT_EQ("status:x", matchVal(*r.root));
  EXPECT_TRUE(hasWarning(r, "field_narrowed"));
}

TEST_F(SimpleQueryParserTest, unknownFieldPhraseSplits) {
  // unknown head before a quote: 'price:' is literal text, the phrase parses
  // separately (never an error)
  auto r = parse("price:\"a b\"");
  const auto& b = asBool(*r.root);
  ASSERT_EQ(2u, b.optional.size());
  EXPECT_EQ("price:", matchVal(b.optional[0]));
  EXPECT_NE(nullptr, std::get_if<api::PhraseQuery>(&b.optional[1].kind));
}

TEST_F(SimpleQueryParserTest, depthClampDeclared) {
  std::string q(100, '(');
  q += "foo";
  q += std::string(100, ')');
  auto r = parse(q);
  ASSERT_NE(nullptr, r.root);
  EXPECT_EQ("foo", matchVal(*r.root));
  EXPECT_TRUE(hasWarning(r, "depth_clamped"));
}

TEST_F(SimpleQueryParserTest, mixedFieldedAndBare) {
  fields = {"title", "body"};
  auto r = parse("status:in_stock forrest gump");
  const auto& b = asBool(*r.root);
  ASSERT_EQ(3u, b.optional.size());
  EXPECT_EQ("status", asMatch(b.optional[0]).field);   // fielded stays single
  asBool(b.optional[1]);                                // bare words expand per field
}

// ---- FieldType-directed arm selection (the schema-aware emission) ----

TEST_F(SimpleQueryParserTest, quotedValueOnStringFieldIsExactMatch) {
  // STRING is unanalyzed: the quoted text is one exact term, not a phrase
  auto r = parse("status:\"in stock\"");
  const auto& m = asMatch(*r.root);
  EXPECT_EQ("status", m.field);
  EXPECT_EQ("in stock", matchVal(*r.root));
}

TEST_F(SimpleQueryParserTest, unfieldedPhraseSelectsArmPerField) {
  fields = {"title", "status"};
  auto r = parse("\"a b\"");
  const auto& b = asBool(*r.root);
  ASSERT_EQ(2u, b.optional.size());
  EXPECT_NE(nullptr, std::get_if<api::PhraseQuery>(&b.optional[0].kind));  // TEXT
  EXPECT_EQ("a b", matchVal(b.optional[1]));                               // STRING: exact term
  EXPECT_EQ("status", asMatch(b.optional[1]).field);
}

TEST_F(SimpleQueryParserTest, matchOperatorOnlyOnTextLeaves) {
  op = Operator::AND;
  // analyzed TEXT carries the operator (it shapes multi-term combination);
  // unanalyzed STRING is a single term, so it stays unset in the echo
  EXPECT_EQ(Operator::AND, asMatch(*parse("title:foo").root).operator_);
  EXPECT_EQ(Operator::OPERATOR_UNSPECIFIED, asMatch(*parse("status:foo").root).operator_);
}

// ---- numeric column fields: exact match; wildcard/fuzzy and bad values degrade ----

TEST_F(SimpleQueryParserTest, numericFieldExactMatch) {
  // popularity:10 style - an exact numeric match (a degenerate [10,10] range
  // downstream), no analyzer operator on the leaf
  auto r = parse("count:10");
  const auto& m = asMatch(*r.root);
  EXPECT_EQ("count", m.field);
  EXPECT_EQ("10", matchVal(*r.root));
  EXPECT_EQ(Operator::OPERATOR_UNSPECIFIED, m.operator_);
  EXPECT_TRUE(r.warnings.empty());
}

TEST_F(SimpleQueryParserTest, floatFieldExactMatch) {
  auto r = parse("rating:4.5");
  const auto& m = asMatch(*r.root);
  EXPECT_EQ("rating", m.field);
  EXPECT_EQ("4.5", matchVal(*r.root));
  EXPECT_TRUE(r.warnings.empty());
}

TEST_F(SimpleQueryParserTest, dateFieldExactMatch) {
  auto r = parse("created:2024-01-01");
  const auto& m = asMatch(*r.root);
  EXPECT_EQ("created", m.field);
  EXPECT_EQ("2024-01-01", matchVal(*r.root));
  EXPECT_TRUE(r.warnings.empty());
}

TEST_F(SimpleQueryParserTest, quotedNumericValueIsExactMatch) {
  // quotes on a numeric field are value delimiters, not a phrase
  auto r = parse("count:\"10\"");
  const auto& m = asMatch(*r.root);
  EXPECT_EQ("count", m.field);
  EXPECT_EQ("10", matchVal(*r.root));
  EXPECT_TRUE(r.warnings.empty());
}

TEST_F(SimpleQueryParserTest, numericFieldWildcardDegrades) {
  // '*' has no numeric meaning: the whole token becomes text, with a teaching
  // warning
  auto r = parse("count:10*");
  const auto* p = std::get_if<api::PrefixQuery>(&r.root->kind);
  ASSERT_NE(nullptr, p);
  EXPECT_EQ("body", p->field);
  EXPECT_EQ("count:10", p->prefix);
  EXPECT_TRUE(hasWarning(r, "numeric_field_syntax"));
}

TEST_F(SimpleQueryParserTest, numericFieldFuzzyDegrades) {
  auto r = parse("count:10~1");
  const auto* f = std::get_if<api::FuzzyQuery>(&r.root->kind);
  ASSERT_NE(nullptr, f);
  EXPECT_EQ("body", f->field);
  EXPECT_EQ("count:10", f->term);
  EXPECT_TRUE(hasWarning(r, "numeric_field_syntax"));
}

TEST_F(SimpleQueryParserTest, numericFieldExistenceStar) {
  // field:* is the universal has-a-value idiom: an unbounded range over the
  // column, same as expr (and no warning - nothing degraded)
  auto r = parse("count:*");
  const auto* rq = std::get_if<api::RangeQuery>(&r.root->kind);
  ASSERT_NE(nullptr, rq);
  EXPECT_EQ("count", rq->field);
  EXPECT_FALSE(rq->gte.has_value() || rq->gt.has_value() || rq->lte.has_value() ||
               rq->lt.has_value());
  EXPECT_TRUE(r.warnings.empty());
}

TEST_F(SimpleQueryParserTest, starColonStarIsMatchAll) {
  EXPECT_TRUE(std::holds_alternative<bool>(parse("*:*").root->kind));
  // composes like any clause
  auto r = parse("*:* -foo");
  const auto& b = asBool(*r.root);
  ASSERT_EQ(1u, b.prohibited.size());
}

TEST_F(SimpleQueryParserTest, numericFieldBadValueDegrades) {
  // not a number: literal text over the default field, never a build error
  auto r = parse("count:abc");
  const auto& m = asMatch(*r.root);
  EXPECT_EQ("body", m.field);
  EXPECT_EQ("count:abc", matchVal(*r.root));
  EXPECT_TRUE(hasWarning(r, "numeric_field_value"));
}

TEST_F(SimpleQueryParserTest, numericFieldBadQuotedValueDegrades) {
  // degrades like an unknown field: the head stays literal text and the
  // value stays a phrase (dropping the head would silently widen results)
  auto r = parse("created:\"not-a-date\"");
  const auto& b = asBool(*r.root);
  ASSERT_EQ(2u, b.optional.size());
  EXPECT_EQ("created:", matchVal(b.optional[0]));
  ASSERT_NE(nullptr, std::get_if<api::PhraseQuery>(&b.optional[1].kind));
  EXPECT_TRUE(hasWarning(r, "numeric_field_value"));
}

TEST_F(SimpleQueryParserTest, numericFieldNotColumnStoredDegradesSilently) {
  // a numeric field without a column cannot be match-queried; it is not
  // targetable and degrades to text like an unknown field (no warning)
  schema->fieldTypeMap["qty"] = std::make_shared<IntFieldType>("qty", FieldType::INDEX_DOCS);
  auto r = parse("qty:5");
  EXPECT_EQ("body", asMatch(*r.root).field);
  EXPECT_EQ("qty:5", matchVal(*r.root));
  EXPECT_TRUE(r.warnings.empty());
}

// ---- min_match binds to user clauses, never to per-field expansion ----

TEST_F(SimpleQueryParserTest, minMatchBindsToUserClauses) {
  fields = {"title", "body"};
  minMatch = 2;
  auto r = parse("foo bar");
  const auto& b = asBool(*r.root);
  EXPECT_EQ(2, b.min_match);
  ASSERT_EQ(2u, b.optional.size());   // two user clauses, each a field expansion
  EXPECT_EQ(0, asBool(b.optional[0]).min_match);  // expansion untouched
}

TEST_F(SimpleQueryParserTest, minMatchOnSingleClauseIsSilentlyInapplicable) {
  fields = {"title", "body"};
  minMatch = 2;
  // one user clause: its field expansion must NOT absorb min_match (that
  // would require the term in BOTH fields).  Whether min_match applies is
  // contingent on user input, so inapplicability costs no warning.
  auto r = parse("foo");
  const auto& b = asBool(*r.root);
  EXPECT_EQ(0, b.min_match);
  EXPECT_TRUE(r.warnings.empty());
}

TEST_F(SimpleQueryParserTest, minMatchOnRequiredClausesIsSilentlyInapplicable) {
  minMatch = 2;
  auto r = parse("foo +bar");
  const auto& b = asBool(*r.root);
  ASSERT_EQ(1u, b.required.size());  // +bar; foo is optional
  EXPECT_EQ(0, b.min_match);         // a required clause is present: inapplicable
  EXPECT_TRUE(r.warnings.empty());
}

// ---- quote-aware grouping ----

TEST_F(SimpleQueryParserTest, parensInsidePhrasesAreNotStructural) {
  // the ')' inside the phrase must not close the group
  auto r = parse("(\"a ) b\" c)");
  const auto& b = asBool(*r.root);
  ASSERT_EQ(2u, b.optional.size());
  const auto* p = std::get_if<api::PhraseQuery>(&b.optional[0].kind);
  ASSERT_NE(nullptr, p);
  EXPECT_EQ("a ) b", p->text);
  EXPECT_EQ("c", matchVal(b.optional[1]));

  // '(' inside a phrase does not open a group either
  auto r2 = parse("\"(a\" b");
  const auto& b2 = asBool(*r2.root);
  ASSERT_EQ(2u, b2.optional.size());
  EXPECT_NE(nullptr, std::get_if<api::PhraseQuery>(&b2.optional[0].kind));
}

TEST_F(SimpleQueryParserTest, unmatchedParenFloodStaysWellBehaved) {
  std::string q(1000, '(');
  q += "foo";
  auto r = parse(q);
  ASSERT_NE(nullptr, r.root);
  EXPECT_EQ("foo", matchVal(*r.root));
}

TEST_F(SimpleQueryParserTest, unmatchedQuoteDoesNotPoisonLaterGroups) {
  // the unterminated quote degrades and its contents reparse - the group
  // after it must still be a group (an unpaired quote is not a quote)
  auto r = parse("\"foo +(bar | baz)");
  const auto& b = asBool(*r.root);
  ASSERT_EQ(1u, b.required.size());  // the +(...) group
  ASSERT_EQ(1u, b.optional.size());  // bare foo
  EXPECT_EQ("foo", matchVal(b.optional[0]));
  const auto& inner = asBool(b.required[0]);
  EXPECT_EQ(2u, inner.optional.size());
}

TEST_F(SimpleQueryParserTest, escapedQuoteDoesNotProtectParens) {
  // an escaped quote is a literal character, so the group is structural
  auto r = parse("x\\\" (a b)");
  const auto& b = asBool(*r.root);
  ASSERT_EQ(2u, b.optional.size());
  EXPECT_EQ("x\"", matchVal(b.optional[0]));
  EXPECT_EQ(2u, asBool(b.optional[1]).optional.size());
}

TEST_F(SimpleQueryParserTest, minMatchCountsGroupsAsClauses) {
  minMatch = 2;
  auto r = parse("(a b) (c d)");
  const auto& b = asBool(*r.root);
  EXPECT_EQ(2, b.min_match);
  ASSERT_EQ(2u, b.optional.size());
  EXPECT_TRUE(r.warnings.empty());
}

TEST_F(SimpleQueryParserTest, minMatchAfterEmptyGroupIsSilentlyInapplicable) {
  minMatch = 2;
  auto r = parse("() a");  // the empty group is nothing; one clause remains
  EXPECT_EQ("a", matchVal(*r.root));
  EXPECT_TRUE(r.warnings.empty());
}

TEST_F(SimpleQueryParserTest, minMatchOnMatchAllIsSilentlyInapplicable) {
  minMatch = 2;
  auto r = parse("*");
  EXPECT_TRUE(std::holds_alternative<bool>(r.root->kind));
  EXPECT_TRUE(r.warnings.empty());
}

TEST_F(SimpleQueryParserTest, minMatchOnEmptyInputStaysSilent) {
  // "at least N of zero clauses" is unsatisfiable: matching nothing IS the
  // semantics honored, so no warning
  minMatch = 2;
  auto r = parse("   ");
  EXPECT_EQ(nullptr, r.root);
  EXPECT_TRUE(r.warnings.empty());
}

TEST_F(SimpleQueryParserTest, quotesOpenOnlyWhereAValueBegins) {
  // mid-token quotes are ordinary term bytes (apostrophe-safe search boxes)
  EXPECT_EQ("can\"t", matchVal(*parse("can\"t").root));

  auto r = parse("say\"hi there\"");
  const auto& b = asBool(*r.root);
  ASSERT_EQ(2u, b.optional.size());
  EXPECT_EQ("say\"hi", matchVal(b.optional[0]));
  EXPECT_EQ("there\"", matchVal(b.optional[1]));

  // ...but a quote still opens a phrase at a boundary or after one sign
  const auto* p = std::get_if<api::PhraseQuery>(&parse("\"foo bar\"").root->kind);
  ASSERT_NE(nullptr, p);
  const auto& neg = asBool(*parse("-\"foo bar\"").root);
  ASSERT_EQ(1u, neg.prohibited.size());
  EXPECT_NE(nullptr, std::get_if<api::PhraseQuery>(&neg.prohibited[0].kind));
}

TEST_F(SimpleQueryParserTest, unknownFieldQuotedValueKeepsPhrase) {
  // site: is not a field; the head stays literal text and the quoted value
  // is still a phrase over the default fields
  auto r = parse("site:\"foo bar\"");
  const auto& b = asBool(*r.root);
  ASSERT_EQ(2u, b.optional.size());
  EXPECT_EQ("site:", matchVal(b.optional[0]));
  const auto* p = std::get_if<api::PhraseQuery>(&b.optional[1].kind);
  ASSERT_NE(nullptr, p);
  EXPECT_EQ("foo bar", p->text);
}

TEST_F(SimpleQueryParserTest, unterminatedFieldedQuote) {
  // the unpaired quote is not a quote: the token continues without it
  // (this used to emit the phrase bytes twice and lose the field entirely)
  auto r = parse("status:\"abc");
  const auto& m = asMatch(*r.root);
  EXPECT_EQ("status", m.field);
  EXPECT_EQ("abc", matchVal(*r.root));
}

TEST_F(SimpleQueryParserTest, parenTableMatchesQuoteRule) {
  // a boundary quote protects parens inside the phrase...
  auto r = parse("(\"a )\" b)");
  const auto& g = asBool(*r.root);
  ASSERT_EQ(2u, g.optional.size());
  const auto* p = std::get_if<api::PhraseQuery>(&g.optional[0].kind);
  ASSERT_NE(nullptr, p);
  EXPECT_EQ("a )", p->text);

  // ...but a mid-token quote does not hide a real group behind it
  r = parse("say\"hi (a b)");
  const auto& t = asBool(*r.root);
  ASSERT_EQ(2u, t.optional.size());
  EXPECT_EQ("say\"hi", matchVal(t.optional[0]));
  EXPECT_EQ(2u, asBool(t.optional[1]).optional.size());
}
