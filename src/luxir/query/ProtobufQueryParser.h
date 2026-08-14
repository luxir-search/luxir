#pragma once

#include <optional>
#include <span>
#include <string_view>

#include "luxir/api/luxir_types.hpp"

// Lowers a request-side api::Query tree into the engine's Query tree.
//
// DECLARATION ONLY: the implementation is in ProtobufQueryParser.cpp. The parser has to know
// every concrete query type to dispatch the Query oneof, but its callers only ever construct
// one and call parse(), so those ~13 query headers (plus Schema, the expr/simple-query parsers
// and the value-expression parser) do not belong in anyone else's translation unit. Keeping
// them here cost ~8s of frontend per including TU, which is why tests that only need parse()
// had to be consolidated into one file to contain it.
//
// The recursive descent (parse -> parseBoolean -> parseQueryList -> parse) lives entirely
// inside the .cpp, so an arbitrarily deep query tree is walked without crossing this boundary;
// callers cross it once per request.
//
// The types below are forward-declared on purpose: the members are references, so nothing here
// needs a complete type. Callers already have ParseContext (they built it) and Query (they use
// the result), and they include those headers themselves.

namespace luxir {

class MemPool;
class Query;
class Schema;
class ValueProgram;
struct ParseContext;
struct ValueResult;

// The search REQUEST proto tree is a NON-OWNING concrete view (luxir::api::*) over the
// kept-alive request bytes (see SearchRequest.h: ReqProto = luxir::api::SearchRequest).
// Every request-side message the parsers read is a borrowed view: scalars are bare
// values, strings/bytes are views, repeated fields are spans, and message fields are
// (optional_)indirect views over the same bytes.

class ProtobufQueryParser {
  ParseContext& context;
  MemPool& pool;      // = context.pool (the parsed query tree's storage)
  Schema& schema;     // = context.schema

  Query* parseMatch(const luxir::api::Match& matchQuery);

  // Copy a repeated string field into a pool-allocated MUTABLE span of
  // string_views (createPhraseFromTerms rewrites entries in place).  The source
  // views point at the request bytes, which outlive the query tree, so no byte
  // copies are made here.
  std::span<std::string_view> toSpan(std::span<const std::string_view> vals);
  // Same, for a repeated bytes field: reinterpret each binary term as a
  // string_view over the request bytes.
  std::span<std::string_view> binToSpan(std::span<const ::hpp_proto::bytes_view> vals);

  Query* parsePhrase(const luxir::api::PhraseQuery& phraseQuery);
  Query* parsePrefix(const luxir::api::PrefixQuery& prefixQuery);
  Query* parseWildcard(const luxir::api::WildcardQuery& wildcardQuery);
  Query* parseRegex(const luxir::api::RegexQuery& regexQuery);
  Query* parseExists(const luxir::api::ExistsQuery& existsQuery);
  Query* parseRange(const luxir::api::RangeQuery& rangeQuery);
  Query* parseGeoBox(const luxir::api::GeoBoxQuery& geoBoxQuery);
  Query* parseGeoDistance(const luxir::api::GeoDistanceQuery& geoDistanceQuery);
  Query* parseFuzzy(const luxir::api::FuzzyQuery& fuzzyQuery);
  Query* parseKnn(const luxir::api::KnnQuery& knnQuery);
  std::span<Query*> parseQueryList(std::span<const luxir::api::Query> queries);
  Query* parseBoolean(const luxir::api::BooleanQuery& booleanQuery);
  Query* parseSimpleQuery(const luxir::api::SimpleQuery& sq, const luxir::api::Query& node);
  Query* parseExpr(const luxir::api::ExprQuery& exprQuery, const luxir::api::Query& node);
  Query* parseConstantScore(const luxir::api::ConstantScoreQuery& constantScoreQuery);
  Query* parseBoost(const luxir::api::BoostQuery& boostQuery);
  Query* parseRescore(const luxir::api::RescoreQuery& rescoreQuery);

  static std::optional<float> constantOutput(const ValueProgram& program);
  static bool exactlyRepresented(const ValueResult& result, float value);

public:
  // The context's pool stores the parsed query tree; the schema decides what
  // types of queries to produce; warnings go to the context's sink.  The
  // context, its pool, and any parsed protobuf objects must outlive the tree.
  explicit ProtobufQueryParser(ParseContext& context);

  // Return a single string_view from a protobuf Val or empty string view if the
  // Val is not a string.  Only for reading search-op ARGUMENTS (static so the
  // search-op parser can use it without an instance); query VALUES go through
  // the coercion contract instead (QueryBuilder's Val overloads).
  static std::string_view getString(const luxir::api::Val& val);

  Query* parse(const luxir::api::Query& pquery);
};

} // luxir
