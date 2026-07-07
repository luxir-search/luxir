#pragma once

#include <cstring>
#include <format>
#include <limits>
#include <optional>
#include <span>
#include <stdexcept>
#include <string_view>
#include <vector>

#include "solux/analysis/Analyzer.h"
#include "solux/query/BooleanQuery.h"
#include "solux/query/FuzzyQuery.h"
#include "solux/query/MatchNoDocsQuery.h"
#include "solux/query/NumericRangeQuery.h"
#include "solux/query/PhraseQuery.h"
#include "solux/query/PrefixQuery.h"
#include "solux/query/Query.h"
#include "solux/query/TermQuery.h"
#include "solux/schema/Schema.h"
#include "solux/schema/ValCoerce.h"
#include "solux/util/StrRef.h"

namespace solux {

// Builds Query objects from primitives. Independent of JSON / Protobuf so both
// can use it.
// This is the single place where query-time text analysis is applied.
//
// Query nodes and any analyzed term bytes are allocated in the pool, which
// (with the caller's source bytes for pass-through terms) must outlive the
// returned query tree.
class QueryBuilder {
  MemPool& pool;
  Schema& schema;

public:
  // The numeric field types stored in the shared int column (INT raw,
  // FLOAT/DOUBLE sortable bits, DATE epoch millis).  Match and range on these
  // build a NumericRangeQuery over the column.  Public because schema-aware
  // string parsers select arms by the same classification.
  static bool isNumericColumnType(FieldType::Type t) {
    return t == FieldType::Type::INT || t == FieldType::Type::FLOAT
        || t == FieldType::Type::DOUBLE || t == FieldType::Type::DATE;
  }

private:

  // Resolve a field to its TextFieldType, throwing if it is not a text field:
  // phrase / analyzed queries only make sense over analyzed text.
  TextFieldType& textFieldType(std::string_view field) {
    FieldType& fieldType = *schema.getFieldTypeEx(field);
    if (fieldType.type() != FieldType::Type::TEXT) {
      throw std::runtime_error(std::format("Phrase query on non-text field: {}", field));
    }
    return (TextFieldType&)fieldType;
  }

  // Copy transient token bytes into the request pool. Analyzer chains reuse
  // their output buffers across tokens (the borrow contract), so a term we keep
  // past the next pull must be copied out. Terms are indexed truncated to
  // PackedTerm::MAX_LEN; truncate identically here so an oversized query token
  // matches what ingest indexed.
  std::string_view copyTerm(std::string_view term) {
    term = PackedTerm::truncate(term);
    if (term.empty()) return {};
    char* dst = pool.alloc(term.size());
    std::memcpy(dst, term.data(), term.size());
    return {dst, term.size()};
  }

  // Collapse a term list into the right query type:
  //   0 terms -> match nothing (a phrase with no terms cannot match, and
  //              PhraseQuery itself requires >= 2 terms)
  //   1 term  -> TermQuery (a one-term phrase is just a term match)
  //   N terms -> PhraseQuery
  // terms and positions must already live in storage that outlives the query
  // tree (the pool, or the caller's source bytes for pass-through terms).
  Query* buildPhrase(std::string_view field, std::span<std::string_view> terms,
                     std::span<const int32_t> positions) {
    if (terms.empty()) {
      return pool.make<MatchNoDocsQuery>();
    }
    if (terms.size() == 1) {
      return pool.make<TermQuery>(field, terms[0]);
    }
    return pool.make<PhraseQuery>(field, terms, positions);
  }

public:
  // How the multiple terms an analyzed text field produces are combined.
  // Mirrors the OpenSearch match "operator".
  enum class Operator { OR, AND };

  QueryBuilder(MemPool& pool, Schema& schema) : pool(pool), schema(schema) {}

  Query* matchNoDocs() {
    return pool.make<MatchNoDocsQuery>();
  }

  // Normalize multiterm query input (a prefix or fuzzy term) for a TEXT
  // field: the field's normalization chain applies - case/character folds,
  // never segmentation - so THOM* finds what "Thomas" indexed (the classic
  // multiterm trap; Lucene's Analyzer::normalize).  Returns a view that
  // outlives the query tree (the input, or a pool copy when folding rewrote
  // it).  STRING/ID input stays verbatim - callers skip this for them.
  std::string_view normalizeMultiterm(TextFieldType& fieldType, std::string_view field,
                                      std::string_view text) {
    auto chain = fieldType.createAnalyzer(field);
    std::string norm(text);
    chain->normalizeTerm(norm);
    if (norm == text) return text;
    return copyTerm(norm);
  }

  // Build a prefix query over term-backed fields. The prefix is normalized
  // (not tokenized) for analyzed TEXT fields and used verbatim for STRING/ID;
  // the field and prefix views must outlive the returned query.
  Query* createPrefixQuery(std::string_view field, std::string_view prefix) {
    FieldType& fieldType = *schema.getFieldTypeEx(field);
    switch (fieldType.type()) {
      case FieldType::Type::TEXT:
        prefix = normalizeMultiterm((TextFieldType&)fieldType, field, prefix);
        [[fallthrough]];
      case FieldType::Type::ID:
      case FieldType::Type::STRING:
        // Indexed terms carry at most PackedTerm::MAX_LEN bytes; a longer
        // prefix is truncated so it matches terms of oversized source values.
        return pool.make<PrefixQuery>(field, PackedTerm::truncate(prefix));
      default:
        throw std::runtime_error(std::format("Prefix query on unsupported field type: {}", field));
    }
  }

  // OpenSearch-style AUTO fuzziness by byte length.
  static int autoMaxEdits(size_t termLen) {
    if (termLen <= 2) return 0;
    if (termLen <= 5) return 1;
    return 2;
  }

  // Build a fuzzy query over term-backed fields. The term is normalized (not
  // tokenized) for analyzed TEXT fields - AUTO edits are computed from the
  // normalized bytes - and used verbatim for STRING/ID.
  // Defaults: maxEdits = AUTO, prefixLength = 1, maxExpansions = 0 (complete).
  Query* createFuzzyQuery(std::string_view field, std::string_view term,
                          std::optional<int> maxEdits = std::nullopt,
                          std::optional<int> prefixLength = std::nullopt,
                          int maxExpansions = 0) {
    FieldType& fieldType = *schema.getFieldTypeEx(field);
    switch (fieldType.type()) {
      case FieldType::Type::TEXT:
        term = normalizeMultiterm((TextFieldType&)fieldType, field, term);
        break;
      case FieldType::Type::ID:
      case FieldType::Type::STRING:
        break;
      default:
        throw std::runtime_error(std::format("Fuzzy query on unsupported field type: {}", field));
    }
    term = PackedTerm::truncate(term);  // indexed terms are truncated; compare in their space
    int resolvedEdits;
    if (!maxEdits) {
      resolvedEdits = autoMaxEdits(term.size());
    } else if (*maxEdits < 0 || *maxEdits > 2) {
      // Keep fuzzy within the common 0..2 range; wider scans get expensive fast.
      throw std::runtime_error(std::format("Fuzzy query max_edits must be 0..2 (got {})", *maxEdits));
    } else {
      resolvedEdits = *maxEdits;
    }
    // The default prefix limits scans; explicit 0 allows a full-field scan.
    int resolvedPrefix = prefixLength.value_or(1);
    if (resolvedPrefix < 0) {
      throw std::runtime_error(
        std::format("Fuzzy query prefix_length must not be negative (got {})", resolvedPrefix));
    }
    if (maxExpansions < 0) {
      throw std::runtime_error(
        std::format("Fuzzy query max_expansions must not be negative (got {})", maxExpansions));
    }
    return pool.make<FuzzyQuery>(field, term, resolvedEdits, resolvedPrefix, maxExpansions);
  }

  // Build a match query for `field` against raw value `value`.
  //   * TEXT field: run the field's analyzer and combine the resulting terms.
  //     Term bytes are copied into the pool (analyzer buffers are transient).
  //   * STRING / ID field: matched verbatim as a single term, no analysis. The
  //     value must outlive the query tree (caller's storage).
  // The 0/1/N collapse applies: 0 terms -> match nothing, 1 -> TermQuery,
  // N -> BooleanQuery.
  //
  // How the N terms combine is resolved to a "must match at least k of N":
  //   * minMatch > 0 wins (the OR..AND middle ground), clamped into [1, N].
  //   * else `op`: OR -> k = 1 (disjunction), AND -> k = N (conjunction).
  // k <= 1 builds a disjunction, k >= N a conjunction, and the middle a
  // min-should-match boolean.
  Query* createMatchQuery(std::string_view field, std::string_view value,
                          Operator op = Operator::OR, int minMatch = 0) {
    FieldType& fieldType = *schema.getFieldTypeEx(field);
    switch (fieldType.type()) {
      case FieldType::Type::TEXT: {
        auto& textType = (TextFieldType&)fieldType;
        auto chain = textType.createAnalyzer(field);
        TokenChain& tc = *chain;
        Token& tok = tc.head.getToken();
        TokenStream& tail = *tc.tail;

        std::vector<std::string_view> terms;
        tc.head.setValue(value);
        tc.reset();
        while (tail.incrementToken()) {
          terms.push_back(copyTerm(tok.text));
        }

        if (terms.empty()) {
          return matchNoDocs();
        }
        if (terms.size() == 1) {
          return pool.make<TermQuery>(field, terms[0]);
        }
        auto clauses = pool.make_span<Query*>(terms.size());
        for (size_t i = 0; i < terms.size(); i++) {
          clauses[i] = pool.make<TermQuery>(field, terms[i]);
        }
        int n = (int)terms.size();
        int k = minMatch > 0 ? minMatch : (op == Operator::AND ? n : 1);
        std::span<Query*> none{};
        if (k >= n) {
          return pool.make<BooleanQuery>(clauses, none, none, none);  // conjunction (all)
        }
        if (k <= 1) {
          return pool.make<BooleanQuery>(none, clauses, none, none);  // disjunction (any)
        }
        return pool.make<BooleanQuery>(none, clauses, none, none, k);  // min-should-match
      }
      case FieldType::Type::ID:
      case FieldType::Type::STRING:
        // Indexed truncated (StrHandler/IdHandler); truncate to match.
        return pool.make<TermQuery>(field, PackedTerm::truncate(value));
      case FieldType::Type::INT:
      case FieldType::Type::FLOAT:
      case FieldType::Type::DOUBLE:
      case FieldType::Type::DATE: {
        // Exact numeric match == a degenerate inclusive [v, v] range; route
        // through createRangeQuery so it shares the same column validation.
        if (value.empty()) return matchNoDocs();
        solux::api::Val v;
        v.kind = value;  // string arm; coerceColInt64 parses per the field type
        return createRangeQuery(field, &v, nullptr, &v, nullptr);
      }
      default:
        throw std::runtime_error(std::format("Match query on unsupported field type: {}", field));
    }
  }

  // Match against a wire Val: coerce to term text per the field type (the
  // query-time half of the coercion contract; see schema/ValCoerce.h), then
  // build as usual.  A numeric Val against a text/string field matches its
  // canonical decimal rendering - the same rendering ingest indexes - so
  // index-time and query-time coercion agree.  Uncoercible Vals (arrays,
  // maps) throw.
  Query* createMatchQuery(std::string_view field, const solux::api::Val& val,
                          Operator op = Operator::OR, int minMatch = 0) {
    FieldType& fieldType = *schema.getFieldTypeEx(field);
    if (isNumericColumnType(fieldType.type())) {
      // Exact numeric match == a degenerate inclusive [v, v] range; route
      // through createRangeQuery so it shares the same column validation.  An
      // array/uncoercible Val throws there (multi-value Match semantics are
      // undefined yet); op / min_match don't apply to a single numeric value.
      if (coerce::isNull(val)) return matchNoDocs();
      return createRangeQuery(field, &val, nullptr, &val, nullptr);
    }
    char buf[coerce::TEXT_BUF_SIZE];
    std::string_view text = coerce::isNull(val)
        ? std::string_view{}
        : fieldType.coerceTerm(val, field, buf);
    // A rendered numeric lives in stack-local buf: TEXT analysis copies terms
    // out anyway, but the STRING/ID pass-through keeps the view, so copy it
    // into the pool.  String/bytes arms view the request bytes, which already
    // outlive the query tree.
    if (text.data() == buf) {
      text = copyTerm(text);
    }
    return createMatchQuery(field, text, op, minMatch);
  }

  // Build a numeric range query over `field`'s column from raw bound Vals (any
  // may be null for an open-ended side).  At most one of gte/gt (lower) and one
  // of lte/lt (upper) may be set.  Bounds are coerced to the field's encoded
  // int64 (FieldType::coerceColInt64) and folded to an inclusive [lo, hi]
  // window; an empty range collapses to match-nothing.  The field must be a
  // column-stored numeric type.
  //
  // DATE bounds round by the granularity the literal names (the window from
  // DateFieldType::coerceDateRange): gte/lt use the window start, lte/gt the
  // window end, so [2024-01 TO 2024-06] covers January through June inclusive
  // and {2024-01 TO 2024-06} excludes both whole months.  Match on a DATE
  // field routes through here with gte == lte, so equality on a day matches
  // the whole day.
  Query* createRangeQuery(std::string_view field,
                          const solux::api::Val* gte, const solux::api::Val* gt,
                          const solux::api::Val* lte, const solux::api::Val* lt) {
    FieldType& fieldType = *schema.getFieldTypeEx(field);
    if (!isNumericColumnType(fieldType.type())) {
      throw std::runtime_error(std::format("Range query on unsupported field type: {}", field));
    }
    if (!fieldType.hasColumn()) {
      throw std::runtime_error(std::format("Range query field '{}' is not column-stored", field));
    }

    bool hasGte = gte && !coerce::isNull(*gte);
    bool hasGt  = gt  && !coerce::isNull(*gt);
    bool hasLte = lte && !coerce::isNull(*lte);
    bool hasLt  = lt  && !coerce::isNull(*lt);
    if (hasGte && hasGt) {
      throw std::runtime_error(std::format("Range query on '{}' sets both gte and gt", field));
    }
    if (hasLte && hasLt) {
      throw std::runtime_error(std::format("Range query on '{}' sets both lte and lt", field));
    }

    // Coerce every supplied bound first, so a malformed bound is always a
    // request error regardless of whether the range would collapse to empty.
    std::optional<int64_t> loEnc, hiEnc;
    if (fieldType.type() == FieldType::Type::DATE) {
      auto& dateType = (DateFieldType&)fieldType;
      // Window edges chosen so the existing +/-1 exclusive fold below lands
      // on the granule boundary: gt = the window's last milli (+1 = past it),
      // lt = the window's first milli (-1 = before it).
      if (hasGte) loEnc = dateType.coerceDateRange(*gte, field).first;
      if (hasGt)  loEnc = dateType.coerceDateRange(*gt, field).second - 1;
      if (hasLte) hiEnc = dateType.coerceDateRange(*lte, field).second - 1;
      if (hasLt)  hiEnc = dateType.coerceDateRange(*lt, field).first;
    } else {
      if (hasGte || hasGt) loEnc = fieldType.coerceColInt64(hasGte ? *gte : *gt, field);
      if (hasLte || hasLt) hiEnc = fieldType.coerceColInt64(hasLte ? *lte : *lt, field);
    }

    int64_t lo = std::numeric_limits<int64_t>::min();
    int64_t hi = std::numeric_limits<int64_t>::max();
    // Fold exclusive bounds to inclusive in encoded int64 space: since stored
    // values are integers, v > k  <=>  v >= k+1 (and v < k  <=>  v <= k-1).
    // Guard the extremes so the +/-1 never overflows.
    if (hasGte) {
      lo = *loEnc;
    } else if (hasGt) {
      if (*loEnc == std::numeric_limits<int64_t>::max()) return matchNoDocs();  // > max
      lo = *loEnc + 1;
    }
    if (hasLte) {
      hi = *hiEnc;
    } else if (hasLt) {
      if (*hiEnc == std::numeric_limits<int64_t>::min()) return matchNoDocs();  // < min
      hi = *hiEnc - 1;
    }
    if (lo > hi) return matchNoDocs();  // empty range
    return pool.make<NumericRangeQuery>(field, lo, hi);
  }

  // Build a phrase query from un-analyzed input by running the field's analyzer.
  // `values` is the raw text to analyze: a single element for a whole text
  // string ("Thomas Anderson"), or one element per word for a word list
  // (["Yonik","Seeley"]). Each value is analyzed independently.
  //
  // `valuePositions` is optional and, when set, parallel to `values` (the proto
  // words + positions form): it gives the intended phrase position of each
  // value.
  //   * empty: positions accumulate into one continuous stream, so a value that
  //     expands to several tokens occupies several positions and a value that
  //     drops to zero tokens contributes none (no gap).
  //   * provided: each value's tokens start at its given position; a value that
  //     expands to multiple tokens consumes extra positions, and every later
  //     value is shifted by that overflow so the caller's gaps survive
  //     multi-token expansion.
  Query* createPhraseQuery(std::string_view field, std::span<const std::string_view> values,
                           std::span<const int32_t> valuePositions = {}) {
    if (!valuePositions.empty() && valuePositions.size() != values.size()) {
      throw std::runtime_error(std::format(
        "Phrase query positions size {} does not match words size {}",
        valuePositions.size(), values.size()));
    }

    TextFieldType& fieldType = textFieldType(field);
    auto chain = fieldType.createAnalyzer(field);
    TokenChain& tc = *chain;
    Token& tok = tc.head.getToken();
    TokenStream& tail = *tc.tail;

    // Collected here, then copied into pool-backed spans. Query parsing is a
    // per-request cold path, so a transient std::vector is fine.
    std::vector<std::string_view> terms;
    std::vector<int32_t> positions;

    if (valuePositions.empty()) {
      int32_t pos = -1;  // first token's positionIncrement (>= 1) lands it at >= 0
      for (std::string_view value : values) {
        tc.head.setValue(value);
        tc.reset();
        while (tail.incrementToken()) {
          pos += tok.positionIncrement;
          terms.push_back(copyTerm(tok.text));
          positions.push_back(pos);
        }
      }
    } else {
      int32_t carry = 0;  // extra positions consumed by prior values' expansion
      for (size_t i = 0; i < values.size(); i++) {
        int32_t base = valuePositions[i] + carry;
        int32_t relPos = -1;  // position within this value, relative to base
        tc.head.setValue(values[i]);
        tc.reset();
        while (tail.incrementToken()) {
          relPos += tok.positionIncrement;
          terms.push_back(copyTerm(tok.text));
          positions.push_back(base + relPos);
        }
        // A single-token value spans relPos 0 (no overflow); a value spanning
        // relPos slots pushes the rest by relPos. A dropped value (relPos < 0)
        // leaves following positions untouched.
        if (relPos > 0) {
          carry += relPos;
        }
      }
    }

    if (terms.empty()) {
      return matchNoDocs();
    }
    return buildPhrase(field,
                       pool.copy_span(std::span<std::string_view>(terms.data(), terms.size())),
                       pool.copy_span(std::span<int32_t>(positions.data(), positions.size())));
  }

  // Build a phrase query from already-analyzed terms; the terms are used
  // verbatim, with no analysis. `positions` may be empty (defaults to
  // 0,1,2,...) or must match the term count.
  //
  // The provided span contents are not copied and thus should outlive the returned query.
  Query* createPhraseFromTerms(std::string_view field, std::span<std::string_view> terms,
                               std::span<const int32_t> positions) {
    textFieldType(field);  // validate it is a text field

    // Verbatim terms still honor the indexed-term length cap; rewrite entries
    // in place (the span is mutable by contract).
    for (auto& t : terms) t = PackedTerm::truncate(t);

    if (!positions.empty() && positions.size() != terms.size()) {
      throw std::runtime_error(std::format(
        "Phrase query positions size {} does not match terms size {}", positions.size(), terms.size()));
    }
    if (positions.empty() && terms.size() >= 2) {
      auto pos = pool.make_span<int32_t>(terms.size());
      for (size_t i = 0; i < terms.size(); i++) {
        pos[i] = (int32_t)i;
      }
      positions = pos;
    }
    return buildPhrase(field, terms, positions);
  }
};

} // namespace solux
