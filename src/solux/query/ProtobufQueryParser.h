#pragma once

#include "PhraseQuery.h"
#include "QueryBuilder.h"
#include "solux/query/Query.h"
#include "solux/query/TermQuery.h"
#include "solux/query/AllQuery.h"
#include "solux/query/BooleanQuery.h"
#include "solux/query/ConstantScoreQuery.h"
#include "solux/query/ForcePrepareQuery.h"
#include "solux/query/KnnQuery.h"
#include "solux/schema/Schema.h"
#include "protos/solux.grpc.pb.h"

namespace solux {

class ProtobufQueryParser {
  MemPool& pool;
  Schema& schema;
public:
  // The provided pool will be used to store the parsed query tree.
  // We need access to the schema to figure out what types of queries to produce.
  // Both the pool and any parsed protobuf objects must outlive the query tree.
  ProtobufQueryParser(MemPool& pool, Schema& schema) : pool(pool), schema(schema) {
  }

  // return a single string_view from a protobuf Val or empty string view if the Val is not a string
  // do we need to distinguish between an explicit empty string and a missing string?
  std::string_view getString(const solux::proto::Val& val) {
    switch(val.kind_case()) {
      case solux::proto::Val::kS:
        // protobuf returns a reference to a std::string, so it's OK to return a string_view of it as long
        // as we keep the protobuf around.
        return val.s();
      case solux::proto::Val::kBin:
        return val.bin();
      default:
        return {};
    }
  }

  solux::Query* parseMatch(const solux::proto::Match& matchQuery) {
    std::string_view field = matchQuery.field();

    if (matchQuery.min_match() < 0) {
      throw std::runtime_error("Match 'min_match' must not be negative");
    }

    auto op = matchQuery.operator_() == solux::proto::Match::AND
                ? QueryBuilder::Operator::AND
                : QueryBuilder::Operator::OR;

    QueryBuilder builder(pool, schema);
    return builder.createMatchQuery(field, getString(matchQuery.val()), op, matchQuery.min_match());
  }

  // Copy a protobuf repeated string/bytes field into a pool-allocated span of
  // string_views. The views point at the proto storage, which outlives the
  // query tree, so no byte copies are made here.
  std::span<std::string_view> toSpan(const google::protobuf::RepeatedPtrField<std::string>& vals) {
    auto out = pool.make_span<std::string_view>(vals.size());
    for (int i = 0; i < vals.size(); i++) {
      out[i] = vals.Get(i);
    }
    return out;
  }

  solux::Query* parsePhrase(const solux::proto::PhraseQuery& phraseQuery) {
    std::string_view field = phraseQuery.field();
    QueryBuilder builder(pool, schema);

    std::span<const int32_t> positions(phraseQuery.positions().begin(),
                                       (size_t)phraseQuery.positions().size());

    // Exactly one of text / words / terms / terms_bin selects the phrase input.
    // text and words are un-analyzed (run through the field's analyzer here, at
    // query time); terms and terms_bin are already analyzed and used verbatim.
    // We only translate proto into primitives -- the builder owns the analysis
    // and query-shape decisions so JSON / string parsers can share it.
    bool hasText = !phraseQuery.text().empty();
    bool hasWords = !phraseQuery.words().empty();
    bool hasTerms = !phraseQuery.terms().empty();
    bool hasTermsBin = !phraseQuery.terms_bin().empty();
    if (hasText + hasWords + hasTerms + hasTermsBin > 1) {
      throw std::runtime_error(
        "Phrase query must set exactly one of text / words / terms / terms_bin");
    }

    if (hasText) {
      if (!positions.empty()) {
        throw std::runtime_error(
          "Phrase query 'positions' cannot be combined with 'text' (text has no word boundaries to position)");
      }
      std::string_view text = phraseQuery.text();
      return builder.createPhraseQuery(field, std::span<const std::string_view>(&text, 1));
    }
    if (hasWords) {
      // positions (when given) are one per word; the builder shifts them to
      // absorb words that analyze to multiple tokens.
      return builder.createPhraseQuery(field, toSpan(phraseQuery.words()), positions);
    }
    if (hasTerms) {
      return builder.createPhraseFromTerms(field, toSpan(phraseQuery.terms()), positions);
    }
    if (hasTermsBin) {
      return builder.createPhraseFromTerms(field, toSpan(phraseQuery.terms_bin()), positions);
    }

    // No phrase terms at all.
    if (!positions.empty()) {
      throw std::runtime_error("Phrase query has 'positions' but no terms");
    }
    return builder.matchNoDocs();
  }


  solux::Query* parsePrefix(const solux::proto::PrefixQuery& prefixQuery) {
    QueryBuilder builder(pool, schema);
    return builder.createPrefixQuery(prefixQuery.field(), prefixQuery.prefix());
  }

  solux::Query* parseKnn(const solux::proto::KnnQuery& knnQuery) {
    std::string_view field = knnQuery.field();
    FieldType& fieldType = *schema.getFieldTypeEx(field);
    if (fieldType.type() != FieldType::Type::VECTOR) {
      throw std::runtime_error(std::format("KnnQuery on non-vector field: {}", field));
    }
    auto& vectorType = (const VectorFieldType&)fieldType;
    if (!vectorType.knnSearchable()) {
      throw std::runtime_error(std::format("KnnQuery on vector field without metric: {}", field));
    }
    if (!knnQuery.query().has_f32()) {
      throw std::runtime_error(std::format(
        "KnnQuery for field '{}' is missing query vector (only f32 supported in v1)", field));
    }
    const auto& f32 = knnQuery.query().f32().v();
    // RepeatedField<float> is contiguous; the proto storage outlives the
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

    int32_t k = knnQuery.k();
    if (k <= 0) {
      throw std::runtime_error(std::format("KnnQuery for field '{}' must have k > 0 (got {})", field, k));
    }
    float minScanFraction = knnQuery.min_scan_fraction();
    if (minScanFraction < 0.0f || minScanFraction > 1.0f) {
      throw std::runtime_error(std::format(
        "KnnQuery for field '{}' has min_scan_fraction {} outside [0,1]",
        field, minScanFraction));
    }

    return pool.make<solux::KnnQuery>(
      field, vectorType, queryVec, k, knnQuery.nprobe(), knnQuery.refine_candidates(),
      minScanFraction, knnQuery.exact());
  }

  std::span<Query*> parseQueryList(const google::protobuf::RepeatedPtrField<solux::proto::Query>& queries) {
    if (queries.empty()) return {};
    auto out = pool.make_span<Query*>(queries.size());
    for (int i = 0; i < queries.size(); i++) {
      out[i] = parse(queries[i]);
    }
    return out;
  }

  solux::Query* parseBoolean(const solux::proto::BooleanQuery& booleanQuery) {
    int minMatch = booleanQuery.min_match();
    if (minMatch < 0) {
      throw std::runtime_error(std::format("BooleanQuery min_match must not be negative (got {})", minMatch));
    }
    if (minMatch >= 1) {
      // The min-should-match scorer only constrains the optional group; its
      // interaction with required/filter clauses is not wired yet.
      if (booleanQuery.optional().empty() || !booleanQuery.required().empty() || !booleanQuery.filter().empty()) {
        throw std::runtime_error(
          "BooleanQuery min_match is only supported for optional-only boolean queries (no required/filter)");
      }
      // Asking for more matches than there are clauses just means "all of them".
      if (minMatch > booleanQuery.optional().size()) {
        minMatch = booleanQuery.optional().size();
      }
    }
    auto required = parseQueryList(booleanQuery.required());
    auto optional = parseQueryList(booleanQuery.optional());
    auto prohibited = parseQueryList(booleanQuery.prohibited());
    auto filter = parseQueryList(booleanQuery.filter());
    return pool.make<solux::BooleanQuery>(required, optional, prohibited, filter, minMatch);
  }

  solux::Query* parseForcePrepare(const solux::proto::ForcePrepareQuery& forcePrepareQuery) {
    if (!forcePrepareQuery.has_query() ||
        forcePrepareQuery.query().kind_case() == solux::proto::Query::KIND_NOT_SET) {
      throw std::runtime_error("ForcePrepareQuery requires a child query");
    }
    return pool.make<solux::ForcePrepareQuery>(parse(forcePrepareQuery.query()));
  }

  solux::Query* parseConstantScore(const solux::proto::ConstantScoreQuery& constantScoreQuery) {
    if (!constantScoreQuery.has_query() ||
        constantScoreQuery.query().kind_case() == solux::proto::Query::KIND_NOT_SET) {
      throw std::runtime_error("ConstantScoreQuery requires a child query");
    }
    float score = constantScoreQuery.has_score() ? constantScoreQuery.score() : 1.0f;
    return pool.make<solux::ConstantScoreQuery>(parse(constantScoreQuery.query()), score);
  }

  solux::Query* parse(const solux::proto::Query& pquery) {
    switch(pquery.kind_case()) {
      case solux::proto::Query::kMatch: {
        return parseMatch(pquery.match());
      }
      case solux::proto::Query::kPhrase: {
        return parsePhrase(pquery.phrase());
      }
      case solux::proto::Query::kPrefix: {
        return parsePrefix(pquery.prefix());
      }
      case solux::proto::Query::kAll: {
        return pool.make<solux::AllQuery>();
      }
      case solux::proto::Query::kKnn: {
        return parseKnn(pquery.knn());
      }
      case solux::proto::Query::kBoolean: {
        return parseBoolean(pquery.boolean());
      }
      case solux::proto::Query::kConstantScore: {
        return parseConstantScore(pquery.constant_score());
      }
      case solux::proto::Query::kForcePrepare: {
        return parseForcePrepare(pquery.force_prepare());
      }
      default:
        throw std::runtime_error(std::format("Unknown query type for proto field {} ({})", (int)pquery.kind_case(), pquery.GetDescriptor()->FindFieldByNumber(pquery.kind_case())->name()));
    }
    std::unreachable();
  }

};

} // solux
