#pragma once

#include <algorithm>
#include <cassert>
#include <charconv>
#include <cstdint>
#include <memory_resource>
#include <optional>
#include <span>
#include <string_view>

#include <fmt/format.h>

#include "solux/api/build.h"
#include "solux/api/solux_types.hpp"
#include "solux/schema/Schema.h"

namespace solux {

// Forgiving end-user query parser (the simple_query arm): Lucene
// SimpleQueryParser's character state machine adapted to emit a
// solux::api::Query subtree, plus Gmail-style fielded terms (field:value,
// field:"a phrase").
//
// Contract: NEVER fails to parse.  Every input degrades to some query:
// unbalanced quotes and parentheses are extraneous characters, a colon token
// whose field name is not queryable is literal text, garbage after ~ is
// swallowed.  Degradations worth declaring go on the warnings list.
//
// The parser is SCHEMA-AWARE but ANALYZER-BLIND: it consults FieldType to
// emit the most natural arm for each clause (a quoted value against an
// unanalyzed STRING/ID field is an exact-term Match, not a PhraseQuery; a
// future typo control applies only to TEXT clauses) so the emitted tree - the
// one echo mode shows - is the honest representation.  It still never
// interprets text: token bytes pass through unanalyzed, and analysis happens
// at query build like every other node.
//
// Byte-oriented lexing: every metachar is ASCII and UTF-8 is
// self-synchronizing, so multi-byte sequences pass through opaquely.
// Whitespace is ASCII plus U+3000 (ideographic space).
//
// The emitted tree is allocated in the caller's arena (per-request pool),
// spliced where the simple_query arm sat, and lowered by the same
// ProtobufQueryParser -> QueryBuilder path as every other node.
struct SimpleQueryOptions {
  // The fields unfielded (bare) clauses expand over; must be non-empty and
  // every name must resolve to a queryable field (the caller validates).
  // The views must outlive the emitted tree (request-backed or static).
  std::span<const std::string_view> fields;
  // Arm selection by FieldType; also answers which names field: syntax may
  // target.  The schema snapshot is immutable for the request lifetime.
  Schema* schema = nullptr;
  // Narrows which queryable names field: syntax may reach (empty = all).
  // A queryable name excluded by this list degrades to text WITH a warning;
  // a name the schema cannot query is just text, silently.
  std::span<const std::string_view> allowed_fields;
  // Clause juxtaposition AND multi-term combination within an analyzed token
  // (Lucene passes its default operator into per-token analysis the same
  // way).  Unset = OR.
  api::Match_::Operator operator_ = api::Match_::Operator::OPERATOR_UNSPECIFIED;
  // Minimum number of top-level optional USER clauses that must match.
  // Applied here, where user clauses are distinguishable from per-field
  // expansion; silently inapplicable when the top level cannot honor it
  // (that is contingent on user input, which must not cost warnings).
  int32_t min_match = 0;
};

struct SimpleQueryResult {
  // nullptr = no effective clauses (empty/all-operator input): match nothing.
  const api::Query* root = nullptr;
  std::span<const api::Warning> warnings;  // arena-backed
};

class SimpleQueryParser {
  // Parenthesis nesting beyond this depth is treated as extraneous characters
  // (declared).  A fixed backstop until the request-scoped nesting budget
  // exists; it also bounds the emitted tree depth for the recursive lowering.
  static constexpr int MAX_DEPTH = 64;
  static constexpr size_t NPOS = (size_t)-1;

  enum class Occur : uint8_t { NONE, MUST, SHOULD };

  const SimpleQueryOptions& opts;
  std::pmr::memory_resource& mr;
  Occur defaultOccur;

  const char* data = nullptr;
  size_t pos = 0;
  size_t end = 0;
  size_t inputEnd = 0;  // q.size(); `end` narrows during group recursion

  std::pmr::vector<char> buf;  // escape-processed token/phrase bytes (transient)
  api::build::SpanBuilder<api::Warning> warnings;

  // Expansion fields with their resolved types (arm selection).
  struct ExpField {
    std::string_view name;
    FieldType* type;
  };
  std::pmr::vector<ExpField> expFields;

  // Lazily built, quote- and escape-aware matching-close table:
  // parenClose[i] = position of the ')' closing the '(' at i, or NPOS.  A ')'
  // inside "..." never closes a group (Lucene is quote-blind here and breaks
  // on phrases containing parens - deviation spent where Lucene is wrong).
  // One O(n) pass also removes the quadratic rescan a run of unmatched '('
  // would otherwise cost.
  std::pmr::vector<size_t> parenClose;
  bool parenTableBuilt = false;

  // One clause level under construction, mirroring Lucene's State: `top`
  // holds the tree while it is a single node; once composed, `clauses` holds
  // the current flat level (all one occur - an operator change collapses the
  // level into a node and starts a new one holding it).
  struct TreeState {
    const api::Query* top = nullptr;
    std::pmr::vector<const api::Query*> clauses;
    Occur clausesOccur = Occur::NONE;
    Occur currentOp = Occur::NONE;
    Occur prevOp = Occur::NONE;
    int notCount = 0;
    explicit TreeState(std::pmr::memory_resource& mr) : clauses(&mr) {}
    bool hasAny() const { return top != nullptr || !clauses.empty(); }
  };

public:
  // Term-backed and indexed fields, which support every arm (match / phrase /
  // prefix / fuzzy) and which bare clauses may expand over.  Numeric column
  // fields are targetable too but only for exact match - see numericQueryable.
  // Everything else degrades to text.
  static bool termQueryable(FieldType& fieldType) {
    switch (fieldType.type()) {
      case FieldType::Type::TEXT:
      case FieldType::Type::STRING:
      case FieldType::Type::ID:
        return fieldType.indexed();
      default:
        return false;
    }
  }

  SimpleQueryParser(const SimpleQueryOptions& opts, std::pmr::memory_resource& arena)
      : opts(opts), mr(arena), buf(&arena), warnings(arena), expFields(&arena),
        parenClose(&arena) {
    assert(!opts.fields.empty());
    assert(opts.schema != nullptr);
    defaultOccur = opts.operator_ == api::Match_::Operator::AND ? Occur::MUST : Occur::SHOULD;
    expFields.reserve(opts.fields.size());
    for (std::string_view f : opts.fields) {
      FieldType* fieldType = opts.schema->getFieldTypePtr(f);
      assert(fieldType != nullptr && termQueryable(*fieldType));  // caller validated
      expFields.push_back({f, fieldType});
    }
  }

  SimpleQueryResult parse(std::string_view q) {
    data = q.data();
    pos = 0;
    end = q.size();
    inputEnd = q.size();

    // Input that is exactly "*" (ignoring whitespace) selects everything
    // (Lucene SimpleQueryParser special case).
    size_t first = NPOS;
    size_t lastEnd = 0;
    for (size_t i = 0; i < end;) {
      if (size_t n = wsLen(i)) {
        i += n;
        continue;
      }
      if (first == NPOS) first = i;
      lastEnd = ++i;
    }
    if (first == NPOS) {
      return {nullptr, warnings.finish()};  // empty / all-whitespace
    }
    if (lastEnd - first == 1 && data[first] == '*') {
      api::Query all;
      all.kind = true;
      return {allocQuery(all), warnings.finish()};
    }

    TreeState st(mr);
    runRange(st, 0, end, 0);

    // min_match applies over the top-level USER clauses (never to a leaf's
    // per-field expansion, which is also a BooleanQuery - the shapes are
    // indistinguishable downstream, so the decision lives here).  When the
    // top level cannot honor it - a single clause, a required level from
    // +/operator=AND, match-all, empty input - it is SILENTLY inapplicable:
    // whether min_match applies is contingent on what the end user typed,
    // and a query writer should not have to know user input to author a
    // warning-free query.  The semantics stay consistent with the engine's
    // clamp (min_match above the clause count means "all of them", so a
    // single clause is already its own min_match).
    const api::Query* root;
    if (!st.clauses.empty()) {
      const api::Query* collapsed = collapse(st);
      if (opts.min_match > 0 && st.clausesOccur == Occur::SHOULD) {
        // our own freshly built arena node; the engine clamps to the count
        std::get<api::BooleanQuery>(const_cast<api::Query*>(collapsed)->kind).min_match =
            opts.min_match;
      }
      root = collapsed;
    } else {
      root = st.top;
    }
    return {root, warnings.finish()};
  }

private:
  // ---- arena emission helpers ----

  const api::Query* allocQuery(const api::Query& node) {
    api::Query* p = (api::Query*)mr.allocate(sizeof(api::Query), alignof(api::Query));
    new (p) api::Query(node);
    return p;
  }

  api::Val* allocVal(std::string_view text) {
    api::Val* v = (api::Val*)mr.allocate(sizeof(api::Val), alignof(api::Val));
    new (v) api::Val();
    v->kind = text;
    return v;
  }

  std::string_view arenaStr(std::string_view s) { return api::build::arenaStr(mr, s); }

  // Expand a per-field leaf maker over the expansion fields: one field emits
  // the leaf directly; several emit a Boolean optional group (scores sum
  // across fields; a max-scoring dismax alternative is a future engine
  // question).
  template <typename MK>
  const api::Query* expand(MK&& mkKind) {
    if (expFields.size() == 1) {
      return allocQuery(mkKind(expFields[0]));
    }
    size_t n = expFields.size();
    api::Query* arr = (api::Query*)mr.allocate(sizeof(api::Query) * n, alignof(api::Query));
    for (size_t i = 0; i < n; i++) {
      new (&arr[i]) api::Query(mkKind(expFields[i]));
    }
    api::BooleanQuery bq;
    bq.optional = std::span<const api::Query>(arr, n);
    api::Query q;
    q.kind = bq;
    return allocQuery(q);
  }

  api::Query matchLeaf(std::string_view field, FieldType& fieldType, api::Val* val) {
    api::Match m;
    m.field = field;
    m.val = val;
    // the operator shapes analyzed multi-term combination; unanalyzed
    // STRING/ID matches are single terms, so it would be echo noise there
    if (fieldType.type() == FieldType::Type::TEXT) {
      m.operator_ = opts.operator_;
    }
    api::Query q;
    q.kind = m;
    return q;
  }

  // field empty = unfielded (expand over opts.fields).  value/field bytes are
  // transient (buf); copies go to the arena here.
  const api::Query* makeDefault(std::string_view field, FieldType* fieldType,
                                std::string_view value) {
    api::Val* val = allocVal(arenaStr(value));
    if (!field.empty()) return allocQuery(matchLeaf(arenaStr(field), *fieldType, val));
    return expand([&](const ExpField& f) { return matchLeaf(f.name, *f.type, val); });
  }

  // Quoted text takes the natural arm per field type: a positional
  // PhraseQuery for analyzed TEXT; for unanalyzed STRING/ID the whole quoted
  // text is one exact term, so it is a plain Match.
  const api::Query* makePhrase(std::string_view field, FieldType* fieldType,
                               std::string_view text) {
    api::Val* val = nullptr;  // lazily shared by non-TEXT arms
    auto mk = [&](std::string_view f, FieldType& t) {
      if (t.type() == FieldType::Type::TEXT) {
        api::PhraseQuery p;
        p.field = f;
        p.text = text;  // already arena-backed (parsePhraseBody copies)
        api::Query q;
        q.kind = p;
        return q;
      }
      if (val == nullptr) val = allocVal(text);
      return matchLeaf(f, t, val);
    };
    if (!field.empty()) return allocQuery(mk(arenaStr(field), *fieldType));
    return expand([&](const ExpField& f) { return mk(f.name, *f.type); });
  }

  const api::Query* makePrefix(std::string_view field, FieldType* fieldType,
                               std::string_view value) {
    unused(fieldType);  // prefix is term-space-native for all queryable types
    std::string_view v = arenaStr(value);
    auto mk = [&](std::string_view f) {
      api::PrefixQuery p;
      p.field = f;
      p.prefix = v;
      api::Query q;
      q.kind = p;
      return q;
    };
    if (!field.empty()) return allocQuery(mk(arenaStr(field)));
    return expand([&](const ExpField& f) { return mk(f.name); });
  }

  const api::Query* makeFuzzy(std::string_view field, FieldType* fieldType,
                              std::string_view value, std::optional<int32_t> maxEdits) {
    unused(fieldType);  // fuzzy is term-space-native for all queryable types
    std::string_view v = arenaStr(value);
    auto mk = [&](std::string_view f) {
      api::FuzzyQuery fq;
      fq.field = f;
      fq.term = v;
      fq.max_edits = maxEdits;  // unset = AUTO by term length
      api::Query q;
      q.kind = fq;
      return q;
    };
    if (!field.empty()) return allocQuery(mk(arenaStr(field)));
    return expand([&](const ExpField& f) { return mk(f.name); });
  }

  // Lucene's negation shape: MUST_NOT(branch) + SHOULD(match-all).
  const api::Query* negate(const api::Query* branch) {
    api::Query* opt = (api::Query*)mr.allocate(sizeof(api::Query), alignof(api::Query));
    new (opt) api::Query();
    opt->kind = true;  // the `all` arm
    api::Query* pro = (api::Query*)mr.allocate(sizeof(api::Query), alignof(api::Query));
    new (pro) api::Query(*branch);
    api::BooleanQuery bq;
    bq.optional = std::span<const api::Query>(opt, 1);
    bq.prohibited = std::span<const api::Query>(pro, 1);
    api::Query q;
    q.kind = bq;
    return allocQuery(q);
  }

  // Materialize the state's tree: the active clause level becomes a
  // BooleanQuery node (required for MUST, optional for SHOULD).
  const api::Query* collapse(TreeState& st) {
    if (st.clauses.empty()) return st.top;  // single node or nothing
    size_t n = st.clauses.size();
    api::Query* arr = (api::Query*)mr.allocate(sizeof(api::Query) * n, alignof(api::Query));
    for (size_t i = 0; i < n; i++) {
      new (&arr[i]) api::Query(*st.clauses[i]);
    }
    api::BooleanQuery bq;
    if (st.clausesOccur == Occur::MUST) {
      bq.required = std::span<const api::Query>(arr, n);
    } else {
      bq.optional = std::span<const api::Query>(arr, n);
    }
    api::Query q;
    q.kind = bq;
    return allocQuery(q);
  }

  void warn(std::string_view code, std::string_view message) {
    for (const auto& w : warnings.finish()) {
      if (w.code == code) return;  // one declaration per degradation kind
    }
    warnings.emplace_back(api::Warning{code, arenaStr(message)});
  }

  // ---- schema consultation (arm selection only; text stays uninterpreted) ----

  // Column-numeric field types (INT/FLOAT/DOUBLE/DATE, stored in the shared int
  // column).  field: syntax targets these with an exact-match arm only
  // (field:value or a quoted value); wildcard/fuzzy have no numeric meaning and
  // degrade to text.  Requires column storage - the match scan reads the column,
  // so a numeric field without it is not targetable and degrades to text.
  static bool numericQueryable(FieldType& fieldType) {
    switch (fieldType.type()) {
      case FieldType::Type::INT:
      case FieldType::Type::FLOAT:
      case FieldType::Type::DOUBLE:
      case FieldType::Type::DATE:
        return fieldType.hasColumn();
      default:
        return false;
    }
  }

  static std::string_view numericTypeName(FieldType::Type t) {
    switch (t) {
      case FieldType::Type::INT: return "integer";
      case FieldType::Type::DATE: return "date";
      default: return "number";  // FLOAT / DOUBLE
    }
  }

  // Whether `value` can be coerced to the numeric field's native value.  A value
  // that cannot (price:abc) is not a numeric query and degrades to text, so
  // simple_query still never hard-fails at build.  Uses the same coercion
  // contract as ingest / query build, which signals failure by throwing.
  bool numericCoercible(FieldType& fieldType, std::string_view value) {
    api::Val v;
    v.kind = value;
    try {
      fieldType.coerceColInt64(v, fieldType.name());
      return true;
    } catch (const std::exception&) {
      return false;
    }
  }

  // The queryable FieldType for a field: token's head, or null (degrade).
  // Term-backed fields support every arm; numeric column fields are resolved
  // here too but the caller restricts them to the exact-match arm.
  FieldType* fieldFor(std::string_view name) {
    FieldType* fieldType = opts.schema->getFieldTypePtr(name);
    if (fieldType == nullptr) return nullptr;
    return termQueryable(*fieldType) || numericQueryable(*fieldType) ? fieldType : nullptr;
  }

  bool isAllowed(std::string_view name) {
    return opts.allowed_fields.empty()
        || std::find(opts.allowed_fields.begin(), opts.allowed_fields.end(), name)
               != opts.allowed_fields.end();
  }

  // Resolve a fielded-term head: non-null when field: syntax applies.  A
  // queryable name excluded by allowed_fields degrades WITH a declaration;
  // schema-unknown names are normal text, silently (Gmail's "re: hello").
  FieldType* fieldedHead(std::string_view head) {
    FieldType* fieldType = fieldFor(head);
    if (fieldType == nullptr) return nullptr;
    if (isAllowed(head)) return fieldType;
    warn("field_narrowed",
         fmt::format("field '{}' is outside this request's allowed_fields; treated as text", head));
    return nullptr;
  }

  // ---- lexing ----

  // Whitespace length at i: ASCII space/tab/newline/return, or U+3000
  // (ideographic space, E3 80 80 - the one non-ASCII whitespace two decades
  // of Lucene needed).  0 = not whitespace.
  size_t wsLen(size_t i) const {
    char c = data[i];
    if (c == ' ' || c == '\t' || c == '\n' || c == '\r') return 1;
    if ((uint8_t)c == 0xE3 && i + 2 < end && (uint8_t)data[i + 1] == 0x80 &&
        (uint8_t)data[i + 2] == 0x80) {
      return 3;
    }
    return 0;
  }

  bool tokenFinished(size_t i) const {
    char c = data[i];
    return c == '"' || c == '|' || c == '+' || c == '(' || c == ')' || wsLen(i) > 0;
  }

  // At '~': consume it and the following characters up to a token boundary.
  // nullopt = bare '~' (AUTO); otherwise the parsed number, with garbage
  // swallowed to 0 and negatives floored to 0 (Lucene behaviors).
  std::optional<int> parseFuzziness() {
    ++pos;
    size_t s = pos;
    while (pos < end && !tokenFinished(pos)) ++pos;
    std::string_view digits(data + s, pos - s);
    if (digits.empty()) return std::nullopt;
    int v = 0;
    auto [p, ec] = std::from_chars(digits.data(), digits.data() + digits.size(), v);
    if (ec != std::errc() || p != digits.data() + digits.size()) return 0;
    return v < 0 ? 0 : v;
  }

  // At an opening '"': collect the phrase bytes (escape-processed) through
  // the closing quote, consuming a trailing ~N (slop is not on the wire yet,
  // so it is declared ignored).  Returns an ARENA-BACKED copy, or nullopt
  // with pos reset past the opening quote when unterminated (the quote is
  // then extraneous and its contents reparse as tokens).
  std::optional<std::string_view> parsePhraseBody() {
    size_t start = ++pos;
    buf.clear();
    bool escaped = false;
    while (pos < end) {
      char c = data[pos];
      if (!escaped) {
        if (c == '\\') {
          escaped = true;
          ++pos;
          continue;
        }
        if (c == '"') {
          ++pos;  // past the closing quote
          if (pos < end && data[pos] == '~') {
            auto slop = parseFuzziness();
            if (!slop.has_value() || *slop > 0) {
              warn("phrase_slop_ignored",
                   "phrase slop (\"...\"~N) is not supported yet; the phrase matches exactly");
            }
          }
          return arenaStr(std::string_view(buf.data(), buf.size()));
        }
      }
      escaped = false;
      buf.push_back(c);
      ++pos;
    }
    pos = start;
    return std::nullopt;
  }

  // The position of the ')' closing the '(' at openPos, honoring quotes and
  // escapes, or NPOS.  Backed by a lazily built table whose quote handling
  // mirrors the parse's degradation rule: quotes pair greedily left-to-right,
  // and an UNPAIRED quote is not a quote (the parse treats it as extraneous
  // and reparses its "contents"), so it must not hide the parens after it.
  // Only real quoted regions protect parens.
  size_t matchingClose(size_t openPos) {
    if (!parenTableBuilt) {
      parenTableBuilt = true;
      parenClose.assign(inputEnd, NPOS);

      // phase 1: pair unescaped quotes greedily
      std::pmr::vector<std::pair<size_t, size_t>> quoted(&mr);
      {
        bool escaped = false;
        size_t open = NPOS;
        for (size_t i = 0; i < inputEnd; i++) {
          if (escaped) {
            escaped = false;
            continue;
          }
          char c = data[i];
          if (c == '\\') {
            escaped = true;
          } else if (c == '"') {
            if (open == NPOS) {
              open = i;
            } else {
              quoted.push_back({open, i});
              open = NPOS;
            }
          }
        }
        // an unpaired trailing `open` is dropped: not a quote
      }

      // phase 2: pair parens, skipping the quoted regions
      std::pmr::vector<size_t> stack(&mr);
      bool escaped = false;
      size_t qi = 0;
      for (size_t i = 0; i < inputEnd; i++) {
        if (qi < quoted.size() && i == quoted[qi].first) {
          i = quoted[qi].second;  // jump to the closing quote (loop ++ steps past)
          qi++;
          continue;
        }
        if (escaped) {
          escaped = false;
          continue;
        }
        char c = data[i];
        if (c == '\\') {
          escaped = true;
        } else if (c == '(') {
          stack.push_back(i);
        } else if (c == ')' && !stack.empty()) {
          parenClose[stack.back()] = i;
          stack.pop_back();
        }
      }
    }
    return parenClose[openPos];
  }

  // ---- the Lucene state machine ----

  void runRange(TreeState& st, size_t start, size_t stop, int depth) {
    size_t savedPos = pos;
    size_t savedEnd = end;
    pos = start;
    end = stop;

    while (pos < end) {
      char c = data[pos];
      if (c == '(') {
        consumeSubQuery(st, depth);
      } else if (c == ')') {
        ++pos;  // extraneous closing parenthesis: ignored
      } else if (c == '"') {
        consumePhrase(st);
      } else if (c == '+') {
        // ignored if an operation is already pending or there is nothing yet
        // to combine with
        if (st.currentOp == Occur::NONE && st.hasAny()) st.currentOp = Occur::MUST;
        ++pos;
      } else if (c == '|') {
        if (st.currentOp == Occur::NONE && st.hasAny()) st.currentOp = Occur::SHOULD;
        ++pos;
      } else if (c == '-') {
        // two in a row negate each other; even whitespace between '-' and its
        // clause breaks the negation (hence the notCount reset below)
        ++st.notCount;
        ++pos;
        continue;
      } else if (size_t n = wsLen(pos)) {
        pos += n;
      } else {
        consumeToken(st);
      }
      st.notCount = 0;
    }

    pos = savedPos;
    end = savedEnd;
  }

  const api::Query* parseRange(size_t start, size_t stop, int depth) {
    TreeState st(mr);
    runRange(st, start, stop, depth);
    return collapse(st);
  }

  void consumeSubQuery(TreeState& st, int depth) {
    size_t open = pos;
    size_t start = open + 1;
    size_t close = matchingClose(open);
    if (close == NPOS || close >= end) {
      // no closing parenthesis in range: the opening one is extraneous;
      // contents reparse
      pos = start;
    } else if (close == start) {
      // "()" resets a pending operation (it would have applied to this group)
      st.currentOp = Occur::NONE;
      pos = close + 1;
    } else if (depth + 1 >= MAX_DEPTH) {
      warn("depth_clamped",
           "parenthesis nesting exceeds the supported depth; deeper parentheses are ignored");
      pos = start;
    } else {
      const api::Query* sub = parseRange(start, close, depth + 1);
      pos = close + 1;
      buildQueryTree(st, sub);
    }
  }

  void consumePhrase(TreeState& st) {
    auto text = parsePhraseBody();
    if (!text) return;  // unterminated: opening quote extraneous
    if (text->empty()) {
      // "" resets a pending operation (Lucene behavior)
      st.currentOp = Occur::NONE;
      return;
    }
    buildQueryTree(st, makePhrase({}, nullptr, *text));
  }

  void consumeToken(TreeState& st) {
    buf.clear();
    bool escaped = false;
    bool prefix = false;
    bool sawFuzzy = false;
    std::optional<size_t> firstColon;  // buf position of the first unescaped ':'

    while (pos < end) {
      char c = data[pos];
      if (!escaped) {
        if (c == '\\') {
          escaped = true;
          prefix = false;
          ++pos;
          continue;
        }
        if (tokenFinished(pos)) {
          // `field:` immediately followed by a quote is a fielded phrase.  For a
          // numeric field the quotes are just value delimiters, so makePhrase
          // emits an exact match (matchLeaf), not a positional phrase.
          if (c == '"' && firstColon && *firstColon > 0 && *firstColon == buf.size() - 1) {
            std::string_view head(buf.data(), *firstColon);
            if (FieldType* fieldType = fieldedHead(head)) {
              std::string_view field = arenaStr(head);  // head dangles once buf is reused
              auto text = parsePhraseBody();
              if (text) {
                if (text->empty()) {
                  st.currentOp = Occur::NONE;
                } else if (numericQueryable(*fieldType) && !numericCoercible(*fieldType, *text)) {
                  // Uncoercible numeric value: degrade the quoted text to a
                  // phrase over the default fields (declared).
                  warn("numeric_field_value",
                       fmt::format("'{}' is not a valid {} for field '{}'; treated as text",
                                   *text, numericTypeName(fieldType->type()), field));
                  buildQueryTree(st, makePhrase({}, nullptr, *text));
                } else {
                  buildQueryTree(st, makePhrase(field, fieldType, *text));
                }
                return;
              }
              // unterminated phrase: fall through, `field:` is literal text
            }
          }
          break;
        }
        if (!buf.empty() && c == '~') {
          sawFuzzy = true;
          break;
        }
        if (c == ':' && !firstColon) firstColon = buf.size();
        prefix = !buf.empty() && c == '*';
      }
      escaped = false;
      buf.push_back(c);
      ++pos;
    }

    if (buf.empty()) return;

    // fuzzy wins over prefix (Lucene branch order); ~0 degrades to a plain term
    std::optional<int32_t> fuzzEdits;
    bool fuzzZero = false;
    if (sawFuzzy) {
      auto v = parseFuzziness();
      if (!v.has_value()) {
        fuzzEdits = std::nullopt;  // bare '~': AUTO by term length
      } else if (*v == 0) {
        fuzzZero = true;
      } else if (*v > 2) {
        warn("fuzzy_clamped",
             fmt::format("fuzzy edit distance {} clamped to the supported maximum of 2", *v));
        fuzzEdits = 2;
      } else {
        fuzzEdits = *v;
      }
    }

    std::string_view token(buf.data(), buf.size());
    if (!sawFuzzy && prefix) token.remove_suffix(1);  // drop the trailing '*'

    // fielded classification: split at the first unescaped colon; a head that
    // is not a queryable/allowed field leaves the whole token as literal text
    std::string_view field{};
    FieldType* fieldType = nullptr;
    std::string_view value = token;
    if (firstColon && *firstColon > 0 && *firstColon < token.size()) {
      std::string_view head = token.substr(0, *firstColon);
      std::string_view tail = token.substr(*firstColon + 1);
      // an empty value is only meaningful with a prefix star (field:* = has field)
      if (!tail.empty() || (prefix && !sawFuzzy)) {
        if (FieldType* ft = fieldedHead(head)) {
          if (numericQueryable(*ft)) {
            // Numeric column fields take only an exact-match arm (field:value).
            // Wildcard '*' / fuzzy '~' have no numeric meaning, and a value that
            // is not a valid number/date cannot be a numeric query; both degrade
            // to text (like an unknown field) with a declaration.
            if (prefix || sawFuzzy) {
              warn("numeric_field_syntax",
                   fmt::format("field '{}' is numeric; wildcard '*' and fuzzy '~' do not "
                               "apply - use {}:value for an exact match", head, head));
            } else if (!numericCoercible(*ft, tail)) {
              warn("numeric_field_value",
                   fmt::format("'{}' is not a valid {} for field '{}'; treated as text",
                               tail, numericTypeName(ft->type()), head));
            } else {
              field = head;
              fieldType = ft;
              value = tail;
            }
          } else {
            field = head;
            fieldType = ft;
            value = tail;
          }
        }
      }
    }

    const api::Query* branch;
    if (sawFuzzy) {
      branch = fuzzZero ? makeDefault(field, fieldType, value)
                        : makeFuzzy(field, fieldType, value, fuzzEdits);
    } else if (prefix) {
      branch = makePrefix(field, fieldType, value);
    } else {
      branch = makeDefault(field, fieldType, value);
    }
    buildQueryTree(st, branch);
  }

  // Fold a consumed clause into the tree (Lucene buildQueryTree): the level
  // stays flat while the operator repeats; an operator change collapses the
  // level into a single node and starts a new level holding it.
  void buildQueryTree(TreeState& st, const api::Query* branch) {
    if (branch == nullptr) return;
    if (st.notCount % 2 == 1) branch = negate(branch);

    if (!st.hasAny()) {
      st.top = branch;
    } else {
      if (st.currentOp == Occur::NONE) st.currentOp = defaultOccur;
      if (st.prevOp != st.currentOp) {
        const api::Query* old = collapse(st);
        st.top = nullptr;
        st.clauses.clear();
        st.clauses.push_back(old);
        st.clausesOccur = st.currentOp;
      }
      st.clauses.push_back(branch);
      st.prevOp = st.currentOp;
    }
    st.currentOp = Occur::NONE;
  }
};

// Parse a simple_query string into an api::Query subtree in `arena`.
inline SimpleQueryResult parseSimpleQuery(std::string_view q, const SimpleQueryOptions& options,
                                          std::pmr::memory_resource& arena) {
  SimpleQueryParser parser(options, arena);
  return parser.parse(q);
}

} // namespace solux
