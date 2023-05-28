#pragma once

#include "solux/query/Query.h"
#include "solux/query/TermQuery.h"
#include "solux/query/AllQuery.h"
#include "solux/schema/Schema.h"
#include "protos/solux.grpc.pb.h"

namespace solux {

class ProtobufQueryParser {
  MemPool& pool;
  Schema& schema;
public:
  // The provided pool will be used to store the parsed query tree.
  // We need access to the schema to figure out what types of queries to produce?
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


  solux::Query* parse(const solux::proto::Query& pquery) {
    switch(pquery.kind_case()) {
      case solux::proto::Query::kMatch: {
        return parseMatch(pquery.match());
      }
      case solux::proto::Query::kAll: {
        return pool.make<solux::AllQuery>();
      }
      default:
        throw std::runtime_error(std::format("Unknown query type for proto field {} ({})", (int)pquery.kind_case(), pquery.GetDescriptor()->FindFieldByNumber(pquery.kind_case())->name()));
    }
    std::unreachable();
  }

};

} // solux

