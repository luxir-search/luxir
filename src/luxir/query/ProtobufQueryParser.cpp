#include "luxir/query/ProtobufQueryParser.h"

#include <cmath>
#include <format>
#include <limits>
#include <stdexcept>
#include <variant>

// Reached through the `luxir/...` path (not dir-relative quotes) so they resolve via the
// -isystem src include, matching how every other TU sees them. A dir-relative quote makes
// them non-system headers and surfaces pre-existing -Wsign-compare noise the tree suppresses
// deliberately (include_directories(SYSTEM ./src) in the root CMakeLists).
#include "luxir/query/PhraseQuery.h"
#include "luxir/query/QueryBuilder.h"
#include "luxir/query/AllQuery.h"
#include "luxir/query/BooleanQuery.h"
#include "luxir/query/BoostQuery.h"
#include "luxir/query/ConstantScoreQuery.h"
#include "luxir/query/ExprParser.h"
#include "luxir/query/GeoBoxQuery.h"
#include "luxir/query/GeoDistanceQuery.h"
#include "luxir/query/KnnQuery.h"
#include "luxir/query/ParseContext.h"
#include "luxir/query/Query.h"
#include "luxir/query/RescoreQuery.h"
#include "luxir/query/SimpleQueryParser.h"
#include "luxir/query/TermQuery.h"
#include "luxir/schema/Schema.h"
#include "luxir/util/Overloaded.h"
#include "luxir/value/ValueExprParser.h"

namespace luxir {

ProtobufQueryParser::ProtobufQueryParser(ParseContext& context)
  : context(context), pool(context.pool), schema(context.schema) {
}

std::string_view ProtobufQueryParser::getString(const luxir::api::Val& val) {
  // The Val oneof holds the request's string view directly (s arm) or the raw
  // request bytes (bin arm); both view the kept-alive request buffer.
  if (auto* s = std::get_if<std::string_view>(&val.kind)) {
    return *s;
  }
  if (auto* bin = std::get_if<::hpp_proto::bytes_view>(&val.kind)) {
    return std::string_view((const char*)bin->data(), bin->size());
  }
  return {};
}

luxir::Query* ProtobufQueryParser::parseMatch(const luxir::api::Match& matchQuery) {
  std::string_view field = matchQuery.field;

  if (matchQuery.min_match < 0) {
    throw std::runtime_error("Match 'min_match' must not be negative");
  }

  auto op = matchQuery.operator_ == luxir::api::Match_::Operator::AND
              ? QueryBuilder::Operator::AND
              : QueryBuilder::Operator::OR;

  QueryBuilder builder(
      pool, schema, context.coerceContext, context.opName, context.warnings);
  if (matchQuery.val.has_value()) {
    // Val goes through the coercion contract: a numeric val against a
    // text/string field matches its canonical rendering (it used to
    // silently match nothing).
    return builder.createMatchQuery(field, *matchQuery.val, op, matchQuery.min_match);
  }
  return builder.createMatchQuery(field, std::string_view{}, op, matchQuery.min_match);
}

luxir::Query* ProtobufQueryParser::parseAnyOf(
    const luxir::api::AnyOfQuery& anyOfQuery) {
  if (!anyOfQuery.values.has_value()) {
    throw std::runtime_error("AnyOfQuery requires values");
  }
  QueryBuilder builder(
      pool, schema, context.coerceContext, context.opName, context.warnings);
  CanonicalValueSet values = builder.canonicalizeFieldValues(
      anyOfQuery.field,
      ValueSequence(*anyOfQuery.values, "AnyOfQuery values"));
  return builder.createAnyOfQuery(values);
}

std::span<std::string_view> ProtobufQueryParser::toSpan(std::span<const std::string_view> vals) {
  auto out = pool.make_span<std::string_view>(vals.size());
  for (size_t i = 0; i < vals.size(); i++) {
    out[i] = vals[i];
  }
  return out;
}

std::span<std::string_view> ProtobufQueryParser::binToSpan(
    std::span<const ::hpp_proto::bytes_view> vals) {
  auto out = pool.make_span<std::string_view>(vals.size());
  for (size_t i = 0; i < vals.size(); i++) {
    out[i] = std::string_view((const char*)vals[i].data(), vals[i].size());
  }
  return out;
}

luxir::Query* ProtobufQueryParser::parsePhrase(const luxir::api::PhraseQuery& phraseQuery) {
  std::string_view field = phraseQuery.field;
  QueryBuilder builder(
      pool, schema, context.coerceContext, context.opName, context.warnings);

  std::span<const int32_t> positions = phraseQuery.positions;
  if (phraseQuery.slop < 0) {
    throw std::runtime_error("Phrase query slop must be nonnegative");
  }
  if (phraseQuery.words.size() > QueryBuilder::MAX_PHRASE_SLOTS
      || phraseQuery.terms.size() > QueryBuilder::MAX_PHRASE_SLOTS
      || phraseQuery.terms_bin.size() > QueryBuilder::MAX_PHRASE_SLOTS
      || positions.size() > QueryBuilder::MAX_PHRASE_SLOTS) {
    throw std::runtime_error(std::format(
        "Phrase query exceeds the {} raw-slot limit", QueryBuilder::MAX_PHRASE_SLOTS));
  }

  // Exactly one of text / words / terms / terms_bin selects the phrase input.
  // text and words are un-analyzed (run through the field's analyzer here, at
  // query time); terms and terms_bin are already analyzed and used verbatim.
  // We only translate proto into primitives -- the builder owns the analysis
  // and query-shape decisions so JSON / string parsers can share it.
  bool hasText = !phraseQuery.text.empty();
  bool hasWords = !phraseQuery.words.empty();
  bool hasTerms = !phraseQuery.terms.empty();
  bool hasTermsBin = !phraseQuery.terms_bin.empty();
  if (hasText + hasWords + hasTerms + hasTermsBin > 1) {
    throw std::runtime_error(
      "Phrase query must set exactly one of text / words / terms / terms_bin");
  }

  if (hasText) {
    if (!positions.empty()) {
      throw std::runtime_error(
        "Phrase query 'positions' cannot be combined with 'text' (text has no word boundaries to position)");
    }
    std::string_view text = phraseQuery.text;
    return builder.createPhraseQuery(field, std::span<const std::string_view>(&text, 1), {},
                                     phraseQuery.slop);
  }
  if (hasWords) {
    // positions (when given) are one per word; the builder shifts them to
    // absorb words that analyze to multiple tokens.
    return builder.createPhraseQuery(field, phraseQuery.words, positions, phraseQuery.slop);
  }
  if (hasTerms) {
    return builder.createPhraseFromTerms(field, toSpan(phraseQuery.terms), positions,
                                         phraseQuery.slop);
  }
  if (hasTermsBin) {
    return builder.createPhraseFromTerms(field, binToSpan(phraseQuery.terms_bin), positions,
                                         phraseQuery.slop);
  }

  // No phrase terms at all.
  if (!positions.empty()) {
    throw std::runtime_error("Phrase query has 'positions' but no terms");
  }
  return builder.matchNoDocs();
}

luxir::Query* ProtobufQueryParser::parsePrefix(const luxir::api::PrefixQuery& prefixQuery) {
  QueryBuilder builder(
      pool, schema, context.coerceContext, context.opName, context.warnings);
  return builder.createPrefixQuery(prefixQuery.field, prefixQuery.prefix);
}

luxir::Query* ProtobufQueryParser::parseWildcard(const luxir::api::WildcardQuery& wildcardQuery) {
  QueryBuilder builder(
      pool, schema, context.coerceContext, context.opName, context.warnings);
  return builder.createWildcardQuery(wildcardQuery.field, wildcardQuery.pattern);
}

luxir::Query* ProtobufQueryParser::parseRegex(const luxir::api::RegexQuery& regexQuery) {
  QueryBuilder builder(
      pool, schema, context.coerceContext, context.opName, context.warnings);
  return builder.createRegexQuery(regexQuery.field, regexQuery.pattern);
}

luxir::Query* ProtobufQueryParser::parseExists(const luxir::api::ExistsQuery& existsQuery) {
  QueryBuilder builder(
      pool, schema, context.coerceContext, context.opName, context.warnings);
  return builder.createExistsQuery(existsQuery.field);
}

luxir::Query* ProtobufQueryParser::parseRange(const luxir::api::RangeQuery& rangeQuery) {
  auto ptr = [](const ::hpp_proto::optional_indirect_view<luxir::api::Val>& v)
      -> const luxir::api::Val* { return v.has_value() ? &*v : nullptr; };
  QueryBuilder builder(
      pool, schema, context.coerceContext, context.opName, context.warnings);
  return builder.createRangeQuery(rangeQuery.field, ptr(rangeQuery.gte), ptr(rangeQuery.gt),
                                  ptr(rangeQuery.lte), ptr(rangeQuery.lt));
}

luxir::Query* ProtobufQueryParser::parseGeoBox(const luxir::api::GeoBoxQuery& geoBoxQuery) {
  return pool.make<luxir::GeoBoxQuery>(
      geoBoxQuery.field, geoBoxQuery.min_lat, geoBoxQuery.max_lat,
      geoBoxQuery.min_lon, geoBoxQuery.max_lon);
}

luxir::Query* ProtobufQueryParser::parseGeoDistance(
    const luxir::api::GeoDistanceQuery& geoDistanceQuery) {
  return pool.make<luxir::GeoDistanceQuery>(
      geoDistanceQuery.field, geoDistanceQuery.lat, geoDistanceQuery.lon,
      geoDistanceQuery.radius_meters);
}

luxir::Query* ProtobufQueryParser::parseFuzzy(const luxir::api::FuzzyQuery& fuzzyQuery) {
  if (fuzzyQuery.max_expansions < 0) {
    throw std::runtime_error(
      std::format("Fuzzy query max_expansions must not be negative (got {})",
                  fuzzyQuery.max_expansions));
  }
  QueryBuilder builder(
      pool, schema, context.coerceContext, context.opName, context.warnings);
  std::optional<int> maxEdits = fuzzyQuery.max_edits.has_value()
      ? std::optional<int>(*fuzzyQuery.max_edits) : std::nullopt;
  std::optional<int> prefixLength = fuzzyQuery.prefix_length.has_value()
      ? std::optional<int>(*fuzzyQuery.prefix_length) : std::nullopt;
  return builder.createFuzzyQuery(fuzzyQuery.field, fuzzyQuery.term,
                                  maxEdits, prefixLength, fuzzyQuery.max_expansions);
}

luxir::Query* ProtobufQueryParser::parseKnn(const luxir::api::KnnQuery& knnQuery) {
  std::string_view field = knnQuery.field;
  FieldType& fieldType = *schema.getFieldTypeEx(field);
  if (fieldType.type() != FieldType::Type::VECTOR) {
    throw std::runtime_error(std::format("KnnQuery on non-vector field: {}", field));
  }
  auto& vectorType = (const VectorFieldType&)fieldType;
  if (!vectorType.knnSearchable()) {
    throw std::runtime_error(std::format("KnnQuery on vector field without metric: {}", field));
  }
  if (!knnQuery.query.has_value() || !knnQuery.query->f32.has_value()) {
    throw std::runtime_error(std::format(
      "KnnQuery for field '{}' is missing query vector (only f32 supported in v1)", field));
  }
  const auto& f32 = knnQuery.query->f32->v;
  // The repeated float field is contiguous; the request storage outlives the
  // pool-allocated query tree (request arena), so pointing into it is safe.
  std::span<const float> queryVec(f32.data(), (size_t)f32.size());
  if (queryVec.empty()) {
    throw std::runtime_error(std::format(
      "KnnQuery for field '{}' must have a non-empty query vector", field));
  }
  if (vectorType.dims() > 0 && (int32_t)queryVec.size() != vectorType.dims()) {
    throw std::runtime_error(std::format(
      "KnnQuery: query vector dims {} do not match schema dims {} for field '{}'",
      queryVec.size(), vectorType.dims(), field));
  }

  int32_t k = knnQuery.k;
  if (k <= 0) {
    throw std::runtime_error(std::format("KnnQuery for field '{}' must have k > 0 (got {})", field, k));
  }
  float minScanFraction = knnQuery.min_scan_fraction;
  if (minScanFraction < 0.0f || minScanFraction > 1.0f) {
    throw std::runtime_error(std::format(
      "KnnQuery for field '{}' has min_scan_fraction {} outside [0,1]",
      field, minScanFraction));
  }

  return pool.make<luxir::KnnQuery>(
    field, vectorType, queryVec, k, knnQuery.nprobe, knnQuery.refine_candidates,
    minScanFraction, knnQuery.exact);
}

std::span<Query*> ProtobufQueryParser::parseQueryList(
    std::span<const luxir::api::Query> queries) {
  if (queries.empty()) return {};
  auto out = pool.make_span<Query*>(queries.size());
  for (size_t i = 0; i < queries.size(); i++) {
    out[i] = parse(queries[i]);
  }
  return out;
}

luxir::Query* ProtobufQueryParser::parseBoolean(const luxir::api::BooleanQuery& booleanQuery) {
  int minMatch = booleanQuery.min_match;
  if (minMatch < 0) {
    throw std::runtime_error(std::format("BooleanQuery min_match must not be negative (got {})", minMatch));
  }
  if (minMatch >= 1) {
    if (booleanQuery.optional.empty()) {
      throw std::runtime_error("BooleanQuery min_match needs optional clauses to apply to");
    }
    // Asking for more matches than there are clauses just means "all of them".
    if (minMatch > (int)booleanQuery.optional.size()) {
      minMatch = (int)booleanQuery.optional.size();
    }
  }
  auto required = parseQueryList(booleanQuery.required);
  auto optional = parseQueryList(booleanQuery.optional);
  auto prohibited = parseQueryList(booleanQuery.prohibited);
  auto filter = parseQueryList(booleanQuery.filter);
  return pool.make<luxir::BooleanQuery>(required, optional, prohibited, filter, minMatch);
}

luxir::Query* ProtobufQueryParser::parseSimpleQuery(
    const luxir::api::SimpleQuery& sq, const luxir::api::Query& node) {
  // Envelope validation errors freely: the never-fails contract covers the
  // STRING q, not the request shape (request-shape errors are
  // author-controlled and deterministic).
  if (sq.fields.empty()) {
    throw std::runtime_error(
      "simple_query requires a non-empty 'fields' list (the fields bare words search)");
  }
  if (sq.min_match < 0) {
    throw std::runtime_error("simple_query 'min_match' must not be negative");
  }
  for (std::string_view f : sq.fields) {
    FieldType* fieldType = schema.getFieldTypePtr(f);
    if (fieldType == nullptr || !SimpleQueryParser::termQueryable(*fieldType)) {
      throw std::runtime_error(std::format(
        "simple_query 'fields' entry '{}' is not a queryable text/string/id field", f));
    }
  }

  // The parser is schema-aware (arm selection by FieldType; analysis still
  // happens at build) and applies min_match itself, where user clauses are
  // distinguishable from per-field expansion.  allowed_fields only narrows;
  // entries the schema cannot query are dead.
  SimpleQueryOptions options(context.coerceContext);
  options.fields = sq.fields;
  options.schema = &schema;
  options.allowed_fields = sq.allowed_fields;
  options.operator_ = sq.operator_;
  options.min_match = sq.min_match;

  SimpleQueryResult result = luxir::parseSimpleQuery(sq.q, options, pool);
  for (const auto& w : result.warnings) {
    context.warn(w.code, w.message);
  }

  if (result.root == nullptr) {
    // Nothing parsed (e.g. all-whitespace q).  There is no structured
    // match-nothing arm to splice, so the string stays; the warnings
    // channel declares the degradation.
    QueryBuilder builder(
        pool, schema, context.coerceContext, context.opName, context.warnings);
    return builder.matchNoDocs();
  }
  // Splice the expansion over the simple_query arm, same as expr: both live
  // in the request storage, so any later serialization of the request shows
  // the structured equivalent instead of the opaque string.  sq lives
  // inside node.kind and dies here; everything it fed (options, result) was
  // copied or points at request/pool bytes that outlive the splice.
  const_cast<luxir::api::Query&>(node).kind = result.root->kind;
  return parse(*result.root);
}

// Parse the expr string into an api::Query subtree, splice that expansion
// over the expr arm in the request tree (both live in the request arena, so
// lifetimes are identical - and any later serialization of the request
// shows the canonical structured equivalent instead of the opaque string),
// then lower it through the same path as every other node.
luxir::Query* ProtobufQueryParser::parseExpr(
    const luxir::api::ExprQuery& exprQuery, const luxir::api::Query& node) {
  if (exprQuery.q.empty()) {
    throw std::runtime_error("expr requires a non-empty query string");
  }
  ExprOptions options;
  options.schema = &schema;
  options.vars = exprQuery.vars;
  options.nestingBudget = &context.nestingBudget;
  const luxir::api::Query* root = luxir::parseExpr(exprQuery.q, options, pool);
  const_cast<luxir::api::Query&>(node).kind = root->kind;
  return parse(*root);
}

luxir::Query* ProtobufQueryParser::parseConstantScore(
    const luxir::api::ConstantScoreQuery& constantScoreQuery) {
  if (!constantScoreQuery.query.has_value() ||
      constantScoreQuery.query->kind.index() == 0) {
    throw std::runtime_error("ConstantScoreQuery requires a child query");
  }
  float score = constantScoreQuery.score.has_value() ? *constantScoreQuery.score : 1.0f;
  return pool.make<luxir::ConstantScoreQuery>(parse(*constantScoreQuery.query), score);
}

luxir::Query* ProtobufQueryParser::parseBoost(const luxir::api::BoostQuery& boostQuery) {
  if (!boostQuery.query.has_value() || boostQuery.query->kind.index() == 0) {
    throw std::runtime_error("BoostQuery requires a child query");
  }
  float boost = boostQuery.boost.has_value() ? *boostQuery.boost : 1.0f;
  if (!std::isfinite(boost) || boost < 0.0f) {
    throw std::runtime_error(std::format(
      "BoostQuery boost must be finite and non-negative (got {})", boost));
  }
  return pool.make<luxir::BoostQuery>(parse(*boostQuery.query), boost);
}

std::optional<float> ProtobufQueryParser::constantOutput(const ValueProgram& program) {
  if (!program.constantScalar.has_value()) {
    return std::nullopt;
  }
  const ValueResult& result = *program.constantScalar;
  constexpr double MAX_FLOAT = (double)std::numeric_limits<float>::max();
  if (result.type == ValueType::DOUBLE) {
    double value = result.doubleValue;
    if (!std::isfinite(value) || value < -MAX_FLOAT || value > MAX_FLOAT) {
      throw std::runtime_error(
          "RescoreQuery constant is outside the finite float score range");
    }
    return (float)value;
  }
  return (float)result.intValue;
}

bool ProtobufQueryParser::exactlyRepresented(const ValueResult& result, float value) {
  return result.type == ValueType::DOUBLE
      ? (double)value == result.doubleValue
      : (long double)value == (long double)result.intValue;
}

luxir::Query* ProtobufQueryParser::parseRescore(const luxir::api::RescoreQuery& rescoreQuery) {
  if (!rescoreQuery.query.has_value()
      || rescoreQuery.query->kind.index() == 0) {
    throw std::runtime_error("RescoreQuery requires a child query");
  }
  if (rescoreQuery.expr.empty()) {
    throw std::runtime_error("RescoreQuery requires a non-empty expression");
  }
  ValueExprOptions options;
  options.schema = &schema;
  options.vars = rescoreQuery.vars;
  options.nestingBudget = &context.nestingBudget;
  ValueProgram* program =
      ValueExprParser(options, context.arena).parse(rescoreQuery.expr);
  ValueType rootType = program->root().type;
  if (valueArray(rootType) || rootType == ValueType::COLUMN_ONLY) {
    throw std::runtime_error(
        "RescoreQuery expression must produce a scalar numeric value");
  }

  luxir::Query* child = parse(*rescoreQuery.query);
  if (program->root().kind == ValueNodeKind::SCORE) {
    return child;
  }
  std::optional<float> constant = constantOutput(*program);
  if (constant.has_value()
      && exactlyRepresented(*program->constantScalar, *constant)) {
    return pool.make<luxir::ConstantScoreQuery>(child, *constant);
  }
  return pool.make<luxir::RescoreQuery>(child, program, constant);
}

luxir::Query* ProtobufQueryParser::parse(const luxir::api::Query& pquery) {
  // The one recursion choke point for structured trees: every nested node
  // passes through here, so the shared budget bounds tree depth no matter
  // which parser (wire, JSON, expr, simple_query) produced the tree.
  NestingScope nesting(context);
  // Exhaustive dispatch over the Query oneof: a new arm is a compile error until handled.
  return std::visit(luxir::overloaded{
    [&](const luxir::api::Match& m) -> luxir::Query* { return parseMatch(m); },
    [&](const luxir::api::AnyOfQuery& a) -> luxir::Query* { return parseAnyOf(a); },
    [&](const luxir::api::PhraseQuery& p) -> luxir::Query* { return parsePhrase(p); },
    [&](const luxir::api::PrefixQuery& p) -> luxir::Query* { return parsePrefix(p); },
    [&](const luxir::api::WildcardQuery& w) -> luxir::Query* { return parseWildcard(w); },
    [&](const luxir::api::RegexQuery& r) -> luxir::Query* { return parseRegex(r); },
    [&](const luxir::api::ExistsQuery& e) -> luxir::Query* { return parseExists(e); },
    [&](const luxir::api::RangeQuery& r) -> luxir::Query* { return parseRange(r); },
    [&](const luxir::api::GeoBoxQuery& g) -> luxir::Query* { return parseGeoBox(g); },
    [&](const luxir::api::GeoDistanceQuery& g) -> luxir::Query* {
      return parseGeoDistance(g);
    },
    [&](const luxir::api::FuzzyQuery& f) -> luxir::Query* { return parseFuzzy(f); },
    [&](const luxir::api::SimpleQuery& s) -> luxir::Query* { return parseSimpleQuery(s, pquery); },
    [&](const luxir::api::ExprQuery& e) -> luxir::Query* { return parseExpr(e, pquery); },
    [&](bool) -> luxir::Query* { return pool.make<luxir::AllQuery>(); },  // the `all` arm
    [&](const luxir::api::KnnQuery& k) -> luxir::Query* { return parseKnn(k); },
    [&](const luxir::api::BooleanQuery& b) -> luxir::Query* { return parseBoolean(b); },
    [&](const luxir::api::ConstantScoreQuery& c) -> luxir::Query* { return parseConstantScore(c); },
    [&](const luxir::api::BoostQuery& b) -> luxir::Query* { return parseBoost(b); },
    [&](const luxir::api::RescoreQuery& r) -> luxir::Query* {
      return parseRescore(r);
    },
    // Unset oneof selects all documents, same as the explicit `all` arm.
    [&](std::monostate) -> luxir::Query* { return pool.make<luxir::AllQuery>(); },
  }, pquery.kind);
}

} // luxir
