#pragma once

#include "solux/util/proto.h"
#include <variant>

#include "SearchRequest.h"
#include "ops/RootOp.h"
#include "ops/SearchOp.h"
#include "ops/FacetOp.h"
#include "ops/StrFacetOp.h"
#include "ops/StatsOp.h"
#include "ops/FusionOp.h"
#include "ops/TopDocsReq.h"
#include "solux/query/BooleanQuery.h"
#include "solux/query/ForcePrepareQuery.h"
#include "solux/query/ProtobufQueryParser.h"
#include "solux/util/Overloaded.h"

namespace solux {

class ProtobufSearchParser {
  SearchRequest& req;
  TopDocsReq* firstQuery = nullptr;

  // The request's named-op maps (SearchRequest.ops, TopDocs.ops, FieldFacet.ops,
  // RangeFacet.ops) all share this non-owning type: a span of (name, SearchOp
  // view) pairs over the request bytes (solux::api::map_view<sv, indirect_view<SearchOp>>).
  using OpsMap = decltype(ReqProto::ops);



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

    RootOp& rootOp = *solux::arenaCreate<RootOp>(req.arena, req);
    rootOp.parent = nullptr;
    addSubs(rootOp, req.proto.ops);

    // figure out if any facets are using the first query as their input.
    // If so, add those facets under the first query with a signal that they should add their
    // results at the top-level of the response.
    return &rootOp;
  }

  // Op and filter names appear in path-based addressing (debug/warning entries like
  // ops.q.top_docs.filter[0]), URL overlays, and cross-references (Domain
  // include/exclude), so they are restricted to path-safe characters.
  static void validateName(std::string_view name, const char* kind) {
    bool ok = !name.empty();
    for (char c : name) {
      if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
            c == '_' || c == '-')) {
        ok = false;
        break;
      }
    }
    if (!ok) {
      throw std::runtime_error(std::string(kind) + " name '" + std::string(name) +
                               "' is invalid: names are restricted to [A-Za-z0-9_-]+");
    }
  }

  void addSubs(SearchOp& currOp, OpsMap ops) {
    for (auto& [name, searchOp] : lastWins(ops)) {
      // map values are indirect views over the request bytes; deref to the SearchOp.
      // lastWins() collapses duplicate op names (protobuf map dedup semantics).
      validateName(name, "op");
      auto* sub = parseOp(name, **searchOp);
      if (sub == nullptr) {
        continue; // skip this op
      }
      currOp.subOps[name] = sub;
      sub->parent = &currOp;
    }
  }

  SearchOp* parseOp(std::string_view name, const solux::api::SearchOp& searchOp) {
    // Exhaustive dispatch over the SearchOp oneof: a new arm is a compile error until handled.
    return std::visit(solux::overloaded{
      [&](const solux::api::TopDocs& topDocs) -> SearchOp* {
        auto* qr = parseTopDocs(name, topDocs);
        addSubs(*qr, topDocs.ops);
        return qr;
      },
      [&](const solux::api::Fusion& fusion) -> SearchOp* { return parseFusion(name, fusion); },
      [&](const solux::api::FieldFacet& facetReq) -> SearchOp* {
        auto* facet = createFieldFacetReq(name, facetReq);
        addSubs(*facet, facetReq.ops);
        return facet;
      },
      [&](const solux::api::RangeFacet& facetReq) -> SearchOp* {
        std::string_view facetField = facetReq.field;
        int64_t start = facetReq.start.value_or(0);
        int64_t end = facetReq.end.value_or(0);
        int64_t gap = facetReq.gap; // bare: unset (0) -> 1 via the clamp below
        if (gap <= 0) {
          gap = 1; // ensure gap is positive
        }
        int64_t minCount = -1;
        if (facetReq.mincount.has_value()) {
          minCount = *facetReq.mincount;
        }
        bool missing = facetReq.missing;
        // IntFacetRangeReq does integer bucket arithmetic on raw column
        // values; FLOAT/DOUBLE columns hold sortable bits, which would
        // produce silently wrong buckets.  Refuse until range faceting
        // learns to decode them.
        auto& rangeFtype = req.schema->getFieldTypeEx(facetField);
        if (rangeFtype->type() == FieldType::FLOAT || rangeFtype->type() == FieldType::DOUBLE) {
          throw std::runtime_error("Range facet over float/double field not yet supported: " + std::string(facetField));
        }
        if (facetReq.mincount.has_value() && *facetReq.mincount < 1) {
          throw std::runtime_error("facet '" + std::string(name) + "': mincount < 1 (zero-count buckets) is not supported for range facets");
        }
        if (!facetReq.ops.empty() || !facetReq.sorts.empty()) {
          throw std::runtime_error("facet '" + std::string(name) + "': sub-ops/sorts are not yet supported for range facets");
        }
        FacetReq* facet = solux::arenaCreate<IntFacetRangeReq>(req.arena, req, facetReq, facetField, name, start, end, gap, minCount, missing);
        addSubs(*facet, facetReq.ops);
        return facet;
      },
      [&](const solux::api::GenOp& genOp) -> SearchOp* {
        StatsOp::Kind kind;
        if (genOp.name == "avg" || genOp.name == "average") {
          kind = StatsOp::AVG;
        } else if (genOp.name == "min") {
          kind = StatsOp::MIN;
        } else if (genOp.name == "max") {
          kind = StatsOp::MAX;
        } else {
          throw std::runtime_error("Unknown generic operation: " + std::string(genOp.name));
        }
        if (genOp.args.empty()) {
          throw std::runtime_error("Generic operation '" + std::string(genOp.name) + "' requires a field argument");
        }
        std::string_view statsField = ProtobufQueryParser::getString(genOp.args[0]);
        auto& statsFtype = req.schema->getFieldTypeEx(statsField);
        return solux::arenaCreate<StatsOp>(req.arena, req, name, statsField, statsFtype->type(), kind);
      },
      [&](std::monostate) -> SearchOp* { throw std::runtime_error("search op oneof not set"); },
    }, searchOp.kind);
  }

  FacetReq* createFieldFacetReq(std::string_view facetName, const solux::api::FieldFacet& facetReq) {
    std::string_view facetField = facetReq.field;
    int64_t limit = 5; // default limit
    if (facetReq.limit.has_value()) {
      limit = *facetReq.limit;
    }
    int64_t minCount = -1;
    if (facetReq.mincount.has_value()) {
      minCount = *facetReq.mincount;
    }
    bool missing = facetReq.missing;

    // Resolve everything that could throw here in the parser and pass the
    // results into the op ctors (see TopDocsReq's ctor comment).
    auto& ftype = req.schema->getFieldTypeEx(facetField);

    FacetReq* facet = nullptr;
    switch (ftype->type()) {
      // DATE is epoch millis in the int column; it terms-facets exactly like
      // INT (distinct millis values).  Date-histogram bucketing is a range
      // facet, not a terms facet.
      case FieldType::Type::DATE:
      case FieldType::Type::INT: {
        if (facetReq.mincount.has_value() && *facetReq.mincount < 1) {
          throw std::runtime_error("facet '" + std::string(facetName) + "': mincount < 1 (zero-count buckets) is not supported for int field facets");
        }
        if (!facetReq.ops.empty() || !facetReq.sorts.empty()) {
          throw std::runtime_error("facet '" + std::string(facetName) + "': sub-ops/sorts are not yet supported for int field facets");
        }
        auto range = IntFacetReq::scanGlobalRange(*req.reader, facetField);
        facet = solux::arenaCreate<IntFacetReq>(req.arena, req, facetReq, facetField, facetName, limit, minCount, missing, range.min, range.max, range.useVector);
        break;
      }
      case FieldType::Type::ID:
      case FieldType::Type::STRING: {
        auto ordMap = req.reader->getOrdMap(facetField);
        facet = solux::arenaCreate<StrFacetOp>(req.arena, req, facetReq, facetField, facetName, limit, minCount, missing, std::move(ordMap));
        break;
      }
      case FieldType::Type::TEXT:
        if (!facetReq.ops.empty() || !facetReq.sorts.empty()) {
          throw std::runtime_error("facet '" + std::string(facetName) + "': sub-ops/sorts are not yet supported for text field facets");
        }
        facet = solux::arenaCreate<FullTextFacetReq>(req.arena, req, facetReq, facetField, facetName, limit, minCount, missing);
        break;
      default: ;
    }
    if (facet == nullptr) {
      throw std::runtime_error("Unknown facet field type: " + std::string(facetField));
    }
    return facet;
  }

  // Build SortField list from a proto SortSpec repeated field.  Schema lookups
  // that may throw are done here in the parser; the result is passed to the
  // TopDocsReq ctor.
  struct ParsedSorts {
    std::vector<SortField> sortFields;
    bool useFieldSort = false;
  };
  ParsedSorts parseSorts(std::span<const solux::api::SortSpec> sorts) {
    ParsedSorts out;
    if (sorts.empty()) return out;
    out.useFieldSort = true;
    static ScoreFieldType scoreType;
    static DocFieldType docType;
    for (const auto& sortSpec : sorts) {
      SortField::SortOrder order = sortSpec.dir == solux::api::SortSpec_::SortDir::DESC ?
        SortField::DESC : SortField::ASC;
      FieldComparator::MissingValue missing = FieldComparator::MISSING_LAST;
      if (sortSpec.field == "_score_") {
        out.sortFields.emplace_back(sortSpec.field, scoreType, order, missing);
      } else if (sortSpec.field == "_docid_") {
        out.sortFields.emplace_back(sortSpec.field, docType, order, missing);
      } else {
        auto fieldTypePtr = req.schema->getFieldTypeEx(sortSpec.field);
        if (!fieldTypePtr) {
          throw std::runtime_error(std::string("Field not found in schema: ") + std::string(sortSpec.field));
        }
        out.sortFields.emplace_back(sortSpec.field, *fieldTypePtr, order, missing);
      }
    }
    return out;
  }

  // Build Weights for a filter list.  Weight ctors that may throw (e.g. KnnQuery
  // dim validation) are resolved here; the resulting span is passed to the op ctor.
  std::span<Query::Weight*> buildFilterWeights(
      std::span<std::pair<std::string_view, Query*>> filters,
      Query::Context& qcontext, int32_t requestFlags) {
    if (filters.empty()) return {};
    // Filters constrain matches but never contribute to score. Preserve any
    // future request flags, but clear NEED_SCORES.
    int32_t filterFlags = requestFlags & ~Query::NEED_SCORES;
    auto weights = req.requestPool.make_span<Query::Weight*>(filters.size());
    for (size_t i = 0; i < filters.size(); i++) {
      weights[i] = filters[i].second->createWeight(qcontext, filterFlags);
    }
    return weights;
  }

  // Build a TopDocsReq from a TopDocs proto.  Caller decides whether to
  // attach sub-ops (the kTopDocs path in parseOp does; parseFusion does
  // not, since per-source ops are ignored by spec).
  TopDocsReq* parseTopDocs(std::string_view name, const solux::api::TopDocs& topDocsReq) {
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
    ParseContext parseContext{
      req.requestPool, *req.schema,
      CoerceContext{req.dateMathNowEpochMillis, *req.timeZone}, name, &req.warnings};
    ProtobufQueryParser parser(parseContext);
    if (!topDocsReq.query.has_value()) {
      throw std::runtime_error("TopDocs requires a query");
    }
    Query* query = parser.parse(*topDocsReq.query);
    int64_t offset = topDocsReq.offset;
    unused(offset); // TODO
    int64_t specifiedLimit = topDocsReq.limit.has_value() ? *topDocsReq.limit : 10;
    // limit to actual number of docs in the index (or all if limit == -1)
    int64_t limit = specifiedLimit < 0 ? req.reader->maxDoc() : std::min(specifiedLimit, req.reader->maxDoc());

    auto filters = parseNamedFilters(parser, topDocsReq.filter);

    // By default TopDocs filters are ordinary Boolean filter clauses, so one
    // query tree owns both matching and preparation. Keep the named filter
    // metadata on TopDocsReq; the toggle preserves the former passive domain
    // path as the benchmark baseline.
    bool foldFilters = !filters.empty() && !TopDocsReq::disableTopDocsFilterFoldForTests;
    if (foldFilters) {
      auto mandatory = req.requestPool.make_span<Query*>(1);
      mandatory[0] = query;
      auto filterClauses = req.requestPool.make_span<Query*>(filters.size());
      for (size_t i = 0; i < filters.size(); i++) {
        filterClauses[i] = filters[i].second;
      }
      query = req.requestPool.make<BooleanQuery>(
        mandatory, std::span<Query*>{}, std::span<Query*>{}, filterClauses);
    }
    // The test-only wrapper must cover the complete effective query. This
    // also leaves the Boolean tree visible to normalization before wrapping.
    if (req.testForcePrepare) {
      query = req.requestPool.make<ForcePrepareQuery>(query);
    }

    // Sort field schema lookup and Weight ctors that may throw are resolved
    // here in the parser and passed to the TopDocsReq ctor.
    auto parsedSorts = parseSorts(topDocsReq.sorts);
    auto* qcontext = Query::Context::create(&req.arena, req.requestPool, *req.reader,
                                            {}, &req.warnings);
    // Flags for this request's main query. Filters inherit these after
    // buildFilterWeights clears NEED_SCORES.
    //
    // NEED_SCORES is purely computational (fuzzy match semantics no longer
    // hang off it - the expansion cap is a property of the query).  A
    // count-/domain-only request (limit 0, no get_scores) reads no score, so
    // it skips norms/impacts/BM25 and lets boolean prep pick non-scoring
    // iterators.  Ranked requests (limit > 0) need scores to order docs even
    // when get_scores doesn't return them; field sorts keep that conservative
    // behavior for now.
    int32_t requestFlags = (limit > 0 || topDocsReq.get_scores)
        ? Query::NEED_SCORES : 0;
    auto* weight = query->createWeight(*qcontext, requestFlags);
    auto filterWeights = foldFilters
      ? std::span<Query::Weight*>{}
      : buildFilterWeights(filters, *qcontext, requestFlags);

    auto* qr = solux::arenaCreate<TopDocsReq>(
      req.arena, req, name, topDocsReq, *qcontext, query, weight, limit,
      std::move(parsedSorts.sortFields), parsedSorts.useFieldSort,
      filters, filterWeights);

    if (firstQuery == nullptr) {
      firstQuery = qr;
    }
    return qr;
  }

  std::span<std::pair<std::string_view, Query*>> parseNamedFilters(
      ProtobufQueryParser& parser,
      std::span<const solux::api::NamedQuery> filtersProto) {
    std::span<std::pair<std::string_view, Query*>> out;
    if (!filtersProto.empty()) {
      out = req.requestPool.make_span<std::pair<std::string_view, Query*>>(filtersProto.size());
      for (size_t i = 0; i < filtersProto.size(); i++) {
        auto& f = filtersProto[i];
        validateName(f.name, "filter");
        if (!f.query.has_value()) {
          throw std::runtime_error("filter '" + std::string(f.name) + "' requires a query");
        }
        out[i] = {f.name, parser.parse(*f.query)};
      }
    }
    return out;
  }

  SearchOp* parseFusion(std::string_view name, const solux::api::Fusion& fusionProto) {
    if (!fusionProto.ops.empty()) {
      throw std::runtime_error("Fusion sub-ops are not yet supported");
    }
    if (fusionProto.sources.empty()) {
      throw std::runtime_error("Fusion requires at least one source");
    }
    if (!fusionProto.rrf.has_value()) {
      throw std::runtime_error("Fusion requires a fusion method (only RRF is supported)");
    }
    if (fusionProto.rrf->k < 0) {
      throw std::runtime_error("RrfFusion.k must be >= 0 (0 selects the default)");
    }
    int32_t rrfK = fusionProto.rrf->k > 0 ? fusionProto.rrf->k : 60;

    // Build each source as a full TopDocsReq, wired to deliver its merged
    // collector back to the FusionOp::Calc instead of self-emitting.  The
    // sink closure captures nothing at parse time; it resolves the parent
    // FusionOp::Calc at runtime via the source Calc's parent pointer.
    // Per-source `ops` are intentionally not attached (Fusion spec ignores
    // them).  Fusion.sources is a map whose values are TopDocs directly (not
    // indirect), so srcProto is the message itself.
    std::vector<TopDocsReq*> sources;
    sources.reserve(fusionProto.sources.size());
    for (auto& [srcName, srcProto] : lastWins(fusionProto.sources)) {  // dedup duplicate source names, last-wins
      size_t idx = sources.size();
      auto* src = parseTopDocs(srcName, *srcProto);
      src->rankingSink = [idx](TopDocsReq::Calc& calc, MergeableCollector* mc) {
        static_cast<FusionOp::Calc*>(calc.getParent())->acceptSourceRanking(idx, mc);
      };
      sources.push_back(src);
    }

    ParseContext parseContext{
      req.requestPool, *req.schema,
      CoerceContext{req.dateMathNowEpochMillis, *req.timeZone}, name, &req.warnings};
    ProtobufQueryParser parser(parseContext);
    auto sharedFilters = parseNamedFilters(parser, fusionProto.filter);
    // Reuse the first source's qcontext to build the shared filter
    // weights.  All sources share the same reader/pool, so any qcontext
    // works; reusing one avoids an otherwise-unneeded allocation.
    // Shared filters use the same request flag path as TopDocs filters.
    auto sharedFilterWeights = buildFilterWeights(sharedFilters, sources.front()->qcontext, Query::NEED_SCORES);

    int64_t specifiedLimit = fusionProto.limit.has_value() ? *fusionProto.limit : 10;
    int64_t limit = specifiedLimit < 0 ? req.reader->maxDoc() : std::min(specifiedLimit, req.reader->maxDoc());

    auto* fusion = solux::arenaCreate<FusionOp>(
      req.arena, req, name, fusionProto, std::move(sources), limit,
      sharedFilters, sharedFilterWeights, rrfK);

    // Sources are children of this FusionOp in the SearchOp tree but not
    // in `subOps` (different lifecycle - they deliver via rankingSink
    // rather than emit).  Set parent so any downstream code that walks up
    // the tree finds the right ancestor.
    for (auto* src : fusion->sources) {
      src->parent = fusion;
    }

    return fusion;
  }

};

}
