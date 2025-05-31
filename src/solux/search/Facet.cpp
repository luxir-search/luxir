#include "Facet.h"

#include "solux/schema/Schema.h"

namespace solux {

FacetReq* FacetReq::createFieldFacetReq(Schema& schema, std::string_view facetName, const proto::FieldFacet& facetReq,
                                        google::protobuf::Arena& arena, IndexReader& reader) {
  auto facetField = facetReq.field();
  auto limit = 5; // default limit
  if (facetReq.has_limit()) {
    limit = facetReq.limit();
  }
  auto missing = facetReq.missing();
  //arena allocate FacetReq
  FacetReq* facet = nullptr;
  auto& ftype = schema.getFieldTypeEx(facetField);

  switch (ftype->type()) {
    case FieldType::Type::INT:
      facet = google::protobuf::Arena::Create<IntFacetReq>(&arena, reader, facetField, facetName, limit, missing);
      break;
    case FieldType::Type::STRING:
      facet = google::protobuf::Arena::Create<StrFacetReq>(&arena, reader, facetField, facetName, limit, missing);
      break;
    default: ;
  }
  if (facet == nullptr) {
    throw std::runtime_error("Unknown facet field type: " + std::string(facetField));
  }
  return facet;
}

}
