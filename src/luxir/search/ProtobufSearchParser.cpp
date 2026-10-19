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

#include "luxir/search/SearchRequest.h"
#include "luxir/query/ParseContext.h"
#include "luxir/search/ops/RootOp.h"
#include "luxir/search/ops/SearchOp.h"
#include "luxir/search/ops/FacetOp.h"
#include "luxir/search/ops/StrFacetOp.h"
#include "luxir/search/ops/ExprStatsOp.h"
#include "luxir/search/ops/FusionOp.h"
#include "luxir/search/ops/TopDocsReq.h"
#include "luxir/query/AllQuery.h"
#include "luxir/search/RangeFacetGrid.h"
#include "luxir/query/BooleanQuery.h"
#include "luxir/query/ForcePrepareQuery.h"
#include "luxir/query/MinScoreQuery.h"
#include "luxir/query/ProtobufQueryParser.h"
#include "luxir/query/QueryBuilder.h"
#include "luxir/query/QueryShape.h"
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

struct SearchParserImpl {
  SearchRequest& req;
  FieldResolver fields;
  TopDocsReq* firstQuery = nullptr;
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
  };

  enum class OpsPlacement : uint8_t {
    REQUEST_ROOT,
    TOP_DOCS,
    FACET_BUCKET,
    GROUP_BUCKET,
    FUSION,
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
  explicit SearchParserImpl(SearchRequest& req)
    : req(req), fields(*req.schema, req.resolvedFields), maxOpDepth(req.searchConfig.max_op_depth) {
  }

  RootOp* parse() {
    RootOp& rootOp = *luxir::arenaCreate<RootOp>(req.arena, req);
    rootOp.parent = nullptr;

    ParseContext parseContext{
      req.requestPool, *req.schema, req.arena,
      CoerceContext{req.dateMathNowEpochMillis, *req.timeZone},
      "root", &req.warnings, &fields};
    ProtobufQueryParser parser(parseContext);
    ParsedDomains domains = parseChildDomains(req.proto.ops, parser);
    if (!domains.empty()) {
      rootOp.planning = luxir::arenaCreate<Query::PlanningContext>(
          req.arena, req.requestPool, *req.reader,
          Query::PlanningContext::Limits{}, &req.warnings,
          FilterKeyContext{.schemaGen = req.schema->gen_,
                           .timeZone = req.proto.time_zone},
          req.filterUses);
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
      ParseContext context{req.requestPool, *req.schema, req.arena,
          CoerceContext{req.dateMathNowEpochMillis, *req.timeZone},
          currOp.name, &req.warnings, &fields};
      context.collapse = enclosingCollapse;
      ProtobufQueryParser parser(context);
      auto domains = parseChildDomains(ops, parser);
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
        bucketPlan->planning = luxir::arenaCreate<Query::PlanningContext>(
            req.arena, req.requestPool, *req.reader, Query::PlanningContext::Limits{}, &req.warnings,
            FilterKeyContext{.schemaGen = req.schema->gen_, .timeZone = req.proto.time_zone}, req.filterUses);
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
  }

  SearchOp* parseOp(std::string_view name, const luxir::api::SearchOp& searchOp,
                    int depth, OpsPlacement placement) {
    if (searchOp.domain && placement == OpsPlacement::FUSION) {
      throw RequestError("domain is not supported under Fusion.ops");
    }
    // Exhaustive dispatch over the SearchOp oneof: a new arm is a compile error until handled.
    return std::visit(luxir::overloaded{
      [&](const luxir::api::TopDocs& topDocs) -> SearchOp* {
        auto* oldCollapse = enclosingCollapse;
        auto* oldMeasure = inheritedFacetMeasure;
        auto restore = scope_guard([&]() noexcept {
          enclosingCollapse = oldCollapse;
          inheritedFacetMeasure = oldMeasure;
        });
        auto topDocsPlacement = depth == 1 ? TopDocsPlacement::ROOT_OP
                                           : TopDocsPlacement::NESTED_OP;
        auto* qr = parseTopDocs(name, topDocs, topDocsPlacement);
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
          ParseContext parseContext{
            req.requestPool, *req.schema, req.arena,
            CoerceContext{req.dateMathNowEpochMillis, *req.timeZone},
            name, &req.warnings, &fields};
          parseContext.collapse = enclosingCollapse;
          ProtobufQueryParser parser(parseContext);
          plan = createQueryFacetPlan(
              name, facetReq, placement, parser);
          validateQueryFacetQueries(plan);
        }
        FacetReq* facet = createQueryFacetReq(name, facetReq, plan);
        auto* oldMeasure = inheritedFacetMeasure;
        inheritedFacetMeasure = facet->countDistinct;
        auto restore = scope_guard([&]() noexcept { inheritedFacetMeasure = oldMeasure; });
        addSubs(*facet, facetReq.ops, depth, OpsPlacement::FACET_BUCKET);
        return facet;
      },
      [&](const luxir::api::ExprOp& exprOp) -> SearchOp* {
        AggregateExprOptions options{req.schema.get(), exprOp.vars, name};
        options.fieldResolver = &fields;
        AggregateProgram* program =
            AggregateExprParser(options, req.arena).parse(exprOp.expr);
        auto fields = resolveAggregateFields(*program, req.requestPool, *req.reader);
        return luxir::arenaCreate<ExprStatsOp>(req.arena, req, name, *program, fields);
      },
      [&](std::monostate) -> SearchOp* { throw std::runtime_error("search op oneof not set"); },
    }, searchOp.kind);
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
        req.arena, *req.reader, spec.missing, getNumber);
    std::unordered_set<std::string> resolved;
    for (std::string_view name : spec.field) {
      auto field = req.schema->resolveFor(name, OpClass::VALUE);
      auto& type = *field.fieldType;
      if (type.multiValued() || !type.hasColumn()) {
        throw RequestError("collapse key must be single-valued with a column: " + std::string(name));
      }
      if (!resolved.insert(field.physicalName).second) {
        throw RequestError("duplicate resolved collapse field: " + std::string(name));
      }
      using Type = luxir::api::GroupComponent_::Type;
      Type wireType;
      switch (type.type()) {
        case FieldType::STRING: wireType = Type::STRING; break;
        case FieldType::ID: wireType = Type::ID; break;
        case FieldType::INT: wireType = Type::INT; break; // Luxir integers are signed int64.
        case FieldType::DATE: wireType = Type::DATE; break;
        default: throw RequestError("unsupported collapse key type: " + std::string(name));
      }
      bool ordinal = type.type() == FieldType::STRING || type.type() == FieldType::ID;
      if (ordinal && !type.isSet(FieldType::INDEX_DOCS)) {
        throw RequestError("string collapse key must be indexed: " + std::string(name));
      }
      collapse->fields.push_back({field.physicalName, wireType,
          ordinal ? req.reader->getOrdMap(field.physicalName) : nullptr,
          req.reader->field(field.physicalName)});
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
      if (facetReq.count_distinct->get_number || !facetReq.count_distinct->head_sort.empty()
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

  FacetReq* createFieldFacetReq(
      std::string_view facetName, const luxir::api::FieldFacet& facetReq,
      OpsPlacement placement,
      const CanonicalValueSet* preparedSelected = nullptr) {
    validateSelectionEnvelope(facetName, facetReq, placement);
    const CollapsePlan* measure = resolveMeasure(facetReq);
    const auto& target = fields.resolve(facetReq.field, OpClass::VALUE, std::format("facet '{}'", facetName));
    auto facetField = api::build::arenaStr(req.requestPool, target.physicalName);
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
    auto* ftype = target.fieldType;

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
      CoerceContext selectionContext{
          req.dateMathNowEpochMillis, *req.timeZone};
      QueryBuilder builder(req.requestPool, *req.schema,
                           selectionContext, facetName, &req.warnings, &fields);
      ownedSelected = builder.canonicalizeFieldValues(
          target, selectedValues, true);
      preparedSelected = &ownedSelected;
    }
    if (preparedSelected != nullptr && !preparedSelected->empty()) {
      if (ftype->type() == FieldType::Type::DATE
          || ftype->type() == FieldType::Type::INT) {
        selectedInts = preparedSelected->numericsInInputOrder();
      } else if (ftype->type() == FieldType::Type::ID
                 || ftype->type() == FieldType::Type::STRING
                 || ftype->type() == FieldType::Type::TEXT) {
        selectedStrings = preparedSelected->termsInInputOrder();
      }
    }

    FieldFacetReq* facet = nullptr;
    switch (ftype->type()) {
      // DATE is epoch millis in the int column; it terms-facets exactly like
      // INT (distinct millis values).  Date-histogram bucketing is a range
      // facet, not a terms facet.
      case FieldType::Type::DATE:
      case FieldType::Type::INT: {
        if (facetReq.mincount.has_value() && *facetReq.mincount < 1) {
          throw std::runtime_error("facet '" + std::string(facetName) + "': mincount < 1 (zero-count buckets) is not supported for int field facets");
        }
        facet = luxir::arenaCreate<IntFacetReq>(req.arena, req, facetReq,
            facetField, facetName, limit, minCount, missing, selectedInts);
        break;
      }
      case FieldType::Type::ID:
      case FieldType::Type::STRING: {
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
        auto ordMap = req.reader->getOrdMap(facetField);
        facet = luxir::arenaCreate<StrFacetOp>(req.arena, req, facetReq,
            facetField, facetName, limit, minCount, missing,
            std::move(ordMap), selectedStrings);
        break;
      }
      case FieldType::Type::TEXT:
        if (measure != nullptr) throw RequestError("TEXT field_facet distinct counts are not supported yet");
        facet = luxir::arenaCreate<FullTextFacetReq>(req.arena, req, facetReq,
            facetField, facetName, limit, minCount, missing, selectedStrings);
        break;
      default: ;
    }
    if (facet == nullptr) {
      throw std::runtime_error("Unknown facet field type: " + std::string(facetField));
    }
    configureMeasure(*facet, measure);
    return facet;
  }

  FacetReq* createRangeFacetReq(
      std::string_view facetName, const luxir::api::RangeFacet& facetReq,
      OpsPlacement placement) {
    validateSelectionEnvelope(facetName, facetReq, placement);
    const CollapsePlan* measure = resolveMeasure(facetReq);
    const auto& target = fields.resolve(facetReq.field, OpClass::VALUE, std::format("facet '{}'", facetName));
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
    if (fieldType->type() != FieldType::Type::INT
        && fieldType->type() != FieldType::Type::DATE
        && fieldType->type() != FieldType::Type::FLOAT
        && fieldType->type() != FieldType::Type::DOUBLE) {
      throw std::runtime_error("facet '" + std::string(facetName)
          + "': range facets require an INT, DATE, FLOAT, or DOUBLE field");
    }

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
    CoerceContext context{req.dateMathNowEpochMillis, zone};

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

    auto dateBound = [&](const api::Val& value) {
      auto& dateType = (DateFieldType&)*fieldType;
      DateRange range = dateType.coerceDateRange(value, facetField, context);
      if (range.granuleSkipped) {
        const auto* text = std::get_if<std::string_view>(&value.kind);
        if (text != nullptr) {
          warnOnce("date_granule_skipped",
              "facet '" + std::string(facetName) + "', DATE field '"
              + std::string(facetField) + "': date granule '" + std::string(*text)
              + "' skipped: no such local granule in " + zone.name());
        }
      }
      return range.lo;
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
        auto* facet = luxir::arenaCreate<IntFacetRangeReq>(req.arena, req,
            facetField, facetName, std::move(grid), encodedStart, encodedEnd,
            fieldType->type(), minCount, facetReq.missing);
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
          req.arena, req, facetField, facetName, parsed.values,
          parsed.affine, parsed.affineGap, fieldType->type(), minCount,
          facetReq.missing, selected, parsed.runs);
      configureMeasure(*facet, measure);
      return facet;
    }

    auto intBound = [&](const api::Val& value) {
      return fieldType->type() == FieldType::Type::DATE
          ? dateBound(value)
          : fieldType->coerceColInt64(value, facetField, context);
    };
    std::optional<int64_t> givenStart, givenEnd;
    if (facetReq.start.has_value()) givenStart = intBound(*facetReq.start);
    if (facetReq.end.has_value()) givenEnd = intBound(*facetReq.end);
    if (givenStart && givenEnd && *givenStart >= *givenEnd) {
      throw std::runtime_error("facet '" + std::string(facetName)
          + "': range start must be less than end");
    }
    auto grid = [&]() {
      if (calendar) {
        const auto& spec = std::get<api::CalendarGap>(facetReq.gap_kind);
        return RangeFacetGrid::calendar(*calendarGridUnit, spec.n, zone,
            dataBounds ? std::nullopt : givenStart);
      }
      const auto& gapValue = std::get<api::Val>(facetReq.gap_kind);
      bool date = fieldType->type() == FieldType::Type::DATE;
      auto integer = [&](const api::Val& value, std::string_view name) {
        if (!date) return fieldType->coerceColInt64(value, facetField, context);
        const auto* millis = std::get_if<int64_t>(&value.kind);
        if (!millis) {
          throw std::runtime_error("facet '" + std::string(facetName)
              + "': a DATE " + std::string(name) + " must be integer milliseconds");
        }
        return *millis;
      };
      int64_t gap = integer(gapValue, "fixed gap");
      int64_t origin = 0;
      if (dataBounds) {
        if (facetReq.origin.has_value()) origin = integer(*facetReq.origin, "origin");
      } else {
        if (date) {
          auto civil = civilFromInstant(*givenStart, zone);
          if (!civil) throw std::runtime_error("range bounds are outside the supported civil range");
          origin = civil->millis;
        } else origin = *givenStart;
      }
      return RangeFacetGrid::fixed(fieldType->type(),
                                   gap, origin, zone);
    }();
    if (dataBounds) {
      auto* facet = luxir::arenaCreate<IntFacetRangeReq>(req.arena, req,
          facetField, facetName, std::move(grid), givenStart, givenEnd,
          fieldType->type(), minCount, facetReq.missing);
      configureMeasure(*facet, measure);
      return facet;
    }
    auto parsed = makeGridFences(facetName, grid, *givenStart, *givenEnd, true);

    auto selected = resolveSelectedBuckets(parsed, [&](const api::Val& value) {
      return intBound(value);
    });

    auto* facet = luxir::arenaCreate<IntFacetRangeReq>(
        req.arena, req, facetField, facetName, parsed.values,
        parsed.affine, parsed.affineGap, fieldType->type(), minCount,
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
      options.fieldResolver = &fields;
      options.opName = "sort";
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

  ParsedDomains parseChildDomains(
      OpsMap ops, ProtobufQueryParser& parser) {
    ParsedDomains out;
    for (const auto& [key, childView] : lastWins(ops)) {
      const api::SearchOp& child = **childView;
      if (!child.domain.has_value()) continue;
      const api::Domain& domain = *child.domain;
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

  void addDomainVariants(
      OpsMap ops, std::span<ParsedFilter> filters,
      const ParsedDomains& domains, Query::PlanningContext* planning,
      DomainVariantPlan& plan) {
    for (const auto& [key, childView] : lastWins(ops)) {
      unused(childView);
      auto found = domains.find(key);
      const ParsedDomain* domain =
          found == domains.end() ? nullptr : &found->second;
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
          [&](size_t filter) {
            return std::ranges::find(filters[filter].exceptOps, key)
                != filters[filter].exceptOps.end();
          },
          reset ? &*reset : nullptr,
          domain != nullptr && domain->applyParentFilters,
          extras);
      if (!created) continue;
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
        throw std::runtime_error("facet '" + std::string(facetName)
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
    plan.planning = luxir::arenaCreate<Query::PlanningContext>(
        req.arena, req.requestPool, *req.reader,
        Query::PlanningContext::Limits{}, &req.warnings,
        FilterKeyContext{.schemaGen = req.schema->gen_,
                         .timeZone = req.proto.time_zone},
        req.filterUses);
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
      const FacetParsePlan& plan) {
    assert(plan.planning != nullptr);
    Query::Context* context = Query::Context::create(
        &req.arena, *plan.planning);
    auto weights = req.requestPool.make_span<Query::Weight*>(
        plan.bucketQueries.size());
    for (size_t i = 0; i < weights.size(); i++) {
      weights[i] = plan.bucketQueries[i]->createWeight(*context, 0);
    }
    auto* facet = luxir::arenaCreate<QueryFacetReq>(
        req.arena, req, facetReq, facetName, plan.bucketQueries, weights);
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
                             parseContext.coerceContext, key, &req.warnings, &fields);
        CanonicalValueSet canonical = builder.canonicalizeFieldValues(
            fields.resolve(fieldProto->field, OpClass::VALUE), selected, true);
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

  // Build a TopDocsReq from a TopDocs proto. Placement is semantic: ordinary
  // TopDocs can serve a canonical-root whole-reader fact, while a Fusion
  // source delivers candidates into a parent-owned domain.
  TopDocsReq* parseTopDocs(std::string_view name,
                           const luxir::api::TopDocs& topDocsReq,
                           TopDocsPlacement placement) {
    /*
    message TopDocs {
            Query query = 1;
            int64 offset = 2;
            optional sint64 limit = 3;
            bool get_number = 4;          // return the number of matching documents
            bool get_scores = 5;          // return the relevancy score for each document returned
            repeated string fields = 6;   // fields to return for each document (empty: every retrievable field)
            repeated SortSpec sort = 7;
    }
    */

    // Place the Query in the requestPool since it uses things like string_view that directly reference
    // the request.
    DiversifyPlan* diversify = nullptr;
    if (topDocsReq.diversify) {
      auto keys = diversifyKeySpec(topDocsReq);
      auto* resolved = resolveCollapse(keys,
          placement != TopDocsPlacement::FUSION_SOURCE && topDocsReq.get_number);
      diversify = luxir::arenaCreate<DiversifyPlan>(req.arena,
          *resolved, (size_t)*topDocsReq.diversify->max_per_key, req.memoryTracker);
    }
    CollapsePlan* collapse = nullptr;
    if (topDocsReq.collapse) {
      if (placement == TopDocsPlacement::FUSION_SOURCE) {
        const auto& spec = *topDocsReq.collapse;
        if (!spec.head_sort.empty() || !spec.sort.empty() || spec.get_number || !spec.ops.empty()) {
          throw RequestError("fusion source '" + std::string(name)
              + "': collapse supports only field and missing; head_sort, sort, get_number and ops"
                " require source finalization");
        }
      }
      collapse = resolveCollapse(*topDocsReq.collapse,
          placement != TopDocsPlacement::FUSION_SOURCE && topDocsReq.get_number);
      enclosingCollapse = collapse;
      // Facets under a collapsed list count groups unless they say otherwise.
      inheritedFacetMeasure = collapse;
    }
    ParseContext parseContext{
      req.requestPool, *req.schema, req.arena,
      CoerceContext{req.dateMathNowEpochMillis, *req.timeZone}, name, &req.warnings, &fields};
    parseContext.collapse = enclosingCollapse;
    ProtobufQueryParser parser(parseContext);
    // An absent query selects all documents: the domain is then whatever the
    // filters carve out (browse / filter-only search). Boolean normalization
    // eliminates the match-all when filters fold in beside it.
    Query* query = topDocsReq.query.has_value()
      ? parser.parse(*topDocsReq.query)
      : req.requestPool.make<AllQuery>();
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
    int64_t maxDoc = req.reader->maxDoc();
    int64_t limit = specifiedLimit < 0 ? maxDoc : std::min(specifiedLimit, maxDoc);
    // Keep limit as the page size for request-shape decisions. Fusion source
    // offsets are response-shape fields and do not enlarge candidate pools.
    int64_t topCount = limit;
    if (limit > 0 && placement != TopDocsPlacement::FUSION_SOURCE) {
      topCount = offset >= maxDoc ? maxDoc : offset + std::min(limit, maxDoc - offset);
    }

    if (placement == TopDocsPlacement::FUSION_SOURCE) {
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
    auto filters = appendDerivedFilters(
        explicitFilters,
        prepareFacetSelections(
            topDocsReq, parseContext, parser,
            specifiedLimit != 0 || topDocsReq.get_number));
    ParsedDomains parsedDomains = parseChildDomains(topDocsReq.ops, parser);

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
    bool countClauseDisabled =
        BooleanQuery::disableFilterClauseCountForTests
        && limit == 0 && topDocsReq.get_number;
    CollectionRequirements requirements{
      .needRankedDocs = limit > 0,
      .needExactCount = topDocsReq.get_number,
      .needExactDomain = !topDocsReq.ops.empty()
    };
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
    auto* planningContext = luxir::arenaCreate<Query::PlanningContext>(
      req.arena, req.requestPool, *req.reader, Query::PlanningContext::Limits{},
      &req.warnings,
      FilterKeyContext{.schemaGen = req.schema->gen_,
                       .timeZone = req.proto.time_zone},
      req.filterUses);
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
    if (limit > 0
        && (topDocsReq.get_scores || parsedSorts.rankNeedsScores
            || (headSort && headSort->rankNeedsScores))) {
      requestFlags |= Query::NEED_SCORES;
    }
    // Competitive-score pruning requires a score-ranked heap. Sub-ops permit
    // pruning because a separate exhaustive windowed pass produces their
    // complete match domain and exact count before the ranking pass. Match-all
    // reuses the incoming domain and its cardinality instead.
    bool allowPruning = requirements.needRankedDocs
        && !parsedSorts.useFieldSort
        && (!requirements.needExactCount || requirements.needExactDomain);
    if (allowPruning && !(diversify && topDocsReq.get_number)
        && !(collapse && !topDocsReq.collapse->sort.empty())) {
      requestFlags |= Query::ALLOW_PRUNING;
    }
    Query::ScoreProfile scoreProfile = luxir::scoreProfile(*query);
    // Document cardinality is not a distinct-group count; collapse totals
    // read every eligible key through the normal exhaustive driver.
    bool exactCountTopK = collapse == nullptr && diversify == nullptr && requirements.needRankedDocs
        && requirements.needExactCount && !requirements.needExactDomain
        && !parsedSorts.useFieldSort
        && parsedSorts.rankNeedsScores;
    bool pureCountShape = collapse == nullptr && diversify == nullptr
        && !QueryPrep::disableWholeMembershipPlanForTests
        && limit == 0 && topDocsReq.get_number
        && !requirements.needExactDomain && !parsedSorts.useFieldSort
        && (foldFilters || filters.empty());
    FilterCache::Use* cacheFirstMembershipUse = nullptr;
    auto* cache = req.reader->filterCache();
    if (pureCountShape && canOmitWeightForCacheFirstMembership(*query)
        && !directCountAvailable(*query, *req.reader)
        && cache != nullptr && cache->enabled()) {
      auto candidate = planningContext->lookupExistingFilterUse(
          *query, placement == TopDocsPlacement::ROOT_OP);
      if (candidate.has_value()) {
        cacheFirstMembershipUse = planningContext->acceptExistingFilterUse(
            std::move(*candidate), FilterCache::AdmissionLane::WHOLE);
      }
      if (cacheFirstMembershipUse != nullptr) {
        skipCount(SkipStats::cacheFirstMembershipWeightSkips);
      }
    }

    bool cacheFirstTopKCount = false;
    if (cacheFirstMembershipUse == nullptr
        && placement == TopDocsPlacement::ROOT_OP
        && !QueryPrep::disableWholeMembershipPlanForTests
        && exactCountTopK && (foldFilters || filters.empty())
        && canOmitWeightForCacheFirstMembership(*query)
        && !directCountAvailable(*query, *req.reader)
        && cache != nullptr && cache->enabled()) {
      auto candidate = planningContext->lookupExistingFilterUse(*query, true);
      if (candidate.has_value()) {
        cacheFirstMembershipUse = planningContext->acceptExistingFilterUse(
            std::move(*candidate), FilterCache::AdmissionLane::WHOLE);
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
    bool fieldSortBuildVetoed = false;
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
      auto fieldSortPreflight =
          TopDocsReq::planCacheFirstFieldSortWholeMembership(
              *query, *planningContext, parsedSorts, true);
      cacheFirstMembershipUse = fieldSortPreflight.acceptedUse;
      fieldSortBuildVetoed = fieldSortPreflight.buildVetoed;
      cacheFirstFieldSort = fieldSortPreflight.omitsWeight();
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
            *weight, *req.reader, topCount);
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
    if (weight != nullptr && !cacheFirstTopKCount
        && !disableTopKCountComposition && exactCountTopK
        && !weight->needsPrepare() && weight->canComposeExactCountTopK()
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
    std::span<uint8_t> wholeFieldSortCacheRoutes;
    bool wholeConstantRanking = false;
    bool pureCount = pureCountShape && filterWeights.empty();
    bool wholeTopKCount = !QueryPrep::disableWholeMembershipPlanForTests
        && exactCountTopK
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
    bool readerStableWhole = weight != nullptr && weight->needsPrepare()
        && placement != TopDocsPlacement::FUSION_SOURCE
        && (pureCount || wholeFieldSort);
    if (weight != nullptr && weight->needsPrepare() && !readerStableWhole) {
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
          bool everySegmentEmpty = !req.reader->segments().empty();
          for (auto& segment : req.reader->segments()) {
            DocSet* rootDomain = segment.liveDocs() == nullptr
                ? nullptr : &segment.liveDocs()->docset();
            auto shortcut = wholeMembershipWeight->countShortcut(segment, rootDomain);
            everySegmentEmpty &= shortcut.how == Query::CountHow::METADATA
                && shortcut.maxMatches == 0;
            if (shortcut.how > Query::COUNT_PROBE_LIMIT) {
              everySegmentConstant = false;
              break;
            }
          }
          if (everySegmentEmpty && hasRequiredConjunction(*query)) {
            everySegmentConstant = false;
          }
        }
        bool fieldSortFullyRouted = false;
        if (!everySegmentConstant && cache != nullptr && cache->enabled()) {
          bool acquireUse = true;
          if (wholeFieldSort && !readerStableWhole) {
            wholeFieldSortCacheRoutes = req.requestPool.make_span<uint8_t>(
                req.reader->segments().size());
            if (fieldSortBuildVetoed) {
              std::fill(wholeFieldSortCacheRoutes.begin(),
                        wholeFieldSortCacheRoutes.end(), 0);
              acquireUse = false;
            } else {
              acquireUse = TopDocsReq::planFieldSortWholeMembershipRoutes(
                  *query, *wholeMembershipWeight, *req.reader, parsedSorts,
                  topCount, wholeFieldSortCacheRoutes);
            }
            fieldSortFullyRouted = !acquireUse;
          }
          // An accepted reader-stable value replaces the ANN preparation pass.
          // Its bounded kNN membership is also a valid ladder fallback, while
          // the numeric exact-domain driver still prices exact cardinality per
          // segment.
          if (acquireUse) {
            wholeMembershipUse = readerStableWhole
                ? qcontext->getFilterUse(
                      *query, FilterKeyScope::READER_STABLE,
                      FilterCache::AdmissionLane::WHOLE)
                : qcontext->getFilterUse(
                      *query, FilterCache::AdmissionLane::WHOLE);
          }
        }
        if (!everySegmentConstant && wholeMembershipUse == nullptr
            && !fieldSortFullyRouted) {
          wholeMembershipWeight = nullptr;
          wholeFieldSortCacheRoutes = {};
        }
      }

      if (cacheFirstTopKCount && wholeTopKCount
          && scoreProfile.kind == Query::ScoreProfile::Kind::VARIABLE) {
        assert(weight != nullptr);
        wholeRankingWeight = weight;
      } else if (weight != nullptr && wholeTopKCount
          && wholeMembershipWeight != nullptr
          && !weight->isConstantScoring()) {
        Query::Weight* countFreeRankingWeight = rankingWeight;
        if (countFreeRankingWeight == nullptr) {
          int32_t rankingFlags =
              requestFlags | Query::NEED_SCORES | Query::ALLOW_PRUNING;
          countFreeRankingWeight =
              query->createWeight(*qcontext, rankingFlags);
        }
        bool reroute = foldFilters
            && !countFreeRankingWeight->needsPrepare()
            && TopDocsReq::admitSparseFilteredTopK(
                *countFreeRankingWeight, *req.reader, topCount);
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
      wholeConstantRanking = wholeTopKCount
          && (weight == nullptr
                  ? scoreProfile.kind
                      != Query::ScoreProfile::Kind::VARIABLE
                  : weight->isConstantScoring());
    }

    auto* qr = luxir::arenaCreate<TopDocsReq>(
      req.arena, req, name, topDocsReq, *planningContext, qcontext,
      query, weight, variantMembershipWeight,
      countWeight, rankingWeight, topCount, std::move(parsedSorts),
      requirements, wholeMembershipWeight, wholeRankingWeight,
      wholeMembershipUse, wholeFieldSortCacheRoutes,
      wholeConstantRanking, scoreProfile,
      (requestFlags & Query::NEED_SCORES) != 0,
      filters, filterWeights, std::move(domainVariants),
      domainQuery, domainQueryWeight,
      domainFilterWeights, residentExactDomainUses);

    qr->collapse = collapse;
    qr->diversify = diversify;
    qr->headSort = std::move(headSort);
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

  SearchOp* parseFusion(std::string_view name, const luxir::api::Fusion& fusionProto, int depth) {
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
      collapse = resolveCollapse(*fusionProto.collapse, fusionProto.get_number);
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

    ParseContext parseContext{
      req.requestPool, *req.schema, req.arena,
      CoerceContext{req.dateMathNowEpochMillis, *req.timeZone}, name, &req.warnings, &fields};
    parseContext.collapse = enclosingCollapse;
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
    int64_t maxDoc = req.reader->maxDoc();
    int64_t limit = specifiedLimit < 0 ? maxDoc : std::min(specifiedLimit, maxDoc);
    int64_t topCount = limit == 0 ? 0
        : offset >= maxDoc ? maxDoc : offset + std::min(limit, maxDoc - offset);

    auto* fusion = luxir::arenaCreate<FusionOp>(
      req.arena, req, name, fusionProto, std::move(sources),
      std::move(sourceWeights), topCount, sharedFilters, sharedFilterWeights,
      method, rrfK);

    fusion->collapse = collapse;
    if (collapse && !fusionProto.collapse->head_sort.empty()) {
      fusion->headSort = parseSorts(fusionProto.collapse->head_sort);
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
