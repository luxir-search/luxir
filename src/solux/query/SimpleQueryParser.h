#pragma once

#include <algorithm>
#include <cassert>
#include <charconv>
#include <cstdint>
#include <limits>
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
// whose field name is not queryable is literal text, mangled decorations
// read as literal text.  Degradations worth declaring go on the warnings
// list.  Specials are POSITIONAL: '+' '-' '|' act only at a clause boundary,
// '*' and '~' only as clean trailing suffixes, and a quote opens a phrase
// only where a value can begin (a boundary, after one leading sign, or right
// after field:) - so can't, say"hi, c++, and a|b are single terms.
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

  enum class Occur : uint8_t { NONE, MUST, SHOULD, MUST_NOT };

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

  // A consumed unit (term / phrase / group) with the occurrence it takes in
  // its clause list, derived from the classic-QueryParser rules below.
  struct Clause {
    const api::Query* q;
    Occur occur;
  };

  // One clause list under construction (the top level or one group), in the
  // classic QueryParser shape: a flat list of clauses, each carrying its own
  // occur, collapsed into a single BooleanQuery (required / optional /
  // prohibited buckets).  A leading '+'/'-' at a clause boundary is a
  // required/prohibited modifier on the next unit (runRange defines the
  // boundary); '|' is an OR conjunction to the next unit that, under the
  // default-AND operator, demotes the preceding clause to optional (Lucene
  // classic QueryParser's addClause).
  struct Level {
    std::pmr::vector<Clause> clauses;  // in source order
    int notCount = 0;                  // pending '-' parity (adjacency)
    bool plus = false;                 // pending '+' (adjacency)
    bool orConj = false;               // a '|' since the last unit
    explicit Level(std::pmr::memory_resource& mr) : clauses(&mr) {}
    bool hasAny() const { return !clauses.empty(); }
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

    Level st(mr);
    runRange(st, 0, end, 0);

    // min_match applies over the top-level USER clauses (never to a leaf's
    // per-field expansion, which is also a BooleanQuery - the shapes are
    // indistinguishable downstream, so the decision lives here).  It binds
    // only to an optional-only top level (the engine restriction, and the
    // classic min-should-match semantics); when the top level cannot honor
    // it - a single clause, any required clause from +/operator=AND, a
    // pure-negative or match-all level, empty input - it is SILENTLY
    // inapplicable, because whether it applies is contingent on what the end
    // user typed and a query writer should not have to know user input to
    // author a warning-free query.  collapse() applies it at the top level.
    return {collapse(st, /*applyMinMatch=*/true), warnings.finish()};
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
                               std::string_view text, int32_t slop,
                               bool suffixPresent) {
    api::Val* val = nullptr;  // lazily shared by non-TEXT arms
    bool warnedInapplicable = false;
    auto mk = [&](std::string_view f, FieldType& t) {
      if (t.type() == FieldType::Type::TEXT) {
        api::PhraseQuery p;
        p.field = f;
        p.text = text;  // already arena-backed (parsePhraseBody copies)
        p.slop = slop;
        api::Query q;
        q.kind = p;
        return q;
      }
      if (suffixPresent && !warnedInapplicable) {
        warn("phrase_slop_inapplicable",
             fmt::format("phrase slop does not apply to non-TEXT field '{}'; "
                         "the quoted value remains an exact match", f));
        warnedInapplicable = true;
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

  api::Query* allocArr(size_t n) {
    return (api::Query*)mr.allocate(sizeof(api::Query) * n, alignof(api::Query));
  }

  // Materialize a clause list into a single node: one positive clause is
  // returned unwrapped (a single term is not a BooleanQuery); otherwise the
  // clauses split into the required / optional / prohibited buckets.  A level
  // with only prohibited clauses gets a match-all optional so `-foo` means
  // "everything except foo" (edismax behavior - the engine matches nothing on
  // a purely negative BooleanQuery).  min_match binds only when applyMinMatch
  // and the level is optional-only with real user optionals.
  const api::Query* collapse(Level& st, bool applyMinMatch) {
    size_t n = st.clauses.size();
    if (n == 0) return nullptr;
    if (n == 1 && st.clauses[0].occur != Occur::MUST_NOT) return st.clauses[0].q;

    size_t nReq = 0, nOpt = 0, nPro = 0;
    for (const Clause& c : st.clauses) {
      if (c.occur == Occur::MUST) nReq++;
      else if (c.occur == Occur::MUST_NOT) nPro++;
      else nOpt++;
    }
    bool pureNegative = (nReq == 0 && nOpt == 0);  // only prohibited -> all-except
    size_t nOptOut = pureNegative ? 1 : nOpt;

    api::Query* req = nReq ? allocArr(nReq) : nullptr;
    api::Query* opt = nOptOut ? allocArr(nOptOut) : nullptr;
    api::Query* pro = nPro ? allocArr(nPro) : nullptr;
    size_t ri = 0, oi = 0, pi = 0;
    for (const Clause& c : st.clauses) {
      if (c.occur == Occur::MUST) new (&req[ri++]) api::Query(*c.q);
      else if (c.occur == Occur::MUST_NOT) new (&pro[pi++]) api::Query(*c.q);
      else new (&opt[oi++]) api::Query(*c.q);
    }
    if (pureNegative) {
      new (&opt[0]) api::Query();
      opt[0].kind = true;  // the match-all `all` arm
    }

    api::BooleanQuery bq;
    if (nReq) bq.required = std::span<const api::Query>(req, nReq);
    if (nOptOut) bq.optional = std::span<const api::Query>(opt, nOptOut);
    if (nPro) bq.prohibited = std::span<const api::Query>(pro, nPro);
    if (applyMinMatch && opts.min_match > 0 && nReq == 0 && nOpt > 0) {
      bq.min_match = opts.min_match;  // the engine clamps to the count
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

  // A token ends at a paren or whitespace.  The operator characters '+' '-'
  // '|' do NOT end a token: they are operators only at a clause boundary
  // (see runRange), so mid-token they are ordinary term bytes and pass
  // through to analysis (c++, at&t, a|b, rock-n-roll ... stay one token).
  // Quotes do not end a token either: a quote opens a phrase only where a
  // value can begin - at a clause boundary, after one leading sign, or right
  // after a token's first ':' (field:"...") - so can"t and say"hi stay one
  // token (same positional rule, applied to quotes).
  bool tokenFinished(size_t i) const {
    char c = data[i];
    return c == '(' || c == ')' || wsLen(i) > 0;
  }

  // Is the '~' at tildePos a clean trailing fuzzy operator: '~' then optional
  // ASCII digits then a token boundary (or end)?  If not - '~' with non-digit
  // junk after it, e.g. abc~2+d or abc~xyz - the '~' is a literal term byte,
  // like '*' anywhere but the token end.  Mangled decorations read as text.
  bool fuzzySuffix(size_t tildePos) const {
    size_t i = tildePos + 1;
    while (i < end && data[i] >= '0' && data[i] <= '9') i++;
    return i >= end || tokenFinished(i);
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

  struct PhraseBody {
    std::string_view text;
    int32_t slop = 0;
    bool suffixPresent = false;
  };

  // At an opening '"': collect the phrase bytes (escape-processed) through
  // the closing quote and consume a trailing ~ suffix. Returns an
  // ARENA-BACKED copy, or nullopt
  // with pos reset past the opening quote when unterminated (the quote is
  // then extraneous and its contents reparse as tokens).
  std::optional<PhraseBody> parsePhraseBody() {
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
          int32_t slop = 0;
          bool suffixPresent = false;
          if (pos < end && data[pos] == '~') {
            suffixPresent = true;
            ++pos;
            size_t suffixStart = pos;
            while (pos < end && !tokenFinished(pos)) ++pos;
            std::string_view suffix(data + suffixStart, pos - suffixStart);
            bool malformed = suffix.empty();
            bool overflow = false;
            int64_t value = 0;
            for (char d : suffix) {
              if (d < '0' || d > '9') {
                malformed = true;
                break;
              }
              if (!overflow) {
                value = value * 10 + (d - '0');
                if (value > std::numeric_limits<int32_t>::max()) {
                  overflow = true;
                }
              }
            }
            if (malformed) {
              warn("phrase_slop_malformed",
                   "malformed phrase slop suffix consumed; exact slop 0 is used");
            } else if (overflow) {
              slop = std::numeric_limits<int32_t>::max();
              warn("phrase_slop_clamped",
                   "phrase slop overflow clamped to INT_MAX");
            } else {
              slop = (int32_t) value;
            }
          }
          return PhraseBody{arenaStr(std::string_view(buf.data(), buf.size())),
                            slop, suffixPresent};
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
  // mirrors the parse's positional rule: a quote OPENS only where a value can
  // begin - a clause boundary (start / after whitespace / after '(' / after
  // one boundary sign) or right after a token's first ':' - and closes at the
  // next unescaped quote.  An UNPAIRED opening quote is not a quote (the
  // parse drops it), so it must not hide the parens after it; only real
  // quoted regions protect parens.  The boundary simulation approximates
  // where exactness would need the paren pairing this pass feeds (real vs
  // extraneous parens); a divergence degrades the parse, never breaks it.
  size_t matchingClose(size_t openPos) {
    if (!parenTableBuilt) {
      parenTableBuilt = true;
      parenClose.assign(inputEnd, NPOS);

      // phase 1: find quote regions with the boundary-aware opening rule
      std::pmr::vector<std::pair<size_t, size_t>> quoted(&mr);
      {
        // wsLen() checks against the (possibly narrowed) `end`; the table
        // covers the whole input
        auto wsAt = [&](size_t i) -> size_t {
          char c = data[i];
          if (c == ' ' || c == '\t' || c == '\n' || c == '\r') return 1;
          if ((uint8_t)c == 0xE3 && i + 2 < inputEnd && (uint8_t)data[i + 1] == 0x80 &&
              (uint8_t)data[i + 2] == 0x80) {
            return 3;
          }
          return 0;
        };
        bool escaped = false;
        bool canOpen = true;      // a value can begin here
        bool signTaken = false;   // one boundary sign consumed; a value may follow
        bool colonJustBefore = false;
        bool colonSeen = false;
        size_t tokenLen = 0;
        auto tokenByte = [&] {
          canOpen = false;
          signTaken = false;
          colonJustBefore = false;
          tokenLen++;
        };
        for (size_t i = 0; i < inputEnd; i++) {
          if (escaped) {
            escaped = false;
            tokenByte();  // an escaped byte is a token byte (even a ':')
            continue;
          }
          char c = data[i];
          if (size_t n = wsAt(i)) {
            i += n - 1;
            canOpen = true;
            signTaken = false;
            colonSeen = false;
            colonJustBefore = false;
            tokenLen = 0;
          } else if (c == '\\') {
            escaped = true;
          } else if (c == '(') {
            canOpen = true;
            signTaken = false;
            colonSeen = false;
            colonJustBefore = false;
            tokenLen = 0;
          } else if (c == ')') {
            // neutral for the boundary, like the parse; ends any token
            signTaken = false;
            colonSeen = false;
            colonJustBefore = false;
            tokenLen = 0;
          } else if (c == '+' || c == '-' || c == '|') {
            if (canOpen && !signTaken) {
              signTaken = true;  // canOpen survives: -"a b" opens a phrase
              colonJustBefore = false;
            } else {
              tokenByte();
            }
          } else if (c == '"') {
            size_t close = NPOS;
            if (canOpen || colonJustBefore) {
              bool esc2 = false;
              for (size_t j = i + 1; j < inputEnd; j++) {
                if (esc2) {
                  esc2 = false;
                } else if (data[j] == '\\') {
                  esc2 = true;
                } else if (data[j] == '"') {
                  close = j;
                  break;
                }
              }
            }
            if (close != NPOS) {
              quoted.push_back({i, close});
              i = close;
              canOpen = false;
              signTaken = false;
              colonSeen = false;
              colonJustBefore = false;
              tokenLen = 0;
            } else {
              tokenByte();  // no open position or unterminated: a literal byte
            }
          } else if (c == ':') {
            colonJustBefore = !colonSeen && tokenLen > 0;
            colonSeen = true;
            canOpen = false;
            signTaken = false;
            tokenLen++;
          } else {
            tokenByte();
          }
        }
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

  void runRange(Level& st, size_t start, size_t stop, int depth) {
    size_t savedPos = pos;
    size_t savedEnd = end;
    pos = start;
    end = stop;

    // '+' '-' '|' are operators only at a clause boundary: the start of the
    // range, just after whitespace, or just after an opening '('.  Anywhere
    // else they are ordinary term bytes (see tokenFinished), so `+foo`/`-foo`
    // is a modifier but `a+b`/`c++`/`a|b` is one literal token, and only the
    // FIRST leading sign is a modifier (`+-foo` = require the term "-foo").
    // A quote opens a phrase at a boundary OR right after that one sign
    // (-"a b" prohibits the phrase); anywhere else it is a term byte.
    // Whitespace re-opens a boundary and drops any pending sign that never
    // reached a unit.  orConj survives whitespace (it is the conjunction to
    // the next unit, spaces and all) and is cleared when that unit lands.
    bool atBoundary = true;
    bool afterOp = false;  // one boundary sign consumed; a value may follow
    while (pos < end) {
      char c = data[pos];
      bool quoteOpens = atBoundary || afterOp;
      afterOp = false;
      if (c == '(') {
        // a consumed group is a unit (boundary after it); an extraneous '('
        // is ignored and leaves the boundary state untouched
        if (consumeSubQuery(st, depth)) atBoundary = false;
      } else if (c == ')') {
        ++pos;  // extraneous ')': ignored, neutral for boundary state
      } else if (c == '"' && quoteOpens) {
        consumePhrase(st);
        atBoundary = false;
      } else if (c == '+' && atBoundary) {
        st.plus = true;  // required modifier on the next unit
        ++pos;
        atBoundary = false;
        afterOp = true;
        continue;
      } else if (c == '|' && atBoundary) {
        // OR conjunction to the next unit; ignored with nothing before it
        if (st.hasAny()) st.orConj = true;
        ++pos;
        atBoundary = false;
        afterOp = true;
        continue;
      } else if (c == '-' && atBoundary) {
        ++st.notCount;  // prohibited modifier on the next unit
        ++pos;
        atBoundary = false;
        afterOp = true;
        continue;
      } else if (size_t n = wsLen(pos)) {
        pos += n;
        atBoundary = true;
      } else {
        consumeToken(st);
        atBoundary = false;
      }
      // A pending sign binds only to a unit; whitespace or a stray unit
      // boundary between the sign and its unit drops it.  orConj is not
      // cleared here - it is cleared when the next unit is consumed.
      st.plus = false;
      st.notCount = 0;
    }

    pos = savedPos;
    end = savedEnd;
  }

  const api::Query* parseRange(size_t start, size_t stop, int depth) {
    Level st(mr);
    runRange(st, start, stop, depth);
    return collapse(st, /*applyMinMatch=*/false);  // min_match is a top-level knob
  }

  // Returns true only when a real group unit was consumed (so the caller
  // leaves a clause boundary behind it); an extraneous '(' is neutral.
  bool consumeSubQuery(Level& st, int depth) {
    size_t open = pos;
    size_t start = open + 1;
    size_t close = matchingClose(open);
    if (close == NPOS || close >= end) {
      // no closing parenthesis in range: the opening one is extraneous;
      // contents reparse (the '(' does not disturb the boundary state)
      pos = start;
      return false;
    } else if (close == start) {
      // "()" drops a pending modifier (it would have applied to this group)
      clearPending(st);
      pos = close + 1;
      return false;
    } else if (depth + 1 >= MAX_DEPTH) {
      warn("depth_clamped",
           "parenthesis nesting exceeds the supported depth; deeper parentheses are ignored");
      pos = start;
      return false;
    } else {
      const api::Query* sub = parseRange(start, close, depth + 1);
      pos = close + 1;
      addUnit(st, sub);  // null (empty group) just drops the pending modifier
      return sub != nullptr;
    }
  }

  void consumePhrase(Level& st) {
    auto phrase = parsePhraseBody();
    if (!phrase) return;  // unterminated: opening quote extraneous
    if (phrase->text.empty()) {
      // "" drops a pending modifier (Lucene behavior)
      clearPending(st);
      return;
    }
    addUnit(st, makePhrase({}, nullptr, phrase->text, phrase->slop,
                           phrase->suffixPresent));
  }

  void consumeToken(Level& st) {
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
        if (tokenFinished(pos)) break;
        if (c == '"') {
          // A quote is special mid-token only right after the token's first
          // ':' - the fielded-value position (status:"in stock"; for a
          // numeric field the quotes are just value delimiters, so makePhrase
          // emits an exact match).  Anywhere else it is an ordinary term byte
          // (can"t, say"hi stay one token).
          if (firstColon && *firstColon > 0 && *firstColon == buf.size() - 1) {
            // parsePhraseBody reuses buf: copy the token bytes (and the head
            // view they back) out first
            std::string_view tokenText = arenaStr(std::string_view(buf.data(), buf.size()));
            std::string_view field = tokenText.substr(0, *firstColon);
            FieldType* fieldType = fieldedHead(field);
            auto phrase = parsePhraseBody();
            if (phrase) {
              if (fieldType == nullptr) {
                // the head is not a queryable field: the token so far stays
                // literal text and the quoted value is an unfielded phrase
                // (site:"foo bar" from a search box keeps its phrase-ness)
                addUnit(st, makeDefault({}, nullptr, tokenText));
                if (!phrase->text.empty()) {
                  addUnit(st, makePhrase({}, nullptr, phrase->text, phrase->slop,
                                         phrase->suffixPresent));
                }
              } else if (phrase->text.empty()) {
                clearPending(st);
              } else if (numericQueryable(*fieldType)
                         && !numericCoercible(*fieldType, phrase->text)) {
                // Uncoercible numeric value: degrade like an unknown field -
                // the head stays literal text and the quoted value is a
                // phrase over the default fields (declared).
                warn("numeric_field_value",
                     fmt::format("'{}' is not a valid {} for field '{}'; treated as text",
                                 phrase->text, numericTypeName(fieldType->type()), field));
                addUnit(st, makeDefault({}, nullptr, tokenText));
                addUnit(st, makePhrase({}, nullptr, phrase->text, phrase->slop,
                                       phrase->suffixPresent));
              } else {
                addUnit(st, makePhrase(field, fieldType, phrase->text, phrase->slop,
                                       phrase->suffixPresent));
              }
              return;
            }
            // unterminated: the quote is not a quote; drop it and keep
            // scanning the token (pos already sits past it)
            buf.assign(tokenText.begin(), tokenText.end());
            continue;
          }
          // fall through: an ordinary term byte
        }
        if (!buf.empty() && c == '~' && fuzzySuffix(pos)) {
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

    // *:* - the traditional Lucene/Solr match-all spelling
    if (std::string_view(buf.data(), buf.size()) == "*:*") {
      api::Query all;
      all.kind = true;
      addUnit(st, allocQuery(all));
      return;
    }

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
            // field:* is the universal "has a value" idiom: an unbounded range
            // over the column (the same exists query expr compiles).
            if (prefix && !sawFuzzy && tail.empty()) {
              api::RangeQuery r;
              r.field = arenaStr(head);
              api::Query q;
              q.kind = r;
              addUnit(st, allocQuery(q));
              return;
            }
            // Otherwise numeric column fields take only an exact-match arm
            // (field:value).  Wildcard '*' / fuzzy '~' have no numeric meaning,
            // and a value that is not a valid number/date cannot be a numeric
            // query; both degrade to text (like an unknown field), declared.
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
    addUnit(st, branch);
  }

  void clearPending(Level& st) {
    st.plus = false;
    st.notCount = 0;
    st.orConj = false;
  }

  // Add a consumed unit as a clause, deriving its occur from the pending
  // modifiers/conjunction (classic Lucene QueryParser addClause, restricted to
  // this dialect's operator set: conjunction is none or '|'/OR, modifier is
  // none, '+'/required, or '-'/prohibited).  The default operator decides only
  // BARE clauses: '-' always prohibits and '+' always requires.  Under the
  // default-AND operator an OR conjunction demotes the preceding clause to
  // optional too, so `a | b` stays a disjunction.  A null branch (empty group)
  // just consumes the pending modifiers.
  void addUnit(Level& st, const api::Query* branch) {
    if (branch == nullptr) {
      clearPending(st);
      return;
    }
    bool prohibited = (st.notCount % 2 == 1);
    bool plus = st.plus;
    bool orConj = st.orConj;
    clearPending(st);

    if (defaultOccur == Occur::MUST && orConj && !st.clauses.empty()) {
      Clause& prev = st.clauses.back();
      if (prev.occur != Occur::MUST_NOT) prev.occur = Occur::SHOULD;
    }

    Occur occ = prohibited                     ? Occur::MUST_NOT
              : defaultOccur == Occur::MUST     ? (orConj ? Occur::SHOULD : Occur::MUST)
              :                                   (plus ? Occur::MUST : Occur::SHOULD);
    st.clauses.push_back({branch, occ});
  }
};

// Parse a simple_query string into an api::Query subtree in `arena`.
inline SimpleQueryResult parseSimpleQuery(std::string_view q, const SimpleQueryOptions& options,
                                          std::pmr::memory_resource& arena) {
  SimpleQueryParser parser(options, arena);
  return parser.parse(q);
}

} // namespace solux
