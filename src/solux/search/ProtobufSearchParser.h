#pragma once

#include "SearchRequest.h"
#include "ops/RootOp.h"
#include "ops/SearchOp.h"
#include "ops/FacetOp.h"
#include "ops/StrFacetOp.h"
#include "ops/StatsOp.h"
#include "ops/FusionOp.h"
#include "ops/TopDocsReq.h"
#include "solux/query/ProtobufQueryParser.h"

namespace solux {

class ProtobufSearchParser {
  SearchRequest& req;
  TopDocsReq* firstQuery = nullptr;





public:
  // The provided pool will be used to store the parsed query tree.
  // We need access to the schema to figure out what types of queries to produce?
  // Both the pool and any parsed protobuf objects must outlive the query tree.
  explicit ProtobufSearchParser(SearchRequest& req) : req(req) {
  }

  SearchOp* parse() {
    // Top level facets default to being nested under the first top-level query
    // for their input.
    // Their results go at the top-level however.

    RootOp& rootOp = *google::protobuf::Arena::Create<RootOp>(&req.arena, req);
    rootOp.parent = nullptr;
    addSubs(rootOp, req.proto.ops());

    // figure out if any facets are using the first query as their input.
    // If so, add those facets under the first query with a signal that they should add their
    // results at the top-level of the response.
    return &rootOp;
  }

  void addSubs(SearchOp& currOp, const google::protobuf::Map<std::string, solux::proto::SearchOp>& ops) {
    for (auto& [name, searchOp] : ops) {
      auto* sub = parseOp(name, searchOp);
      if (sub == nullptr) {
        continue; // skip this op
      }
      currOp.subOps[name] = sub;
      sub->parent = &currOp;
    }
  }

  SearchOp* parseOp(std::string_view name, const solux::proto::SearchOp& searchOp) {
    switch (searchOp.kind_case()) {
      case solux::proto::SearchOp::kTopDocs: {
        return parseTopDocs(name, searchOp.top_docs());
      }
      case solux::proto::SearchOp::kFusion: {
        return parseFusion(name, searchOp.fusion());
      }
      case solux::proto::SearchOp::kFieldFacet: {
        auto& facetReq = searchOp.field_facet();
        auto* facet = createFieldFacetReq(name, facetReq);
        addSubs(*facet, searchOp.field_facet().ops());
        return facet;
      } // end case
        break;
      case solux::proto::SearchOp::kRangeFacet: {
        auto& facetReq = searchOp.range_facet();
        auto facetField = facetReq.field();
        auto start = facetReq.start();
        auto end = facetReq.end();
        auto gap = facetReq.has_gap() ? facetReq.gap() : 1; // default gap is 1
        if (gap <= 0) {
          gap = 1; // ensure gap is positive
        }
        int64_t minCount = -1;
        if (facetReq.has_mincount()) {
          minCount = facetReq.mincount();
        }
        auto missing = facetReq.missing();
        FacetReq* facet = google::protobuf::Arena::Create<IntFacetRangeReq>(&req.arena, req, facetReq, facetField, name, start, end, gap, minCount, missing);
        addSubs(*facet, searchOp.range_facet().ops());
        return facet;
      }
        break;
        case solux::proto::SearchOp::kGenOp: {
          if (searchOp.gen_op().name() == "avg" || searchOp.gen_op().name() == "average") {
            auto& avgOp = searchOp.gen_op();
            auto* avg = google::protobuf::Arena::Create<AvgOp>(&req.arena, req, name, avgOp.args(0).s());
            return avg;
          } else {
            throw std::runtime_error("Unknown generic operation: " + std::string(searchOp.gen_op().name()));
          }
        }
      default:
        throw std::runtime_error("Unknown search operation");
    }
  }

  FacetReq* createFieldFacetReq(std::string_view facetName, const proto::FieldFacet& facetReq) {
    auto facetField = facetReq.field();
    int64_t limit = 5; // default limit
    if (facetReq.has_limit()) {
      limit = facetReq.limit();
    }
    int64_t minCount = -1;
    if (facetReq.has_mincount()) {
      minCount = facetReq.mincount();
    }
    auto missing = facetReq.missing();

    // Resolve everything that could throw before Arena::Create: the arena
    // registers the cleanup entry pre-construction, so a throwing ctor
    // leaves the cleanup list pointing at uninitialized memory which
    // crashes at arena reset.
    auto& ftype = req.schema->getFieldTypeEx(facetField);

    FacetReq* facet = nullptr;
    switch (ftype->type()) {
      case FieldType::Type::INT: {
        auto range = IntFacetReq::scanGlobalRange(*req.reader, facetField);
        facet = google::protobuf::Arena::Create<IntFacetReq>(&req.arena, req, facetReq, facetField, facetName, limit, minCount, missing, range.min, range.max, range.useVector);
        break;
      }
      case FieldType::Type::ID:
      case FieldType::Type::STRING: {
        auto ordMap = req.reader->getOrdMap(facetField);
        facet = google::protobuf::Arena::Create<StrFacetOp>(&req.arena, req, facetReq, facetField, facetName, limit, minCount, missing, std::move(ordMap));
        break;
      }
      case FieldType::Type::TEXT:
        facet = google::protobuf::Arena::Create<FullTextFacetReq>(&req.arena, req, facetReq, facetField, facetName, limit, minCount, missing);
        break;
      default: ;
    }
    if (facet == nullptr) {
      throw std::runtime_error("Unknown facet field type: " + std::string(facetField));
    }
    return facet;
  }

  // Build SortField list from a proto::SortSpec repeated field.  Schema
  // lookups may throw, so this must run before any Arena::Create that
  // consumes the result.
  struct ParsedSorts {
    std::vector<SortField> sortFields;
    bool useFieldSort = false;
  };
  ParsedSorts parseSorts(const google::protobuf::RepeatedPtrField<proto::SortSpec>& sorts) {
    ParsedSorts out;
    if (sorts.empty()) return out;
    out.useFieldSort = true;
    static ScoreFieldType scoreType;
    static DocFieldType docType;
    for (const auto& sortSpec : sorts) {
      SortField::SortOrder order = sortSpec.dir() == proto::SortSpec::DESC ?
        SortField::DESC : SortField::ASC;
      FieldComparator::MissingValue missing = FieldComparator::MISSING_LAST;
      if (sortSpec.field() == "_score_") {
        out.sortFields.emplace_back(sortSpec.field(), scoreType, order, missing);
      } else if (sortSpec.field() == "_docid_") {
        out.sortFields.emplace_back(sortSpec.field(), docType, order, missing);
      } else {
        auto fieldTypePtr = req.schema->getFieldTypeEx(sortSpec.field());
        if (!fieldTypePtr) {
          throw std::runtime_error(std::string("Field not found in schema: ") + std::string(sortSpec.field()));
        }
        out.sortFields.emplace_back(sortSpec.field(), *fieldTypePtr, order, missing);
      }
    }
    return out;
  }

  // Build Weights for a filter list.  Weight ctors may throw (e.g. KnnQuery
  // dim validation), so this must run before any Arena::Create that takes
  // the resulting span.
  std::span<Query::Weight*> buildFilterWeights(
      std::span<std::pair<std::string_view, Query*>> filters,
      Query::Context& qcontext) {
    if (filters.empty()) return {};
    auto weights = req.requestPool.make_span<Query::Weight*>(filters.size());
    for (size_t i = 0; i < filters.size(); i++) {
      weights[i] = filters[i].second->createWeight(qcontext);
    }
    return weights;
  }

  SearchOp* parseTopDocs(std::string_view name, const solux::proto::TopDocs& topDocsReq) {
    /*
    message TopDocs {
            Query query = 1;
            int64 offset = 2;
            optional sint64 limit = 3;
            bool get_number = 4;          // return the number of matching documents
            bool get_scores = 5;          // return the relevancy score for each document returned
            repeated string fields = 6;   // fields to return for each document
            repeated SortSpec sorts = 7;
    }
    */

    // Place the Query in the requestPool since it uses things like string_view that directly reference
    // the request.
    ProtobufQueryParser parser(req.requestPool, *req.schema);
    Query* query = parser.parse(topDocsReq.query());
    int64_t offset = topDocsReq.offset();
    unused(offset); // TODO
    int64_t specifiedLimit = topDocsReq.has_limit() ? topDocsReq.limit() : 10;
    // limit to actual number of docs in the index (or all if limit == -1)
    int64_t limit = specifiedLimit < 0 ? req.reader->maxDoc() : std::min(specifiedLimit, req.reader->maxDoc());

    auto filters = parseNamedFilters(parser, topDocsReq.filter());

    // All work that may throw (sort field schema lookup, Weight ctors)
    // must complete before Arena::Create<TopDocsReq>.
    auto parsedSorts = parseSorts(topDocsReq.sorts());
    auto* qcontext = Query::Context::create(&req.arena, req.requestPool, *req.reader);
    auto* weight = query->createWeight(*qcontext);
    auto filterWeights = buildFilterWeights(filters, *qcontext);

    auto* qr = google::protobuf::Arena::Create<TopDocsReq>(
      &req.arena, req, name, topDocsReq, *qcontext, query, weight, limit,
      std::move(parsedSorts.sortFields), parsedSorts.useFieldSort,
      filters, filterWeights);

    addSubs(*qr, topDocsReq.ops());

    if (firstQuery == nullptr) {
      firstQuery = qr;
    }
    return qr;
  }

  std::span<std::pair<std::string_view, Query*>> parseNamedFilters(
      ProtobufQueryParser& parser,
      const google::protobuf::RepeatedPtrField<solux::proto::NamedQuery>& filtersProto) {
    std::span<std::pair<std::string_view, Query*>> out;
    if (filtersProto.size() > 0) {
      out = req.requestPool.make_span<std::pair<std::string_view, Query*>>(filtersProto.size());
      for (int i = 0; i < filtersProto.size(); i++) {
        auto& f = filtersProto[i];
        out[i] = {f.name(), parser.parse(f.query())};
      }
    }
    return out;
  }

  SearchOp* parseFusion(std::string_view name, const solux::proto::Fusion& fusionProto) {
    if (fusionProto.ops_size() > 0) {
      throw std::runtime_error("Fusion sub-ops are not yet supported");
    }
    if (fusionProto.sources().empty()) {
      throw std::runtime_error("Fusion requires at least one source");
    }
    if (!fusionProto.has_rrf()) {
      throw std::runtime_error("Fusion requires a fusion method (only RRF is supported)");
    }
    if (fusionProto.rrf().k() < 0) {
      throw std::runtime_error("RrfFusion.k must be >= 0 (0 selects the default)");
    }
    int32_t rrfK = fusionProto.rrf().k() > 0 ? fusionProto.rrf().k() : 60;

    ProtobufQueryParser parser(req.requestPool, *req.schema);
    auto* qcontext = Query::Context::create(&req.arena, req.requestPool, *req.reader);

    // Fully populate each Source (sort fields, Weight, filter weights)
    // before Arena::Create<FusionOp>.
    std::vector<FusionOp::Source> sources;
    sources.reserve(fusionProto.sources().size());
    for (auto& [srcName, srcProto] : fusionProto.sources()) {
      FusionOp::Source src;
      src.name = srcName;
      src.sourceProto = &srcProto;
      src.query = parser.parse(srcProto.query());
      int64_t srcSpecifiedLimit = srcProto.has_limit() ? srcProto.limit() : 10;
      src.topCount = srcSpecifiedLimit < 0
        ? req.reader->maxDoc()
        : std::min(srcSpecifiedLimit, req.reader->maxDoc());
      src.filters = parseNamedFilters(parser, srcProto.filter());

      auto parsedSorts = parseSorts(srcProto.sorts());
      src.sortFields = std::move(parsedSorts.sortFields);
      src.useFieldSort = parsedSorts.useFieldSort;
      src.weight = src.query->createWeight(*qcontext);
      src.filterWeights = buildFilterWeights(src.filters, *qcontext);

      sources.emplace_back(std::move(src));
    }

    auto sharedFilters = parseNamedFilters(parser, fusionProto.filter());
    auto sharedFilterWeights = buildFilterWeights(sharedFilters, *qcontext);

    int64_t specifiedLimit = fusionProto.has_limit() ? fusionProto.limit() : 10;
    int64_t limit = specifiedLimit < 0 ? req.reader->maxDoc() : std::min(specifiedLimit, req.reader->maxDoc());

    auto* fusion = google::protobuf::Arena::Create<FusionOp>(
      &req.arena, req, name, fusionProto, *qcontext, std::move(sources), limit,
      sharedFilters, sharedFilterWeights, rrfK);

    return fusion;
  }

};

}