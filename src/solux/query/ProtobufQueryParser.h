#pragma once

#include <cmath>
#include <variant>

#include "PhraseQuery.h"
#include "QueryBuilder.h"
#include "solux/query/ExprParser.h"
#include "solux/query/ParseContext.h"
#include "solux/query/Query.h"
#include "solux/query/SimpleQueryParser.h"
#include "solux/query/TermQuery.h"
#include "solux/query/AllQuery.h"
#include "solux/query/BooleanQuery.h"
#include "solux/query/BoostQuery.h"
#include "solux/query/ConstantScoreQuery.h"
#include "solux/query/GeoBoxQuery.h"
#include "solux/query/GeoDistanceQuery.h"
#include "solux/query/KnnQuery.h"
#include "solux/schema/Schema.h"
#include "solux/api/solux_types.hpp"
#include "solux/util/Overloaded.h"

namespace solux {

// The search REQUEST proto tree is a NON-OWNING concrete view (solux::api::*) over the
// kept-alive request bytes (see SearchRequest.h: ReqProto = solux::api::SearchRequest).
// Every request-side message the parsers read is a borrowed view: scalars are bare
// values, strings/bytes are views, repeated fields are spans, and message fields are
// (optional_)indirect views over the same bytes.

class ProtobufQueryParser {
  ParseContext& context;
  MemPool& pool;      // = context.pool (the parsed query tree's storage)
  Schema& schema;     // = context.schema
public:
  // The context's pool stores the parsed query tree; the schema decides what
  // types of queries to produce; warnings go to the context's sink.  The
  // context, its pool, and any parsed protobuf objects must outlive the tree.
  explicit ProtobufQueryParser(ParseContext& context)
    : context(context), pool(context.pool), schema(context.schema) {
  }

  // Return a single string_view from a protobuf Val or empty string view if the
  // Val is not a string.  Only for reading search-op ARGUMENTS (static so the
  // search-op parser can use it without an instance); query VALUES go through
  // the coercion contract instead (QueryBuilder's Val overloads).
  static std::string_view getString(const solux::api::Val& val) {
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

  solux::Query* parseMatch(const solux::api::Match& matchQuery) {
    std::string_view field = matchQuery.field;

    if (matchQuery.min_match < 0) {
      throw std::runtime_error("Match 'min_match' must not be negative");
    }

    auto op = matchQuery.operator_ == solux::api::Match_::Operator::AND
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

  // Copy a repeated string field into a pool-allocated MUTABLE span of
  // string_views (createPhraseFromTerms rewrites entries in place).  The source
  // views point at the request bytes, which outlive the query tree, so no byte
  // copies are made here.
  std::span<std::string_view> toSpan(std::span<const std::string_view> vals) {
    auto out = pool.make_span<std::string_view>(vals.size());
    for (size_t i = 0; i < vals.size(); i++) {
      out[i] = vals[i];
    }
    return out;
  }

  // Same, for a repeated bytes field: reinterpret each binary term as a
  // string_view over the request bytes.
  std::span<std::string_view> binToSpan(std::span<const ::hpp_proto::bytes_view> vals) {
    auto out = pool.make_span<std::string_view>(vals.size());
    for (size_t i = 0; i < vals.size(); i++) {
      out[i] = std::string_view((const char*)vals[i].data(), vals[i].size());
    }
    return out;
  }

  solux::Query* parsePhrase(const solux::api::PhraseQuery& phraseQuery) {
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


  solux::Query* parsePrefix(const solux::api::PrefixQuery& prefixQuery) {
    QueryBuilder builder(
        pool, schema, context.coerceContext, context.opName, context.warnings);
    return builder.createPrefixQuery(prefixQuery.field, prefixQuery.prefix);
  }

  solux::Query* parseRange(const solux::api::RangeQuery& rangeQuery) {
    auto ptr = [](const ::hpp_proto::optional_indirect_view<solux::api::Val>& v)
        -> const solux::api::Val* { return v.has_value() ? &*v : nullptr; };
    QueryBuilder builder(
        pool, schema, context.coerceContext, context.opName, context.warnings);
    return builder.createRangeQuery(rangeQuery.field, ptr(rangeQuery.gte), ptr(rangeQuery.gt),
                                    ptr(rangeQuery.lte), ptr(rangeQuery.lt));
  }

  solux::Query* parseGeoBox(const solux::api::GeoBoxQuery& geoBoxQuery) {
    return pool.make<solux::GeoBoxQuery>(
        geoBoxQuery.field, geoBoxQuery.min_lat, geoBoxQuery.max_lat,
        geoBoxQuery.min_lon, geoBoxQuery.max_lon);
  }

  solux::Query* parseGeoDistance(
      const solux::api::GeoDistanceQuery& geoDistanceQuery) {
    return pool.make<solux::GeoDistanceQuery>(
        geoDistanceQuery.field, geoDistanceQuery.lat, geoDistanceQuery.lon,
        geoDistanceQuery.radius_meters);
  }

  solux::Query* parseFuzzy(const solux::api::FuzzyQuery& fuzzyQuery) {
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

  solux::Query* parseKnn(const solux::api::KnnQuery& knnQuery) {
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

    return pool.make<solux::KnnQuery>(
      field, vectorType, queryVec, k, knnQuery.nprobe, knnQuery.refine_candidates,
      minScanFraction, knnQuery.exact);
  }

  std::span<Query*> parseQueryList(std::span<const solux::api::Query> queries) {
    if (queries.empty()) return {};
    auto out = pool.make_span<Query*>(queries.size());
    for (size_t i = 0; i < queries.size(); i++) {
      out[i] = parse(queries[i]);
    }
    return out;
  }

  solux::Query* parseBoolean(const solux::api::BooleanQuery& booleanQuery) {
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
    return pool.make<solux::BooleanQuery>(required, optional, prohibited, filter, minMatch);
  }

  solux::Query* parseSimpleQuery(const solux::api::SimpleQuery& sq, const solux::api::Query& node) {
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

    SimpleQueryResult result = solux::parseSimpleQuery(sq.q, options, pool);
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
    const_cast<solux::api::Query&>(node).kind = result.root->kind;
    return parse(*result.root);
  }

  // Parse the expr string into an api::Query subtree, splice that expansion
  // over the expr arm in the request tree (both live in the request arena, so
  // lifetimes are identical - and any later serialization of the request
  // shows the canonical structured equivalent instead of the opaque string),
  // then lower it through the same path as every other node.
  solux::Query* parseExpr(const solux::api::ExprQuery& exprQuery, const solux::api::Query& node) {
    if (exprQuery.q.empty()) {
      throw std::runtime_error("expr requires a non-empty query string");
    }
    ExprOptions options;
    options.schema = &schema;
    options.vars = exprQuery.vars;
    options.nestingBudget = &context.nestingBudget;
    const solux::api::Query* root = solux::parseExpr(exprQuery.q, options, pool);
    const_cast<solux::api::Query&>(node).kind = root->kind;
    return parse(*root);
  }

  solux::Query* parseConstantScore(const solux::api::ConstantScoreQuery& constantScoreQuery) {
    if (!constantScoreQuery.query.has_value() ||
        constantScoreQuery.query->kind.index() == 0) {
      throw std::runtime_error("ConstantScoreQuery requires a child query");
    }
    float score = constantScoreQuery.score.has_value() ? *constantScoreQuery.score : 1.0f;
    return pool.make<solux::ConstantScoreQuery>(parse(*constantScoreQuery.query), score);
  }

  solux::Query* parseBoost(const solux::api::BoostQuery& boostQuery) {
    if (!boostQuery.query.has_value() || boostQuery.query->kind.index() == 0) {
      throw std::runtime_error("BoostQuery requires a child query");
    }
    float boost = boostQuery.boost.has_value() ? *boostQuery.boost : 1.0f;
    if (!std::isfinite(boost) || boost < 0.0f) {
      throw std::runtime_error(std::format(
        "BoostQuery boost must be finite and non-negative (got {})", boost));
    }
    return pool.make<solux::BoostQuery>(parse(*boostQuery.query), boost);
  }

  solux::Query* parse(const solux::api::Query& pquery) {
    // The one recursion choke point for structured trees: every nested node
    // passes through here, so the shared budget bounds tree depth no matter
    // which parser (wire, JSON, expr, simple_query) produced the tree.
    NestingScope nesting(context);
    // Exhaustive dispatch over the Query oneof: a new arm is a compile error until handled.
    return std::visit(solux::overloaded{
      [&](const solux::api::Match& m) -> solux::Query* { return parseMatch(m); },
      [&](const solux::api::PhraseQuery& p) -> solux::Query* { return parsePhrase(p); },
      [&](const solux::api::PrefixQuery& p) -> solux::Query* { return parsePrefix(p); },
      [&](const solux::api::RangeQuery& r) -> solux::Query* { return parseRange(r); },
      [&](const solux::api::GeoBoxQuery& g) -> solux::Query* { return parseGeoBox(g); },
      [&](const solux::api::GeoDistanceQuery& g) -> solux::Query* {
        return parseGeoDistance(g);
      },
      [&](const solux::api::FuzzyQuery& f) -> solux::Query* { return parseFuzzy(f); },
      [&](const solux::api::SimpleQuery& s) -> solux::Query* { return parseSimpleQuery(s, pquery); },
      [&](const solux::api::ExprQuery& e) -> solux::Query* { return parseExpr(e, pquery); },
      [&](bool) -> solux::Query* { return pool.make<solux::AllQuery>(); },  // the `all` arm
      [&](const solux::api::KnnQuery& k) -> solux::Query* { return parseKnn(k); },
      [&](const solux::api::BooleanQuery& b) -> solux::Query* { return parseBoolean(b); },
      [&](const solux::api::ConstantScoreQuery& c) -> solux::Query* { return parseConstantScore(c); },
      [&](const solux::api::BoostQuery& b) -> solux::Query* { return parseBoost(b); },
      [&](std::monostate) -> solux::Query* { throw std::runtime_error("query oneof not set"); },
      [&](std::string_view) -> solux::Query* {  // the bare `field` string arm is not a query
        throw std::runtime_error("field-only query arm is not a valid query");
      },
    }, pquery.kind);
  }

};

} // solux
