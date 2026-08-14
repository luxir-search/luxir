#pragma once

#include <charconv>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory_resource>
#include <optional>
#include <span>
#include <stdexcept>
#include <string_view>
#include <variant>
#include <vector>

#include <fmt/format.h>

#include "luxir/api/build.h"
#include "luxir/api/luxir_types.hpp"
#include "luxir/util/Cursor.h"
#include "luxir/query/ExprFunctions.h"
#include "luxir/query/QueryBuilder.h"
#include "luxir/query/SimpleQueryParser.h"
#include "luxir/schema/Schema.h"
#include "luxir/value/ValueLex.h"

namespace luxir {

// Parser for "expr", the Luxir query language (the expr Query arm): the
// PROGRAMMER'S query string, with a rigorous grammar and precise byte-offset
// parse errors.  Raw end-user input belongs in simple_query, which never
// fails; expr never degrades.
//
// The language is STRUCTURE-ONLY sugar over the Query tree: fields, boolean
// combination, ranges, term decorations, and the function form.  The parser
// never interprets text - value bytes pass through to the emitted arm
// unanalyzed, and analysis happens at query build per field type.  Every
// expression has an exact structured equivalent (the emitted subtree).
//
// The parser is SCHEMA-AWARE but ANALYZER-BLIND: FieldType picks the natural
// arm (quoted text on an unanalyzed STRING/ID field is an exact-term Match,
// not a PhraseQuery; ranges over numeric/DATE columns emit the range arm),
// never the lexical shape of a value (zip:02134 stays a string because the
// schema says so - no number sniffing).
//
// Grammar summary (docs/guide/query-language.md is the full reference):
//   field:value  field:"a phrase"  field:'a phrase'  field:(boolean scope)
//   AND / OR / NOT with real precedence (NOT > AND > OR); +req / -prohib
//   prefixes; juxtaposed clauses are SHOULD.  AND/OR may not mix with +/- or
//   with juxtaposition at one level (parenthesize) - not Lucene QP's coin
//   flip.
//   field:[lo TO hi]  field:{lo TO hi}  (mixed brackets fine, * = open end)
//   field:>=v  field:>v  field:<=v  field:<v
//   term* (prefix)  term~N (fuzzy, bare ~ = AUTO)  field:* (has a value)
//   *:* (match all)  $name (a value bound in vars)  name(args...) (function
//   form: any query type by its JSON name, arguments by field name)
//
// Positional specials, not Lucene's reserved-char sprawl: ':' splits only at
// the FIRST unescaped colon of a clause, '*' and '~' act only as clean
// trailing suffixes, and quotes open a string only where a value can begin -
// so url:https://x, time:12:30, and title:don't need no escaping.  '^N' boosts
// a clause and '^=N' assigns it a constant score; elsewhere '^' is a literal
// byte.
// Mid-token '*' and '?' are PERMANENTLY literal (never wildcards): future
// wildcard/regex query types arrive as named functions, and quoting a value
// is the universal way to make it literal to the grammar.
//
// Unfielded bare words are a parse error naming the fixes (deleted from the
// language by design: expr has no default field - use simple_query for
// search-box strings).
//
// Byte-oriented lexing over a bounds-checked Cursor: every metachar is ASCII
// and UTF-8 is self-synchronizing, so multi-byte sequences pass through
// opaquely and the parser never reads outside the input.  The emitted tree is
// arena-allocated; token views point into the input string (which must
// outlive the tree - it views the kept-alive request bytes) or into the arena
// when escape processing rewrote them.
struct ExprOptions {
  // Arm selection by FieldType.  Required; the schema snapshot is immutable
  // for the request lifetime.
  Schema* schema = nullptr;
  // $name bindings (the expr message's vars).  Bound as VALUES: a var is
  // spliced into the tree as its Val, never re-parsed as syntax, so end-user
  // input can pass through safely.
  api::map_view<std::string_view, ::hpp_proto::indirect_view<api::Val>> vars;
  // Shared request-scoped nesting budget (ParseContext.nestingBudget): every
  // recursive construct debits the one counter the structured-query walk also
  // debits, so stacking parsers cannot reset the available depth.  nullptr =
  // parser-local default (tests).
  int* nestingBudget = nullptr;
};

class ExprParser {
  static constexpr int DEFAULT_NESTING_BUDGET = 128;
  static constexpr size_t NPOS = (size_t)-1;
  // Extra stop bytes inside argument lists; term/value tokens elsewhere treat
  // them as ordinary bytes (only quotes, parens, and whitespace are
  // structural everywhere).
  static constexpr std::string_view ARG_STOPS = ",)]=[";

  const ExprOptions& opts;
  std::pmr::memory_resource& mr;
  Cursor cur{std::string_view{}};
  int localBudget = DEFAULT_NESTING_BUDGET;
  int* budget;

  // The ambient field of an enclosing field:(...) scope; terms inside the
  // scope bind to it, and an inner field:... overrides it (Lucene-compatible
  // distribution: title:(a OR b) == title:a OR title:b).
  struct FieldScope {
    std::string_view field;
    FieldType* type;
  };

  enum class Conj : uint8_t { NONE, AND, OR };
  enum class Mod : uint8_t { NONE, PLUS, MINUS, NOT };

  struct Item {
    const api::Query* node;
    size_t pos;  // byte offset, for error messages
    Conj conj;   // connective BEFORE this item (NONE for the first)
    Mod mod;
  };

  struct DepthScope {
    ExprParser& p;
    DepthScope(ExprParser& p, size_t pos) : p(p) {
      // check before debiting: a throwing ctor runs no dtor, and the budget
      // must balance on unwind (it is shared across the request)
      if (*p.budget <= 0) {
        p.fail(pos, "expression nesting exceeds the supported depth");
      }
      --*p.budget;
    }
    ~DepthScope() { ++*p.budget; }
  };

public:
  ExprParser(const ExprOptions& opts, std::pmr::memory_resource& arena)
      : opts(opts), mr(arena), budget(opts.nestingBudget ? opts.nestingBudget : &localBudget) {}

  const api::Query* parse(std::string_view q) {
    cur = Cursor(q);
    cur.skipWs();
    if (cur.atEnd()) fail(0, "empty expression");
    const api::Query* root = parseLevel(nullptr, {});
    cur.skipWs();
    if (!cur.atEnd()) fail(cur.position(), "unexpected trailing input");
    return root;
  }

private:
  // ---- errors ----

  [[noreturn]] void fail(size_t pos, std::string_view msg) {
    size_t from = pos > 20 ? pos - 20 : 0;
    throw std::runtime_error(fmt::format("expr parse error at byte {}: {} (context: \"{}<HERE>{}\")",
                                         pos, msg, cur.slice(from, pos), cur.slice(pos, pos + 20)));
  }

  // ---- arena emission ----

  api::Query* allocQuery() {
    api::Query* p = (api::Query*)mr.allocate(sizeof(api::Query), alignof(api::Query));
    new (p) api::Query();
    return p;
  }

  api::Val* allocVal(std::string_view text) {
    api::Val* v = (api::Val*)mr.allocate(sizeof(api::Val), alignof(api::Val));
    new (v) api::Val();
    v->kind = text;
    return v;
  }

  std::string_view arenaStr(std::string_view s) { return api::build::arenaStr(mr, s); }

  std::span<const api::Query> querySpan(std::span<const api::Query* const> nodes) {
    if (nodes.empty()) return {};
    api::Query* arr =
        (api::Query*)mr.allocate(sizeof(api::Query) * nodes.size(), alignof(api::Query));
    for (size_t i = 0; i < nodes.size(); i++) {
      new (&arr[i]) api::Query(*nodes[i]);
    }
    return {arr, nodes.size()};
  }

  const api::Query* matchAll() {
    api::Query* q = allocQuery();
    q->kind = true;
    return q;
  }

  const api::Query* makeMatch(std::string_view field, const api::Val* val) {
    api::Match m;
    m.field = field;
    m.val = val;
    api::Query* q = allocQuery();
    q->kind = m;
    return q;
  }

  api::Query* copyQuery(const api::Query& source) {
    api::Query* q = allocQuery();
    *q = source;
    return q;
  }

  const api::Query* makeBoost(const api::Query* child, float boost) {
    api::BoostQuery b;
    b.query = copyQuery(*child);
    b.boost = boost;
    api::Query* q = allocQuery();
    q->kind = b;
    return q;
  }

  const api::Query* makeConstantScore(const api::Query* child, float score) {
    api::ConstantScoreQuery c;
    c.query = copyQuery(*child);
    c.score = score;
    api::Query* q = allocQuery();
    q->kind = c;
    return q;
  }

  // ---- schema consultation (arm selection only; text stays uninterpreted) ----

  static bool termQueryable(FieldType& ft) { return SimpleQueryParser::termQueryable(ft); }

  static bool numericQueryable(FieldType& ft) {
    return QueryBuilder::isNumericColumnType(ft.type()) && ft.hasColumn();
  }

  FieldType& resolveField(std::string_view name, size_t pos) {
    FieldType* ft = opts.schema->getFieldTypePtr(name);
    if (ft == nullptr) fail(pos, fmt::format("unknown field '{}'", name));
    return *ft;
  }

  void requireValueQueryable(std::string_view field, FieldType& ft, size_t pos) {
    if (!termQueryable(ft) && !numericQueryable(ft)) {
      fail(pos, fmt::format(
          "field '{}' does not support term, phrase, or range syntax", field));
    }
  }

  // ---- keywords / identifiers ----

  static bool identStart(char c) {
    return value::lex::identifierStart(c);
  }
  static bool identChar(char c) { return value::lex::identifierChar(c); }
  static bool digit(char c) { return value::lex::digit(c); }

  static bool isIdentifier(std::string_view s) {
    return value::lex::identifier(s);
  }

  static bool isStop(char c, std::string_view stops) {
    return stops.find(c) != std::string_view::npos;
  }

  // True when the input `ahead` bytes past the cursor begins a new token:
  // end of input, whitespace (the full U+3000 sequence, not just its lead
  // byte), a structural character, or a stop byte.  Real NUL bytes,
  // non-whitespace multi-byte sequences, and quotes are ordinary token
  // bytes here, so NOT-x / ANDroid / AND"x" are tokens, not operators.
  bool boundaryAt(size_t ahead, std::string_view stops) const {
    if (ahead >= cur.remaining()) return true;  // end of input
    char c = cur.peekAt(ahead);
    if (c == '(' || c == ')' || c == '[' || c == '{') return true;
    if (isStop(c, stops)) return true;
    return cur.wsLenAt(ahead) > 0;
  }

  bool consumeKeyword(std::string_view kw, std::string_view stops) {
    if (!cur.startsWith(kw) || !boundaryAt(kw.size(), stops)) return false;
    cur.advance(kw.size());
    return true;
  }

  // ---- token scanning ----

  // A scanned token: processed bytes (escapes applied) plus escape-aware
  // decoration analysis.  text views the input when no escape was seen, else
  // an arena copy.
  struct Token {
    std::string_view text;
    size_t pos = 0;           // byte offset of the first byte
    bool escaped = false;
    // clean trailing suffixes; mid-token occurrences are literal bytes
    size_t tildeAt = NPOS;    // '~' followed only by digits, or NPOS
    size_t caretAt = NPOS;    // score suffix '^N' / '^=N', or NPOS
    bool constantScore = false;
    bool negativeScore = false;
    bool doubleScoreDecoration = false;
    bool trailingStar = false;
    bool starBeforeTilde = false;  // ...*~N (rejected: prefix+fuzzy combine)
    bool empty() const { return text.empty(); }
  };

  // Quotes are deliberately NOT terminators: a quote is special only where a
  // value can BEGIN (operand / field-value / argument / endpoint position -
  // each dispatches on the quote before scanning a token).  Mid-token quotes
  // are ordinary bytes, so title:don't and say"hi" are single terms - the
  // positional-specials rule applied to quotes.
  bool tokenTerminator(char c, std::string_view stops) const {
    if (c == '(' || c == ')') return true;
    return isStop(c, stops);
  }

  // Scan a token: bytes up to whitespace / a paren / a stop byte.
  // '\' escapes the next byte (any byte).  When stopAtColon, the scan also
  // ends AT the first unescaped ':' without consuming it (the fielded-clause
  // boundary); otherwise ':' is an ordinary byte (positional specials: only
  // the FIRST colon of a clause is structural).
  Token scanToken(std::string_view stops, bool stopAtColon) {
    Token t;
    t.pos = cur.position();
    std::pmr::vector<char> buf(&mr);
    std::pmr::vector<uint8_t> esc(&mr);  // parallel: byte came from an escape
    while (!cur.atEnd() && cur.wsLen() == 0) {
      char c = cur.peek();
      if (c == '\\') {
        if (cur.remaining() < 2) fail(cur.position(), "dangling '\\' escape at end of input");
        cur.advance();
        buf.push_back(cur.peek());
        esc.push_back(1);
        cur.advance();
        t.escaped = true;
        continue;
      }
      if (tokenTerminator(c, stops)) break;
      if (stopAtColon && c == ':') break;
      buf.push_back(c);
      esc.push_back(0);
      cur.advance();
    }

    size_t n = buf.size();
    // clean trailing suffix: the last unescaped `mark` followed only by
    // unescaped bytes suffixOk accepts
    auto cleanSuffix = [&](size_t endAt, char mark, auto&& suffixOk) -> size_t {
      for (size_t i = endAt; i-- > 0;) {
        if (esc[i]) return NPOS;
        if (buf[i] == mark) return i;
        if (!suffixOk(buf[i])) return NPOS;
      }
      return NPOS;
    };
    t.caretAt = cleanSuffix(n, '^', [](char c) {
      return digit(c) || c == '.' || c == '=' || c == '-';
    });
    size_t termEnd = t.caretAt == NPOS ? n : t.caretAt;
    if (t.caretAt != NPOS) {
      size_t suffix = t.caretAt + 1;
      t.constantScore = suffix < n && buf[suffix] == '=';
      if (t.constantScore) suffix++;
      t.negativeScore = suffix < n && buf[suffix] == '-';
      t.doubleScoreDecoration =
          cleanSuffix(t.caretAt, '^', [](char c) {
            return digit(c) || c == '.' || c == '=' || c == '-';
          }) != NPOS;
    }
    t.trailingStar = termEnd > 0 && buf[termEnd - 1] == '*' && !esc[termEnd - 1];
    t.tildeAt = cleanSuffix(termEnd, '~', [](char c) { return digit(c); });
    if (t.tildeAt == NPOS) {
      // ~N.N is old Lucene float similarity: teach rather than silently
      // matching the literal token (a genuinely literal ~1.5 can be quoted)
      size_t floatTilde = cleanSuffix(termEnd, '~', [](char c) { return digit(c) || c == '.'; });
      if (floatTilde != NPOS && floatTilde + 1 < termEnd) {
        fail(t.pos + floatTilde,
             "fuzzy edit distance is a whole number of edits (term~1, term~2); "
             "float similarity is not supported - quote the value for a literal '~'");
      }
    }
    if (t.tildeAt != NPOS && t.tildeAt > 0 && buf[t.tildeAt - 1] == '*' && !esc[t.tildeAt - 1]) {
      t.starBeforeTilde = true;
    }

    t.text = t.escaped ? arenaStr(std::string_view(buf.data(), n))
                       : cur.slice(t.pos, cur.position());
    return t;
  }

  // Quoted string body: cursor sits on the opening quote (single or double;
  // identical semantics - single quotes just avoid JSON escaping).  The
  // escape set is deliberately small - \" \' \\ - any other byte after '\'
  // keeps the backslash literally.  Errors on an unterminated quote.
  std::string_view scanQuoted() {
    return value::lex::scanQuoted(cur, mr,
        [&](size_t pos, std::string_view msg) { fail(pos, msg); });
  }

  float parseScoreNumber(std::string_view text, size_t pos) {
    if (text.empty()) fail(pos, "score decoration requires a number after '^' or '^='");
    float value = 0.0f;
    auto [p, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (ec != std::errc() || p != text.data() + text.size() || !std::isfinite(value)) {
      fail(pos, fmt::format("score decoration expects a finite number (got '{}')", text));
    }
    if (value < 0.0f) fail(pos, "score decoration must not be negative");
    return value;
  }

  const api::Query* decorateToken(const api::Query* node, const Token& t) {
    if (t.caretAt == NPOS) return node;
    if (t.doubleScoreDecoration) {
      fail(t.pos + t.caretAt, "at most one score decoration per clause");
    }
    size_t valueAt = t.caretAt + 1 + (t.constantScore ? 1 : 0);
    std::string_view number = t.text.substr(valueAt);
    if (t.negativeScore) {
      fail(t.pos + valueAt, "score decoration must not be negative");
    }
    float value = parseScoreNumber(number, t.pos + valueAt);
    return t.constantScore ? makeConstantScore(node, value) : makeBoost(node, value);
  }

  // After a group/phrase/range/$var/function: consume its one legal score
  // decoration, while retaining precise errors for unrelated suffixes.
  const api::Query* finishDecorations(const api::Query* node, std::string_view what) {
    char c = cur.peek();
    if (c == '~') fail(cur.position(), fmt::format("'~' does not apply to {}", what));
    if (c == '^') {
      cur.advance();
      bool constant = cur.consume('=');  // longest match: '^=' before '^'
      size_t numberAt = cur.position();
      if (cur.peek() == '-') {
        fail(numberAt, "score decoration must not be negative");
      }
      std::string_view number = cur.takeWhile([](char b) { return digit(b) || b == '.'; });
      float value = parseScoreNumber(number, numberAt);
      node = constant ? makeConstantScore(node, value) : makeBoost(node, value);
      if (cur.peek() == '^') {
        fail(cur.position(), "at most one score decoration per clause");
      }
      c = cur.peek();
    }
    if (c == '*') fail(cur.position(), fmt::format("'*' does not apply to {}", what));
    return node;
  }

  // ---- $var binding ----

  // Cursor sits on '$'.  Returns the bound Val (bind-as-values: never
  // re-parsed as syntax).
  const api::Val* parseVarRef() {
    size_t pos = cur.position();
    std::string_view name = value::lex::scanVariable(cur);
    if (name.empty()) fail(pos, "'$' must be followed by a variable name");
    const ::hpp_proto::indirect_view<api::Val>* v = opts.vars.find(name);
    if (v == nullptr) {
      fail(pos, fmt::format("undefined variable ${} (bind it in the expr 'vars' map)", name));
    }
    return v->pointer();
  }

  // ---- levels: juxtaposition / AND / OR / NOT / +/- ----

  const api::Query* parseLevel(const FieldScope* scope, std::string_view stops) {
    DepthScope depth(*this, cur.position());
    std::pmr::vector<Item> items(&mr);

    for (;;) {
      cur.skipWs();
      if (cur.atEnd() || isStop(cur.peek(), stops)) break;

      size_t itemPos = cur.position();
      Conj conj = Conj::NONE;
      if (consumeKeyword("AND", stops)) {
        conj = Conj::AND;
      } else if (consumeKeyword("OR", stops)) {
        conj = Conj::OR;
      }
      if (conj != Conj::NONE) {
        if (items.empty()) fail(itemPos, "expression cannot start with AND/OR");
        cur.skipWs();
        itemPos = cur.position();
        if (cur.atEnd() || isStop(cur.peek(), stops)) {
          fail(itemPos, "expected a clause after AND/OR");
        }
      }

      Mod mod = Mod::NONE;
      if (consumeKeyword("NOT", stops)) {
        mod = Mod::NOT;
        cur.skipWs();
        bool nextIsNot = cur.startsWith("NOT") && boundaryAt(3, stops);
        if (cur.peek() == '+' || (cur.peek() == '-' && !minusSignOfNumber(scope)) || nextIsNot) {
          fail(cur.position(), "at most one of +/-/NOT per clause");
        }
        if (cur.atEnd() || isStop(cur.peek(), stops)) {
          fail(cur.position(), "expected a clause after NOT");
        }
      } else if ((cur.peek() == '+' || cur.peek() == '-') && !minusSignOfNumber(scope)) {
        mod = cur.peek() == '+' ? Mod::PLUS : Mod::MINUS;
        cur.advance();
        // a sign binds only when adjacent to its clause
        if (cur.atEnd() || cur.wsLen() > 0 || isStop(cur.peek(), stops)) {
          fail(itemPos, fmt::format("expected a clause immediately after '{}'",
                                    mod == Mod::PLUS ? '+' : '-'));
        }
      }

      const api::Query* node = parseOperand(scope, stops);
      items.push_back({node, itemPos, conj, mod});
    }

    return buildLevel(items);
  }

  // In a NUMERIC field's scope, a '-' right before a number binds tightest -
  // it is the value's sign, not an exclusion: year_i:(-5) matches -5.
  // Prohibition there is spelled NOT ("NOT 5").  '+' stays an operator: a
  // leading plus is not part of any numeric literal the coercion accepts,
  // and "+5 required" happens to mean what the writer meant anyway.
  bool minusSignOfNumber(const FieldScope* scope) const {
    if (cur.peek() != '-' || scope == nullptr || !numericQueryable(*scope->type)) return false;
    char next = cur.peekAt(1);
    return digit(next) || next == '.';
  }

  // Fold one level's items into a query node.
  //
  // Two modes per level, never mixed (Lucene QP's operator/prefix interaction
  // was a coin flip; here mixing is a parse error):
  //  * infix: clauses joined by AND/OR, NOT as the unary operator, real
  //    precedence NOT > AND > OR.  +/- are rejected.
  //  * juxtaposition: whitespace-separated clauses with optional +/-/NOT
  //    prefixes folding into ONE flat boolean (bare = optional, + = required,
  //    -/NOT = prohibited) - the classic search-box model.
  // A purely negative level gets a match-all optional injected, so NOT a /
  // -a means "everything except a" in the canonical parser output. The engine
  // supplies the same complement semantics for raw prohibited-only booleans.
  const api::Query* buildLevel(std::span<const Item> items) {
    if (items.empty()) return nullptr;

    bool infix = false;
    for (const Item& it : items) {
      if (it.conj != Conj::NONE) infix = true;
    }

    if (!infix) {
      if (items.size() == 1 && items[0].mod == Mod::NONE) {
        return items[0].node;
      }
      // Juxtaposed comparisons/ranges on ONE field would combine as SHOULD:
      // matching EITHER side of count:(>5 <10) is almost never what the
      // writer meant, and the mistake hides (nearly every doc with a value
      // matches).  Make them pick an operator.  Different fields, required
      // (+), and prohibited ranges compose deliberately and stay legal.
      for (size_t i = 0; i < items.size(); i++) {
        if (items[i].mod != Mod::NONE) continue;
        const auto* r = std::get_if<api::RangeQuery>(&items[i].node->kind);
        if (r == nullptr) continue;
        for (size_t j = 0; j < i; j++) {
          if (items[j].mod != Mod::NONE) continue;
          const auto* prev = std::get_if<api::RangeQuery>(&items[j].node->kind);
          if (prev != nullptr && prev->field == r->field) {
            fail(items[i].pos,
                 "juxtaposed ranges/comparisons on one field combine as OR (either may "
                 "match); write AND or OR between them");
          }
        }
      }
      std::pmr::vector<const api::Query*> required(&mr), optional(&mr), prohibited(&mr);
      for (const Item& it : items) {
        switch (it.mod) {
          case Mod::PLUS: required.push_back(it.node); break;
          case Mod::MINUS:
          case Mod::NOT: prohibited.push_back(it.node); break;
          default: optional.push_back(it.node); break;
        }
      }
      return makeBoolean(required, optional, prohibited);
    }

    for (size_t i = 1; i < items.size(); i++) {
      if (items[i].conj == Conj::NONE) {
        fail(items[i].pos,
             "mix of AND/OR and juxtaposed clauses at one level; add the operator or parentheses");
      }
    }
    for (const Item& it : items) {
      if (it.mod == Mod::PLUS || it.mod == Mod::MINUS) {
        fail(it.pos,
             "+/- prefixes cannot mix with AND/OR at the same level; use NOT or parentheses");
      }
    }

    // precedence: split at OR into AND-runs
    std::pmr::vector<const api::Query*> orClauses(&mr);
    size_t runStart = 0;
    for (size_t i = 1; i <= items.size(); i++) {
      if (i == items.size() || items[i].conj == Conj::OR) {
        orClauses.push_back(buildAndRun(items.subspan(runStart, i - runStart)));
        runStart = i;
      }
    }
    if (orClauses.size() == 1) return orClauses[0];
    std::pmr::vector<const api::Query*> none(&mr), empty(&mr);
    return makeBoolean(none, orClauses, empty);
  }

  // An AND-joined run: positives are required, NOT'd clauses prohibited, in
  // one boolean node ("a AND NOT b" is the natural required+prohibited pair).
  const api::Query* buildAndRun(std::span<const Item> run) {
    if (run.size() == 1 && run[0].mod != Mod::NOT) return run[0].node;
    std::pmr::vector<const api::Query*> required(&mr), optional(&mr), prohibited(&mr);
    for (const Item& it : run) {
      (it.mod == Mod::NOT ? prohibited : required).push_back(it.node);
    }
    return makeBoolean(required, optional, prohibited);
  }

  const api::Query* makeBoolean(std::pmr::vector<const api::Query*>& required,
                                std::pmr::vector<const api::Query*>& optional,
                                std::pmr::vector<const api::Query*>& prohibited) {
    if (required.empty() && optional.empty()) {
      required.push_back(matchAll());  // purely negative, non-scoring carrier
    }
    api::BooleanQuery bq;
    bq.required = querySpan(required);
    bq.optional = querySpan(optional);
    bq.prohibited = querySpan(prohibited);
    api::Query* q = allocQuery();
    q->kind = bq;
    return q;
  }

  // ---- operands ----

  const api::Query* parseOperand(const FieldScope* scope, std::string_view stops) {
    size_t pos = cur.position();
    char c = cur.peek();

    if (c == '(') {
      cur.advance();
      const api::Query* node = parseLevel(scope, ")");
      if (!cur.consume(')')) fail(pos, "unmatched '('");
      if (node == nullptr) fail(pos, "empty parentheses");
      return finishDecorations(node, "a group");
    }
    if (c == '"' || c == '\'') {
      if (scope == nullptr) {
        fail(pos,
             "unfielded phrase (expr has no default field): write field:\"...\" or use "
             "simple_query for search-box input");
      }
      return parsePhraseForm(scope->field, *scope->type);
    }
    if (c == '[' || c == '{') {
      if (scope == nullptr) fail(pos, "a range needs a field: field:[low TO high]");
      return parseRangeForm(scope->field, *scope->type);
    }
    if (c == '<' || c == '>') {
      if (scope == nullptr) fail(pos, "a comparison needs a field: field:>=value");
      return parseComparisonForm(scope->field, *scope->type, stops);
    }
    if (c == '$') {
      if (scope == nullptr) {
        fail(pos, "a $variable is a value, not a clause; use field:$name or a function argument");
      }
      requireValueQueryable(scope->field, *scope->type, pos);
      const api::Val* val = parseVarRef();
      return finishDecorations(makeMatch(scope->field, val), "a $variable");
    }
    if (c == ')' || c == ',' || c == ']' || c == '}' || c == '=') {
      fail(pos, fmt::format("unexpected '{}'", c));
    }

    // *:* - the traditional match-all spelling
    if (c == '*' && cur.peekAt(1) == ':' && cur.peekAt(2) == '*'
        && (boundaryAt(3, stops) || cur.peekAt(3) == '^')) {
      cur.advance(3);
      return finishDecorations(matchAll(), "match-all");
    }

    Token head = scanToken(stops, /*stopAtColon=*/true);
    if (cur.peek() == ':' && !cur.atEnd()) {
      if (head.empty()) fail(pos, "expected a field name before ':'");
      cur.advance();  // past ':'
      FieldType& ft = resolveField(head.text, head.pos);
      return parseValueForm(head.text, ft, stops);
    }

    if (head.empty()) fail(pos, fmt::format("unexpected '{}'", cur.peek()));

    // a keyword in operand position can only be user confusion (AND/OR here
    // means a connective just preceded: "a AND OR b")
    if (head.text == "TO" || head.text == "AND" || head.text == "OR" || head.text == "NOT") {
      fail(head.pos,
           fmt::format("'{}' cannot be used as a term; quote or escape it", head.text));
    }

    // function call: identifier immediately followed by '('
    if (cur.peek() == '(' && !head.escaped && isIdentifier(head.text)) {
      return finishDecorations(parseFunction(head), "a function call");
    }

    if (scope != nullptr) {
      return emitTerm(scope->field, *scope->type, head);
    }
    fail(head.pos,
         fmt::format("unfielded term '{}' (expr has no default field): write field:{}, "
                     "match(..., field=...), or use simple_query for search-box input",
                     head.text, head.text));
  }

  // ---- fielded value forms ----

  const api::Query* parseValueForm(std::string_view field, FieldType& ft, std::string_view stops) {
    char c = cur.peek();
    size_t pos = cur.position();
    if (c == '(') {
      cur.advance();
      FieldScope scope{field, &ft};
      const api::Query* node = parseLevel(&scope, ")");
      if (!cur.consume(')')) fail(pos, "unmatched '(' in field group");
      if (node == nullptr) fail(pos, "empty field group");
      return finishDecorations(node, "a group");
    }
    if (c == '"' || c == '\'') return parsePhraseForm(field, ft);
    if (c == '[' || c == '{') return parseRangeForm(field, ft);
    if (c == '<' || c == '>') return parseComparisonForm(field, ft, stops);
    if (c == '$') {
      requireValueQueryable(field, ft, pos);
      const api::Val* val = parseVarRef();
      return finishDecorations(makeMatch(field, val), "a $variable");
    }

    Token value = scanToken(stops, /*stopAtColon=*/false);
    if (value.empty()) fail(pos, fmt::format("expected a value after '{}:'", field));
    return emitTerm(field, ft, value);
  }

  // A quoted value: the natural arm per field type.  TEXT gets a positional
  // phrase; unanalyzed STRING/ID matches the whole text as one exact term;
  // numeric columns match the exact value (the quotes only delimit).
  const api::Query* parsePhraseForm(std::string_view field, FieldType& ft) {
    requireValueQueryable(field, ft, cur.position());
    std::string_view body = scanQuoted();
    int32_t slop = 0;
    if (cur.peek() == '~') {
      size_t slopPos = cur.position();
      cur.advance();
      size_t digitsPos = cur.position();
      std::string_view digits = cur.takeWhile([](char c) { return digit(c); });
      if (digits.empty()) {
        fail(slopPos, "phrase slop requires a nonnegative integer after '~'");
      }
      auto [p, ec] = std::from_chars(digits.data(), digits.data() + digits.size(), slop);
      if (ec != std::errc() || p != digits.data() + digits.size()) {
        fail(digitsPos, "phrase slop exceeds INT_MAX");
      }
      char next = cur.peek();
      if (!cur.atEnd() && cur.wsLen() == 0 && next != ')' && next != '^'
          && next != '*' && next != '~') {
        fail(cur.position(), "malformed phrase slop suffix");
      }
      if (ft.type() != FieldType::Type::TEXT) {
        fail(slopPos, fmt::format("phrase slop does not apply to non-TEXT field '{}'", field));
      }
    }
    const api::Query* node;
    if (ft.type() == FieldType::Type::TEXT) {
      api::PhraseQuery p;
      p.field = field;
      p.text = body;
      p.slop = slop;
      api::Query* q = allocQuery();
      q->kind = p;
      node = q;
    } else {
      node = makeMatch(field, allocVal(body));
    }
    return finishDecorations(node, "a quoted value");
  }

  // ---- ranges and comparisons ----
  // Any queryable field takes a range (resolveField already gated
  // queryability): numeric/date columns get the numeric arm's semantics,
  // term-backed fields a byte-order term range - the builder decides.

  // One range endpoint: a bare token, a quoted value, or $var; '*' = open end
  // (returns nullptr).  Values stay strings - the field type coerces them at
  // build (the same contract ingest uses), so dates, floats, and ints all
  // find what was indexed.
  const api::Val* parseRangeEndpoint(std::string_view stops) {
    cur.skipWs();
    size_t pos = cur.position();
    char c = cur.peek();
    if (c == '$') return parseVarRef();
    if (c == '"' || c == '\'') return allocVal(scanQuoted());
    Token t = scanToken(stops, /*stopAtColon=*/false);
    if (t.empty() || (t.text == "TO" && !t.escaped)) {
      fail(pos, "expected a range endpoint (or * for an open end)");
    }
    if (t.text == "*" && !t.escaped) return nullptr;  // open end
    return allocVal(t.text);
  }

  const api::Query* parseRangeForm(std::string_view field, FieldType& ft) {
    requireValueQueryable(field, ft, cur.position());
    bool loInclusive = cur.peek() == '[';
    cur.advance();

    const api::Val* lo = parseRangeEndpoint(",]}");
    cur.skipWs();
    if (!consumeKeyword("TO", "]}")) {
      fail(cur.position(), "expected TO in range (field:[low TO high])");
    }
    const api::Val* hi = parseRangeEndpoint(",]}");

    cur.skipWs();
    bool hiInclusive;
    if (cur.consume(']')) {
      hiInclusive = true;
    } else if (cur.consume('}')) {
      hiInclusive = false;
    } else {
      fail(cur.position(), "unterminated range: expected ']' or '}'");
    }
    api::RangeQuery r;
    r.field = field;
    if (lo != nullptr) (loInclusive ? r.gte : r.gt) = lo;
    if (hi != nullptr) (hiInclusive ? r.lte : r.lt) = hi;
    api::Query* q = allocQuery();
    q->kind = r;
    return finishDecorations(q, "a range");
  }

  const api::Query* parseComparisonForm(std::string_view field, FieldType& ft,
                                        std::string_view stops) {
    requireValueQueryable(field, ft, cur.position());
    char op = cur.peek();
    cur.advance();
    bool orEqual = cur.consume('=');
    cur.skipWs();
    if (cur.peek() == '*') {
      fail(cur.position(), "a comparison needs a value (use field:* for existence)");
    }
    const api::Val* v = parseRangeEndpoint(stops);
    if (v == nullptr) fail(cur.position(), "a comparison needs a value");
    api::RangeQuery r;
    r.field = field;
    if (op == '>') {
      (orEqual ? r.gte : r.gt) = v;
    } else {
      (orEqual ? r.lte : r.lt) = v;
    }
    api::Query* q = allocQuery();
    q->kind = r;
    return finishDecorations(q, "a comparison");
  }

  // ---- term emission (decorations + FieldType arm selection) ----

  const api::Query* emitTerm(std::string_view field, FieldType& ft, const Token& t) {
    std::string_view text = t.caretAt == NPOS ? t.text : t.text.substr(0, t.caretAt);

    // fuzzy: a clean trailing ~N / ~ suffix (mid-token '~' is a literal byte)
    if (t.tildeAt != NPOS) {
      if (t.starBeforeTilde) fail(t.pos, "cannot combine '*' and '~' on one term");
      std::string_view base = text.substr(0, t.tildeAt);
      std::string_view digits = text.substr(t.tildeAt + 1);
      if (base.empty()) fail(t.pos, "expected a term before '~'");
      std::optional<int32_t> maxEdits;  // unset = AUTO by term length
      if (!digits.empty()) {
        int v = 0;
        auto [p, ec] = std::from_chars(digits.data(), digits.data() + digits.size(), v);
        if (ec != std::errc() || v > 2) {
          fail(t.pos + t.tildeAt,
               fmt::format("fuzzy edit distance '{}' exceeds the maximum of 2", digits));
        }
        if (v == 0) {
          return decorateToken(makeMatch(field, allocVal(base)), t);  // ~0 = exact
        }
        maxEdits = v;
      }
      if (!termQueryable(ft)) {
        fail(t.pos, fmt::format("fuzzy ('~') does not apply to numeric field '{}'", field));
      }
      api::FuzzyQuery fq;
      fq.field = field;
      fq.term = base;
      fq.max_edits = maxEdits;
      api::Query* q = allocQuery();
      q->kind = fq;
      return decorateToken(q, t);
    }

    // field:* - a value was supplied for this field. QueryBuilder centrally
    // validates that the field is indexed or column-stored.
    if (text == "*" && !t.escaped) {
      api::ExistsQuery e;
      e.field = field;
      api::Query* q = allocQuery();
      q->kind = e;
      return decorateToken(q, t);
    }

    requireValueQueryable(field, ft, t.pos);

    // prefix: an unescaped trailing '*' (mid-token '*' is a literal byte)
    if (t.trailingStar) {
      if (!termQueryable(ft)) {
        fail(t.pos, fmt::format("prefix ('*') does not apply to numeric field '{}'", field));
      }
      api::PrefixQuery p;
      p.field = field;
      p.prefix = text.substr(0, text.size() - 1);
      api::Query* q = allocQuery();
      q->kind = p;
      return decorateToken(q, t);
    }

    return decorateToken(makeMatch(field, allocVal(text)), t);
  }

  // ---- the function form: name(main value, arg=value, ...) ----

  const api::Query* parseFunction(const Token& name) {
    DepthScope depth(*this, name.pos);
    cur.advance();  // past '('

    if (name.text == "expr") {
      fail(name.pos, "expr is not callable within expr; write the expression inline");
    }
    if (name.text == "all") {
      cur.skipWs();
      if (!cur.consume(')')) fail(cur.position(), "all() takes no arguments");
      return matchAll();
    }

    api::Query* q = allocQuery();
    bool known =
        expr::withCallableArm(*q, name.text, [&](auto& arm) {
          parseArgs(name.text, arm);
          if constexpr (std::is_same_v<std::remove_cvref_t<decltype(arm)>,
                                       api::RescoreQuery>) {
            arm.vars = opts.vars;
          }
        });
    if (!known) fail(name.pos, fmt::format("unknown query function '{}'", name.text));
    return q;
  }

  template <typename Arm>
  void parseArgs(std::string_view fn, Arm& arm) {
    cur.skipWs();
    if (cur.consume(')')) return;

    bool sawNamed = false;
    int positionalCount = 0;
    std::pmr::vector<std::string_view> seen(&mr);
    for (;;) {
      cur.skipWs();
      size_t argPos = cur.position();
      if (cur.atEnd()) fail(argPos, fmt::format("unterminated {}(...) call", fn));

      std::string_view argName = tryNamedArg();
      if (!argName.empty()) {
        sawNamed = true;
        for (std::string_view s : seen) {
          if (s == argName) fail(argPos, fmt::format("duplicate argument '{}'", argName));
        }
        seen.push_back(argName);
        bool found = expr::withMember(
            arm, argName, [&](auto& member) { assignArg(fn, argName, argPos, member); });
        if (!found) fail(argPos, fmt::format("unknown argument '{}' for {}()", argName, fn));
      } else {
        if (sawNamed) fail(argPos, "positional argument after named arguments");
        std::string_view arg = positionalCount == 0
            ? expr::mainValueArg(fn) : expr::valueExprArg(fn);
        if (arg.empty()) {
          if (positionalCount != 0) {
            fail(argPos, fmt::format(
                "{}() takes one positional argument; name the others", fn));
          }
          fail(argPos, fmt::format("{}() takes only named arguments (name=value)", fn));
        }
        positionalCount++;
        bool found = expr::withMember(
            arm, arg, [&](auto& member) { assignPositional(fn, arg, argPos, member); });
        if (!found) fail(argPos, fmt::format(
            "internal: {}() positional value '{}' not found", fn, arg));
      }

      cur.skipWs();
      auto separator = value::lex::consumeListSeparator(cur);
      if (separator == value::lex::ListSeparator::CLOSE) return;
      if (separator != value::lex::ListSeparator::COMMA) {
        fail(cur.position(), fmt::format("expected ',' or ')' in {}(...)", fn));
      }
    }
  }

  // Named-argument lookahead: identifier (ws) '=' - consumed when it matches,
  // fully restored otherwise, so raw text like "a=b" only becomes an argument
  // when 'a' really is one (and then errors as unknown, teaching the quote).
  std::string_view tryNamedArg() {
    size_t save = cur.position();
    std::string_view name = value::lex::scanIdentifier(cur);
    if (name.empty()) {
      cur.seek(save);
      return {};
    }
    cur.skipWs();
    if (!cur.consume('=')) {
      cur.seek(save);
      return {};
    }
    return name;
  }

  template <class T>
  static constexpr bool isOptional = false;
  template <class T>
  static constexpr bool isOptional<std::optional<T>> = true;

  // Typed argument assignment: the value grammar follows the member's proto
  // type (this is where "arguments are typed" lives).  Anything the string
  // language cannot express (vectors, binary terms) is a precise error
  // pointing at the structured form.
  template <typename M>
  void assignArg(std::string_view fn, std::string_view argName, size_t argPos, M& member) {
    using T = std::remove_cvref_t<M>;
    if constexpr (std::is_same_v<T, std::string_view>) {
      member = argName == expr::valueExprArg(fn)
          ? parseRawText(fn) : parseStringValue(argName);
    } else if constexpr (std::is_same_v<T, int32_t> || std::is_same_v<T, int64_t> ||
                         std::is_same_v<T, float> || std::is_same_v<T, double>) {
      member = parseNumberValue<T>(argName);
    } else if constexpr (std::is_same_v<T, bool>) {
      member = parseBoolValue(argName);
    } else if constexpr (std::is_same_v<T, api::Match_::Operator>) {
      member = parseOperatorValue(argName);
    } else if constexpr (isOptional<T>) {
      assignArg(fn, argName, argPos, member.emplace());
    } else if constexpr (std::is_same_v<T, ::hpp_proto::optional_indirect_view<api::Val>>) {
      member = parseValValue(argName);
    } else if constexpr (std::is_same_v<T, ::hpp_proto::optional_indirect_view<api::Query>>) {
      member = parseQueryValue(argName, ",)");
    } else if constexpr (std::is_same_v<T, std::span<const std::string_view>>) {
      member = parseStringList(argName);
    } else if constexpr (std::is_same_v<T, std::span<const api::Query>>) {
      member = parseQueryList(argName);
    } else if constexpr (std::is_same_v<T, std::span<const int32_t>>) {
      member = parseIntList(argName);
    } else {
      fail(argPos, fmt::format("argument '{}' of {}() has a type the expr language cannot "
                               "express; use the structured form",
                               argName, fn));
    }
  }

  // The declared positional slot.  Parse form follows the member type: a
  // sub-expression for Query slots (constant_score(status:active)), raw text
  // for string/Val slots - where quotes PROTECT (delimit) but never MEAN:
  // match("foo bar") and match(foo bar) deliver the same bytes, and phrase
  // semantics come from calling phrase(), not from quoting (in TERM position
  // quotes do mean - two positions, two grammars).
  template <typename M>
  void assignPositional(std::string_view fn, std::string_view argName, size_t argPos, M& member) {
    using T = std::remove_cvref_t<M>;
    if constexpr (std::is_same_v<T, ::hpp_proto::optional_indirect_view<api::Query>>) {
      member = parseQueryValue(argName, ",)");
    } else if constexpr (std::is_same_v<T, ::hpp_proto::optional_indirect_view<api::Val>>) {
      const api::Val* whole = tryWholeVarValue();
      member = whole != nullptr ? whole : allocVal(parseRawText(fn));
    } else if constexpr (std::is_same_v<T, std::string_view>) {
      const api::Val* whole = tryWholeVarValue();
      member = whole != nullptr ? varString(*whole, argName) : parseRawText(fn);
    } else {
      fail(argPos, fmt::format("{}() does not take a positional argument", fn));
    }
  }

  // ---- typed argument values ----

  // A $name that IS the whole value (followed by ',' / ')' / ']'), or null
  // with the cursor restored.  Mid-text '$' stays literal ("costs $5").
  const api::Val* tryWholeVarValue() {
    cur.skipWs();
    if (cur.peek() != '$') return nullptr;
    size_t save = cur.position();
    const api::Val* v = parseVarRef();
    size_t after = cur.position();
    cur.skipWs();
    char c = cur.peek();
    cur.seek(after);
    if (c == ',' || c == ')' || c == ']' || c == '\0') return v;
    cur.seek(save);
    return nullptr;
  }

  std::string_view varString(const api::Val& v, std::string_view argName) {
    if (const auto* s = std::get_if<std::string_view>(&v.kind)) return *s;
    fail(cur.position(), fmt::format("variable for '{}' is not a string", argName));
  }

  std::string_view parseStringValue(std::string_view argName) {
    cur.skipWs();
    size_t pos = cur.position();
    char c = cur.peek();
    if (c == '"' || c == '\'') return scanQuoted();
    if (c == '$') return varString(*parseVarRef(), argName);
    Token t = scanToken(ARG_STOPS, /*stopAtColon=*/false);
    if (t.empty()) fail(pos, fmt::format("expected a value for '{}'", argName));
    return t.text;
  }

  template <typename T>
  T parseNumberValue(std::string_view argName) {
    cur.skipWs();
    size_t pos = cur.position();
    if (cur.peek() == '$') {
      const api::Val& v = *parseVarRef();
      if (const auto* i = std::get_if<int64_t>(&v.kind)) {
        if constexpr (std::is_same_v<T, int32_t>) {
          if (*i < std::numeric_limits<int32_t>::min() || *i > std::numeric_limits<int32_t>::max()) {
            fail(pos, fmt::format("variable for '{}' is out of range", argName));
          }
        }
        return (T)*i;
      }
      if constexpr (std::is_floating_point_v<T>) {
        if (const auto* d = std::get_if<double>(&v.kind)) return (T)*d;
      }
      fail(pos, fmt::format("variable for '{}' is not a number", argName));
    }
    std::string_view text = value::lex::scanNumber(cur);
    T out{};
    auto [p, ec] = std::from_chars(text.data(), text.data() + text.size(), out);
    bool delimited = cur.atEnd() || cur.wsLen() > 0 || ARG_STOPS.find(cur.peek()) != std::string_view::npos;
    if (text.empty() || !delimited || ec != std::errc() || p != text.data() + text.size() ||
        (std::is_floating_point_v<T> && !std::isfinite(out))) {
      fail(pos, fmt::format("argument '{}' expects a number (finite value required)", argName));
    }
    return out;
  }

  bool parseBoolValue(std::string_view argName) {
    cur.skipWs();
    size_t pos = cur.position();
    if (cur.peek() == '$') {
      const api::Val& v = *parseVarRef();
      if (const auto* b = std::get_if<bool>(&v.kind)) return *b;
      fail(pos, fmt::format("variable for '{}' is not a boolean", argName));
    }
    Token t = scanToken(ARG_STOPS, /*stopAtColon=*/false);
    if (t.text == "true") return true;
    if (t.text == "false") return false;
    fail(pos, fmt::format("argument '{}' expects true or false (got '{}')", argName, t.text));
  }

  api::Match_::Operator parseOperatorValue(std::string_view argName) {
    cur.skipWs();
    size_t pos = cur.position();
    std::string_view text;
    if (cur.peek() == '$') {
      text = varString(*parseVarRef(), argName);
    } else {
      text = scanToken(ARG_STOPS, /*stopAtColon=*/false).text;
    }
    if (text == "AND") return api::Match_::Operator::AND;
    if (text == "OR") return api::Match_::Operator::OR;
    fail(pos, fmt::format("argument '{}' expects AND or OR (got '{}')", argName, text));
  }

  const api::Val* parseValValue(std::string_view argName) {
    cur.skipWs();
    size_t pos = cur.position();
    char c = cur.peek();
    if (c == '$') return parseVarRef();
    if (c == '"' || c == '\'') return allocVal(scanQuoted());
    if (c == '[') fail(pos, fmt::format("argument '{}' does not take a list", argName));
    Token t = scanToken(ARG_STOPS, /*stopAtColon=*/false);
    if (t.empty()) fail(pos, fmt::format("expected a value for '{}'", argName));
    return allocVal(t.text);
  }

  const api::Query* parseQueryValue(std::string_view argName, std::string_view stops) {
    cur.skipWs();
    size_t pos = cur.position();
    const api::Query* node = parseLevel(nullptr, stops);
    if (node == nullptr) fail(pos, fmt::format("expected a query expression for '{}'", argName));
    return node;
  }

  std::span<const std::string_view> parseStringList(std::string_view argName) {
    cur.skipWs();
    size_t pos = cur.position();
    if (cur.peek() == '$') {
      const api::Val& v = *parseVarRef();
      if (const auto* arr = std::get_if<api::ArrStr>(&v.kind)) return arr->v;
      fail(pos, fmt::format("variable for '{}' is not a string array", argName));
    }
    if (!cur.consume('[')) {
      fail(pos, fmt::format("argument '{}' expects a list: {}=[a, b]", argName, argName));
    }
    std::pmr::vector<std::string_view> vals(&mr);
    cur.skipWs();
    if (!cur.consume(']')) {
      for (;;) {
        vals.push_back(parseStringValue(argName));
        cur.skipWs();
        if (cur.consume(']')) break;
        if (!cur.consume(',')) fail(cur.position(), "expected ',' or ']' in list");
      }
    }
    if (vals.empty()) return {};
    auto* arr = (std::string_view*)mr.allocate(sizeof(std::string_view) * vals.size(),
                                               alignof(std::string_view));
    for (size_t i = 0; i < vals.size(); i++) new (&arr[i]) std::string_view(vals[i]);
    return {arr, vals.size()};
  }

  std::span<const api::Query> parseQueryList(std::string_view argName) {
    cur.skipWs();
    size_t pos = cur.position();
    if (!cur.consume('[')) {
      fail(pos, fmt::format("argument '{}' expects a list of queries: {}=[...]", argName, argName));
    }
    std::pmr::vector<const api::Query*> nodes(&mr);
    cur.skipWs();
    if (!cur.consume(']')) {
      for (;;) {
        nodes.push_back(parseQueryValue(argName, ",]"));
        cur.skipWs();
        if (cur.consume(']')) break;
        if (!cur.consume(',')) fail(cur.position(), "expected ',' or ']' in list");
      }
    }
    return querySpan(nodes);
  }

  std::span<const int32_t> parseIntList(std::string_view argName) {
    cur.skipWs();
    size_t pos = cur.position();
    if (!cur.consume('[')) {
      fail(pos, fmt::format("argument '{}' expects a list of integers", argName));
    }
    std::pmr::vector<int32_t> vals(&mr);
    cur.skipWs();
    if (!cur.consume(']')) {
      for (;;) {
        vals.push_back(parseNumberValue<int32_t>(argName));
        cur.skipWs();
        if (cur.consume(']')) break;
        if (!cur.consume(',')) fail(cur.position(), "expected ',' or ']' in list");
      }
    }
    if (vals.empty()) return {};
    auto* arr = (int32_t*)mr.allocate(sizeof(int32_t) * vals.size(), alignof(int32_t));
    for (size_t i = 0; i < vals.size(); i++) arr[i] = vals[i];
    return {arr, vals.size()};
  }

  // Raw text for the declared positional slot: verbatim bytes to the first
  // top-level ',' or ')' (parens/brackets nest), trailing whitespace trimmed,
  // no escape processing.  A value that is entirely ONE quoted string
  // delivers its unescaped content (quote the whole value to protect commas
  // and parens); a quote anywhere else is an ordinary byte, so
  // match(don't stop, ...) just works.
  std::string_view parseRawText(std::string_view fn) {
    cur.skipWs();
    size_t start = cur.position();

    char c = cur.peek();
    if (c == '"' || c == '\'') {
      size_t save = cur.position();
      std::string_view body = scanQuoted();
      size_t after = cur.position();
      cur.skipWs();
      char nxt = cur.peek();
      cur.seek(after);
      if (nxt == ',' || nxt == ')') return body;
      cur.seek(save);  // the quote was interior; raw text includes it
    }

    int parens = 0;
    int brackets = 0;
    size_t lastNonWs = start;
    while (!cur.atEnd()) {
      if (size_t n = cur.wsLen()) {
        cur.advance(n);
        continue;
      }
      char b = cur.peek();
      if (b == '(') parens++;
      if (b == '[') brackets++;
      if (b == ']' && brackets > 0) brackets--;
      if (b == ')') {
        if (parens == 0) break;
        parens--;
      }
      if (b == ',' && parens == 0 && brackets == 0) break;
      cur.advance();
      lastNonWs = cur.position();
    }
    if (cur.atEnd()) fail(start, fmt::format("unterminated {}(...) call", fn));
    std::string_view text = cur.slice(start, lastNonWs);
    if (text.empty()) fail(start, "expected a value");
    return text;
  }
};

// Parse an expr query string into an api::Query subtree allocated in `arena`.
// Throws std::runtime_error with byte-offset context on any parse error.
inline const api::Query* parseExpr(std::string_view q, const ExprOptions& options,
                                   std::pmr::memory_resource& arena) {
  ExprParser parser(options, arena);
  return parser.parse(q);
}

} // namespace luxir
