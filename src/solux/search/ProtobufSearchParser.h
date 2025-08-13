#pragma once

#include "SearchRequest.h"
#include "ops/RootOp.h"
#include "ops/SearchOp.h"
#include "ops/FacetOp.h"
#include "ops/StatsOp.h"
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
      case solux::proto::SearchOp::kFieldFacet: {
        auto& facetReq = searchOp.field_facet();
        auto* facet = FacetReq::createFieldFacetReq(req, name, facetReq, req.arena);
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

    std::span<std::pair<std::string_view, Query*>> filters;
    if (topDocsReq.filter().size() > 0) {
      filters = req.requestPool.make_span<std::pair<std::string_view, Query*>>(topDocsReq.filter().size());
      for (int i = 0; i < topDocsReq.filter().size(); i++) {
        auto& filter = topDocsReq.filter(i);
        auto* filterQuery = parser.parse(filter.query());
        filters[i] = {filter.name(), filterQuery};
      }
    }

    auto* qcontext = google::protobuf::Arena::Create<Query::Context>(&req.arena, req.requestPool, *req.reader);
    auto* qr = google::protobuf::Arena::Create<TopDocsReq>(&req.arena, req, name, topDocsReq, *qcontext, query, limit, filters);

    addSubs(*qr, topDocsReq.ops());

    if (firstQuery == nullptr) {
      firstQuery = qr;
    }
    return qr;
  }

};

}