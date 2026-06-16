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
    FieldType& fieldType = *schema.getFieldTypeEx(field);
    // float boost = 1.0f;

    switch(fieldType.type()) {
      case FieldType::Type::TEXT: {
        // TODO: for a text field, we need to tokenize the string and handle multiple terms
        std::string_view val = getString(matchQuery.val());
        return pool.make<solux::TermQuery>(field, val);
      }
      case FieldType::Type::ID:
      case FieldType::Type::STRING: {
        std::string_view term = getString(matchQuery.val());
        return pool.make<solux::TermQuery>(field, term);
      }
      case FieldType::Type::INT:
      default:
        throw std::runtime_error("Unknown field type");
    }
    std::unreachable();
  }

  solux::Query* parsePhrase(const solux::proto::PhraseQuery& phraseQuery) {
    std::string_view field = phraseQuery.field();
    QueryBuilder builder(pool, schema);

    std::span<const int32_t> positions(phraseQuery.positions().begin(),
                                       (size_t)phraseQuery.positions().size());

    // Exactly one of text / words / terms / terms_bin is expected. text and
    // words are un-analyzed (run through the field's analyzer here, at query
    // time); terms and terms_bin are already analyzed and used verbatim. We
    // only translate proto into primitives -- the builder owns the analysis and
    // query-shape decisions so JSON / string parsers can share it.
    if (!phraseQuery.text().empty()) {
      if (!positions.empty()) {
        throw std::runtime_error(
          "Phrase query 'positions' cannot be combined with 'text' (positions come from analysis)");
      }
      std::string_view text = phraseQuery.text();
      return builder.createPhraseQuery(field, std::span<const std::string_view>(&text, 1));
    }
    if (!phraseQuery.words().empty()) {
      // positions (when given) are one per word; the builder shifts them to
      // absorb words that analyze to multiple tokens.
      auto words = pool.make_span<std::string_view>(phraseQuery.words().size());
      for (int i = 0; i < phraseQuery.words().size(); i++) {
        words[i] = phraseQuery.words(i);
      }
      return builder.createPhraseQuery(field, words, positions);
    }
    if (!phraseQuery.terms().empty()) {
      auto terms = pool.make_span<std::string_view>(phraseQuery.terms().size());
      for (int i = 0; i < phraseQuery.terms().size(); i++) {
        terms[i] = phraseQuery.terms(i);
      }
      return builder.createPhraseFromTerms(field, terms, positions);
    }
    if (!phraseQuery.terms_bin().empty()) {
      auto terms = pool.make_span<std::string_view>(phraseQuery.terms_bin().size());
      for (int i = 0; i < phraseQuery.terms_bin().size(); i++) {
        terms[i] = phraseQuery.terms_bin(i);
      }
      return builder.createPhraseFromTerms(field, terms, positions);
    }

    // Nothing to match on.
    return builder.matchNoDocs();
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
    if (booleanQuery.min_match() > 1) {
      throw std::runtime_error(std::format(
        "BooleanQuery min_match > 1 is not supported yet (got {})", booleanQuery.min_match()));
    }
    if (booleanQuery.min_match() == 1 &&
        (booleanQuery.optional().empty() || !booleanQuery.required().empty() || !booleanQuery.filter().empty())) {
      throw std::runtime_error(
        "BooleanQuery min_match=1 is only supported for optional-only boolean queries");
    }
    auto required = parseQueryList(booleanQuery.required());
    auto optional = parseQueryList(booleanQuery.optional());
    auto prohibited = parseQueryList(booleanQuery.prohibited());
    auto filter = parseQueryList(booleanQuery.filter());
    return pool.make<solux::BooleanQuery>(required, optional, prohibited, filter);
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
