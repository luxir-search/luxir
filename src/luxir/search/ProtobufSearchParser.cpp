// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include "luxir/search/ProtobufSearchParser.h"

#include "luxir/util/proto.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <unordered_map>
#include <unordered_set>
#include <variant>
#include <vector>

#include "luxir/api/RequestFlags.h"
#include "luxir/search/SearchOpRegistry.h"
#include "luxir/search/SearchRequest.h"
#include "luxir/search/SearchEngine.h"
#include "luxir/search/SearchOverrides.h"
#include "luxir/query/ParseContext.h"
#include "luxir/search/ops/RootOp.h"
#include "luxir/search/ops/SearchOp.h"
#include "luxir/search/ops/FacetOp.h"
#include "luxir/search/ops/StrFacetOp.h"
#include "luxir/search/ops/ExprStatsOp.h"
#include "luxir/search/ops/FusionOp.h"
#include "luxir/search/ops/TopDocsReq.h"
#include "luxir/query/AllQuery.h"
#include "luxir/query/ToParentQuery.h"
#include "luxir/query/BoostQuery.h"
#include "luxir/query/ConstantScoreQuery.h"
#include "luxir/query/FittedTerms.h"
#include "luxir/search/RangeFacetGrid.h"
#include "luxir/query/BooleanQuery.h"
#include "luxir/query/ForcePrepareQuery.h"
#include "luxir/query/MinScoreQuery.h"
#include "luxir/query/ProtobufQueryParser.h"

#include "luxir/query/QueryBuilder.h"
#include "luxir/query/QueryShape.h"
#include "luxir/query/KnnQuery.h"
#include "luxir/schema/ValCoerce.h"
#include "luxir/util/NumericUtils.h"
#include "luxir/util/Overloaded.h"
#include "luxir/value/ValueExprParser.h"
#include "luxir/value/AggregateExprParser.h"

namespace luxir {

// The whole parser body. It is file-local: it has to name every request type (all of ops/,
// query/ and value/), and nothing outside this TU needs any of it - callers construct a
// ProtobufSearchParser and call parse(). Keeping this in the header cost every including TU
// ~12s of frontend and ~2GB of peak RSS.
namespace {

struct FacetParsePlan {
  FacetReq* execution = nullptr;
  std::span<Query*> bucketQueries;
  std::span<const size_t> selectedBuckets;
  Query::PlanningContext* planning = nullptr;
  // Whether the selection's derived filter was planned. Selected bucket
  // queries are normally validated through that filter; when it is elided
  // (no consumer), the facet-side sweep must validate them instead.
  bool refinerPlanned = true;
};

// Prefix for every error about the fusion named `fusionName`.
std::string fusionPrefix(std::string_view fusionName) {
  return "fusion '" + std::string(fusionName) + "': ";
}

// Resolve Fusion.weights (named at the API boundary) into one weight per
// source, aligned with `sourceNames` (the source execution order), so no
// fusion method depends on map ordering. Shared by every fusion method.
// Unlisted sources get weight 1. Rejects names that are not sources,
// negative, non-finite, or out-of-float-range weights, and a configuration
// with no weight > 0.
// A zero entry means the caller should disable that source.
std::vector<double> resolveSourceWeights(
    std::string_view fusionName,
    const luxir::api::map_view<std::string_view, double>& weights,
    std::span<const std::string_view> sourceNames) {
  std::vector<double> resolved(sourceNames.size(), 1.0);
  for (const auto& [key, weight] : lastWins(weights)) {
    auto it = std::ranges::find(sourceNames, key);
    if (it == sourceNames.end()) {
      throw std::runtime_error(fusionPrefix(fusionName)
          + "weights names unknown source '" + std::string(key) + "'");
    }
    // Scores are float, so a weight must also be finite as a float.
    if (!std::isfinite(*weight) || *weight < 0
        || *weight > (double)std::numeric_limits<float>::max()) {
      throw std::runtime_error(fusionPrefix(fusionName) + "weight for source '"
          + std::string(key) + "' must be finite, >= 0, and within float range");
    }
    resolved[(size_t)(it - sourceNames.begin())] = *weight;
  }
  if (std::ranges::none_of(resolved, [](double w) { return w > 0; })) {
    throw std::runtime_error(fusionPrefix(fusionName)
        + "at least one source weight must be > 0");
  }
  return resolved;
}

// Also used before ops are constructed: domains and selections belong to the
// child's options even though its parent pointer does not exist yet.
struct OpParseContext {
  uint64_t flags = 0;

  OpParseContext child(const api::SearchOp& op, std::string_view name) const {
    try {
      return {RequestFlags::resolve(flags, op.flags, op.flags_set, RequestFlags::Level::OP)};
    } catch (const RequestError& error) {
      throw RequestError("op '" + std::string(name) + "': " + error.what());
    }
  }
};

struct SearchParserImpl {
  SearchRequest& req;
  ParseView parseView;
  OpParseContext options;
  TopDocsReq* firstQuery = nullptr;
  NamedClauses namedClauses;
  size_t nonDefaultDomainVariants = 0;
  std::unordered_map<const api::SearchOp*, FacetParsePlan> preparedFacets;
  const CollapsePlan* enclosingCollapse = nullptr;
  const CollapsePlan* inheritedFacetMeasure = nullptr;
  std::span<ParsedFilter>* owningList = nullptr;
  std::string_view owningListChild;
  std::vector<std::shared_ptr<const FacetGroupPlan>> facetGroupPlans;
  // Root-level ops are depth 1. Ops nested past the configured cap are rejected
  // in addSubs, the single funnel for op recursion.
  const int maxOpDepth;
  static constexpr size_t MAX_RANGE_BUCKETS = 100000;

  enum class TopDocsPlacement : uint8_t {
    ROOT_OP,
    NESTED_OP,
    FUSION_SOURCE,
    ROW_OP,
  };

  enum class OpsPlacement : uint8_t {
    REQUEST_ROOT,
    TOP_DOCS,
    FACET_BUCKET,
    GROUP_BUCKET,
    FUSION,
    PLUGIN,
  };

  struct ParsedFences {
    std::span<const int64_t> values;
    int64_t affineGap = 0;
    bool affine = false;
    std::span<const RangeFacetRun> runs;
  };

  struct ParsedDomain {
    Query* query = nullptr;
    bool applyParentFilters = false;
    std::vector<Query*> filters;
  };

  using ParsedDomains =
      std::unordered_map<std::string_view, ParsedDomain>;

  // The request's named-op maps (SearchRequest.ops, TopDocs.ops, FieldFacet.ops,
  // RangeFacet.ops) all share this non-owning type: a span of (name, SearchOp
  // view) pairs over the request bytes (luxir::api::map_view<sv, indirect_view<SearchOp>>).
  using OpsMap = decltype(ReqProto::ops);

  void warnOnce(std::string_view code, const std::string& message) {
    req.warnOnce(code, message);
  }

  static std::optional<CalendarUnit> calendarUnit(
      api::CalendarGap::Unit unit) {
    using Unit = api::CalendarGap::Unit;
    switch (unit) {
      case Unit::DAY: return CalendarUnit::DAY;
      case Unit::WEEK: return CalendarUnit::WEEK;
      case Unit::MONTH: return CalendarUnit::MONTH;
      case Unit::QUARTER: return CalendarUnit::QUARTER;
      case Unit::YEAR: return CalendarUnit::YEAR;
      case Unit::UNKNOWN: return std::nullopt;
    }
    return std::nullopt;
  }

  ParsedFences makeGridFences(std::string_view facetName,
                              const RangeFacetGrid& grid,
                              int64_t lo, int64_t hi, bool clip) {
    try {
      auto built = grid.fences(lo, hi, clip, MAX_RANGE_BUCKETS);
      for (int64_t civil : built.skippedCivil) {
        warnOnce("calendar_bucket_skipped",
            "facet '" + std::string(facetName) + "': calendar bucket "
            + formatEpochMillisIso8601(civil).substr(0, 10)
            + " skipped: no such local day in " + grid.timeZone().name());
      }
      auto fences = req.requestPool.make_span<int64_t>(built.values.size());
      std::copy(built.values.begin(), built.values.end(), fences.begin());
      std::span<RangeFacetRun> runs;
      if (!built.affineGap) {
        auto compressed = grid.runs(built.values);
        if (!compressed.empty()) {
          runs = req.requestPool.make_span<RangeFacetRun>(compressed.size());
          std::copy(compressed.begin(), compressed.end(), runs.begin());
        }
      }
      return {fences, built.affineGap, built.affineGap != 0, runs};
    } catch (const std::runtime_error& error) {
      throw std::runtime_error("facet '" + std::string(facetName) + "': " + error.what());
    }
  }

public:
  // The provided pool will be used to store the parsed query tree.
  // We need access to the schema to figure out what types of queries to produce?
  // Both the pool and any parsed protobuf objects must outlive the query tree.

  const std::shared_ptr<IndexReader>& view() const { return parseView.reader(); }

  explicit SearchParserImpl(SearchRequest& req)
    : req(req), parseView(*req.schema, req.resolvedFields, &req.reader, &req), maxOpDepth(req.searchConfig.max_op_depth) {
  }

  // Every planning context of the request draws on one multi-term budget.
  Query::PlanningContext* createPlanningContext() {
    return req.createPlanningContext(*view());
  }

  // All request parse paths share the operator rules and field resolver.
  ParseContext createParseContext(std::string_view name) {
    ParseContext parseContext{req.requestPool, *req.schema, req.arena,
        coerceContext(*req.timeZone), name, &req.warnings, &parseView, &req.engine.getNode()};
    parseContext.minLiteralPrefix = req.searchConfig.multiterm_min_prefix_length;
    parseContext.collapse = enclosingCollapse;
    parseContext.names = &namedClauses;
    return parseContext;
  }

  CoerceContext coerceContext(const TimeZone& zone) const {
    return {req.dateMathNowEpochMillis, zone, RequestFlags::dateInputUnit(options.flags),
            (options.flags & RequestFlags::DECIMAL_SCALED) != 0};
  }

  RootOp* parse() {
    req.flags = RequestFlags::resolve(0, req.proto.flags, req.proto.flags_set);
    options.flags = req.flags;
    RootOp& rootOp = *luxir::arenaCreate<RootOp>(req.arena, req);
    rootOp.parent = nullptr;
    rootOp.flags = options.flags;

    auto parseContext = createParseContext("root");
    ParsedDomains domains = parseChildDomains(req.proto.ops, parseContext);
    if (!domains.empty()) {
      rootOp.planning = createPlanningContext();
      validateDomainQueries(domains, *rootOp.planning);
      rootOp.domainVariants.begin(0);
      addDomainVariants(
          req.proto.ops, {}, domains, rootOp.planning,
          rootOp.domainVariants);
      if (!rootOp.domainVariants.empty()) {
        rootOp.weightContext = Query::Context::create(
            &req.arena, *rootOp.planning);
        rootOp.domainVariants.buildWeights(
            *rootOp.weightContext, *rootOp.planning, 0);
      }
    }
    addSubs(rootOp, req.proto.ops, 0, OpsPlacement::REQUEST_ROOT);
    return &rootOp;
  }

  // Op names appear in path-based addressing and URL overlays, so they are
  // restricted to path-safe characters.
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

  void groupExprStats(SearchOp& parent, OpsPlacement placement,
      const boost::unordered_flat_map<std::string_view, SearchOp*>& block) {
    if (disableAggregateSharingForTests || block.size() < 2) return;
    struct Candidate {
      ExprStatsOp* op;
      const DomainVariantPlan* plan;
      size_t variant;
    };
    const DomainVariantPlan* parentPlan = nullptr;
    if (placement == OpsPlacement::REQUEST_ROOT) {
      parentPlan = &static_cast<RootOp&>(parent).domainVariants;
    } else if (placement == OpsPlacement::TOP_DOCS) {
      parentPlan = &static_cast<TopDocsReq&>(parent).domainVariants;
    }
    std::vector<Candidate> candidates;
    for (auto& [name, child] : block) {
      auto* metric = dynamic_cast<ExprStatsOp*>(child);
      if (metric == nullptr || !metric->hasOnlyIndirectProducers()) continue;
      auto* plan = parentPlan ? parentPlan
          : child->bucketDomain ? &child->bucketDomain->variants : nullptr;
      size_t variant = plan ? plan->variantForChild(name) : 0;
      candidates.push_back({metric, variant == 0 ? nullptr : plan, variant});
    }
    std::ranges::sort(candidates, {}, [](const Candidate& candidate) {
      return candidate.op->name;
    });
    std::vector<Candidate> leaders;
    for (const Candidate& candidate : candidates) {
      bool shared = false;
      for (const Candidate& leader : leaders) {
        if (candidate.plan == leader.plan && candidate.variant == leader.variant
            && candidate.op->shareProducersFrom(*leader.op)) {
          shared = true;
          break;
        }
      }
      if (!shared) leaders.push_back(candidate);
    }
  }

  void addSubs(SearchOp& currOp, OpsMap ops, int depth,
               OpsPlacement placement,
               boost::unordered_flat_map<std::string_view, SearchOp*>* destination = nullptr,
               std::span<ParsedFilter>* listFilters = nullptr) {
    if (ops.empty()) return;
    if (depth >= maxOpDepth) {
      throw std::runtime_error("search operation nesting exceeds the maximum depth of "
                               + std::to_string(maxOpDepth));
    }
    bool entersList = placement == OpsPlacement::TOP_DOCS || placement == OpsPlacement::GROUP_BUCKET;
    auto* currentList = entersList
        ? (listFilters ? listFilters : &static_cast<TopDocsReq&>(currOp).filters) : nullptr;
    BucketDomainPlan* bucketPlan = nullptr;
    if (placement == OpsPlacement::FACET_BUCKET || placement == OpsPlacement::GROUP_BUCKET) {
      auto context = createParseContext(currOp.name);
      auto domains = parseChildDomains(ops, context);
      if (!domains.empty()) {
        std::vector<Query*> filters;
        auto* owner = placement == OpsPlacement::GROUP_BUCKET ? currentList : owningList;
        if (owner) for (const auto& filter : *owner) {
          if (placement == OpsPlacement::GROUP_BUCKET
              || std::ranges::find(filter.exceptOps, owningListChild) == filter.exceptOps.end()) {
            filters.push_back(filter.query);
          }
        }
        for (auto& [key, domain] : domains) {
          if (domain.applyParentFilters) {
            domain.filters.insert(domain.filters.end(), filters.begin(), filters.end());
          }
          domain.applyParentFilters = false;
        }
        bucketPlan = luxir::arenaCreate<BucketDomainPlan>(req.arena);
        bucketPlan->planning = createPlanningContext();
        validateDomainQueries(domains, *bucketPlan->planning);
        bucketPlan->variants.begin(0);
        addDomainVariants(ops, {}, domains, bucketPlan->planning, bucketPlan->variants);
        auto* weights = Query::Context::create(&req.arena, *bucketPlan->planning);
        bucketPlan->variants.buildWeights(*weights, *bucketPlan->planning, 0);
      }
    }
    for (auto& [name, searchOp] : lastWins(ops)) {
      // map values are indirect views over the request bytes; deref to the SearchOp.
      // lastWins() collapses duplicate op names (protobuf map dedup semantics).
      validateName(name, "op");
      if ((placement == OpsPlacement::FACET_BUCKET || placement == OpsPlacement::GROUP_BUCKET)
          && (name == "val" || name == "count" || name == "rank"
              || name == "singleton" || name == "missing")) {
        throw RequestError("reserved bucket/group op name: " + std::string(name));
      }
      SearchOp* sub;
      auto* nextList = entersList ? currentList : owningList;
      if (nextList && nextList->empty()) nextList = nullptr;
      auto nextChild = !entersList ? owningListChild
          : nextList && placement == OpsPlacement::TOP_DOCS ? name : std::string_view{};
      if (nextList != owningList || (nextList && nextChild != owningListChild)) {
        auto* oldList = owningList;
        auto oldChild = owningListChild;
        auto restore = scope_guard([&]() noexcept {
          owningList = oldList;
          owningListChild = oldChild;
        });
        owningList = nextList;
        owningListChild = nextChild;
        sub = parseOp(name, **searchOp, depth + 1, placement);
      } else {
        sub = parseOp(name, **searchOp, depth + 1, placement);
      }
      if (sub == nullptr) {
        continue; // skip this op
      }
      (destination ? *destination : currOp.subOps)[name] = sub;
      sub->parent = &currOp;
      if (bucketPlan && (**searchOp).domain) sub->bucketDomain = bucketPlan;
      // Inheriting filters use the incoming domain; only a reset (here or
      // below) needs recoverable identities and their prefix memo.
      if (((**searchOp).domain && (**searchOp).domain->query) || sub->needsStructuralScope) {
        sub->needsStructuralScope = true;
        currOp.needsStructuralScope = true;
      }
    }
    groupExprStats(currOp, placement, destination ? *destination : currOp.subOps);
  }

  std::string domainPath(std::string_view to) {
    if (to == "/") return {};
    if (to.empty()) throw RequestError("domain.to requires a path, .., or /");
    if (to == ".." || to.starts_with("../")) {
      size_t levels = 0;
      for (;;) {
        auto slash = to.find('/');
        if (to.substr(0, slash) != "..") {
          throw RequestError("domain.to requires .. at every upward step; sibling paths such as '../name' are reserved");
        }
        ++levels;
        if (slash == std::string_view::npos) break;
        to.remove_prefix(slash + 1);
      }
      std::string path(view()->path());
      while (levels--) {
        if (path.empty()) throw RequestError("domain.to cannot move above the root view");
        auto dot = path.rfind('.');
        path.resize(dot == std::string::npos ? 0 : dot);
      }
      return path;
    }
    if (to.find('/') != std::string_view::npos) {
      throw RequestError("domain.to uses dots to descend; '/' only separates .. steps or names the root view");
    }
    return parseView.fields.resolvePath(to).physicalName;
  }

  bool movesView(const api::Domain& domain) {
    return domain.to && domainPath(*domain.to) != view()->path();
  }

  SearchOp* parseOp(std::string_view name, const luxir::api::SearchOp& searchOp,
                    int depth, OpsPlacement placement) {
    auto inherited = options;
    options = inherited.child(searchOp, name);
    auto restoreOptions = scope_guard([&]() noexcept { options = inherited; });
    auto* previousCollapse = enclosingCollapse;
    auto* previousMeasure = inheritedFacetMeasure;
    auto* previousList = owningList;
    auto restoreScope = scope_guard([&]() noexcept {
      enclosingCollapse = previousCollapse;
      inheritedFacetMeasure = previousMeasure;
      owningList = previousList;
    });
    if (searchOp.domain && searchOp.domain->from_match) {
      throw RequestError("domain.from_match '" + std::string(*searchOp.domain->from_match) + "' is only supported in row_ops");
    }
    InputDomainPlan* input = nullptr;
    std::optional<ParseView::Scope> viewScope;
    if (searchOp.domain && movesView(*searchOp.domain)) {
      const auto& domain = *searchOp.domain;
      const auto& destination = req.pinView(domainPath(*domain.to));
      bool descending = destination->path().size() > view()->path().size();
      input = arenaCreate<InputDomainPlan>(req.arena);
      input->mapping = {&req.ownershipRelation(descending ? *destination : *view(),
          descending ? *view() : *destination),
          descending ? RowRelation::Direction::EXPAND : RowRelation::Direction::PROJECT};

      enclosingCollapse = nullptr;
      inheritedFacetMeasure = nullptr;
      owningList = nullptr;
      viewScope.emplace(parseView, destination->path());
      auto context = createParseContext(name);
      auto parsed = parseDomain(domain, name, context);
      input->applyParentFilters = parsed.applyParentFilters;
      parsed.applyParentFilters = false;
      if (parsed.query || !parsed.filters.empty()) {
        input->planning = createPlanningContext();
        ParsedDomains domains;
        domains.emplace(name, std::move(parsed));
        validateDomainQueries(domains, *input->planning);
        input->variants.begin(0);
        addDomainRoute(name, &domains.at(name), [](size_t) { return false; },
            input->planning, input->variants);
        auto* weights = Query::Context::create(&req.arena, *input->planning);
        input->variants.buildWeights(*weights, *input->planning, 0);
      }
    }
    if (searchOp.domain && placement == OpsPlacement::FUSION) {
      throw RequestError("domain is not supported under Fusion.ops");
    }
    if (searchOp.domain && placement == OpsPlacement::PLUGIN) {
      throw RequestError("domain is not supported under a plugin operation's ops");
    }
    // Exhaustive dispatch over the SearchOp oneof: a new arm is a compile error until handled.
    SearchOp* op = std::visit(luxir::overloaded{
      [&](const luxir::api::TopDocs& topDocs) -> SearchOp* {
        auto* oldCollapse = enclosingCollapse;
        auto* oldMeasure = inheritedFacetMeasure;
        auto restore = scope_guard([&]() noexcept {
          enclosingCollapse = oldCollapse;
          inheritedFacetMeasure = oldMeasure;
        });
        // An explicit domain supplies external eligibility, as for nested lists.
        auto topDocsPlacement = depth == 1 && !searchOp.domain
            ? TopDocsPlacement::ROOT_OP : TopDocsPlacement::NESTED_OP;
        auto* qr = parseTopDocs(name, topDocs, topDocsPlacement);
        parseRowOps(*qr, topDocs, depth);
        if (topDocs.collapse) {
          addSubs(*qr, topDocs.ops, depth, OpsPlacement::TOP_DOCS);
          // Keep the nearest collapse for explicit group units, while a
          // group's facets default to document counts. Child queries still
          // establish their own scores; omitted queries select unscored members.
          inheritedFacetMeasure = nullptr;
          addSubs(*qr, topDocs.collapse->ops, depth, OpsPlacement::GROUP_BUCKET,
                  &qr->groupOps);
        } else {
          addSubs(*qr, topDocs.ops, depth, OpsPlacement::TOP_DOCS);
        }
        return qr;
      },
      [&](const luxir::api::Fusion& fusion) -> SearchOp* {
        return parseFusion(name, fusion, depth);
      },
      [&](const luxir::api::FieldFacet& facetReq) -> SearchOp* {
        FacetReq* facet;
        auto prepared = preparedFacets.find(&searchOp);
        if (prepared != preparedFacets.end()) {
          facet = prepared->second.execution;
          assert(facet != nullptr && prepared->second.bucketQueries.empty());
          preparedFacets.erase(prepared);
        } else {
          facet = createFieldFacetReq(name, facetReq, placement);
        }
        auto* oldMeasure = inheritedFacetMeasure;
        inheritedFacetMeasure = ((FieldFacetReq*)facet)->countDistinct;
        auto restore = scope_guard([&]() noexcept { inheritedFacetMeasure = oldMeasure; });
        addSubs(*facet, facetReq.ops, depth, OpsPlacement::FACET_BUCKET);
        return facet;
      },
      [&](const luxir::api::RangeFacet& facetReq) -> SearchOp* {
        FacetReq* facet;
        auto prepared = preparedFacets.find(&searchOp);
        if (prepared != preparedFacets.end()) {
          facet = prepared->second.execution;
          assert(facet != nullptr && prepared->second.bucketQueries.empty());
          preparedFacets.erase(prepared);
        } else {
          facet = createRangeFacetReq(name, facetReq, placement);
        }
        auto* oldMeasure = inheritedFacetMeasure;
        inheritedFacetMeasure = facet->countDistinct;
        auto restore = scope_guard([&]() noexcept { inheritedFacetMeasure = oldMeasure; });
        addSubs(*facet, facetReq.ops, depth, OpsPlacement::FACET_BUCKET);
        return facet;
      },
      [&](const luxir::api::QueryFacet& facetReq) -> SearchOp* {
        FacetParsePlan plan;
        auto prepared = preparedFacets.find(&searchOp);
        if (prepared != preparedFacets.end()) {
          assert(prepared->second.execution == nullptr
                 && !prepared->second.bucketQueries.empty());
          plan = prepared->second;
          preparedFacets.erase(prepared);
        } else {
          auto parseContext = createParseContext(name);
          ProtobufQueryParser parser(parseContext);
          plan = createQueryFacetPlan(
              name, facetReq, placement, parser);
          validateQueryFacetQueries(plan);
        }
        FacetReq* facet = createQueryFacetReq(
            name, facetReq, plan,
            placement == OpsPlacement::FACET_BUCKET || placement == OpsPlacement::GROUP_BUCKET);
        auto* oldMeasure = inheritedFacetMeasure;
        inheritedFacetMeasure = facet->countDistinct;
        auto restore = scope_guard([&]() noexcept { inheritedFacetMeasure = oldMeasure; });
        addSubs(*facet, facetReq.ops, depth, OpsPlacement::FACET_BUCKET);
        return facet;
      },
      [&](const luxir::api::ExprOp& exprOp) -> SearchOp* {
        AggregateExprOptions options{req.schema.get(), exprOp.vars, name};
        options.fieldResolver = &parseView.fields;
        options.view = &parseView;
        options.shareProducers = !disableAggregateSharingForTests;
        AggregateProgram* program =
            AggregateExprParser(options, req.arena).parse(exprOp.expr);
        auto fields = resolveAggregateFields(*program, req.requestPool, *view());
        return luxir::arenaCreate<ExprStatsOp>(req.arena, req, view(), name, *program, fields);
      },
      [&](const luxir::api::PluginOp& pluginOp) -> SearchOp* {
        return parsePluginOp(name, pluginOp, depth);
      },
      [&](std::monostate) -> SearchOp* { throw std::runtime_error("search op oneof not set"); },
    }, searchOp.kind);
    if (op != nullptr) { op->flags = options.flags; op->inputDomain = input; }
    return op;
  }

  template<typename Facet>
  void validateSelectionEnvelope(std::string_view facetName,
                                 const Facet& facetReq,
                                 OpsPlacement placement) {
    if (facetReq.selection_mode != api::SelectionMode::ANY
        && facetReq.selection_mode != api::SelectionMode::ALL) {
      throw std::runtime_error("facet '" + std::string(facetName)
          + "': selection_mode must be ANY or ALL");
    }
    std::string valueName = "facet '" + std::string(facetName)
        + "': selected";
    ValueSequence selected = facetReq.selected.has_value()
        ? ValueSequence(*facetReq.selected, valueName,
                        ValueSequence::NullError::MUST_NOT_BE_NULL)
        : ValueSequence();
    if (selected.empty()) {
      if (facetReq.selection_mode != api::SelectionMode::ANY) {
        throw std::runtime_error("facet '" + std::string(facetName)
            + "': selection_mode requires nonempty selected");
      }
      return;
    }
    if (placement != OpsPlacement::TOP_DOCS) {
      std::string location = placement == OpsPlacement::REQUEST_ROOT
          ? "request root"
          : placement == OpsPlacement::FUSION ? "Fusion.ops"
          : placement == OpsPlacement::PLUGIN ? "a plugin operation's ops"
                                              : "nested facet bucket";
      throw std::runtime_error("facet '" + std::string(facetName) + "' at "
          + location
          + ": selected is only supported on facets directly inside TopDocs.ops");
    }
  }

  static std::string selectionValueText(const api::Val& value) {
    return std::visit(luxir::overloaded{
      [](std::string_view text) { return "'" + std::string(text) + "'"; },
      [](int64_t number) { return std::to_string(number); },
      [](double number) { return std::to_string(number); },
      [](float number) { return std::to_string(number); },
      [](bool boolean) { return std::string(boolean ? "true" : "false"); },
      [](const auto&) { return std::string("<value>"); },
    }, value.kind);
  }

  CollapsePlan* resolveCollapse(const api::Collapse& spec, bool getNumber) {
    using Missing = luxir::api::Collapse_::MissingPolicy;
    if (spec.missing != Missing::EXPAND && spec.missing != Missing::GROUP
        && spec.missing != Missing::DROP) throw RequestError("invalid collapse.missing");
    if (spec.field.empty()) throw RequestError("collapse.field must not be empty");
    auto* collapse = luxir::arenaCreate<CollapsePlan>(
        req.arena, *view(), spec.missing, getNumber);
    std::unordered_set<std::string> resolved;
    for (std::string_view name : spec.field) {
      auto field = parseView.fields.resolveLocal(name, OpClass::VALUE);
      auto& type = *field.fieldType;
      if (type.multiValued() || !type.hasColumn()) {
        throw RequestError("collapse key must be single-valued with a column: " + std::string(name));
      }
      if (!resolved.insert(field.physicalName).second) {
        throw RequestError("duplicate resolved collapse field: " + std::string(name));
      }
      type.requireCapability(FieldType::COLLAPSE, "collapse", name);
      using Type = luxir::api::GroupComponent_::Type;
      Type wireType;
      switch (type.type()) {
        case FieldType::STRING: wireType = Type::STRING; break;
        case FieldType::ID: wireType = Type::ID; break;
        case FieldType::INT: wireType = Type::INT; break; // Luxir integers are signed int64.
        case FieldType::DATE: wireType = Type::DATE; break;
        case FieldType::BOOLEAN: wireType = Type::BOOLEAN; break;
        case FieldType::DECIMAL: wireType = Type::DECIMAL; break;
        case FieldType::IP: wireType = Type::IP; break;
        default: throw std::logic_error("Unhandled collapse key wire type");
      }
      auto storage = FieldType::storage(type.type());
      bool ordinal = storage == FieldType::Storage::TERMS;
      if (ordinal && !type.isSet(FieldType::INDEX_DOCS)) {
        throw RequestError("string collapse key must be indexed: " + std::string(name));
      }
      collapse->fields.push_back({field.physicalName, wireType, storage,
          ordinal ? view()->getOrdMap(field.physicalName) : nullptr,
          view()->field(field.physicalName), type.valueNature()});
    }
    return collapse;
  }

  template <typename Facet>
  const CollapsePlan* resolveMeasure(const Facet& facetReq) {
    const CollapsePlan* measure = inheritedFacetMeasure;
    using Unit = api::FieldFacet_::CountUnit;
    if (facetReq.count_unit) {
      switch (*facetReq.count_unit) {
        case Unit::DOCS: measure = nullptr; break;
        case Unit::GROUPS:
          if (enclosingCollapse == nullptr) {
            throw RequestError("count_unit groups requires an enclosing collapse");
          }
          measure = enclosingCollapse;
          break;
        default: throw RequestError("invalid facet count_unit");
      }
    }
    if (facetReq.count_distinct) {
      if (facetReq.count_distinct->get_count || !facetReq.count_distinct->head_sort.empty()
          || !facetReq.count_distinct->sort.empty()
          || !facetReq.count_distinct->ops.empty()) {
        throw RequestError("count_distinct accepts only field and missing");
      }
      auto* distinct = resolveCollapse(*facetReq.count_distinct, false);
      if (facetReq.count_unit) {
        bool same = measure != nullptr && measure->missing == distinct->missing
            && measure->fields.size() == distinct->fields.size();
        if (same) for (size_t i = 0; i < measure->fields.size(); i++) {
          same &= measure->fields[i].name == distinct->fields[i].name;
        }
        if (!same) throw RequestError("conflicting count_unit and count_distinct measures");
      }
      measure = distinct;
    }
    return measure;
  }

  void configureMeasure(FacetReq& facet, const CollapsePlan* measure) {
    facet.countDistinct = measure;
    if (measure == nullptr) return;
    for (auto& plan : facetGroupPlans) {
      const auto* spec = &plan->key;
      bool same = spec->fields.size() == measure->fields.size();
      if (same) for (size_t i = 0; i < spec->fields.size(); i++) same &= spec->fields[i].name == measure->fields[i].name;
      if (same) { facet.groupPlan = plan; return; }
    }
    facet.groupPlan = std::make_shared<FacetGroupPlan>(*measure);
    facetGroupPlans.push_back(facet.groupPlan);
  }

  // FieldFacet.term_filter with literals normalized as the field normalizes
  // query text: prefix as a prefix query, regex and exclude_regex as a regex
  // query, include and exclude as any_of values.
  FacetTermFilterSpec parseTermFilter(std::string_view facetName,
                                      const ResolvedFieldHandle& target,
                                      const luxir::api::TermFilter& termFilter) {
    std::string context = std::format("facet '{}': term_filter", facetName);
    if (termFilter.num_partitions == 0 && termFilter.partition != 0) {
      throw RequestError(context + ": partition requires num_partitions");
    }
    if (termFilter.num_partitions != 0 && termFilter.partition >= termFilter.num_partitions) {
      throw RequestError(std::format("{}: partition {} must be less than num_partitions {}",
                                     context, termFilter.partition, termFilter.num_partitions));
    }
    QueryBuilder builder(req.requestPool, *req.schema, coerceContext(*req.timeZone),
                         facetName, &req.warnings, &parseView.fields);
    FacetTermFilterSpec spec;
    // Errors name the term_filter field they came from.
    auto within = [&](std::string_view name, auto&& build) {
      try {
        return build();
      } catch (const std::runtime_error& e) {
        throw RequestError(std::format("{} {}: {}", context, name, e.what()));
      }
    };
    // A positive regex whose literal outgrows fitted terms matches by the kept
    // prefix instead, as the regex query does, so only the others can warn.
    auto compile = [&](std::string_view pattern, bool positive) {
      TermPattern compiled = builder.compileRegexPattern(target, pattern);
      if (compiled.fittedPolicy && !(positive && compiled.fittedPrefix)) {
        warnIfFittedTermsReachable(*view(), &req.warnings, req.requestPool,
            target.physicalName, *compiled.fittedPolicy, [&] {
              return compiled.dfa.maxAcceptedLength() >= (int32_t)PackedTerm::MIN_FITTED_LEN;
            });
      }
      return compiled;
    };
    auto normalize = [&](std::span<const std::string_view> values) {
      std::vector<std::string> terms;
      terms.reserve(values.size());
      for (std::string_view value : values) {
        terms.emplace_back(builder.normalizeLiteral(target, value, true));
      }
      return terms;
    };
    if (!termFilter.prefix.empty()) {
      spec.prefix = within("prefix", [&] {
        return std::string(builder.normalizePrefix(target, termFilter.prefix));
      });
    }
    if (!termFilter.regex.empty()) {
      spec.regex = within("regex", [&] { return compile(termFilter.regex, true); });
    }
    if (!termFilter.exclude_regex.empty()) {
      spec.excludeRegex = within("exclude_regex", [&] {
        return compile(termFilter.exclude_regex, false);
      });
    }
    if (!termFilter.include.empty()) {
      spec.include = within("include", [&] { return normalize(termFilter.include); });
    }
    spec.exclude = within("exclude", [&] { return normalize(termFilter.exclude); });
    spec.partition = termFilter.partition;
    spec.numPartitions = termFilter.num_partitions;
    return spec;
  }

  FacetReq* createFieldFacetReq(
      std::string_view facetName, const luxir::api::FieldFacet& facetReq,
      OpsPlacement placement,
      const CanonicalValueSet* preparedSelected = nullptr) {
    validateSelectionEnvelope(facetName, facetReq, placement);
    const CollapsePlan* measure = resolveMeasure(facetReq);
    const auto& target = parseView.fields.resolveLocal(facetReq.field, OpClass::VALUE, std::format("facet '{}'", facetName));
    auto facetField = api::build::arenaStr(req.requestPool, target.physicalName);
    int64_t limit = 5; // default limit
    if (facetReq.limit.has_value()) {
      // Any negative limit means all, as for top docs; facet ops test the
      // -1 sentinel exactly.
      limit = std::max<int64_t>(*facetReq.limit, -1);
    }
    int64_t minCount = -1;
    if (facetReq.mincount.has_value()) {
      minCount = *facetReq.mincount;
    }
    bool missing = facetReq.missing;

    // Resolve everything that could throw here in the parser and pass the
    // results into the op ctors (see TopDocsReq's ctor comment).
    auto* ftype = target.fieldType;
    ftype->requireCapability(FieldType::FIELD_FACET, "field_facet", facetField);
    if (facetReq.term_filter.has_value()) {
      ftype->requireCapability(FieldType::TERM_FILTER,
                               std::format("facet '{}': term_filter", facetName), facetField);
    }

    std::span<const int64_t> selectedInts;
    std::span<const std::string_view> selectedStrings;
    std::string valueName = "facet '" + std::string(facetName)
        + "': selected";
    ValueSequence selectedValues = facetReq.selected.has_value()
        ? ValueSequence(*facetReq.selected, valueName,
                        ValueSequence::NullError::MUST_NOT_BE_NULL)
        : ValueSequence();
    CanonicalValueSet ownedSelected;
    if (preparedSelected == nullptr && !selectedValues.empty()) {
      CoerceContext selectionContext = coerceContext(*req.timeZone);
      QueryBuilder builder(req.requestPool, *req.schema,
                           selectionContext, facetName, &req.warnings, &parseView.fields);
      ownedSelected = builder.canonicalizeFieldValues(
          target, selectedValues, true);
      preparedSelected = &ownedSelected;
    }
    if (preparedSelected != nullptr && !preparedSelected->empty()) {
      if (FieldType::isNumericColumn(ftype->type())) {
        selectedInts = preparedSelected->numericsInInputOrder();
      } else {
        selectedStrings = preparedSelected->termsInInputOrder();
      }
    }

    FieldFacetReq* facet = nullptr;
    switch (FieldType::storage(ftype->type())) {
      // Numeric columns hold raw integers, date ticks, or sortable floating
      // bits. Field facets count distinct encoded values for all of them;
      // date-histogram bucketing is a range facet, not a terms facet.
      case FieldType::Storage::INT64: {
        if (facetReq.mincount.has_value() && *facetReq.mincount < 1) {
          throw std::runtime_error("facet '" + std::string(facetName) + "': mincount < 1 (zero-count buckets) is not supported for numeric field facets");
        }
        facet = luxir::arenaCreate<IntFacetReq>(req.arena, req, view(), facetReq,
            facetField, facetName, limit, minCount, missing, selectedInts);
        break;
      }
      case FieldType::Storage::TERMS: {
        // A string facet counts term ordinals, so it needs the term dictionary
        // the ord column indexes into. A column-only field (IndexMode::NONE)
        // stores values with nothing to take ordinals from; without this the
        // request reached StrFacetOp and sized its counter from an OrdMap over
        // a field with no terms - a length_error on debug builds and a
        // segfault on release. Faceting raw column bytes is a separate feature,
        // not a degraded form of this one.
        if (!ftype->indexed()) {
          throw std::runtime_error("facet '" + std::string(facetName) + "': field '"
              + std::string(facetField) + "' is column-only (not indexed); string facets "
              "require an indexed field");
        }
        if (!ftype->hasColumn()) {
          throw std::runtime_error("facet '" + std::string(facetName) + "': field '"
              + std::string(facetField) + "' has no column (column: false); string facets "
              "read the column");
        }
        std::optional<FacetTermFilterSpec> termFilter;
        if (facetReq.term_filter.has_value()) {
          termFilter = parseTermFilter(facetName, target, *facetReq.term_filter);
        }
        auto ordMap = view()->getOrdMap(facetField);
        facet = luxir::arenaCreate<StrFacetOp>(req.arena, req, view(), facetReq,
            facetField, facetName, limit, minCount, missing,
            std::move(ordMap), selectedStrings, std::move(termFilter));
        break;
      }
      case FieldType::Storage::TEXT:
        if (measure != nullptr) throw RequestError("TEXT field_facet distinct counts are not supported yet");
        facet = luxir::arenaCreate<FullTextFacetReq>(req.arena, req, view(), facetReq,
            facetField, facetName, limit, minCount, missing, selectedStrings);
        break;
      default: ;
    }
    if (facet == nullptr) {
      throw std::logic_error("Unhandled field facet storage");
    }
    configureMeasure(*facet, measure);
    return facet;
  }

  FacetReq* createRangeFacetReq(
      std::string_view facetName, const luxir::api::RangeFacet& facetReq,
      OpsPlacement placement) {
    validateSelectionEnvelope(facetName, facetReq, placement);
    const CollapsePlan* measure = resolveMeasure(facetReq);
    const auto& target = parseView.fields.resolveLocal(facetReq.field, OpClass::VALUE, std::format("facet '{}'", facetName));
    auto facetField = api::build::arenaStr(req.requestPool, target.physicalName);
    bool dataBounds = facetReq.bounds == api::RangeFacet_::Bounds::DATA;
    if (!dataBounds && facetReq.bounds != api::RangeFacet_::Bounds::EXPLICIT) {
      throw std::runtime_error("facet '" + std::string(facetName)
          + "': bounds must be EXPLICIT or DATA");
    }
    if (dataBounds) {
      if (facetReq.selected.has_value()) {
        throw std::runtime_error("facet '" + std::string(facetName)
            + "': selected is not supported with bounds DATA");
      }
    } else {
      if (facetReq.origin.has_value()) {
        throw std::runtime_error("facet '" + std::string(facetName)
            + "': origin requires bounds DATA");
      }
      if (!facetReq.start.has_value() || !facetReq.end.has_value()) {
        throw std::runtime_error("facet '" + std::string(facetName)
            + "': range start and end are required unless bounds is DATA");
      }
    }
    if (std::holds_alternative<std::monostate>(facetReq.gap_kind)) {
      throw std::runtime_error("facet '" + std::string(facetName)
          + "': range gap or calendar_gap is required");
    }
    auto* fieldType = target.fieldType;
    fieldType->requireCapability(FieldType::RANGE_FACET,
        "range_facet", facetField);

    bool calendar = std::holds_alternative<api::CalendarGap>(facetReq.gap_kind);
    if (fieldType->type() != FieldType::Type::DATE
        && (!facetReq.time_zone.empty() || calendar)) {
      throw std::runtime_error("facet '" + std::string(facetName)
          + "': time_zone and calendar_gap are only valid for DATE fields");
    }

    std::optional<CalendarUnit> calendarGridUnit;
    if (calendar) {
      const auto& spec = std::get<api::CalendarGap>(facetReq.gap_kind);
      calendarGridUnit = calendarUnit(spec.unit);
      if (spec.n <= 0 || !calendarGridUnit) {
        throw std::runtime_error("facet '" + std::string(facetName)
            + "': calendar_gap requires n > 0 and a DAY/WEEK/MONTH/QUARTER/YEAR unit");
      }
      if (facetReq.origin.has_value()) {
        throw std::runtime_error("facet '" + std::string(facetName)
            + "': origin is not supported with calendar_gap");
      }
    }

    int64_t minCount = facetReq.mincount.value_or(0);
    if (minCount < 0) {
      throw std::runtime_error("facet '" + std::string(facetName)
          + "': mincount must be >= 0");
    }

    TimeZone zone = TimeZone::utc();
    if (fieldType->type() == FieldType::Type::DATE) {
      if (facetReq.time_zone.empty()) {
        if (!req.timeZone) {
          throw std::runtime_error(req.timeZoneError);
        }
        zone = *req.timeZone;
      } else {
        auto resolved = resolveTimeZone(facetReq.time_zone);
        if (!resolved) {
          throw std::runtime_error("facet '" + std::string(facetName)
              + "': " + timeZoneResolutionError(facetReq.time_zone));
        }
        zone = *resolved;
      }
    }
    CoerceContext context = coerceContext(zone);

    auto resolveSelectedBuckets = [&](const ParsedFences& parsed,
                                      auto&& encode) {
      std::span<size_t> selected;
      std::string valueName = "facet '" + std::string(facetName)
          + "': selected";
      ValueSequence selectedValues = facetReq.selected.has_value()
          ? ValueSequence(*facetReq.selected, valueName,
                          ValueSequence::NullError::MUST_NOT_BE_NULL)
          : ValueSequence();
      if (selectedValues.empty()) return selected;
      selected = req.requestPool.make_span<size_t>(selectedValues.size());
      std::unordered_set<size_t> seen;
      size_t i = 0;
      selectedValues.visit([&](const auto& viewed) {
        for (const auto& value : viewed) {
          const api::Val* val;
          api::Val scalar;
          if constexpr (std::is_same_v<std::decay_t<decltype(value)>, api::Val>) {
            val = &value;
          } else {
            scalar = coerce::scalarVal(value);
            val = &scalar;
          }
          int64_t fence = encode(*val);
          auto found = std::lower_bound(
              parsed.values.begin(), parsed.values.end() - 1, fence);
          if (found == parsed.values.end() - 1 || *found != fence) {
            throw std::runtime_error("facet '" + std::string(facetName)
                + "': selected value " + selectionValueText(*val)
                + " is not a generated range lower fence");
          }
          size_t bucket = (size_t)(found - parsed.values.begin());
          if (!seen.insert(bucket).second) {
            throw std::runtime_error("facet '" + std::string(facetName)
                + "': duplicate selected value after coercion: "
                + selectionValueText(*val));
          }
          selected[i++] = bucket;
        }
      });
      return selected;
    };

    auto boundFence = [&](const api::Val& value, FieldType::LiteralUse use) {
      auto range = fieldType->coerceIntWindow(value, use, facetField, context);
      if (range.granuleSkipped) {
        const auto* text = std::get_if<std::string_view>(&value.kind);
        if (text != nullptr) {
          warnOnce("date_granule_skipped",
              "facet '" + std::string(facetName) + "', DATE field '"
              + std::string(facetField) + "': date granule '" + std::string(*text)
              + "' skipped: no such local granule in " + zone.name());
        }
      }
      if (use == FieldType::LiteralUse::MEMBER) {
        if (range.empty()) throw RequestError(fmt::format("facet '{}': selected value is outside the field's value domain", facetName));
        if (range.lo != range.hi) throw std::logic_error("MEMBER coercion did not produce a point");
        return range.lo;
      }
      if (use == FieldType::LiteralUse::GTE) return range.empty() ? INT64_MAX : range.lo;
      // LT MIN is empty; otherwise undo the inclusive upper edge. A fence
      // beyond the encoded domain saturates rather than overflowing.
      if (range.empty()) return INT64_MIN;
      return range.hi == INT64_MAX ? INT64_MAX : range.hi + 1;
    };

    bool floating = fieldType->type() == FieldType::Type::FLOAT
                 || fieldType->type() == FieldType::Type::DOUBLE;
    if (floating) {
      auto floatingValue = [&](const api::Val& value, std::string_view name) {
        double result = coerce::toDouble(value, facetField);
        if (!std::isfinite(result)) {
          throw std::runtime_error("facet '" + std::string(facetName)
              + "': range " + std::string(name) + " must be finite");
        }
        return result == 0.0 ? 0.0 : result;
      };
      std::optional<double> givenStart, givenEnd;
      if (facetReq.start.has_value()) givenStart = floatingValue(*facetReq.start, "start");
      if (facetReq.end.has_value()) givenEnd = floatingValue(*facetReq.end, "end");
      if (givenStart && givenEnd && *givenStart >= *givenEnd) {
        throw std::runtime_error("facet '" + std::string(facetName)
            + "': range start must be less than end");
      }
      const auto* gapValue = std::get_if<api::Val>(&facetReq.gap_kind);
      if (gapValue == nullptr) {
        throw std::runtime_error("facet '" + std::string(facetName)
            + "': time_zone and calendar_gap are only valid for DATE fields");
      }
      double gap = floatingValue(*gapValue, "gap");
      if (gap <= 0.0) {
        throw std::runtime_error("facet '" + std::string(facetName)
            + "': range gap must be > 0");
      }
      auto gridType = fieldType->type();
      double origin = dataBounds
          ? (facetReq.origin.has_value() ? floatingValue(*facetReq.origin, "origin") : 0.0)
          : *givenStart;
      auto grid = RangeFacetGrid::floating(gridType, gap, origin);
      auto encodedStart = givenStart ? std::optional<int64_t>(grid.encode(*givenStart)) : std::nullopt;
      auto encodedEnd = givenEnd ? std::optional<int64_t>(grid.encode(*givenEnd)) : std::nullopt;
      if (dataBounds) {
        auto* facet = luxir::arenaCreate<IntFacetRangeReq>(req.arena, req, view(),
            facetField, facetName, std::move(grid), encodedStart, encodedEnd,
            minCount, facetReq.missing);
        configureMeasure(*facet, measure);
        return facet;
      }
      auto parsed = makeGridFences(facetName, grid, *encodedStart, *encodedEnd, true);
      auto selected = resolveSelectedBuckets(parsed, [&](const api::Val& value) {
        double canonical = floatingValue(value, "selected value");
        return fieldType->type() == FieldType::Type::FLOAT
            ? (int64_t)floatToSortableInt32((float)canonical)
            : doubleToSortableInt64(canonical);
      });
      auto* facet = luxir::arenaCreate<IntFacetRangeReq>(
          req.arena, req, view(), facetField, facetName, parsed.values,
          parsed.affine, parsed.affineGap, minCount,
          facetReq.missing, selected, parsed.runs);
      configureMeasure(*facet, measure);
      return facet;
    }

    std::optional<int64_t> givenStart, givenEnd;
    if (facetReq.start.has_value()) givenStart = boundFence(*facetReq.start, FieldType::LiteralUse::GTE);
    if (facetReq.end.has_value()) givenEnd = boundFence(*facetReq.end, FieldType::LiteralUse::LT);
    if (givenStart && givenEnd && *givenStart >= *givenEnd) {
      throw std::runtime_error("facet '" + std::string(facetName)
          + "': range start must be less than end");
    }
    bool date = fieldType->type() == FieldType::DATE;
    auto dateUnit = date ? DateFieldType::unitOf(*fieldType) : DateUnit::MILLIS;
    auto grid = [&]() {
      if (calendar) {
        const auto& spec = std::get<api::CalendarGap>(facetReq.gap_kind);
        return RangeFacetGrid::calendar(*calendarGridUnit, spec.n, zone,
            dataBounds ? std::nullopt : givenStart, dateUnit);
      }
      auto integer = [&](const api::Val& value, std::string_view name) -> __int128 {
        try {
          return fieldType->coerceGridValue(value, facetField, name, context);
        } catch (const RequestError& e) {
          throw RequestError(std::format("facet '{}': {}", facetName, e.what()), e.code);
        }
      };
      auto convertedGap = integer(std::get<api::Val>(facetReq.gap_kind), "fixed gap");
      if (convertedGap <= 0 || convertedGap > INT64_MAX) {
        throw RequestError(std::format("facet '{}': range gap must be > 0 and fit int64 in field units", facetName));
      }
      int64_t gap = (int64_t)convertedGap;
      int64_t origin = dataBounds && facetReq.origin.has_value()
          ? (int64_t)(integer(*facetReq.origin, "origin") % gap) : 0;
      return RangeFacetGrid::fixed(fieldType->type(), gap, origin, zone, dateUnit,
          dataBounds ? std::nullopt : givenStart);
    }();
    if (dataBounds) {
      auto* facet = luxir::arenaCreate<IntFacetRangeReq>(req.arena, req, view(),
          facetField, facetName, std::move(grid), givenStart, givenEnd,
          minCount, facetReq.missing);
      configureMeasure(*facet, measure);
      return facet;
    }
    auto parsed = makeGridFences(facetName, grid, *givenStart, *givenEnd, true);

    auto selected = resolveSelectedBuckets(parsed, [&](const api::Val& value) {
      return boundFence(value, FieldType::LiteralUse::MEMBER);
    });

    auto* facet = luxir::arenaCreate<IntFacetRangeReq>(
        req.arena, req, view(), facetField, facetName, parsed.values,
        parsed.affine, parsed.affineGap, minCount,
        facetReq.missing, selected, parsed.runs);
    configureMeasure(*facet, measure);
    return facet;
  }

  // Build the copyable sort plan. Schema lookups that may throw are resolved
  // here before the plan is passed to TopDocsReq.
  SortPlan parseSorts(std::span<const luxir::api::SortSpec> sorts) {
    SortPlan out;
    if (sorts.empty()) return out;
    out.useFieldSort = true;
    out.rankNeedsScores = false;
    for (const auto& sortSpec : sorts) {
      ValueExprOptions options{req.schema.get(), sortSpec.vars};
      options.fieldResolver = &parseView.fields;
      options.opName = "sort";
      options.view = &parseView;
      ValueProgram* program = ValueExprParser(options, req.arena).parse(sortSpec.expr);
      const ValueNode& root = program->root();
      bool scoreRoot = root.kind == ValueNodeKind::SCORE;
      bool defaultDesc = sortSpec.dir == luxir::api::SortSpec_::SortDir::UNKNOWN
        && scoreRoot;
      SortField::SortOrder order =
        sortSpec.dir == luxir::api::SortSpec_::SortDir::DESC || defaultDesc
          ? SortField::DESC : SortField::ASC;
      // TODO: expose per-sort missing=first/last/custom. Numerics substitute custom
      // values directly; string comparators binary-search global terms at setup and
      // use the matching ord or insertion point.
      FieldComparator::MissingValue missing = FieldComparator::MISSING_LAST;
      if (root.source && (valueArray(root.type) || root.type == ValueType::COLUMN_ONLY)) {
        throw std::runtime_error(fmt::format(
            "sort expression '{}' reads descendant field '{}'; choose an explicit reducer such as {}",
            sortSpec.expr, root.text, ValueFunctionRegistry::arrayReducerNames()));
      }
      if (root.kind == ValueNodeKind::COLUMN) {
        auto* fieldTypePtr = req.schema->physical(root.text);
        out.clauses.emplace_back(SortField(root.text, *fieldTypePtr, order, missing));
      } else if (root.kind == ValueNodeKind::SCORE) {
        out.clauses.emplace_back(SortClause::SCORE, order);
        out.rankNeedsScores = true;
      } else if (root.kind == ValueNodeKind::DOCID) {
        out.clauses.emplace_back(SortClause::DOC, order);
      } else if (root.kind == ValueNodeKind::CONSTANT) {
        throw std::runtime_error(fmt::format(
            "sort expression '{}' reads as the numeric constant {}; to sort a numeric-looking "
            "field name, write col(\"{}\")",
            sortSpec.expr, root.text, root.text));
      } else {
        if (valueArray(root.type)) {
          throw std::runtime_error(fmt::format(
              "sort expression '{}' produces a {}; choose an explicit reducer such as "
              "{}",
              sortSpec.expr, valueTypeName(root.type),
              ValueFunctionRegistry::arrayReducerNames()));
        }
        out.clauses.emplace_back(*program, order);
        out.rankNeedsScores |= program->needsScore;
      }
    }

    bool scoreDesc = out.clauses[0].getKind() == SortClause::SCORE
      && out.clauses[0].getOrder() == SortField::DESC;
    bool canonicalScore = out.clauses.size() == 1 && scoreDesc;
    bool canonicalScoreDoc = out.clauses.size() == 2 && scoreDesc
      && out.clauses[1].getKind() == SortClause::DOC
      && out.clauses[1].getOrder() == SortField::ASC;
    // Explicit doc asc is the implicit tie for both documents and groups.
    if (canonicalScore || canonicalScoreDoc) out.useFieldSort = false;
    return out;
  }

  // Build Weights for a filter list after the serial logical-validation pass;
  // the resulting span is passed to the op constructor.
  std::span<Query::Weight*> buildFilterWeights(
      std::span<ParsedFilter> filters,
      Query::Context& qcontext, int32_t requestFlags) {
    if (filters.empty()) return {};
    // Filters constrain matches but never contribute to score. Preserve any
    // future request flags, but clear NEED_SCORES.
    int32_t filterFlags = requestFlags
        & ~(Query::NEED_SCORES | Query::ALLOW_PRUNING);
    size_t sourceCount = 0;
    for (const auto& filter : filters) {
      sourceCount += filter.sourceCount();
    }
    auto weights = req.requestPool.make_span<Query::Weight*>(sourceCount);
    size_t source = 0;
    for (const auto& filter : filters) {
      for (size_t i = 0; i < filter.sourceCount(); i++) {
        weights[source++] =
            filter.source(i)->createWeight(qcontext, filterFlags);
      }
    }
    assert(source == weights.size());
    return weights;
  }

  ParsedDomain parseDomain(const api::Domain& domain, std::string_view key, ParseContext& parseContext) {
    ProtobufQueryParser parser(parseContext);
    std::string path = "op '" + std::string(key) + "'.domain";
    if (domain.apply_parent_filters.has_value()
        && !domain.query.has_value()) {
      throw std::runtime_error(
          path + ".apply_parent_filters requires domain.query");
    }

    ParsedDomain parsed;
    parsed.applyParentFilters =
        domain.apply_parent_filters.value_or(false);
    if (domain.query.has_value()) {
      if (std::holds_alternative<std::monostate>(domain.query->kind)) {
        throw std::runtime_error(path + ".query requires a query kind");
      }
      parsed.query = parser.parse(*domain.query);
    }
    parsed.filters.reserve(domain.filter.size());
    for (size_t i = 0; i < domain.filter.size(); i++) {
      if (std::holds_alternative<std::monostate>(
              domain.filter[i].kind)) {
        throw std::runtime_error(
            path + ".filter[" + std::to_string(i)
            + "] requires a query kind");
      }
      parsed.filters.push_back(parser.parse(domain.filter[i]));
    }
    return parsed;
  }

  ParsedDomains parseChildDomains(
      OpsMap ops, ParseContext& parseContext) {
    ParsedDomains out;
    for (const auto& [key, childView] : lastWins(ops)) {
      const api::SearchOp& child = **childView;
      auto childOptions = options.child(child, key);
      if (!child.domain.has_value()) continue;
      auto inheritedCoercion = parseContext.coerceContext;
      parseContext.coerceContext.dateInputUnit = RequestFlags::dateInputUnit(childOptions.flags);
      parseContext.coerceContext.decimalScaled = (childOptions.flags & RequestFlags::DECIMAL_SCALED) != 0;
      auto restoreCoercion = scope_guard([&]() noexcept {
        parseContext.coerceContext = inheritedCoercion;
      });
      if (movesView(*child.domain)) {
        if (child.domain->apply_parent_filters.has_value() && !child.domain->query) {
          throw RequestError("domain.apply_parent_filters requires domain.query");
        }
        if (child.domain->query && child.domain->apply_parent_filters.value_or(false)) {
          ParsedDomain parsed;
          parsed.query = req.requestPool.make<AllQuery>();
          parsed.applyParentFilters = true;
          out.emplace(key, std::move(parsed));
        }
        continue;
      }
      auto parsed = parseDomain(*child.domain, key, parseContext);
      if (parsed.query == nullptr && parsed.filters.empty()) continue;
      out.emplace(key, std::move(parsed));
    }
    return out;
  }

  void validateDomainQueries(
      const ParsedDomains& domains, Query::PlanningContext& planning) {
    for (const auto& [key, domain] : domains) {
      unused(key);
      if (domain.query != nullptr) {
        domain.query->validateLogical(planning);
      }
      for (auto* filter : domain.filters) {
        filter->validateLogical(planning);
      }
    }
  }

  void addDomainRoute(std::string_view key, const ParsedDomain* domain,
      std::function_ref<bool(size_t)> excluded, Query::PlanningContext* planning, DomainVariantPlan& plan) {
    std::optional<DomainVariantPlan::QuerySpec> reset;
    std::vector<DomainVariantPlan::QuerySpec> extras;
    if (domain != nullptr) {
      assert(planning != nullptr);
      if (domain->query != nullptr) {
        reset.emplace(DomainVariantPlan::QuerySpec{
            domain->query,
            planning->structuralFilterKey(*domain->query)});
      }
      extras.reserve(domain->filters.size());
      for (auto* filter : domain->filters) {
        extras.push_back({
            filter, planning->structuralFilterKey(*filter)});
      }
    }
    bool created = plan.addChild(
        key,
        excluded,
        reset ? &*reset : nullptr,
        domain != nullptr && domain->applyParentFilters,
        extras);
    if (!created) return;
    if (nonDefaultDomainVariants
        >= DomainVariantPlan::MAX_NON_DEFAULT_VARIANTS) {
      throw std::runtime_error(
          "op '" + std::string(key)
          + (domain != nullptr
                  ? "': domain variant cap 64 exceeded"
                  : "': routed filter variant cap 64 exceeded"));
    }
    nonDefaultDomainVariants++;
  }

  void addDomainVariants(
      OpsMap ops, std::span<ParsedFilter> filters,
      const ParsedDomains& domains, Query::PlanningContext* planning,
      DomainVariantPlan& plan) {
    for (const auto& [key, childView] : lastWins(ops)) {
      unused(childView);
      auto found = domains.find(key);
      const ParsedDomain* domain =
          found == domains.end() ? nullptr : &found->second;
      addDomainRoute(key, domain, [&](size_t filter) {
        return std::ranges::find(filters[filter].exceptOps, key) != filters[filter].exceptOps.end();
      }, planning, plan);
    }
  }

  Query* combineAnySelectionPredicates(std::span<Query*> predicates) {
    assert(!predicates.empty());
    if (predicates.size() == 1) return predicates[0];
    return req.requestPool.make<BooleanQuery>(
        std::span<Query*>{}, predicates, std::span<Query*>{},
        std::span<Query*>{}, 1);
  }

  FacetParsePlan createQueryFacetPlan(
      std::string_view facetName, const api::QueryFacet& facetReq,
      OpsPlacement placement, ProtobufQueryParser& parser) {
    validateSelectionEnvelope(facetName, facetReq, placement);
    if (facetReq.buckets.empty()) {
      throw std::runtime_error("facet '" + std::string(facetName)
          + "': query_facet buckets must be nonempty");
    }

    FacetParsePlan plan;
    plan.bucketQueries = req.requestPool.make_span<Query*>(
        facetReq.buckets.size());
    std::unordered_map<std::string_view, size_t> bucketIndexes;
    bucketIndexes.reserve(facetReq.buckets.size());
    for (size_t i = 0; i < facetReq.buckets.size(); i++) {
      const api::QueryBucket& bucket = facetReq.buckets[i];
      if (bucket.name.empty()) {
        throw std::runtime_error("facet '" + std::string(facetName)
            + "': query_facet bucket names must be nonempty");
      }
      if (!bucketIndexes.emplace(bucket.name, i).second) {
        throw std::runtime_error("facet '" + std::string(facetName)
            + "': duplicate query_facet bucket name '"
            + std::string(bucket.name) + "'");
      }
      if (!bucket.query.has_value()) {
        throw std::runtime_error("facet '" + std::string(facetName)
            + "': query_facet bucket '" + std::string(bucket.name)
            + "' requires a query");
      }
      try {
        plan.bucketQueries[i] = parser.parse(*bucket.query);
      } catch (const std::runtime_error& error) {
        auto info = classifyException(error, ErrorKind::INVALID_REQUEST);
        throw ApiError(info.kind, info.code, "facet '" + std::string(facetName)
            + "': query_facet bucket '" + std::string(bucket.name)
            + "': " + error.what());
      }
    }

    std::string valueName = "facet '" + std::string(facetName)
        + "': selected";
    ValueSequence selected = facetReq.selected.has_value()
        ? ValueSequence(*facetReq.selected, valueName,
                        ValueSequence::NullError::MUST_NOT_BE_NULL)
        : ValueSequence();
    if (selected.empty()) return plan;

    auto indexes = req.requestPool.make_span<size_t>(selected.size());
    std::unordered_set<std::string_view> seen;
    size_t out = 0;
    selected.visit([&](const auto& viewed) {
      for (const auto& value : viewed) {
        std::string_view selectedName;
        using Value = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<Value, api::Val>) {
          const auto* text = std::get_if<std::string_view>(&value.kind);
          if (text == nullptr) {
            throw std::runtime_error(valueName + " values must be strings");
          }
          selectedName = *text;
        } else if constexpr (std::is_same_v<Value, std::string_view>) {
          selectedName = value;
        } else {
          throw std::runtime_error(valueName + " values must be strings");
        }
        auto found = bucketIndexes.find(selectedName);
        if (found == bucketIndexes.end()) {
          throw std::runtime_error("facet '" + std::string(facetName)
              + "': selected bucket name '" + std::string(selectedName)
              + "' does not match any query_facet bucket");
        }
        if (!seen.insert(selectedName).second) {
          throw std::runtime_error("facet '" + std::string(facetName)
              + "': duplicate selected bucket name '"
              + std::string(selectedName) + "'");
        }
        indexes[out++] = found->second;
      }
    });
    plan.selectedBuckets = indexes;
    return plan;
  }

  void validateQueryFacetQueries(FacetParsePlan& plan) {
    plan.planning = createPlanningContext();
    for (Query* query : plan.bucketQueries) {
      query->validateLogical(*plan.planning);
    }
  }

  void validateUnselectedQueryFacetQueries(
      OpsMap ops, Query::PlanningContext& planning) {
    for (const auto& [key, childView] : lastWins(ops)) {
      unused(key);
      const api::SearchOp& child = **childView;
      auto prepared = preparedFacets.find(&child);
      if (prepared == preparedFacets.end()
          || prepared->second.bucketQueries.empty()) {
        continue;
      }
      FacetParsePlan& plan = prepared->second;
      plan.planning = &planning;
      std::unordered_set<size_t> selected;
      if (plan.refinerPlanned) {
        selected.insert(plan.selectedBuckets.begin(),
                        plan.selectedBuckets.end());
      }
      for (size_t i = 0; i < plan.bucketQueries.size(); i++) {
        if (!selected.contains(i)) {
          plan.bucketQueries[i]->validateLogical(planning);
        }
      }
    }
  }

  FacetReq* createQueryFacetReq(
      std::string_view facetName, const api::QueryFacet& facetReq,
      const FacetParsePlan& plan, bool bucketChild) {
    assert(plan.planning != nullptr);
    Query::Context* context = Query::Context::create(
        &req.arena, *plan.planning);
    auto weights = req.requestPool.make_span<Query::Weight*>(
        plan.bucketQueries.size());
    for (size_t i = 0; i < weights.size(); i++) {
      weights[i] = plan.bucketQueries[i]->createWeight(*context, 0);
    }
    std::span<FilterCache::Use*> uses;
    if (bucketChild && &plan.planning->topReader == view().get()) {
      uses = req.requestPool.make_span<FilterCache::Use*>(weights.size());
      for (size_t i = 0; i < weights.size(); i++) {
        uses[i] = !weights[i]->matchesAllDocs() && !weights[i]->needsPrepare()
                && plan.bucketQueries[i]->recoverableAsPredicate()
            ? plan.planning->getFilterUse(*plan.bucketQueries[i]) : nullptr;
      }
    }
    auto* facet = luxir::arenaCreate<QueryFacetReq>(
        req.arena, req, view(), facetReq, facetName, plan.bucketQueries, weights, *plan.planning, uses);
    configureMeasure(*facet, resolveMeasure(facetReq));
    return facet;
  }

  std::vector<ParsedFilter> prepareFacetSelections(
      const luxir::api::TopDocs& topDocsReq,
      ParseContext& parseContext,
      ProtobufQueryParser& parser,
      bool refinedResultRequested) {
    auto children = lastWins(topDocsReq.ops);
    std::sort(children.begin(), children.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });
    auto selectionHasConsumer = [&](
        std::span<const std::string_view> exceptOps) {
      if (refinedResultRequested) return true;
      return std::ranges::any_of(children, [&](const auto& child) {
        return std::ranges::find(exceptOps, child.first) == exceptOps.end();
      });
    };

    std::vector<ParsedFilter> derived;
    for (const auto& [key, childView] : children) {
      const api::SearchOp& child = **childView;
      const auto* fieldProto = std::get_if<api::FieldFacet>(&child.kind);
      const auto* rangeProto = std::get_if<api::RangeFacet>(&child.kind);
      const auto* queryProto = std::get_if<api::QueryFacet>(&child.kind);
      if (fieldProto == nullptr && rangeProto == nullptr
          && queryProto == nullptr) continue;
      if (child.domain && movesView(*child.domain)) {
        const auto& selected = fieldProto ? fieldProto->selected
            : rangeProto ? rangeProto->selected : queryProto->selected;
        if (selected && !ValueSequence(*selected, "facet '" + std::string(key) + "': selected").empty()) {
          throw RequestError("facet '" + std::string(key)
              + "': selected requires a TopDocs owner in the same space; transform the owning list's domain");
        }
        continue;
      }

      auto inherited = options;
      options = inherited.child(child, key);
      auto inheritedCoercion = parseContext.coerceContext;
      parseContext.coerceContext.dateInputUnit = RequestFlags::dateInputUnit(options.flags);
      parseContext.coerceContext.decimalScaled = (options.flags & RequestFlags::DECIMAL_SCALED) != 0;
      auto restoreOptions = scope_guard([&]() noexcept {
        options = inherited;
        parseContext.coerceContext = inheritedCoercion;
      });
      constexpr auto placement = OpsPlacement::TOP_DOCS;
      if (fieldProto != nullptr) {
        validateSelectionEnvelope(key, *fieldProto, placement);
        std::string valueName = "facet '" + std::string(key)
            + "': selected";
        ValueSequence selected = fieldProto->selected.has_value()
            ? ValueSequence(*fieldProto->selected, valueName,
                            ValueSequence::NullError::MUST_NOT_BE_NULL)
            : ValueSequence();
        if (selected.empty()) continue;

        QueryBuilder builder(req.requestPool, *req.schema,
                             parseContext.coerceContext, key, &req.warnings, &parseView.fields);
        CanonicalValueSet canonical = builder.canonicalizeFieldValues(
            parseView.fields.resolveLocal(fieldProto->field, OpClass::VALUE), selected, true);
        FacetReq* facet = createFieldFacetReq(
            key, *fieldProto, OpsPlacement::TOP_DOCS, &canonical);
        preparedFacets.emplace(
            &child, FacetParsePlan{
                .execution = facet,
                .bucketQueries = {},
                .selectedBuckets = {}});

        std::span<const std::string_view> exceptOps;
        if (fieldProto->selection_mode == api::SelectionMode::ANY) {
          auto except = req.requestPool.make_span<std::string_view>(1);
          except[0] = key;
          exceptOps = except;
        }
        if (!selectionHasConsumer(exceptOps)) {
          skipCount(SkipStats::selectionRefinersElided);
          continue;
        }

        auto clauses = req.requestPool.make_span<Query*>(canonical.size());
        for (size_t i = 0; i < clauses.size(); i++) {
          clauses[i] = builder.createAnyOfQuery(canonical.element(i));
          skipCount(SkipStats::selectionValueFiltersPlanned);
        }
        if (fieldProto->selection_mode == api::SelectionMode::ANY) {
          Query* predicate = combineAnySelectionPredicates(clauses);
          derived.push_back({
              predicate, exceptOps,
              clauses.size() == 1 ? std::span<Query* const>{} : clauses});
          skipCount(SkipStats::selectionLogicalFiltersPlanned);
        } else {
          for (Query* clause : clauses) {
            derived.push_back({clause, exceptOps});
            skipCount(SkipStats::selectionLogicalFiltersPlanned);
          }
        }
        continue;
      }

      if (rangeProto != nullptr) {
        validateSelectionEnvelope(key, *rangeProto, placement);
        std::string valueName = "facet '" + std::string(key)
            + "': selected";
        ValueSequence selected = rangeProto->selected.has_value()
            ? ValueSequence(*rangeProto->selected, valueName,
                            ValueSequence::NullError::MUST_NOT_BE_NULL)
            : ValueSequence();
        if (selected.empty()) continue;
        FacetReq* facet = createRangeFacetReq(
            key, *rangeProto, OpsPlacement::TOP_DOCS);
        preparedFacets.emplace(
            &child, FacetParsePlan{
                .execution = facet,
                .bucketQueries = {},
                .selectedBuckets = {}});
        auto& range = static_cast<IntFacetRangeReq&>(*facet);
        auto indexes = range.selectedBucketIndexes();
        auto fences = range.bucketFences();
        std::span<const std::string_view> exceptOps;
        if (rangeProto->selection_mode == api::SelectionMode::ANY) {
          auto except = req.requestPool.make_span<std::string_view>(1);
          except[0] = key;
          exceptOps = except;
        }
        if (!selectionHasConsumer(exceptOps)) {
          skipCount(SkipStats::selectionRefinersElided);
          continue;
        }

        auto clauses = req.requestPool.make_span<Query*>(indexes.size());
        for (size_t i = 0; i < indexes.size(); i++) {
          size_t bucket = indexes[i];
          clauses[i] = req.requestPool.make<NumericPredicateQuery>(
              range.fieldName, fences[bucket], fences[bucket + 1] - 1);
          skipCount(SkipStats::selectionValueFiltersPlanned);
        }
        if (rangeProto->selection_mode == api::SelectionMode::ANY) {
          Query* predicate = combineAnySelectionPredicates(clauses);
          derived.push_back({
              predicate, exceptOps,
              clauses.size() == 1 ? std::span<Query* const>{} : clauses});
          skipCount(SkipStats::selectionLogicalFiltersPlanned);
        } else {
          for (Query* clause : clauses) {
            derived.push_back({clause, exceptOps});
            skipCount(SkipStats::selectionLogicalFiltersPlanned);
          }
        }
        continue;
      }

      FacetParsePlan plan = createQueryFacetPlan(
          key, *queryProto, placement, parser);
      auto [prepared, inserted] = preparedFacets.emplace(&child, plan);
      assert(inserted);
      if (plan.selectedBuckets.empty()) continue;
      std::span<const std::string_view> exceptOps;
      if (queryProto->selection_mode == api::SelectionMode::ANY) {
        auto except = req.requestPool.make_span<std::string_view>(1);
        except[0] = key;
        exceptOps = except;
      }
      if (!selectionHasConsumer(exceptOps)) {
        prepared->second.refinerPlanned = false;
        skipCount(SkipStats::selectionRefinersElided);
        continue;
      }
      auto clauses = req.requestPool.make_span<Query*>(
          plan.selectedBuckets.size());
      for (size_t i = 0; i < clauses.size(); i++) {
        clauses[i] = plan.bucketQueries[plan.selectedBuckets[i]];
        skipCount(SkipStats::selectionValueFiltersPlanned);
      }
      if (queryProto->selection_mode == api::SelectionMode::ANY) {
        Query* predicate = combineAnySelectionPredicates(clauses);
        derived.push_back({
            predicate, exceptOps,
            clauses.size() == 1 ? std::span<Query* const>{} : clauses});
        skipCount(SkipStats::selectionLogicalFiltersPlanned);
      } else {
        for (Query* clause : clauses) {
          derived.push_back({clause, exceptOps});
          skipCount(SkipStats::selectionLogicalFiltersPlanned);
        }
      }
    }
    return derived;
  }

  std::span<ParsedFilter> appendDerivedFilters(
      std::span<ParsedFilter> explicitFilters,
      std::vector<ParsedFilter> derived) {
    if (derived.empty()) return explicitFilters;
    auto filters = req.requestPool.make_span<ParsedFilter>(
        explicitFilters.size() + derived.size());
    std::copy(explicitFilters.begin(), explicitFilters.end(), filters.begin());
    std::move(derived.begin(), derived.end(),
              filters.begin() + explicitFilters.size());
    return filters;
  }

  Query* descendantClause(Query* query, std::string& path) {
    if (query->getKind() == QueryKind::KNN) {
      path = ((KnnQuery*)query)->getSourcePath();
      return query;
    }
    if (query->getKind() == QueryKind::TO_PARENT) {
      auto& join = *(ToParentQuery*)query;
      path = join.sourcePath();
      return join.getChild();
    }
    if (query->getKind() == QueryKind::BOOST) {
      auto& boost = *(BoostQuery*)query;
      auto* child = descendantClause(boost.getChild(), path);
      return req.requestPool.make<BoostQuery>(child, boost.getBoost());
    }
    if (query->getKind() == QueryKind::CONSTANT_SCORE) {
      auto& constant = *(ConstantScoreQuery*)query;
      auto* child = descendantClause(constant.getChild(), path);
      return req.requestPool.make<ConstantScoreQuery>(child, constant.getScoreProfile());
    }
    return query;
  }

  void parseRowOps(TopDocsReq& owner, const api::TopDocs& list, int depth) {
    if (list.row_ops.empty()) return;
    if (list.collapse || list.diversify) throw RequestError("row_ops on collapsed or diversified lists are not supported yet");
    if (depth >= maxOpDepth) throw RequestError("row_ops exceeds the operation nesting limit");
    auto returned = resolveReturnFields(req, *view(), list.fields);
    for (auto& [name, wire] : lastWins(list.row_ops)) {
      validateName(name, "row operation");
      if (name.starts_with('_')) throw RequestError("reserved row operation name '" + std::string(name) + "'");
      for (const auto& field : returned) if (field.outputKey == name) {
        throw RequestError("row operation '" + std::string(name) + "' conflicts with a returned field");
      }
      for (const auto& decorator : owner.decorators) if (decorator->columnName() == name) {
        throw RequestError("row operation '" + std::string(name) + "' conflicts with a decorator");
      }
      const auto& spec = **wire;
      auto* top = std::get_if<api::TopDocs>(&spec.kind);
      if (!top) throw RequestError("row operation '" + std::string(name) + "': this operation kind is not supported yet");
      if (!top->row_ops.empty() || !top->ops.empty() || top->collapse || top->diversify || !top->decorate.empty()) {
        throw RequestError("row operation '" + std::string(name) + "': nested operations, collapse, diversify and decorators are not supported yet");
      }
      auto inherited = options;
      options = options.child(spec, name);
      auto restore = scope_guard([&]() noexcept { options = inherited; });
      std::string path(view()->path());
      Query* selectedRows = nullptr;
      if (spec.domain) {
        const auto& domain = *spec.domain;
        if (domain.query) throw RequestError("row operation '" + std::string(name) + "': domain.query is not supported yet");
        if (domain.apply_parent_filters) throw RequestError("row operation '" + std::string(name) + "': apply_parent_filters requires domain.query");
        if (domain.from_match) {
          auto clauseName = *domain.from_match;
          auto found = namedClauses.find(clauseName);
          auto fail = [&](std::string_view why) {
            throw RequestError("domain.from_match '" + std::string(clauseName) + "': " + std::string(why));
          };
          if (domain.to) fail("cannot be combined with domain.to");
          if (found == namedClauses.end()) fail("unknown clause name");
          const auto& clause = found->second;
          if (clause.list != &list) fail("the clause is not part of this list's query or filters");
          if (clause.prohibited) fail("a prohibited clause has no matched rows");
          path = clause.path;
          selectedRows = descendantClause(clause.query, path);
          auto base = view()->path();
          if (path.empty() || path == base || (!base.empty() && !path.starts_with(std::string(base) + '.'))) {
            fail("the clause does not select descendant rows");
          }
          if (logicalConsumesDomain(*selectedRows)) fail("kNN and other domain-consuming clauses cannot be replayed per row");
        } else if (domain.to) path = domainPath(*domain.to);
      }
      auto scope = parseView.scoped(path);
      auto* copy = arenaCreate<api::TopDocs>(req.arena, *top);
      if (spec.domain && !spec.domain->filter.empty()) {
        auto filters = req.requestPool.make_span<api::Filter>(top->filter.size() + spec.domain->filter.size());
        std::copy(top->filter.begin(), top->filter.end(), filters.begin());
        for (size_t i = 0; i < spec.domain->filter.size(); i++) {
          auto* query = arenaCreate<api::Query>(req.arena, spec.domain->filter[i]);
          filters[top->filter.size() + i].query = query;
        }
        copy->filter = filters;
      }
      auto* row = parseTopDocs(name, *copy, TopDocsPlacement::ROW_OP, selectedRows);
      if (row->weight->needsPrepare() || !row->filterWeights.empty()) {
        throw RequestError("row operation '" + std::string(name) + "': prepared queries and routed filters are not supported yet");
      }
      row->rowFields = resolveReturnFields(req, *row->view, top->fields);
      resolveReturnFieldMetadata(*row->view, row->rowFields);
      row->parent = &owner;
      owner.rowOps.push_back(row);
    }
  }

  // Build a TopDocsReq from a TopDocs proto. Placement is semantic: ordinary
  // TopDocs can serve a canonical-root whole-reader fact, while a Fusion
  // source delivers candidates into a parent-owned domain.
  TopDocsReq* parseTopDocs(std::string_view name,
                           const luxir::api::TopDocs& topDocsReq,
                           TopDocsPlacement placement, Query* selectedRows = nullptr) {
    int64_t countLimit = topDocsReq.count_limit.value_or(0);
    if (countLimit < -2) {
      throw std::runtime_error(
          "top_docs '" + std::string(name) + "': count_limit must be >= -2");
    }
    // An estimate is a bounded count with an estimator behind it. N = 1 holds
    // pruning only until the first hit, and the bounded routes bring the rest:
    // exact when free or when ranking never pruned, and exact on any list that
    // cannot bound its count. Fusion sources report no count of their own.
    bool estimateCount = placement != TopDocsPlacement::FUSION_SOURCE
        && (countLimit == -2
            || ((options.flags & RequestFlags::ESTIMATE_COUNT) != 0
                && (!topDocsReq.count_limit.has_value() || countLimit > 0)));
    if (countLimit == -2 || (estimateCount && countLimit == 0)) countLimit = 1;
    // Place the Query in the requestPool since it uses things like string_view that directly reference
    // the request.
    DiversifyPlan* diversify = nullptr;
    if (topDocsReq.diversify) {
      auto keys = diversifyKeySpec(topDocsReq);
      auto* resolved = resolveCollapse(keys,
          placement != TopDocsPlacement::FUSION_SOURCE && countLimit != 0);
      diversify = luxir::arenaCreate<DiversifyPlan>(req.arena,
          *resolved, (size_t)*topDocsReq.diversify->max_per_key, req.memoryTracker);
    }
    CollapsePlan* collapse = nullptr;
    if (topDocsReq.collapse) {
      if (placement == TopDocsPlacement::FUSION_SOURCE) {
        const auto& spec = *topDocsReq.collapse;
        if (!spec.head_sort.empty() || !spec.sort.empty() || spec.get_count || !spec.ops.empty()) {
          throw RequestError("fusion source '" + std::string(name)
              + "': collapse supports only field and missing; head_sort, sort, get_count and ops"
                " require source finalization");
        }
      }
      collapse = resolveCollapse(*topDocsReq.collapse,
          placement != TopDocsPlacement::FUSION_SOURCE && countLimit != 0);
      enclosingCollapse = collapse;
      // Facets under a collapsed list count groups unless they say otherwise.
      inheritedFacetMeasure = collapse;
    }
    auto parseContext = createParseContext(name);
    parseContext.list = &topDocsReq;
    ProtobufQueryParser parser(parseContext);
    // An absent query selects all documents: the domain is then whatever the
    // filters carve out (browse / filter-only search). Boolean normalization
    // eliminates the match-all when filters fold in beside it.
    Query* query = topDocsReq.query.has_value()
      ? parser.parse(*topDocsReq.query)
      : selectedRows ? selectedRows : req.requestPool.make<AllQuery>();
    Query* resultQuery = topDocsReq.query || selectedRows ? query : nullptr;
    // A minimum score turns the query's score into part of its membership,
    // so the cut-off wraps the query before filters fold in beside it: every
    // ranking, count, and ops-domain route below then sees the same matches.
    if (topDocsReq.min_score.has_value()) {
      if (!std::isfinite(*topDocsReq.min_score)) {
        throw std::runtime_error(
            "top_docs '" + std::string(name) + "': min_score must be finite");
      }
      query = req.requestPool.make<MinScoreQuery>(
          query, *topDocsReq.min_score);
    }
    Query* domainQuery = query;
    int64_t offset = topDocsReq.offset;
    if (offset < 0) {
      throw std::runtime_error(
          "top_docs '" + std::string(name) + "': offset must be >= 0");
    }
    int64_t specifiedLimit = topDocsReq.limit.has_value() ? *topDocsReq.limit : 10;
    // limit to actual number of docs in the index (or all if limit == -1)
    int64_t maxDoc = view()->maxDoc();
    int64_t limit = specifiedLimit < 0 ? maxDoc : std::min(specifiedLimit, maxDoc);
    // Keep limit as the page size for request-shape decisions. Fusion source
    // offsets are response-shape fields and do not enlarge candidate pools.
    int64_t topCount = limit;
    if (limit > 0 && placement != TopDocsPlacement::FUSION_SOURCE) {
      topCount = offset >= maxDoc ? maxDoc : offset + std::min(limit, maxDoc - offset);
    }

    if (placement == TopDocsPlacement::FUSION_SOURCE) {
      if (!topDocsReq.row_ops.empty()) throw RequestError("row_ops on fusion sources are not supported yet");
      if (!topDocsReq.ops.empty()) {
        throw std::runtime_error(
            "fusion source '" + std::string(name)
            + "': ops are not executed under Fusion; put them in Fusion.ops");
      }
      for (size_t i = 0; i < topDocsReq.filter.size(); i++) {
        if (!topDocsReq.filter[i].except_ops.empty()) {
          throw std::runtime_error(
              "fusion source '" + std::string(name) + "'.filter["
              + std::to_string(i)
              + "].except_ops: filter routing has no target on a fusion source");
        }
      }
    }
    auto explicitFilters = parseFilters(
        parser, topDocsReq.filter, &topDocsReq.ops, "top_docs.filter");
    if (selectedRows && topDocsReq.query) {
      auto all = req.requestPool.make_span<ParsedFilter>(explicitFilters.size() + 1);
      std::copy(explicitFilters.begin(), explicitFilters.end(), all.begin());
      all.back() = {selectedRows, {}};
      explicitFilters = all;
    }
    parseContext.list = nullptr;
    auto filters = appendDerivedFilters(
        explicitFilters,
        prepareFacetSelections(
            topDocsReq, parseContext, parser,
            specifiedLimit != 0 || countLimit != 0));
    ParsedDomains parsedDomains = parseChildDomains(topDocsReq.ops, parseContext);

    DomainVariantPlan domainVariants;
    bool routedFilterSyntax = std::any_of(
        filters.begin(), filters.end(), [](const ParsedFilter& filter) {
          return !filter.exceptOps.empty();
        });
    bool routedFilters = routedFilterSyntax || !parsedDomains.empty();
    if (routedFilterSyntax && parsedDomains.empty()) {
      domainVariants.begin(filters.size());
      addDomainVariants(
          topDocsReq.ops, filters, parsedDomains, nullptr,
          domainVariants);
    }

    // By default TopDocs filters are ordinary Boolean filter clauses, so one
    // query tree owns both matching and preparation. Keep the routed filter
    // metadata on TopDocsReq; the toggle preserves the former passive domain
    // path as the benchmark baseline.
    CollectionRequirements requirements{
      .needRankedDocs = limit > 0,
      .countLimit = countLimit,
      .needExactDomain = !topDocsReq.ops.empty()
    };
    bool countClauseDisabled =
        BooleanQuery::disableFilterClauseCountForTests
        && limit == 0 && requirements.needsCount();
    bool foldFilters = !routedFilters && !filters.empty()
        && !disableTopDocsFilterFold
        && !countClauseDisabled;
    if (foldFilters) {
      auto mandatory = req.requestPool.make_span<Query*>(1);
      mandatory[0] = query;
      auto filterClauses = req.requestPool.make_span<Query*>(filters.size());
      for (size_t i = 0; i < filters.size(); i++) {
        filterClauses[i] = filters[i].query;
      }
      query = req.requestPool.make<BooleanQuery>(
        mandatory, std::span<Query*>{}, std::span<Query*>{}, filterClauses);
    }
    // The test-only wrapper must cover the complete effective query. This
    // also leaves the Boolean tree visible to normalization before wrapping.
    if (req.testForcePrepare) {
      query = req.requestPool.make<ForcePrepareQuery>(query);
      domainQuery = req.requestPool.make<ForcePrepareQuery>(domainQuery);
    }

    auto parsedSorts = parseSorts(topDocsReq.sort);
    std::optional<SortPlan> headSort;
    if (topDocsReq.collapse && !topDocsReq.collapse->head_sort.empty()) {
      headSort = parseSorts(topDocsReq.collapse->head_sort);
    }
    auto* planningContext = createPlanningContext();
    // One unconditional serial pass validates the complete effective query,
    // including score decorations that an unscored Weight would not consume.
    // Folded filters are already children of query; passive filters remain
    // separate roots and must be visited explicitly.
    query->validateLogical(*planningContext);
    if (domainQuery != query && req.testForcePrepare) {
      // The test-only domain product has its own ForcePrepareQuery root. Its
      // child is already in the effective tree, but the wrapper itself is a
      // separate createWeight root and must make the same explicit visit.
      domainQuery->validateLogical(*planningContext);
    }
    if (!foldFilters) {
      for (const auto& filter : filters) {
        for (size_t i = 0; i < filter.sourceCount(); i++) {
          filter.source(i)->validateLogical(*planningContext);
        }
      }
    }
    validateUnselectedQueryFacetQueries(
        topDocsReq.ops, *planningContext);
    validateDomainQueries(parsedDomains, *planningContext);
    if (!parsedDomains.empty()) {
      domainVariants.begin(filters.size());
      addDomainVariants(
          topDocsReq.ops, filters, parsedDomains, planningContext,
          domainVariants);
      routedFilters = !domainVariants.empty();
    }

    // Flags for this request's main query. Filters inherit these after
    // buildFilterWeights clears NEED_SCORES.
    //
    // NEED_SCORES is purely computational (fuzzy match semantics no longer
    // hang off it - the expansion cap is a property of the query).  A
    // count-/domain-only request (limit 0) reads no score, so
    // it skips norms/impacts/BM25 and lets boolean prep pick non-scoring
    // iterators. Ranked requests need scores only when their normalized sort
    // plan consumes score (default ranking, SCORE, or a score-dependent EXPR).
    int32_t requestFlags = 0;
    if (limit > 0 && (placement != TopDocsPlacement::ROW_OP || topDocsReq.query || selectedRows)
        && (topDocsReq.get_scores || parsedSorts.rankNeedsScores
            || (headSort && headSort->rankNeedsScores))) {
      requestFlags |= Query::NEED_SCORES;
    }
    // Document cardinality is not a distinct-group count; collapse totals
    // read every eligible key through the normal exhaustive driver.
    bool exactCountTopKShape = collapse == nullptr && diversify == nullptr
        && requirements.needRankedDocs
        && requirements.needsCount() && !requirements.needExactDomain
        && !parsedSorts.useFieldSort
        && parsedSorts.rankNeedsScores;
    // A bounded root list uses an exact count when the whole-membership plan
    // supplies it from metadata or a resident entry. Otherwise collection
    // holds the competitive threshold at lowest until countLimit hits.
    bool boundedCount = exactCountTopKShape
        && placement == TopDocsPlacement::ROOT_OP && requirements.isBounded();
    // Competitive-score pruning requires a score-ranked heap. Sub-ops permit
    // pruning because a separate exhaustive windowed pass produces their
    // complete match domain and exact count before the ranking pass. Match-all
    // reuses the incoming domain and its cardinality instead.
    bool allowPruning = requirements.needRankedDocs
        && !parsedSorts.useFieldSort
        && (!requirements.needsCount() || requirements.needExactDomain || boundedCount);
    if (allowPruning && !(diversify && requirements.needsCount())
        && !(collapse && !topDocsReq.collapse->sort.empty())) {
      requestFlags |= Query::ALLOW_PRUNING;
    }
    Query::ScoreProfile scoreProfile = luxir::scoreProfile(*query);
    bool exactCountTopK = exactCountTopKShape && !boundedCount;
    bool pureCountShape = placement != TopDocsPlacement::ROW_OP
        && collapse == nullptr && diversify == nullptr
        && !QueryPrep::disableWholeMembershipPlanForTests
        && limit == 0 && requirements.needsCount()
        && !requirements.needExactDomain && !parsedSorts.useFieldSort
        && (foldFilters || filters.empty());
    FilterCache::Use* cacheFirstMembershipUse = nullptr;
    auto* cache = view()->filterCache();
    auto wholeCountWork = [&] {
      return !QueryPrep::disableCanonicalCountFactsForTests
          && planningContext->filterScope(*query) <= FilterKeyScope::CORE_STABLE
          ? FilterCache::Work::nanoseconds(0) : FilterCache::Work::assumed();
    };
    auto lookupWholeCount = [&](bool needsIds = false) {
      bool scalar = !needsIds && !QueryPrep::disableCanonicalCountFactsForTests
          && planningContext->filterScope(*query) <= FilterKeyScope::CORE_STABLE
          && std::any_of(view()->segments().begin(), view()->segments().end(),
                         [](auto& segment) { return !segment.hasDeletes(); });
      if (scalar && (placement != TopDocsPlacement::ROOT_OP
          || std::any_of(view()->segments().begin(), view()->segments().end(),
                         [](auto& segment) { return segment.hasDeletes(); }))) {
        return std::optional<FilterCache::ExistingCandidate>{};
      }
      return planningContext->lookupExistingFilterUse(
          *query, placement == TopDocsPlacement::ROOT_OP, scalar);
    };
    if (pureCountShape && canOmitWeightForCacheFirstMembership(*query)
        && !directCountAvailable(*query, *view())
        && cache != nullptr && cache->enabled()) {
      auto candidate = lookupWholeCount();
      if (candidate.has_value()) {
        cacheFirstMembershipUse = planningContext->acceptExistingFilterUse(
            std::move(*candidate), FilterCache::AdmissionLane::WHOLE, wholeCountWork());
      }
      if (cacheFirstMembershipUse != nullptr) {
        skipCount(SkipStats::cacheFirstMembershipWeightSkips);
      }
    }

    bool cacheFirstTopKCount = false;
    if (cacheFirstMembershipUse == nullptr
        && placement == TopDocsPlacement::ROOT_OP
        && !QueryPrep::disableWholeMembershipPlanForTests
        && exactCountTopKShape && (foldFilters || filters.empty())
        && canOmitWeightForCacheFirstMembership(*query)
        && !directCountAvailable(*query, *view())
        && cache != nullptr && cache->enabled()) {
      auto candidate = lookupWholeCount(scoreProfile.kind != Query::ScoreProfile::Kind::VARIABLE);
      if (candidate.has_value()) {
        cacheFirstMembershipUse = planningContext->acceptExistingFilterUse(
            std::move(*candidate), FilterCache::AdmissionLane::WHOLE, wholeCountWork());
      }
      cacheFirstTopKCount = cacheFirstMembershipUse != nullptr;
      if (cacheFirstTopKCount) {
        skipCount(SkipStats::cacheFirstTopKCountWeightSkips);
        if (scoreProfile.kind != Query::ScoreProfile::Kind::VARIABLE) {
          skipCount(SkipStats::cacheFirstConstantTopKWeightSkips);
        }
      }
    }

    std::span<FilterCache::Use*> residentExactDomainUses;
    bool cacheFirstExactDomain = false;
    bool constantExactDomainRanking = requirements.needRankedDocs
        && scoreProfile.kind != Query::ScoreProfile::Kind::VARIABLE;
    bool canOmitForExactDomain = collapse == nullptr && diversify == nullptr && !routedFilters
        && placement == TopDocsPlacement::ROOT_OP
        && requirements.needExactDomain && !parsedSorts.useFieldSort
        && (!requirements.needRankedDocs || constantExactDomainRanking)
        && cache != nullptr && cache->enabled();
    if (canOmitForExactDomain) {
      bool domainIdentity = exactDomainIdentity(*domainQuery);
      auto sourceQueries = req.requestPool.make_span<Query*>(
          (size_t)!domainIdentity + filters.size());
      size_t sourceIndex = 0;
      bool allOmittable = true;
      if (!domainIdentity) {
        sourceQueries[sourceIndex++] = domainQuery;
        allOmittable =
            canOmitWeightForCacheFirstMembership(*domainQuery);
      }
      for (size_t i = 0; i < filters.size(); i++) {
        sourceQueries[sourceIndex++] = filters[i].query;
        allOmittable &=
            canOmitWeightForCacheFirstMembership(*filters[i].query);
      }
      if (allOmittable && !sourceQueries.empty()) {
        bool allowReaderStable = sourceQueries.size() == 1;
        residentExactDomainUses = planningContext->acceptExistingFilterUses(
            sourceQueries, allowReaderStable,
            FilterCache::AdmissionLane::CLAUSE);
        cacheFirstExactDomain = !residentExactDomainUses.empty();
      }
      if (cacheFirstExactDomain) {
        skipCount(SkipStats::cacheFirstExactDomainWeightSkips);
        if (constantExactDomainRanking) {
          skipCount(SkipStats::cacheFirstConstantTopKWeightSkips);
        }
      }
    }

    bool cacheFirstFieldSort = false;
    bool cacheFirstFieldSortShape =
        collapse == nullptr && diversify == nullptr && placement == TopDocsPlacement::ROOT_OP
        && !QueryPrep::disableWholeMembershipPlanForTests
        && limit > 0 && parsedSorts.useFieldSort
        && (requestFlags & Query::NEED_SCORES) == 0
        && !requirements.needExactDomain
        && (foldFilters || filters.empty())
        && canOmitWeightForCacheFirstMembership(*query)
        && cache != nullptr && cache->enabled();
    if (cacheFirstFieldSortShape) {
      cacheFirstMembershipUse = TopDocsReq::planCacheFirstFieldSortWholeMembership(
          *query, *planningContext, true);
      cacheFirstFieldSort = cacheFirstMembershipUse != nullptr;
      if (cacheFirstFieldSort) {
        skipCount(SkipStats::cacheFirstFieldSortWeightSkips);
      }
    }

    bool omitMainWeight = (cacheFirstMembershipUse != nullptr
            && (!cacheFirstTopKCount
                || scoreProfile.kind
                    != Query::ScoreProfile::Kind::VARIABLE))
        || cacheFirstExactDomain || cacheFirstFieldSort;
    bool cacheFirstVariableRanking = cacheFirstTopKCount
        && scoreProfile.kind == Query::ScoreProfile::Kind::VARIABLE;
    int32_t mainWeightFlags = cacheFirstVariableRanking
        ? requestFlags | Query::NEED_SCORES | Query::ALLOW_PRUNING
        : requestFlags;
    Query::Context* qcontext = nullptr;
    if (omitMainWeight) {
      skipCount(SkipStats::cacheFirstQueryContextsOmitted);
    } else {
      qcontext = Query::Context::create(&req.arena, *planningContext);
      skipCount(SkipStats::queryContextsCreated);
    }
    auto* weight = omitMainWeight
        ? nullptr : query->createWeight(*qcontext, mainWeightFlags);
    Query::Weight* variantMembershipWeight = nullptr;
    if (routedFilters) {
      assert(weight != nullptr);
      int32_t membershipFlags =
          requestFlags & ~(Query::NEED_SCORES | Query::ALLOW_PRUNING);
      variantMembershipWeight =
          !weight->needsScores() && !weight->allowsPruning()
              ? weight
              : query->createWeight(*qcontext, membershipFlags);
    }
    if (cacheFirstTopKCount && weight != nullptr) {
      skipCount(SkipStats::cacheFirstTopKRankingWeights);
    }
    bool sparseFilteredTopKReroute = weight != nullptr
        && (allowPruning || cacheFirstVariableRanking) && foldFilters
        && !requirements.needExactDomain && !weight->needsPrepare()
        && TopDocsReq::admitSparseFilteredTopK(
            *weight, *view(), topCount);
    if (sparseFilteredTopKReroute) {
      bool unionFamily = weight->sparseFilteredTopKFamily()
          == Query::Weight::SparseFilteredTopKFamily::UNION;
      mainWeightFlags &= ~Query::ALLOW_PRUNING;
      weight = query->createWeight(*qcontext, mainWeightFlags);
      requestFlags &= ~Query::ALLOW_PRUNING;
      skipCount(SkipStats::sparseFilteredTopKReroutes);
      if (unionFamily) {
        skipCount(SkipStats::sparseFilteredTopKUnionReroutes);
      }
    }
    Query::Weight* countWeight = nullptr;
    Query::Weight* rankingWeight = nullptr;
    bool newComposition = exactCountTopK && weight != nullptr && !weight->canComposeExactCountTopK();
    bool legalComposition = newComposition && topCount <= TopDocsReq::kExactCountTopKMaxDepth
        && !weight->needsPrepare() && !weight->consumesDomain() && !weight->isConstantScoring();
    bool forceComposition = TopDocsReq::forceExactCountTopKCompositionForTests && newComposition;
    bool forcedComposition = forceComposition && legalComposition;
    if (forceComposition && !forcedComposition) skipCount(SkipStats::forcedExactCountTopKCapabilityRejects);
    std::span<uint8_t> provedSegments;
    if (legalComposition && !forcedComposition && !disableTopKCountComposition
        && (foldFilters || filters.empty())
        && !cacheFirstTopKCount && !TopDocsReq::disableProvedDensityCompositionForTests) {
      for (auto& segment : view()->segments()) {
        auto bounds = weight->countShortcut(segment, nullptr);
        int32_t inverse = TopDocsReq::provedDensityCompositionInverseForTests;
        if (inverse <= 0 || bounds.minimum() <= 0
            || bounds.minimum() * inverse < segment.maxDoc()
            || !weight->mayRankScoresExactly(segment)) continue;
        if (provedSegments.empty()) {
          provedSegments = qcontext->pool.make_span<uint8_t>(view()->segments().size());
          std::fill(provedSegments.begin(), provedSegments.end(), 0);
        }
        provedSegments[(size_t)segment.ord] = 1;
      }
    }
    if (weight != nullptr && !cacheFirstTopKCount
        && !disableTopKCountComposition && exactCountTopK
        && !weight->needsPrepare()
        && (weight->canComposeExactCountTopK() || forcedComposition || !provedSegments.empty())
        // A constant-score ranking pass is bounded only when no collector
        // filter can reject its first K matches. Variable-score composition
        // retains its existing filtered candidate route.
        && (filters.empty() || !weight->isConstantScoring())) {
      int32_t countFlags =
          requestFlags & ~(Query::NEED_SCORES | Query::ALLOW_PRUNING);
      int32_t rankingFlags =
          requestFlags | Query::NEED_SCORES | Query::ALLOW_PRUNING;
      countWeight = query->createWeight(*qcontext, countFlags);
      rankingWeight = query->createWeight(*qcontext, rankingFlags);
    }
    auto filterWeights = foldFilters || cacheFirstExactDomain || filters.empty()
      ? std::span<Query::Weight*>{}
      : buildFilterWeights(filters, *qcontext, requestFlags);
    if (domainVariants.sourceCount() > 0) {
      domainVariants.buildWeights(
          *qcontext, *planningContext, requestFlags);
    }
    Query::Weight* domainQueryWeight = nullptr;
    std::span<Query::Weight*> domainFilterWeights;
    if (!routedFilters && !cacheFirstExactDomain && requirements.needExactDomain
        && (!requirements.needRankedDocs
            || (constantExactDomainRanking
                && !parsedSorts.useFieldSort))) {
      if (foldFilters || requirements.needRankedDocs) {
        int32_t domainFlags =
            requestFlags & ~(Query::NEED_SCORES | Query::ALLOW_PRUNING);
        domainQueryWeight =
            domainQuery->createWeight(*qcontext, domainFlags);
        domainFilterWeights =
            buildFilterWeights(filters, *qcontext, domainFlags);
      } else {
        domainQueryWeight = weight;
        domainFilterWeights = filterWeights;
      }
    }

    Query::Weight* wholeMembershipWeight = nullptr;
    Query::Weight* wholeRankingWeight = nullptr;
    FilterCache::Use* wholeMembershipUse = nullptr;
    bool wholeConstantRanking = false;
    bool pureCount = pureCountShape && filterWeights.empty();
    bool wholeTopKCount = !QueryPrep::disableWholeMembershipPlanForTests
        && exactCountTopKShape
        && (cacheFirstTopKCount
            || (weight != nullptr && !weight->needsPrepare()))
        && filterWeights.empty();
    bool wholeFieldSort = !QueryPrep::disableWholeMembershipPlanForTests
        && limit > 0 && parsedSorts.useFieldSort
        && (cacheFirstFieldSort
            || (weight != nullptr && !weight->needsScores()))
        && !requirements.needExactDomain && filterWeights.empty();
    // Nested TopDocs under a match-all parent can inherit the canonical
    // reader domain. Preserve the landed probe there and let
    // probeReaderStable enforce the runtime domain gate. Fusion sources have
    // a different ownership shape and remain excluded.
    bool preparedWhole = weight != nullptr && weight->needsPrepare()
        && placement != TopDocsPlacement::FUSION_SOURCE
        && (pureCount || wholeFieldSort);
    if (weight != nullptr && weight->needsPrepare() && !preparedWhole) {
      pureCount = false;
      wholeFieldSort = false;
    }
    if (pureCount || wholeTopKCount || wholeFieldSort) {
      if (cacheFirstMembershipUse != nullptr) {
        wholeMembershipUse = cacheFirstMembershipUse;
      } else if (wholeTopKCount && countWeight != nullptr
          && !countWeight->needsScores()
          && !countWeight->allowsPruning()) {
        wholeMembershipWeight = countWeight;
      } else if (pureCount || wholeFieldSort) {
        wholeMembershipWeight = weight;
      } else {
        int32_t membershipFlags =
            requestFlags & ~(Query::NEED_SCORES | Query::ALLOW_PRUNING);
        wholeMembershipWeight =
            query->createWeight(*qcontext, membershipFlags);
      }
      if (wholeMembershipWeight != nullptr) {
        assert(!wholeMembershipWeight->needsScores());
        assert(!wholeMembershipWeight->allowsPruning());

        bool everySegmentConstant = wholeMembershipWeight->matchesAllDocs();
        if (!everySegmentConstant) {
          everySegmentConstant = true;
          bool everySegmentEmpty = !view()->segments().empty();
          for (auto& segment : view()->segments()) {
            DocSet* rootDomain = segment.liveDocSet();
            auto shortcut = wholeMembershipWeight->countShortcut(segment, rootDomain);
            everySegmentEmpty &= shortcut.how == Query::CountHow::METADATA
                && shortcut.maxMatches == 0;
            if (shortcut.how > (boundedCount
                    ? Query::CountHow::METADATA : Query::COUNT_PROBE_LIMIT)) {
              everySegmentConstant = false;
              break;
            }
          }
          if (!boundedCount && everySegmentEmpty && hasRequiredConjunction(*query)) {
            everySegmentConstant = false;
          }
        }
        if (!boundedCount && !everySegmentConstant && cache != nullptr && cache->enabled()) {
          bool acquireUse = !wholeFieldSort || preparedWhole
              || QueryPrep::WholeMembershipPlan::observesFieldSort(
                  QueryPrep::WholeMembershipConsumer::FIELD_SORT,
                  planningContext->filterScope(*query), wholeMembershipWeight);
          // An accepted reader-stable value replaces the ANN preparation pass.
          // Its bounded kNN membership is also a valid ladder fallback, while
          // the numeric exact-domain driver still prices exact cardinality per
          // segment. Query-canonical prepared sources may publish raw segment
          // membership through the same lane; preparation alone says nothing
          // about key lifetime.
          if (acquireUse) {
            wholeMembershipUse = qcontext->getFilterUse(
                *query, FilterCache::AdmissionLane::WHOLE);
          }
        }
        // Field-sort constants are useful per segment even without a cache.
        // Prepared membership still requires a cache source and its runtime provenance check.
        if (!everySegmentConstant && wholeMembershipUse == nullptr
            && !(wholeFieldSort && !wholeMembershipWeight->needsPrepare())) {
          wholeMembershipWeight = nullptr;
        }
      }

      if (cacheFirstTopKCount && wholeTopKCount
          && scoreProfile.kind == Query::ScoreProfile::Kind::VARIABLE) {
        assert(weight != nullptr);
        wholeRankingWeight = weight;
      } else if (weight != nullptr && wholeTopKCount
          && wholeMembershipWeight != nullptr
          && !weight->isConstantScoring()) {
        Query::Weight* countFreeRankingWeight = boundedCount ? weight : rankingWeight;
        if (countFreeRankingWeight == nullptr) {
          int32_t rankingFlags =
              requestFlags | Query::NEED_SCORES | Query::ALLOW_PRUNING;
          countFreeRankingWeight =
              query->createWeight(*qcontext, rankingFlags);
        }
        bool reroute = !boundedCount && foldFilters
            && !countFreeRankingWeight->needsPrepare()
            && TopDocsReq::admitSparseFilteredTopK(
                *countFreeRankingWeight, *view(), topCount);
        if (reroute) {
          bool unionFamily = countFreeRankingWeight->sparseFilteredTopKFamily()
              == Query::Weight::SparseFilteredTopKFamily::UNION;
          wholeRankingWeight = weight;
          skipCount(SkipStats::sparseFilteredTopKReroutes);
          if (unionFamily) {
            skipCount(SkipStats::sparseFilteredTopKUnionReroutes);
          }
        } else {
          wholeRankingWeight = countFreeRankingWeight;
        }
      }
      wholeConstantRanking = wholeTopKCount && !cacheFirstVariableRanking
          && (weight == nullptr
                  ? scoreProfile.kind
                      != Query::ScoreProfile::Kind::VARIABLE
                  : weight->isConstantScoring());
    }

    auto* qr = luxir::arenaCreate<TopDocsReq>(
      req.arena, req, view(), name, topDocsReq, *planningContext, qcontext,
      query, weight, variantMembershipWeight,
      countWeight, rankingWeight, topCount, std::move(parsedSorts),
      requirements, wholeMembershipWeight, wholeRankingWeight,
      wholeMembershipUse,
      wholeConstantRanking, scoreProfile,
      (requestFlags & Query::NEED_SCORES) != 0,
      filters, filterWeights, std::move(domainVariants),
      domainQuery, domainQueryWeight,
      domainFilterWeights, residentExactDomainUses);

    qr->collapse = collapse;
    qr->diversify = diversify;
    qr->forcedExactCountTopKComposition = forcedComposition && countWeight != nullptr;
    qr->provedDensitySegments = provedSegments;
    qr->boundedCount = boundedCount && wholeMembershipWeight == nullptr
        && wholeMembershipUse == nullptr;
    if (qr->boundedCount && filterWeights.empty()) {
      int64_t minMatches = 0;
      for (auto& segment : view()->segments()) {
        minMatches += weight->countShortcut(segment, nullptr).minimum();
      }
      qr->countLimitProven = minMatches >= requirements.countLimit;
    }
    qr->estimateCount = estimateCount && qr->boundedCount;
    // A bounded count that metadata does not settle is counted apart from
    // ranking: an unscored pass stops once the total passes the limit, and
    // ranking prunes from its first page instead of holding its threshold
    // until the limit is reached. Constant scoring keeps its windowed drain.
    if (qr->boundedCount && !qr->countLimitProven && !qr->estimateCount
        && !disableBoundedCountSplit && weight != nullptr && !weight->needsPrepare()
        && weight->allowsPruning() && !weight->isConstantScoring()) {
      int64_t remaining = requirements.countLimit;
      for (auto& segment : view()->segments()) {
        // Metadata bounds may be loose, but a count that cannot pass the
        // limit would only duplicate the held ranking pass.
        auto bound = weight->countShortcut(segment, nullptr).maxMatches;
        if (bound > remaining) {
          qr->boundedCountWeight = query->createWeight(
              *qcontext, requestFlags & ~(Query::NEED_SCORES | Query::ALLOW_PRUNING));
          break;
        }
        remaining -= bound;
      }
    }
    qr->headSort = std::move(headSort);
    qr->resultQuery = resultQuery;
    if (placement != TopDocsPlacement::FUSION_SOURCE && !topDocsReq.decorate.empty()) {
      DecoratorContext::QuerySource source{qr->resultQuery, decoratorQueryContext(*qr)};
      DecoratorContext dc{req, parseContext, {&source, 1}};
      qr->decorators = HitDecoratorRegistry::global().make(topDocsReq.decorate, dc);
    } else if (placement == TopDocsPlacement::FUSION_SOURCE && !topDocsReq.decorate.empty()) {
      throw RequestError("fusion sources cannot decorate; put decorate on fusion");
    }
    if (firstQuery == nullptr) {
      firstQuery = qr;
    }
    return qr;
  }

  std::span<ParsedFilter> parseFilters(
      ProtobufQueryParser& parser,
      std::span<const luxir::api::Filter> filtersProto,
      const OpsMap* siblingOps, std::string_view pathPrefix) {
    std::span<ParsedFilter> out;
    if (!filtersProto.empty()) {
      out = req.requestPool.make_span<ParsedFilter>(filtersProto.size());
      for (size_t i = 0; i < filtersProto.size(); i++) {
        auto& f = filtersProto[i];
        std::string path = std::string(pathPrefix) + "[" + std::to_string(i) + "]";
        if (!f.query.has_value()
            || std::holds_alternative<std::monostate>(f.query->kind)) {
          throw std::runtime_error(path + ".query requires a query kind");
        }
        if (siblingOps == nullptr && !f.except_ops.empty()) {
          throw std::runtime_error(path
              + ".except_ops is not supported on Fusion filters");
        }
        for (size_t j = 0; j < f.except_ops.size(); j++) {
          std::string_view key = f.except_ops[j];
          if (key.find('/') != std::string_view::npos) {
            throw std::runtime_error(path + ".except_ops[" + std::to_string(j)
                + "] contains '/'; deeper paths are reserved");
          }
          if (!siblingOps->contains(key)) {
            throw std::runtime_error(path + ".except_ops contains unknown sibling op key '"
                + std::string(key) + "'");
          }
          auto prior = f.except_ops.first(j);
          if (std::ranges::find(prior, key) != prior.end()) {
            throw std::runtime_error(path + ".except_ops contains duplicate op key '"
                + std::string(key) + "'");
          }
        }
        out[i] = {parser.parse(*f.query), f.except_ops};
      }
    }
    return out;
  }

  Query::Context& decoratorQueryContext(TopDocsReq& source) {
    if (source.weightContext == nullptr) {
      source.weightContext = Query::Context::create(&req.arena, source.planning);
      skipCount(SkipStats::queryContextsCreated);
    }
    return *source.weightContext;
  }

  // The parser side of a plugin op: query parsing, a planning context of its
  // own, TopDocs sources and nested ops, all bound to this request.
  class PluginOpContextImpl final : public PluginOpContext {
    SearchParserImpl& parser;
    int depth;
    ParseContext context;
    Query::PlanningContext* planningContext = nullptr;
    Query::Context* executionContext = nullptr;

  public:
    PluginOpContextImpl(SearchParserImpl& parser, std::string_view name, const luxir::api::PluginOp& proto, int depth)
      : PluginOpContext(parser.req, parser.view(), name, proto,
                        ParamReader("op '" + std::string(name) + "': " + std::string(proto.name), proto.params)),
        parser(parser), depth(depth),
        context(parser.createParseContext(name)) {
    }

    ParseContext& parseContext() override { return context; }

    Query::PlanningContext& planning() override {
      if (planningContext == nullptr) {
        planningContext = parser.createPlanningContext();
      }
      return *planningContext;
    }

    Query::Context& weightContext() override {
      if (executionContext == nullptr) executionContext = Query::Context::create(&req.arena, planning());
      return *executionContext;
    }

    Query* parseQuery(const luxir::api::Query& query) override {
      ProtobufQueryParser queryParser(context);
      Query* parsed = queryParser.parse(query);
      parsed->validateLogical(planning());
      return parsed;
    }

    Query* parseQuery(const luxir::api::Val& value) override {
      return parseQuery(*readMessage<luxir::api::Query>(value, "query"));
    }

    TopDocsReq* parseSource(std::string_view sourceName, const luxir::api::Val& topDocs) override {
      auto* proto = readMessage<luxir::api::TopDocs>(topDocs, "top_docs source");
      auto* oldCollapse = parser.enclosingCollapse;
      auto* oldMeasure = parser.inheritedFacetMeasure;
      auto restore = scope_guard([&]() noexcept {
        parser.enclosingCollapse = oldCollapse;
        parser.inheritedFacetMeasure = oldMeasure;
      });
      auto* source = parser.parseTopDocs(sourceName, *proto, TopDocsPlacement::FUSION_SOURCE);
      source->flags = parser.options.flags;
      return source;
    }

    void addSubs(SearchOp& op) override {
      parser.addSubs(op, proto.ops, depth, OpsPlacement::PLUGIN);
    }

  private:
    // A param value read as a request message: the same JSON the message's
    // own field accepts. The text and the views it yields live in the arena.
    template <typename M>
    M* readMessage(const luxir::api::Val& value, std::string_view what) {
      std::string json;
      if (!luxir::api::write_json(value, json)) {
        throw RequestError(params.owner + ": cannot serialize a " + std::string(what) + " param");
      }
      ArenaResource mr(&req.arena);
      std::string_view text = luxir::api::build::arenaStr(mr, json);
      auto* message = new (mr.allocate(sizeof(M), alignof(M))) M();
      std::string error;
      if (!luxir::api::read_json(*message, text, mr, &error)) {
        throw RequestError(params.owner + ": invalid " + std::string(what) + ": " + error);
      }
      return message;
    }
  };

  SearchOp* parsePluginOp(std::string_view name, const luxir::api::PluginOp& pluginOp, int depth) {
    const auto& maker = SearchOpRegistry::global().require(pluginOp.name);
    PluginOpContextImpl context(*this, name, pluginOp, depth);
    SearchOp* op;
    try {
      op = maker(context);
    } catch (const ParamError& e) {
      throw RequestError(e.what());
    }
    if (op == nullptr) throw std::logic_error("search op '" + std::string(pluginOp.name) + "' made no operation");
    return op;
  }

  SearchOp* parseFusion(std::string_view name, const luxir::api::Fusion& fusionProto, int depth) {
    int64_t countLimit = fusionProto.count_limit.value_or(0);
    if (countLimit < -2) {
      throw std::runtime_error(fusionPrefix(name) + "count_limit must be >= -2");
    }
    // Fusion cannot estimate, so a request for one counts exactly.
    if (!fusionProto.count_limit.has_value()
        && (options.flags & RequestFlags::ESTIMATE_COUNT) != 0) {
      countLimit = -1;
    }
    auto* oldCollapse = enclosingCollapse;
    auto* oldMeasure = inheritedFacetMeasure;
    auto restore = scope_guard([&]() noexcept {
      enclosingCollapse = oldCollapse;
      inheritedFacetMeasure = oldMeasure;
    });
    CollapsePlan* collapse = nullptr;
    if (fusionProto.collapse) {
      if (!fusionProto.collapse->sort.empty()) {
        throw RequestError("fusion.collapse.sort is not supported");
      }
      collapse = resolveCollapse(*fusionProto.collapse, countLimit != 0);
      enclosingCollapse = collapse;
      inheritedFacetMeasure = collapse;
    }
    int64_t offset = fusionProto.offset;
    if (offset < 0) {
      throw std::runtime_error(fusionPrefix(name) + "offset must be >= 0");
    }
    if (fusionProto.sources.empty()) {
      throw std::runtime_error(fusionPrefix(name) + "requires at least one source");
    }
    FusionOp::Method method = FusionOp::Method::RRF;
    int32_t rrfK = 60;
    if (const auto* rrf = std::get_if<luxir::api::RrfFusion>(&fusionProto.method)) {
      if (rrf->k.has_value() && *rrf->k < 0) {
        throw std::runtime_error(fusionPrefix(name) + "RrfFusion.k must be >= 0");
      }
      rrfK = rrf->k.value_or(60);
    } else if (const auto* linear =
                   std::get_if<luxir::api::LinearFusion>(&fusionProto.method)) {
      using Normalization = luxir::api::LinearFusion::Normalization;
      if (linear->normalization != Normalization::MIN_MAX) {
        throw std::runtime_error(fusionPrefix(name)
            + "unsupported LinearFusion.normalization "
            + std::to_string((int)linear->normalization));
      }
      method = FusionOp::Method::LINEAR_MIN_MAX;
    } else {
      throw std::runtime_error(fusionPrefix(name)
          + "requires a fusion method (rrf or linear)");
    }

    auto sourceEntries = lastWins(fusionProto.sources);  // dedup duplicate source names, last-wins
    std::vector<std::string_view> sourceNames;
    sourceNames.reserve(sourceEntries.size());
    for (const auto& entry : sourceEntries) sourceNames.push_back(entry.first);
    auto weights = resolveSourceWeights(name, fusionProto.weights, sourceNames);

    // Build each source as a full TopDocsReq, wired to deliver its merged
    // collector back to the FusionOp::Calc instead of self-emitting.  The
    // sink closure captures nothing at parse time; it resolves the parent
    // FusionOp::Calc at runtime via the source Calc's parent pointer.
    // Fusion.sources is a map whose values are TopDocs directly (not
    // indirect), so srcProto is the message itself.  A zero-weight source is
    // disabled: it is neither parsed nor executed, so its documents never
    // reach the fused list, found, or the ops domain.
    std::vector<TopDocsReq*> sources;
    std::vector<double> sourceWeights;
    sources.reserve(sourceEntries.size());
    sourceWeights.reserve(sourceEntries.size());
    for (size_t i = 0; i < sourceEntries.size(); i++) {
      if (weights[i] == 0) continue;
      auto& [srcName, srcProto] = sourceEntries[i];
      size_t idx = sources.size();
      auto* sourceCollapse = enclosingCollapse;
      auto* sourceMeasure = inheritedFacetMeasure;
      auto restoreSource = scope_guard([&]() noexcept {
        enclosingCollapse = sourceCollapse;
        inheritedFacetMeasure = sourceMeasure;
      });
      auto* src = parseTopDocs(
          srcName, *srcProto, TopDocsPlacement::FUSION_SOURCE);
      src->flags = options.flags;
      // Linear fusion normalizes relevance scores; a field-sorted source
      // ranks by something else and carries no score to normalize.
      if (method != FusionOp::Method::RRF && src->sortPlan.useFieldSort) {
        throw std::runtime_error(fusionPrefix(name) + "linear fusion source '"
            + std::string(srcName) + "' must rank by score; use rrf for sorted sources");
      }
      src->rankingSink = [idx](TopDocsReq::CalcBase& calc, MergeableCollector* mc) {
        static_cast<FusionOp::Calc*>(calc.getParent())->acceptSourceRanking(idx, mc);
      };
      sources.push_back(src);
      sourceWeights.push_back(weights[i]);
    }

    auto parseContext = createParseContext(name);
    ProtobufQueryParser parser(parseContext);
    auto sharedFilters = parseFilters(
        parser, fusionProto.filter, nullptr, "fusion.filter");
    // Reuse the first source's execution Context when it has one. A fully
    // resident source may be Context-free, in which case shared filter
    // Weights are unresolved execution and create the Context here.
    // Shared filters use the same request flag path as TopDocs filters.
    for (const auto& filter : sharedFilters) {
      filter.query->validateLogical(sources.front()->planning);
    }
    std::span<Query::Weight*> sharedFilterWeights;
    if (!sharedFilters.empty()) {
      Query::Context* sharedContext = sources.front()->weightContext;
      if (sharedContext == nullptr) {
        sharedContext = Query::Context::create(
            &req.arena, sources.front()->planning);
        skipCount(SkipStats::queryContextsCreated);
      }
      sharedFilterWeights = buildFilterWeights(
          sharedFilters, *sharedContext, Query::NEED_SCORES);
    }

    int64_t specifiedLimit = fusionProto.limit.has_value() ? *fusionProto.limit : 10;
    int64_t maxDoc = view()->maxDoc();
    int64_t limit = specifiedLimit < 0 ? maxDoc : std::min(specifiedLimit, maxDoc);
    int64_t topCount = limit == 0 ? 0
        : offset >= maxDoc ? maxDoc : offset + std::min(limit, maxDoc - offset);

    auto* fusion = luxir::arenaCreate<FusionOp>(
      req.arena, req, view(), name, fusionProto, std::move(sources),
      std::move(sourceWeights), topCount, sharedFilters, sharedFilterWeights,
      method, rrfK);

    fusion->collapse = collapse;
    if (collapse && !fusionProto.collapse->head_sort.empty()) {
      fusion->headSort = parseSorts(fusionProto.collapse->head_sort);
    }
    if (!fusionProto.decorate.empty()) {
      std::vector<DecoratorContext::QuerySource> resultQueries;
      for (auto* source : fusion->sources) {
        resultQueries.push_back({source->resultQuery, decoratorQueryContext(*source)});
      }
      DecoratorContext dc{req, parseContext, resultQueries};
      fusion->decorators = HitDecoratorRegistry::global().make(fusionProto.decorate, dc);
    }
    fusion->diverseSources = std::ranges::any_of(fusion->sources,
        [](const TopDocsReq* source) { return source->collapse != nullptr || source->diversify != nullptr; });

    // Sources are children of this FusionOp in the SearchOp tree but not
    // in `subOps` (different lifecycle - they deliver via rankingSink
    // rather than emit).  Set parent so any downstream code that walks up
    // the tree finds the right ancestor.
    for (auto* src : fusion->sources) {
      src->parent = fusion;
    }

    addSubs(*fusion, fusionProto.ops, depth, OpsPlacement::FUSION);
    if (collapse) {
      inheritedFacetMeasure = nullptr;
      addSubs(*fusion, fusionProto.collapse->ops, depth, OpsPlacement::GROUP_BUCKET,
              &fusion->groupOps, &fusion->filters);
    }
    return fusion;
  }

};

} // anonymous

ProtobufSearchParser::ProtobufSearchParser(SearchRequest& req) : req(req) {
}

RootOp* ProtobufSearchParser::parse() {
  return SearchParserImpl(req).parse();
}

} // luxir
