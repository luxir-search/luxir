// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cstring>
#include <format>
#include <limits>
#include <optional>
#include <span>
#include <stdexcept>
#include <string_view>
#include <variant>
#include <vector>

#include "luxir/analysis/Analyzer.h"
#include "luxir/query/BooleanQuery.h"
#include "luxir/query/AutomatonQuery.h"
#include "luxir/query/ConstantScoreQuery.h"
#include "luxir/query/ExistsQuery.h"
#include "luxir/query/FuzzyQuery.h"
#include "luxir/query/MatchNoDocsQuery.h"
#include "luxir/query/NumericPredicateQuery.h"
#include "luxir/query/PhraseQuery.h"
#include "luxir/query/PrefixQuery.h"
#include "luxir/query/Query.h"
#include "luxir/query/TermQuery.h"
#include "luxir/query/TermInSetQuery.h"
#include "luxir/query/TermRangeQuery.h"
#include "luxir/query/ValueSequence.h"
#include "luxir/query/FieldResolver.h"
#include "luxir/schema/ValCoerce.h"
#include "luxir/util/Clock.h"
#include "luxir/util/DateTime.h"
#include "luxir/util/NumericUtils.h"
#include "luxir/util/StrRef.h"
#include "luxir/util/automaton/WildcardCompiler.h"
#include "luxir/util/automaton/RegExpParser.h"

namespace luxir {

// Pool-backed, sorted, unique field-native values. Canonicalization is kept
// separate from query shape selection so facet pins and their derived filter
// can consume the same bytes instead of independently reproducing the rules.
class CanonicalValueSet {
  friend class QueryBuilder;

  using Terms = std::span<const std::string_view>;
  using Numerics = std::span<const int64_t>;
  ResolvedFieldHandle target{};
  std::variant<std::monostate, Terms, Numerics> values;
  std::variant<std::monostate, Terms, Numerics> inputOrder;

  explicit CanonicalValueSet(const ResolvedFieldHandle& target) : target(target) {}
  CanonicalValueSet(const ResolvedFieldHandle& target, Terms terms)
      : target(target), values(terms) {}
  CanonicalValueSet(const ResolvedFieldHandle& target, Numerics numerics)
      : target(target), values(numerics) {}
  CanonicalValueSet(const ResolvedFieldHandle& target, Terms terms, Terms inputOrder)
      : target(target), values(terms), inputOrder(inputOrder) {}
  CanonicalValueSet(const ResolvedFieldHandle& target, Numerics numerics,
                    Numerics inputOrder)
      : target(target), values(numerics), inputOrder(inputOrder) {}

public:
  CanonicalValueSet() = default;

  bool empty() const {
    return std::visit([](const auto& viewed) -> bool {
      using Viewed = std::decay_t<decltype(viewed)>;
      if constexpr (std::is_same_v<Viewed, std::monostate>) return true;
      else return viewed.empty();
    }, values);
  }

  size_t size() const {
    return std::visit([](const auto& viewed) -> size_t {
      using Viewed = std::decay_t<decltype(viewed)>;
      if constexpr (std::is_same_v<Viewed, std::monostate>) return 0;
      else return viewed.size();
    }, values);
  }

  bool termBacked() const { return std::holds_alternative<Terms>(values); }
  bool numeric() const { return std::holds_alternative<Numerics>(values); }

  Terms terms() const { return std::get<Terms>(values); }
  Numerics numerics() const { return std::get<Numerics>(values); }
  Terms termsInInputOrder() const {
    if (auto* ordered = std::get_if<Terms>(&inputOrder)) return *ordered;
    return terms();
  }
  Numerics numericsInInputOrder() const {
    if (auto* ordered = std::get_if<Numerics>(&inputOrder)) return *ordered;
    return numerics();
  }

  CanonicalValueSet element(size_t index) const {
    if (termBacked()) {
      return CanonicalValueSet(target, terms().subspan(index, 1));
    }
    return CanonicalValueSet(target, numerics().subspan(index, 1));
  }
};

// Builds Query objects from primitives. Independent of JSON / Protobuf so both
// can use it.
// This is the single place where query-time text analysis is applied.
//
// Query nodes and any analyzed term bytes are allocated in the pool, which
// (with the caller's source bytes for pass-through terms) must outlive the
// returned query tree.
class QueryBuilder {
  MemPool& pool;
  FieldResolver localFields;
  FieldResolver& fields;
  CoerceContext coerceContext;
  std::string_view opName;
  std::vector<api::Warning>* warnings;

  // Per-codepoint approximation of field folding for automaton literals.
  // Combining-sequence patterns may not compose exactly like whole terms.
  class FieldCodepointFolder final : public automaton::CodepointFolder {
    TokenChain* chain;

    static void appendUtf8(std::string& out, int32_t codepoint) {
      if (codepoint < 0x80) out.push_back((char)codepoint);
      else if (codepoint < 0x800) {
        out.push_back((char)(0xc0 | (codepoint >> 6)));
        out.push_back((char)(0x80 | (codepoint & 0x3f)));
      } else if (codepoint < 0x10000) {
        out.push_back((char)(0xe0 | (codepoint >> 12)));
        out.push_back((char)(0x80 | ((codepoint >> 6) & 0x3f)));
        out.push_back((char)(0x80 | (codepoint & 0x3f)));
      } else {
        out.push_back((char)(0xf0 | (codepoint >> 18)));
        out.push_back((char)(0x80 | ((codepoint >> 12) & 0x3f)));
        out.push_back((char)(0x80 | ((codepoint >> 6) & 0x3f)));
        out.push_back((char)(0x80 | (codepoint & 0x3f)));
      }
    }

  public:
    explicit FieldCodepointFolder(TokenChain* chain) : chain(chain) {}

    int32_t fold(int32_t codepoint, int32_t* out, int32_t maxOut) const override {
      std::string bytes;
      appendUtf8(bytes, codepoint);
      chain->normalizeTerm(bytes);
      int32_t count = 0;
      for (size_t position = 0; position < bytes.size();) {
        unsigned char first = bytes[position++];
        int32_t width = first < 0x80 ? 1 : (first & 0xe0) == 0xc0 ? 2
            : (first & 0xf0) == 0xe0 ? 3 : (first & 0xf8) == 0xf0 ? 4 : 0;
        if (width == 0 || position + (size_t)width - 1 > bytes.size()) {
          throw std::runtime_error("field normalizer returned invalid UTF-8");
        }
        int32_t folded = width == 1 ? first : first & ((1 << (7 - width)) - 1);
        for (int32_t i = 1; i < width; i++) {
          unsigned char byte = bytes[position++];
          if ((byte & 0xc0) != 0x80) throw std::runtime_error("field normalizer returned invalid UTF-8");
          folded = (folded << 6) | (byte & 0x3f);
        }
        if ((width == 2 && folded < 0x80) || (width == 3 && folded < 0x800)
            || (width == 4 && (folded < 0x10000 || folded > 0x10ffff))
            || (folded >= 0xd800 && folded <= 0xdfff)) {
          throw std::runtime_error("field normalizer returned invalid UTF-8");
        }
        if (count == maxOut) throw std::runtime_error("field normalizer expanded a literal too far");
        out[count++] = folded;
      }
      return count;
    }
  };

public:
  // The numeric field types stored in the shared int column (INT raw,
  // FLOAT/DOUBLE sortable bits, DATE epoch millis).  Match and range on these
  // build a NumericPredicateQuery over the column. Public because schema-aware
  // string parsers select arms by the same classification.
  static bool isNumericColumnType(FieldType::Type t) {
    return t == FieldType::Type::INT || t == FieldType::Type::FLOAT
        || t == FieldType::Type::DOUBLE || t == FieldType::Type::DATE;
  }

private:

  template<typename T>
  int64_t coerceAnyOfNumeric(FieldType& fieldType, std::string_view field,
                             const T& value) {
    if constexpr (std::is_same_v<T, api::Val>) {
      return fieldType.coerceColInt64(value, field, coerceContext);
    } else {
      switch (fieldType.type()) {
        case FieldType::Type::INT:
          return coerce::toInt64Scalar(value, field);
        case FieldType::Type::FLOAT:
          return (int64_t)floatToSortableInt32(
              coerce::toFloatScalar(value, field));
        case FieldType::Type::DOUBLE:
          return doubleToSortableInt64(
              coerce::toDoubleScalar(value, field));
        case FieldType::Type::DATE:
          if constexpr (std::is_same_v<T, int64_t>) {
            return value;
          } else if constexpr (std::is_same_v<T, std::string_view>) {
            int64_t now = coerceContext.dateMathNowEpochMillis.value_or(
                currentEpochMillis());
            if (auto millis = parseDateToEpochMillis(
                    value, now, coerceContext.timeZone)) {
              return *millis;
            }
            throw std::runtime_error(std::format(
                "DATE field '{}': cannot parse '{}' as a date "
                "(expected ISO-8601, epoch millis, or date math)",
                field, value));
          } else {
            api::Val scalar = coerce::scalarVal(value);
            coerce::throwCoerce(field, scalar, "a date");
          }
        default:
          break;
      }
      api::Val scalar = coerce::scalarVal(value);
      return fieldType.coerceColInt64(scalar, field, coerceContext);
    }
  }

  template<typename T>
  std::string_view coerceAnyOfTerm(FieldType& fieldType,
                                   std::string_view field, const T& value,
                                   std::span<char> buf) {
    if constexpr (std::is_same_v<T, api::Val>) {
      return fieldType.coerceTerm(value, field, buf);
    } else {
      return coerce::toTextScalar(value, field, buf);
    }
  }

  // Resolve a field to its TextFieldType, throwing if it is not a text field:
  // phrase / analyzed queries only make sense over analyzed text.
  TextFieldType& textFieldType(const ResolvedFieldHandle& target) {
    const auto& field = target.physicalName;
    FieldType& fieldType = *target.fieldType;
    if (fieldType.type() != FieldType::Type::TEXT) {
      throw std::runtime_error(std::format("Phrase query on non-text field: {}", field));
    }
    return (TextFieldType&)fieldType;
  }

  TextFieldType& positionalTextFieldType(const ResolvedFieldHandle& target) {
    const auto& field = target.physicalName;
    TextFieldType& fieldType = textFieldType(target);
    if (!fieldType.hasPositions()) {
      throw std::runtime_error(std::format(
          "Phrase query requires indexed positions on field: {}", field));
    }
    return fieldType;
  }

  static void validatePositions(std::span<const int32_t> positions,
                                size_t valueCount, std::string_view inputName) {
    if (!positions.empty() && positions.size() != valueCount) {
      throw std::runtime_error(std::format(
          "Phrase query positions size {} does not match {} size {}",
          positions.size(), inputName, valueCount));
    }
    int32_t previous = -1;
    for (int32_t position : positions) {
      if (position < 0) {
        throw std::runtime_error("Phrase query positions must be nonnegative");
      }
      if (position < previous) {
        throw std::runtime_error("Phrase query positions must be nondecreasing");
      }
      previous = position;
    }
  }

  std::string_view normalizeMultiterm(TokenChain& chain,
                                      std::string_view text) {
    std::string norm(text);
    chain.normalizeTerm(norm);
    if (norm == text) return text;
    return poolCopy(norm);
  }

  std::string_view fitTerm(const ResolvedFieldHandle& target, std::string_view text) {
    PackedTerm::TermBuffer scratch;
    auto term = PackedTerm::fitTerm(target.fieldType->longTerms, text, scratch);
    if (!term) {
      throw std::runtime_error(std::format(
          "Field '{}': exact value is {} bytes after normalization; maximum is {}. "
          "Use match/phrase with shorter analyzed tokens or a shorter whole-value string variant",
          target.physicalName, text.size(), PackedTerm::MAX_LEN));
    }
    return term->data() == scratch.data() ? poolCopy(*term) : *term;
  }

  // Collapse a term list into the right query type:
  //   0 terms -> match nothing (a phrase with no terms cannot match, and
  //              PhraseQuery itself requires >= 2 terms)
  //   1 term  -> TermQuery (a one-term phrase is just a term match)
  //   N terms -> PhraseQuery
  // terms and positions must already live in storage that outlives the query
  // tree (the pool, or the caller's source bytes for pass-through terms).
  Query* buildPhrase(const ResolvedFieldHandle& target, std::span<std::string_view> inputTerms,
                     std::span<const int64_t> inputPositions, int32_t slop) {
    auto field = poolCopy(target.physicalName);
    if (slop < 0) {
      throw std::runtime_error("Phrase query slop must be nonnegative");
    }
    positionalTextFieldType(target);
    if (inputTerms.size() != inputPositions.size()) {
      throw std::runtime_error("Phrase query internal term/position size mismatch");
    }
    std::vector<std::string_view> terms;
    std::vector<int32_t> positions;
    terms.reserve(inputTerms.size());
    positions.reserve(inputPositions.size());
    int64_t base = inputPositions.empty() ? 0 : inputPositions[0];
    for (size_t i = 0; i < inputTerms.size(); i++) {
      std::string_view term = fitTerm(target, inputTerms[i]);
      int64_t normalized = inputPositions[i] - base;
      if (normalized < 0 || normalized > std::numeric_limits<int32_t>::max()) {
        throw std::runtime_error(
            "Phrase query normalized positions must be in [0, INT_MAX]");
      }
      int32_t position = (int32_t) normalized;
      bool duplicate = false;
      for (size_t j = 0; j < terms.size(); j++) {
        if (terms[j] == term && positions[j] == position) {
          duplicate = true;
          break;
        }
      }
      if (!duplicate) {
        terms.push_back(term);
        positions.push_back(position);
      }
    }

    if (terms.empty()) {
      return pool.make<MatchNoDocsQuery>();
    }
    if (terms.size() == 1) {
      return pool.make<TermQuery>(field, terms[0]);
    }
    return pool.make<PhraseQuery>(
        field,
        pool.copy_span(std::span<std::string_view>(terms.data(), terms.size())),
        pool.copy_span(std::span<int32_t>(positions.data(), positions.size())),
        slop);
  }

public:
  // How the multiple terms an analyzed text field produces are combined.
  // Mirrors the OpenSearch match "operator".
  enum class Operator { OR, AND };

  QueryBuilder(MemPool& pool, Schema& schema, const CoerceContext& coerceContext,
               std::string_view opName = {},
               std::vector<api::Warning>* warnings = nullptr, FieldResolver* fields = nullptr)
    : pool(pool), localFields(schema), fields(fields ? *fields : localFields),
      coerceContext(coerceContext), opName(opName),
      warnings(warnings) {}

  Query* matchNoDocs() {
    return pool.make<MatchNoDocsQuery>();
  }

  Query* createExistsQuery(std::string_view field) {
    return createExistsQuery(fields.resolve(field, OpClass::EXISTS, opName));
  }

  Query* createExistsQuery(const ResolvedFieldHandle& target) {
    auto field = poolCopy(target.physicalName);
    FieldType& fieldType = *target.fieldType;
    if (!fieldType.indexed() && !fieldType.hasColumn()) {
      throw std::runtime_error(std::format(
          "Exists query requires an indexed or column-stored field: {}", field));
    }
    return pool.make<ExistsQuery>(field);
  }

  CanonicalValueSet canonicalizeFieldValues(
      std::string_view field, ValueSequence values,
      bool preserveInputOrder = false) {
    return canonicalizeFieldValues(fields.resolve(field, OpClass::VALUE, opName), values, preserveInputOrder);
  }

  CanonicalValueSet canonicalizeFieldValues(
      const ResolvedFieldHandle& target, ValueSequence values,
      bool preserveInputOrder = false) {
    auto field = poolCopy(target.physicalName);
    if (values.empty()) {
      return CanonicalValueSet(target);
    }

    FieldType& fieldType = *target.fieldType;
    if (isNumericColumnType(fieldType.type())) {
      std::vector<int64_t> encoded;
      encoded.reserve(values.size());
      values.visit([&](const auto& viewed) {
        for (const auto& value : viewed) {
          encoded.push_back(coerceAnyOfNumeric(fieldType, field, value));
        }
      });
      std::vector<int64_t> inputOrder;
      if (preserveInputOrder) {
        for (int64_t value : encoded) {
          if (!std::ranges::contains(inputOrder, value)) {
            inputOrder.push_back(value);
          }
        }
      }
      std::sort(encoded.begin(), encoded.end());
      encoded.erase(std::unique(encoded.begin(), encoded.end()), encoded.end());
      auto stored = pool.copy_span(std::span<int64_t>(encoded));
      if (preserveInputOrder) {
        auto ordered = pool.copy_span(std::span<int64_t>(inputOrder));
        return CanonicalValueSet(
            target, std::span<const int64_t>(stored),
            std::span<const int64_t>(ordered));
      }
      return CanonicalValueSet(
          target, std::span<const int64_t>(stored));
    }

    bool termBacked = fieldType.type() == FieldType::Type::TEXT
        || fieldType.type() == FieldType::Type::STRING
        || fieldType.type() == FieldType::Type::ID;
    if (!termBacked) {
      throw std::runtime_error(std::format(
          "Field '{}' does not support exact value canonicalization",
          field));
    }

    auto* normalizer = fields.chain(target);
    std::vector<std::string_view> terms;
    terms.reserve(values.size());
    values.visit([&](const auto& viewed) {
      for (const auto& value : viewed) {
        char buf[coerce::TEXT_BUF_SIZE];
        std::string_view term = coerceAnyOfTerm(fieldType, field, value, buf);
        if (fieldType.type() == FieldType::Type::TEXT) {
          normalizer->head.setValue(term);
          normalizer->reset();
          if (!normalizer->tail->incrementToken()) continue;
          // Token bytes are borrowed only until the next pull, including EOF.
          term = poolCopy(normalizer->head.getToken().text);
          if (normalizer->tail->incrementToken()) {
            throw std::runtime_error(std::format(
                "Field '{}': any_of / := requires a single term per exact TEXT value. "
                "Use match (field:value), phrase (field:\"words\"), or a whole-value string variant",
                field));
          }
          term = fitTerm(target, term);
        } else {
          term = normalizeLiteral(target, term, true);
          if (term.data() == buf) term = poolCopy(term);
        }
        terms.push_back(term);
      }
    });
    std::vector<std::string_view> inputOrder;
    if (preserveInputOrder) {
      for (std::string_view term : terms) {
        if (!std::ranges::contains(inputOrder, term)) {
          inputOrder.push_back(term);
        }
      }
    }
    std::sort(terms.begin(), terms.end());
    terms.erase(std::unique(terms.begin(), terms.end()), terms.end());
    auto stored = pool.copy_span(std::span<std::string_view>(terms));
    if (preserveInputOrder) {
      auto ordered = pool.copy_span(std::span<std::string_view>(inputOrder));
      return CanonicalValueSet(
          target, std::span<const std::string_view>(stored),
          std::span<const std::string_view>(ordered));
    }
    return CanonicalValueSet(
        target, std::span<const std::string_view>(stored));
  }

  Query* createAnyOfQuery(const CanonicalValueSet& values) {
    if (values.empty()) return matchNoDocs();

    auto field = poolCopy(values.target.physicalName);
    FieldType& fieldType = *values.target.fieldType;
    if (values.numeric()) {
      if (!isNumericColumnType(fieldType.type()) || !fieldType.hasColumn()) {
        throw std::runtime_error(std::format(
            "AnyOf query field '{}' does not support exact equality through an index or column",
            field));
      }
      auto encoded = values.numerics();
      if (encoded.size() == 1) {
        return pool.make<NumericPredicateQuery>(field, encoded[0], encoded[0]);
      }
      auto intervals = pool.make_span<PointsReader::ValueRange>(encoded.size());
      size_t intervalCount = 0;
      for (int64_t value : encoded) {
        if (intervalCount != 0
            && intervals[intervalCount - 1].hi
                != std::numeric_limits<int64_t>::max()
            && value == intervals[intervalCount - 1].hi + 1) {
          intervals[intervalCount - 1].hi = value;
        } else {
          intervals[intervalCount++] = {value, value};
        }
      }
      return pool.make<NumericPredicateQuery>(
          field, encoded, intervals.first(intervalCount));
    }

    bool termBacked = fieldType.type() == FieldType::Type::TEXT
        || fieldType.type() == FieldType::Type::STRING
        || fieldType.type() == FieldType::Type::ID;
    if (!values.termBacked() || !termBacked || !fieldType.indexed()) {
      throw std::runtime_error(std::format(
          "AnyOf query field '{}' does not support exact equality through an index or column",
          field));
    }
    auto terms = values.terms();
    if (terms.size() == 1) {
      Query* term = pool.make<TermQuery>(field, terms[0]);
      return pool.make<ConstantScoreQuery>(term);
    }
    return pool.make<TermInSetQuery>(field, terms);
  }

  std::string_view normalizeLiteral(const ResolvedFieldHandle& target,
                                    std::string_view text, bool exact) {
    if (auto* chain = fields.chain(target)) text = normalizeMultiterm(*chain, text);
    if (exact && (target.fieldType->type() != FieldType::STRING || target.fieldType->indexed())) {
      text = fitTerm(target, text);
    }
    return text;
  }

  // Build a prefix query over term-backed fields. The prefix is normalized
  // (not tokenized) for analyzed TEXT fields and normalized for STRING; ID is verbatim;
  // the field and prefix views must outlive the returned query.
  Query* createPrefixQuery(std::string_view field, std::string_view prefix) {
    return createPrefixQuery(fields.resolve(field, OpClass::SEARCH, opName), prefix);
  }

  Query* createPrefixQuery(const ResolvedFieldHandle& target, std::string_view prefix) {
    auto field = poolCopy(target.physicalName);
    FieldType& fieldType = *target.fieldType;
    switch (fieldType.type()) {
      case FieldType::Type::TEXT:
      case FieldType::Type::ID:
      case FieldType::Type::STRING:
        prefix = normalizeLiteral(target, prefix, false);
        if (!fieldType.indexed()) {
          throw std::runtime_error(std::format(
              "Prefix query requires an indexed field: {}", field));
        }
        return pool.make<PrefixQuery>(field, PackedTerm::fitPrefix(fieldType.longTerms, prefix));
      default:
        throw std::runtime_error(std::format("Prefix query on unsupported field type: {}", field));
    }
  }

  Query* createWildcardQuery(std::string_view field, std::string_view pattern) {
    return createWildcardQuery(fields.resolve(field, OpClass::SEARCH, opName), pattern);
  }

  Query* createWildcardQuery(const ResolvedFieldHandle& target, std::string_view pattern) {
    auto field = poolCopy(target.physicalName);
    checkAutomatonField("Wildcard", target);
    if (auto* chain = fields.chain(target)) {
      FieldCodepointFolder folder(chain);
      return makeAutomatonQuery(AutomatonQuery::Kind::WILDCARD, "Wildcard", target, field, pattern,
                                &folder, automaton::compileWildcard);
    }
    return makeAutomatonQuery(AutomatonQuery::Kind::WILDCARD, "Wildcard", target, field, pattern,
                              nullptr, automaton::compileWildcard);
  }

  Query* createRegexQuery(std::string_view field, std::string_view pattern) {
    return createRegexQuery(fields.resolve(field, OpClass::SEARCH, opName), pattern);
  }

  Query* createRegexQuery(const ResolvedFieldHandle& target, std::string_view pattern) {
    auto field = poolCopy(target.physicalName);
    checkAutomatonField("Regex", target);
    if (auto* chain = fields.chain(target)) {
      FieldCodepointFolder folder(chain);
      return makeAutomatonQuery(AutomatonQuery::Kind::REGEX, "Regex", target, field, pattern,
                                &folder, automaton::compileRegex);
    }
    return makeAutomatonQuery(AutomatonQuery::Kind::REGEX, "Regex", target, field, pattern,
                              nullptr, automaton::compileRegex);
  }

private:
  FieldType* checkAutomatonField(std::string_view label, const ResolvedFieldHandle& target) {
    const auto& field = target.physicalName;
    FieldType& fieldType = *target.fieldType;
    switch (fieldType.type()) {
      case FieldType::Type::TEXT:
      case FieldType::Type::ID:
      case FieldType::Type::STRING:
        break;
      default:
        throw std::runtime_error(std::format("{} query on unsupported field type: {}", label, field));
    }
    if (!fieldType.indexed()) {
      throw std::runtime_error(std::format("{} query requires an indexed field: {}", label, field));
    }
    return &fieldType;
  }

  std::string_view poolCopy(std::string_view bytes) {
    if (bytes.empty()) return {};
    char* copy = pool.alloc(bytes.size());
    memcpy(copy, bytes.data(), bytes.size());
    return {copy, bytes.size()};
  }

  Query* makeAutomatonQuery(AutomatonQuery::Kind queryKind, std::string_view label,
                            const ResolvedFieldHandle& target,
                            std::string_view field, std::string_view pattern,
                            const automaton::CodepointFolder* folder,
                            automaton::ByteDfa (*compile)(std::string_view, automaton::Budget&,
                                                          const automaton::CodepointFolder*)) {
    automaton::ByteDfa compiled;
    try {
      automaton::Budget budget;
      compiled = compile(pattern, budget, folder);
    } catch (const std::runtime_error& e) {
      throw std::runtime_error(std::format("{} query for field '{}' pattern '{}': {}",
                                           label, field, pattern, e.what()));
    }
    // Do this before classify(), which treats languages requiring >255 bytes
    // as empty. The DFA prefix already accounts for escapes and field folding.
    auto prefixAndState = compiled.commonPrefixAndState();
    auto kept = PackedTerm::fitPrefix(target.fieldType->longTerms, prefixAndState.first);
    if (kept.size() < prefixAndState.first.size()) {
      return pool.make<PrefixQuery>(field, poolCopy(kept));
    }
    std::string exactOrPrefix;
    automaton::ByteDfaKind kind = compiled.classify(&exactOrPrefix);
    if (kind == automaton::ByteDfaKind::NONE) return pool.make<MatchNoDocsQuery>();
    std::string commonPrefix;
    std::string commonSuffix;
    automaton::ByteDfaView::State initialState = compiled.start();
    if (kind == automaton::ByteDfaKind::NORMAL) {
      commonPrefix = std::move(prefixAndState.first);
      auto state = prefixAndState.second;
      commonSuffix = compiled.commonSuffix();
      initialState = state;
      exactOrPrefix.clear();
    }
    std::string_view patternCopy = poolCopy(pattern);
    std::string_view exactOrPrefixCopy = poolCopy(exactOrPrefix);
    DfaScanPlan scanPlan{poolCopy(commonPrefix), poolCopy(commonSuffix), initialState};
    automaton::ByteDfaView frozen = compiled.freeze(pool);
    return pool.make<AutomatonQuery>(field, queryKind, patternCopy, frozen, kind,
                                    exactOrPrefixCopy, scanPlan);
  }

public:

  // OpenSearch-style AUTO fuzziness by byte length.
  static int autoMaxEdits(size_t termLen) {
    if (termLen <= 2) return 0;
    if (termLen <= 5) return 1;
    return 2;
  }

  // Build a fuzzy query over term-backed fields. The term is normalized (not
  // tokenized) for analyzed TEXT fields - AUTO edits are computed from the
  // normalized bytes - and normalized for STRING; ID is verbatim.
  // Defaults: maxEdits = AUTO, prefixLength = 1, maxExpansions = 0 (complete).
  Query* createFuzzyQuery(std::string_view field, std::string_view term,
                          std::optional<int> maxEdits = std::nullopt,
                          std::optional<int> prefixLength = std::nullopt,
                          int maxExpansions = 0) {
    return createFuzzyQuery(fields.resolve(field, OpClass::SEARCH, opName), term, maxEdits, prefixLength, maxExpansions);
  }

  Query* createFuzzyQuery(const ResolvedFieldHandle& target, std::string_view term,
                          std::optional<int> maxEdits = std::nullopt,
                          std::optional<int> prefixLength = std::nullopt,
                          int maxExpansions = 0) {
    auto field = poolCopy(target.physicalName);
    FieldType& fieldType = *target.fieldType;
    switch (fieldType.type()) {
      case FieldType::Type::TEXT:
      case FieldType::Type::ID:
      case FieldType::Type::STRING:
        term = normalizeLiteral(target, term, false);
        break;
      default:
        throw std::runtime_error(std::format("Fuzzy query on unsupported field type: {}", field));
    }
    term = fitTerm(target, term); // Edit distance is measured in indexed term bytes.
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
  //   * STRING / ID field: one term, normalized for STRING, verbatim for ID. The
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
    return createMatchQuery(fields.resolve(field, OpClass::SEARCH, opName), value, op, minMatch);
  }

  Query* createMatchQuery(const ResolvedFieldHandle& target, std::string_view value,
                          Operator op = Operator::OR, int minMatch = 0) {
    auto field = poolCopy(target.physicalName);
    FieldType& fieldType = *target.fieldType;
    switch (fieldType.type()) {
      case FieldType::Type::TEXT: {
        auto* chain = fields.chain(target);
        TokenChain& tc = *chain;
        Token& tok = tc.head.getToken();
        TokenStream& tail = *tc.tail;

        std::vector<std::string_view> terms;
        tc.head.setValue(value);
        tc.reset();
        while (tail.incrementToken()) {
          // Analyzer buffers are borrowed until the next pull.
          terms.push_back(poolCopy(fitTerm(target, tok.text)));
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
        // BooleanQuery dedups after this k decision. For k <= 1, SHOULD
        // dedup is safe; for k >= n the conjunction arm stores no k; for the
        // middle min-should-match arm the ctor's msm > 1 guard preserves
        // duplicate scorer instances.
        std::span<Query*> none{};
        if (k >= n) {
          return pool.make<BooleanQuery>(clauses, none, none, none);  // conjunction (all)
        }
        if (k <= 1) {
          return pool.make<BooleanQuery>(none, clauses, none, none);  // disjunction (any)
        }
        return pool.make<BooleanQuery>(none, clauses, none, none, k);  // min-should-match
      }
      case FieldType::Type::STRING:
      case FieldType::Type::ID:
        return pool.make<TermQuery>(field, normalizeLiteral(target, value, true));
      case FieldType::Type::INT:
      case FieldType::Type::FLOAT:
      case FieldType::Type::DOUBLE:
      case FieldType::Type::DATE: {
        // Exact numeric match == a degenerate inclusive [v, v] range; route
        // through createRangeQuery so it shares the same column validation.
        if (value.empty()) return matchNoDocs();
        luxir::api::Val v;
        v.kind = value;  // string arm; coerceColInt64 parses per the field type
        return createRangeQuery(target, &v, nullptr, &v, nullptr);
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
  Query* createMatchQuery(std::string_view field, const luxir::api::Val& val,
                          Operator op = Operator::OR, int minMatch = 0) {
    return createMatchQuery(fields.resolve(field, OpClass::SEARCH, opName), val, op, minMatch);
  }

  Query* createMatchQuery(const ResolvedFieldHandle& target, const luxir::api::Val& val,
                          Operator op = Operator::OR, int minMatch = 0) {
    auto field = poolCopy(target.physicalName);
    FieldType& fieldType = *target.fieldType;
    if (isNumericColumnType(fieldType.type())) {
      // Exact numeric match == a degenerate inclusive [v, v] range; route
      // through createRangeQuery so it shares the same column validation.  An
      // array/uncoercible Val throws there (multi-value Match semantics are
      // undefined yet); op / min_match don't apply to a single numeric value.
      if (coerce::isNull(val)) return matchNoDocs();
      return createRangeQuery(target, &val, nullptr, &val, nullptr);
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
      text = poolCopy(text);
    }
    return createMatchQuery(target, text, op, minMatch);
  }

  // Build a range query over `field` from raw bound Vals (any may be null for
  // an open-ended side).  At most one of gte/gt (lower) and one of lte/lt
  // (upper) may be set.
  //
  // Numeric/date column fields build a NumericPredicateQuery: bounds coerce to
  // the field's encoded int64 (FieldType::coerceColInt64) and fold to an
  // inclusive [lo, hi] window; an empty range collapses to match-nothing.
  //
  // DATE bounds round by the granularity the literal names (the window from
  // DateFieldType::coerceDateRange): gte/lt use the window start, lte/gt the
  // window end, so [2024-01 TO 2024-06] covers January through June inclusive
  // and {2024-01 TO 2024-06} excludes both whole months.  Match on a DATE
  // field routes through here with gte == lte, so equality on a day matches
  // the whole day.
  //
  // Term-backed fields (TEXT/STRING/ID) build a constant-scoring
  // TermRangeQuery over the terms dictionary in byte order: bounds coerce to
  // term bytes, TEXT bounds fold like the field folds (normalizeMultiterm),
  // and exact bounds obey the post-normalization length limit.
  Query* createRangeQuery(std::string_view field,
                          const luxir::api::Val* gte, const luxir::api::Val* gt,
                          const luxir::api::Val* lte, const luxir::api::Val* lt) {
    return createRangeQuery(fields.resolve(field, OpClass::VALUE, opName), gte, gt, lte, lt);
  }

  Query* createRangeQuery(const ResolvedFieldHandle& target,
                          const luxir::api::Val* gte, const luxir::api::Val* gt,
                          const luxir::api::Val* lte, const luxir::api::Val* lt) {
    auto field = poolCopy(target.physicalName);
    FieldType& fieldType = *target.fieldType;
    bool numeric = isNumericColumnType(fieldType.type());
    switch (fieldType.type()) {
      case FieldType::Type::TEXT:
      case FieldType::Type::ID:
      case FieldType::Type::STRING:
        break;
      default:
        if (!numeric) {
          throw std::runtime_error(std::format("Range query on unsupported field type: {}", field));
        }
        if (!fieldType.hasColumn()) {
          throw std::runtime_error(std::format("Range query field '{}' is not column-stored", field));
        }
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

    if (!numeric) {
      auto termBound = [&](const api::Val& v) -> std::string_view {
        char buf[coerce::TEXT_BUF_SIZE];
        std::string_view t = fieldType.coerceTerm(v, field, buf);
        if (t.data() == buf) t = poolCopy(t);  // rendered numerics live in stack buf
        return normalizeLiteral(target, t, true);
      };
      std::optional<std::string_view> lower, upper;
      if (hasGte || hasGt) lower = termBound(hasGte ? *gte : *gt);
      if (hasLte || hasLt) upper = termBound(hasLte ? *lte : *lt);
      return pool.make<TermRangeQuery>(field, lower, hasGte, upper, hasLte);
    }

    // Coerce every supplied bound first, so a malformed bound is always a
    // request error regardless of whether the range would collapse to empty.
    std::optional<int64_t> loEnc, hiEnc;
    if (fieldType.type() == FieldType::Type::DATE) {
      auto& dateType = (DateFieldType&)fieldType;
      auto dateBound = [&](const api::Val& value) {
        DateRange range = dateType.coerceDateRange(value, field, coerceContext);
        if (range.granuleSkipped && warnings != nullptr) {
          std::string_view text = std::get<std::string_view>(value.kind);
          std::string message = opName.empty()
              ? std::format(
                    "DATE field '{}': date granule '{}' skipped: no such local granule in {}",
                    field, text, coerceContext.timeZone.name())
              : std::format(
                    "op '{}', DATE field '{}': date granule '{}' skipped: "
                    "no such local granule in {}",
                    opName, field, text, coerceContext.timeZone.name());
          bool duplicate = false;
          for (const api::Warning& warning : *warnings) {
            if (warning.code == "date_granule_skipped" && warning.message == message) {
              duplicate = true;
              break;
            }
          }
          if (!duplicate) {
            char* copy = pool.alloc(message.size());
            std::memcpy(copy, message.data(), message.size());
            warnings->push_back({"date_granule_skipped",
                                 std::string_view(copy, message.size())});
          }
        }
        return range;
      };
      // Window edges chosen so the existing +/-1 exclusive fold below lands
      // on the granule boundary: gt = the window's last milli (+1 = past it),
      // lt = the window's first milli (-1 = before it).
      if (hasGte) loEnc = dateBound(*gte).lo;
      if (hasGt)  loEnc = dateBound(*gt).hiExclusive - 1;
      if (hasLte) hiEnc = dateBound(*lte).hiExclusive - 1;
      if (hasLt)  hiEnc = dateBound(*lt).lo;
    } else {
      if (hasGte || hasGt) {
        loEnc = fieldType.coerceColInt64(
            hasGte ? *gte : *gt, field, coerceContext);
      }
      if (hasLte || hasLt) {
        hiEnc = fieldType.coerceColInt64(
            hasLte ? *lte : *lt, field, coerceContext);
      }
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
    return pool.make<NumericPredicateQuery>(field, lo, hi);
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
                           std::span<const int32_t> valuePositions = {}, int32_t slop = 0) {
    return createPhraseQuery(fields.resolve(field, OpClass::SEARCH, opName), values, valuePositions, slop);
  }

  Query* createPhraseQuery(const ResolvedFieldHandle& target, std::span<const std::string_view> values,
                           std::span<const int32_t> valuePositions = {}, int32_t slop = 0) {
    validatePositions(valuePositions, values.size(), "words");
    if (slop < 0) {
      throw std::runtime_error("Phrase query slop must be nonnegative");
    }
    positionalTextFieldType(target);
    auto* chain = fields.chain(target);
    TokenChain& tc = *chain;
    Token& tok = tc.head.getToken();
    TokenStream& tail = *tc.tail;

    // Collected here, then copied into pool-backed spans. Query parsing is a
    // per-request cold path, so a transient std::vector is fine.
    std::vector<std::string_view> terms;
    std::vector<int64_t> positions;

    auto emit = [&](int64_t position) {
      terms.push_back(poolCopy(fitTerm(target, tok.text)));
      positions.push_back(position);
    };

    if (valuePositions.empty()) {
      int64_t pos = -1;  // first token's positionIncrement (>= 1) lands it at >= 0
      for (std::string_view value : values) {
        tc.head.setValue(value);
        tc.reset();
        while (tail.incrementToken()) {
          pos += (int64_t) tok.positionIncrement;
          emit(pos);
        }
      }
    } else {
      int64_t carry = 0;  // extra positions consumed by prior values' expansion
      for (size_t i = 0; i < values.size(); i++) {
        int64_t base = (int64_t) valuePositions[i] + carry;
        int64_t relPos = -1;  // position within this value, relative to base
        tc.head.setValue(values[i]);
        tc.reset();
        while (tail.incrementToken()) {
          relPos += (int64_t) tok.positionIncrement;
          emit(base + relPos);
        }
        // A single-token value spans relPos 0 (no overflow); a value spanning
        // relPos slots pushes the rest by relPos. A dropped value (relPos < 0)
        // leaves following positions untouched.
        if (relPos > 0) {
          carry += relPos;
        }
      }
    }

    return buildPhrase(target,
                       std::span<std::string_view>(terms.data(), terms.size()),
                       std::span<const int64_t>(positions.data(), positions.size()), slop);
  }

  // Build a phrase query from already-analyzed terms; the terms are used
  // verbatim, with no analysis. `positions` may be empty (defaults to
  // 0,1,2,...) or must match the term count.
  //
  // The provided span contents are not copied and thus should outlive the returned query.
  Query* createPhraseFromTerms(std::string_view field, std::span<std::string_view> terms,
                               std::span<const int32_t> positions, int32_t slop = 0) {
    return createPhraseFromTerms(fields.resolve(field, OpClass::SEARCH, opName), terms, positions, slop);
  }

  Query* createPhraseFromTerms(const ResolvedFieldHandle& target, std::span<std::string_view> terms,
                               std::span<const int32_t> positions, int32_t slop = 0) {
    positionalTextFieldType(target);
    validatePositions(positions, terms.size(), "terms");
    if (slop < 0) {
      throw std::runtime_error("Phrase query slop must be nonnegative");
    }
    // Verbatim terms still honor the indexed-term length cap; rewrite entries
    // in place (the span is mutable by contract).
    for (auto& t : terms) t = fitTerm(target, t);

    std::span<const int64_t> canonicalPositions;
    if (positions.empty()) {
      auto pos = pool.make_span<int64_t>(terms.size());
      for (size_t i = 0; i < terms.size(); i++) {
        pos[i] = (int64_t) i;
      }
      canonicalPositions = pos;
    } else {
      auto pos = pool.make_span<int64_t>(positions.size());
      for (size_t i = 0; i < positions.size(); i++) {
        pos[i] = positions[i];
      }
      canonicalPositions = pos;
    }
    return buildPhrase(target, terms, canonicalPositions, slop);
  }
};

} // namespace luxir
