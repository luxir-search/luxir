#pragma once

#include <cstring>
#include <format>
#include <span>
#include <stdexcept>
#include <string_view>
#include <vector>

#include "solux/analysis/Analyzer.h"
#include "solux/query/BooleanQuery.h"
#include "solux/query/MatchNoDocsQuery.h"
#include "solux/query/PhraseQuery.h"
#include "solux/query/PrefixQuery.h"
#include "solux/query/Query.h"
#include "solux/query/TermQuery.h"
#include "solux/schema/Schema.h"

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
  // past the next pull must be copied out.
  std::string_view copyTerm(std::string_view term) {
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
  // Mirrors the Elasticsearch / OpenSearch match "operator".
  enum class Operator { OR, AND };

  QueryBuilder(MemPool& pool, Schema& schema) : pool(pool), schema(schema) {}

  Query* matchNoDocs() {
    return pool.make<MatchNoDocsQuery>();
  }

  // Build a prefix query over term-backed fields. The prefix is not analyzed,
  // and the field and prefix views must outlive the returned query.
  Query* createPrefixQuery(std::string_view field, std::string_view prefix) {
    FieldType& fieldType = *schema.getFieldTypeEx(field);
    switch (fieldType.type()) {
      case FieldType::Type::TEXT:
      case FieldType::Type::ID:
      case FieldType::Type::STRING:
        return pool.make<PrefixQuery>(field, prefix);
      default:
        throw std::runtime_error(std::format("Prefix query on unsupported field type: {}", field));
    }
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
        return pool.make<TermQuery>(field, value);
      default:
        throw std::runtime_error(std::format("Match query on unsupported field type: {}", field));
    }
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
