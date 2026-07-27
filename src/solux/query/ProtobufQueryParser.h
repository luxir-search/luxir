#pragma once

#include <optional>
#include <span>
#include <string_view>

#include "solux/api/solux_types.hpp"

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

namespace solux {

class MemPool;
class Query;
class Schema;
class ValueProgram;
struct ParseContext;
struct ValueResult;

// The search REQUEST proto tree is a NON-OWNING concrete view (solux::api::*) over the
// kept-alive request bytes (see SearchRequest.h: ReqProto = solux::api::SearchRequest).
// Every request-side message the parsers read is a borrowed view: scalars are bare
// values, strings/bytes are views, repeated fields are spans, and message fields are
// (optional_)indirect views over the same bytes.

class ProtobufQueryParser {
  ParseContext& context;
  MemPool& pool;      // = context.pool (the parsed query tree's storage)
  Schema& schema;     // = context.schema

  Query* parseMatch(const solux::api::Match& matchQuery);

  // Copy a repeated string field into a pool-allocated MUTABLE span of
  // string_views (createPhraseFromTerms rewrites entries in place).  The source
  // views point at the request bytes, which outlive the query tree, so no byte
  // copies are made here.
  std::span<std::string_view> toSpan(std::span<const std::string_view> vals);
  // Same, for a repeated bytes field: reinterpret each binary term as a
  // string_view over the request bytes.
  std::span<std::string_view> binToSpan(std::span<const ::hpp_proto::bytes_view> vals);

  Query* parsePhrase(const solux::api::PhraseQuery& phraseQuery);
  Query* parsePrefix(const solux::api::PrefixQuery& prefixQuery);
  Query* parseExists(const solux::api::ExistsQuery& existsQuery);
  Query* parseRange(const solux::api::RangeQuery& rangeQuery);
  Query* parseGeoBox(const solux::api::GeoBoxQuery& geoBoxQuery);
  Query* parseGeoDistance(const solux::api::GeoDistanceQuery& geoDistanceQuery);
  Query* parseFuzzy(const solux::api::FuzzyQuery& fuzzyQuery);
  Query* parseKnn(const solux::api::KnnQuery& knnQuery);
  std::span<Query*> parseQueryList(std::span<const solux::api::Query> queries);
  Query* parseBoolean(const solux::api::BooleanQuery& booleanQuery);
  Query* parseSimpleQuery(const solux::api::SimpleQuery& sq, const solux::api::Query& node);
  Query* parseExpr(const solux::api::ExprQuery& exprQuery, const solux::api::Query& node);
  Query* parseConstantScore(const solux::api::ConstantScoreQuery& constantScoreQuery);
  Query* parseBoost(const solux::api::BoostQuery& boostQuery);
  Query* parseRescore(const solux::api::RescoreQuery& rescoreQuery);

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
  static std::string_view getString(const solux::api::Val& val);

  Query* parse(const solux::api::Query& pquery);
};

} // solux
