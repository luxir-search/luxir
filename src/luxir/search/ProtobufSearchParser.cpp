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
#include "luxir/query/BooleanQuery.h"
#include "luxir/query/ForcePrepareQuery.h"
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

struct SearchParserImpl {
  SearchRequest& req;
  TopDocsReq* firstQuery = nullptr;
  size_t nonDefaultDomainVariants = 0;
  std::unordered_map<const api::SearchOp*, FacetParsePlan> preparedFacets;
  // Root-level ops are depth 1. Ops nested past the configured cap are rejected
  // in addSubs, the single funnel for op recursion.
  const int maxOpDepth;
  static constexpr size_t MAX_RANGE_BUCKETS = 100000;
  static constexpr size_t MAX_SUBOP_RANGE_BUCKETS = 1024;

  enum class TopDocsPlacement : uint8_t {
    ROOT_OP,
    NESTED_OP,
    FUSION_SOURCE,
  };

  enum class OpsPlacement : uint8_t {
    REQUEST_ROOT,
    TOP_DOCS,
    FACET_BUCKET,
    FUSION,
  };

  struct ParsedFences {
    std::span<const int64_t> values;
    int64_t affineGap = 0;
    bool affine = false;
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

  ParsedFences makeAffineFences(std::string_view facetName,
                                 int64_t start, int64_t end, int64_t gap) {
    uint64_t distance = (uint64_t)end - (uint64_t)start;
    uint64_t bucketCount = distance / (uint64_t)gap
                         + (distance % (uint64_t)gap != 0);
    if (bucketCount > MAX_RANGE_BUCKETS) {
      throw std::runtime_error("facet '" + std::string(facetName)
          + "': range exceeds the 100000 bucket limit");
    }
    auto fences = req.requestPool.make_span<int64_t>((size_t)bucketCount + 1);
    for (uint64_t i = 0; i <= bucketCount; i++) {
      __uint128_t product = (__uint128_t)i * (uint64_t)gap;
      uint64_t offset = product < distance ? (uint64_t)product : distance;
      fences[(size_t)i] = (int64_t)((uint64_t)start + offset);
    }
    return {fences, gap, true};
  }

  ParsedFences makeFloatingFences(std::string_view facetName,
                                  FieldType::Type type,
                                  double start, double end, double gap) {
    std::vector<int64_t> built;
    built.reserve(256);
    for (size_t i = 0; ; i++) {
      if (i > MAX_RANGE_BUCKETS) {
        throw std::runtime_error("facet '" + std::string(facetName)
            + "': range exceeds the 100000 bucket limit");
      }
      double nominal = start + (double)i * gap;
      double value = nominal < end ? nominal : end;
      int64_t encoded;
      if (type == FieldType::Type::FLOAT) {
        float rounded = (float)value;
        if (!std::isfinite(rounded)) {
          throw std::runtime_error("facet '" + std::string(facetName)
              + "': fence " + std::to_string(i)
              + " is not finite after FLOAT rounding");
        }
        encoded = (int64_t)floatToSortableInt32(rounded);
      } else {
        encoded = doubleToSortableInt64(value);
      }
      if (!built.empty() && encoded <= built.back()) {
        throw std::runtime_error("facet '" + std::string(facetName)
            + "': fence " + std::to_string(i)
            + " is not strictly increasing after "
            + (type == FieldType::Type::FLOAT ? "FLOAT" : "DOUBLE")
            + " rounding");
      }
      built.push_back(encoded);
      if (value == end) break;
    }

    auto fences = req.requestPool.make_span<int64_t>(built.size());
    std::copy(built.begin(), built.end(), fences.begin());
    return {fences, 0, false};
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

  ParsedFences makeCalendarFences(
      std::string_view facetName, int64_t start, int64_t end,
      const api::CalendarGap& gap, const TimeZone& zone) {
    auto unit = calendarUnit(gap.unit);
    if (gap.n <= 0 || !unit) {
      throw std::runtime_error("facet '" + std::string(facetName)
          + "': calendar_gap requires n > 0 and a DAY/WEEK/MONTH/QUARTER/YEAR unit");
    }
    if (!zone.isIana()
        && (*unit == CalendarUnit::DAY || *unit == CalendarUnit::WEEK)) {
      int64_t daysPerUnit = *unit == CalendarUnit::WEEK ? 7 : 1;
      __int128 fixedGap = (__int128)gap.n * daysPerUnit
                        * datetime_detail::kMsPerDay;
      if (fixedGap > std::numeric_limits<int64_t>::max()) {
        throw std::runtime_error("facet '" + std::string(facetName)
            + "': calendar_gap is too large");
      }
      return makeAffineFences(facetName, start, end, (int64_t)fixedGap);
    }

    CivilCalendarStepper stepper(start, zone);
    if (!stepper.valid() || !civilFromInstant(end, zone)) {
      throw std::runtime_error("facet '" + std::string(facetName)
          + "': calendar bounds are outside the supported civil range");
    }
    auto previous = stepper.at(0, *unit);
    if (!previous || previous->instant != start) {
      throw std::runtime_error("facet '" + std::string(facetName)
          + "': calendar start cannot be resolved in its civil frame");
    }

    std::vector<int64_t> built;
    built.reserve(256);
    built.push_back(start);
    std::optional<int64_t> shiftedFence;
    for (int64_t i = 1; ; i++) {
      __int128 amount = (__int128)i * gap.n;
      if (amount > std::numeric_limits<int64_t>::max()) {
        throw std::runtime_error("facet '" + std::string(facetName)
            + "': calendar fence arithmetic overflow");
      }
      auto fence = stepper.at((int64_t)amount, *unit);
      if (!fence) {
        throw std::runtime_error("facet '" + std::string(facetName)
            + "': calendar fence is outside the supported civil range");
      }
      bool substituted = shiftedFence.has_value();
      int64_t instant = substituted ? *shiftedFence : fence->instant;
      shiftedFence.reset();

      // A whole skipped date can be reported one nominal endpoint late by
      // libstdc++'s text-tzdb reader. Detect it from the forward jump covering
      // the civil interval, drop its bucket now, and carry its shifted instant
      // as the surviving next label's fence.
      if (!substituted) {
        __int128 nextAmount = (__int128)(i + 1) * gap.n;
        if (nextAmount <= std::numeric_limits<int64_t>::max()) {
          auto next = stepper.at((int64_t)nextAmount, *unit);
          if (next && civilIntervalSkipped(
                  fence->civilMillis, next->civilMillis, zone)) {
            std::string label = formatEpochMillisIso8601(
                fence->civilMillis).substr(0, 10);
            warnOnce("calendar_bucket_skipped",
                "facet '" + std::string(facetName) + "': calendar bucket "
                + label + " skipped: no such local day in " + zone.name());
            shiftedFence = fence->instant;
            previous = fence;
            continue;
          }
        }
      }

      if (instant >= end) {
        built.push_back(end);
        if (built.size() - 1 > MAX_RANGE_BUCKETS) {
          throw std::runtime_error("facet '" + std::string(facetName)
              + "': range exceeds the 100000 bucket limit");
        }
        break;
      }
      if (instant < built.back()) {
        throw std::runtime_error("facet '" + std::string(facetName)
            + "': calendar fences are not increasing");
      }
      if (instant == built.back()) {
        std::string label = formatEpochMillisIso8601(previous->civilMillis).substr(0, 10);
        warnOnce("calendar_bucket_skipped",
            "facet '" + std::string(facetName) + "': calendar bucket " + label
            + " skipped: no such local day in " + zone.name());
        previous = fence;
        continue;
      }
      built.push_back(instant);
      if (built.size() - 1 > MAX_RANGE_BUCKETS) {
        throw std::runtime_error("facet '" + std::string(facetName)
            + "': range exceeds the 100000 bucket limit");
      }
      previous = fence;
    }

    auto fences = req.requestPool.make_span<int64_t>(built.size());
    std::copy(built.begin(), built.end(), fences.begin());
    return {fences, 0, false};
  }



public:
  // The provided pool will be used to store the parsed query tree.
  // We need access to the schema to figure out what types of queries to produce?
  // Both the pool and any parsed protobuf objects must outlive the query tree.
  explicit SearchParserImpl(SearchRequest& req)
    : req(req), maxOpDepth(req.searchConfig.max_op_depth) {
  }

  RootOp* parse() {
    RootOp& rootOp = *luxir::arenaCreate<RootOp>(req.arena, req);
    rootOp.parent = nullptr;

    ParseContext parseContext{
      req.requestPool, *req.schema, req.arena,
      CoerceContext{req.dateMathNowEpochMillis, *req.timeZone},
      "root", &req.warnings};
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
               OpsPlacement placement) {
    if (ops.empty()) return;
    if (depth >= maxOpDepth) {
      throw std::runtime_error("search operation nesting exceeds the maximum depth of "
                               + std::to_string(maxOpDepth));
    }
    for (auto& [name, searchOp] : lastWins(ops)) {
      // map values are indirect views over the request bytes; deref to the SearchOp.
      // lastWins() collapses duplicate op names (protobuf map dedup semantics).
      validateName(name, "op");
      auto* sub = parseOp(name, **searchOp, depth + 1, placement);
      if (sub == nullptr) {
        continue; // skip this op
      }
      currOp.subOps[name] = sub;
      sub->parent = &currOp;
    }
  }

  SearchOp* parseOp(std::string_view name, const luxir::api::SearchOp& searchOp,
                    int depth, OpsPlacement placement) {
    if (searchOp.domain.has_value()
        && placement != OpsPlacement::REQUEST_ROOT
        && placement != OpsPlacement::TOP_DOCS) {
      std::string location = placement == OpsPlacement::FUSION
          ? "Fusion.ops" : "a facet bucket";
      throw std::runtime_error(
          "op '" + std::string(name) + "' under " + location
          + ": domain is only supported for ops dispatched by the request "
            "root or TopDocs");
    }
    // Exhaustive dispatch over the SearchOp oneof: a new arm is a compile error until handled.
    return std::visit(luxir::overloaded{
      [&](const luxir::api::TopDocs& topDocs) -> SearchOp* {
        auto placement = depth == 1 ? TopDocsPlacement::ROOT_OP
                                    : TopDocsPlacement::NESTED_OP;
        auto* qr = parseTopDocs(name, topDocs, placement);
        addSubs(*qr, topDocs.ops, depth, OpsPlacement::TOP_DOCS);
        return qr;
      },
      [&](const luxir::api::Fusion& fusion) -> SearchOp* { return parseFusion(name, fusion, depth); },
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
            name, &req.warnings};
          ProtobufQueryParser parser(parseContext);
          plan = createQueryFacetPlan(
              name, facetReq, placement, parser);
          validateQueryFacetQueries(plan);
        }
        FacetReq* facet = createQueryFacetReq(name, facetReq, plan);
        addSubs(*facet, facetReq.ops, depth, OpsPlacement::FACET_BUCKET);
        return facet;
      },
      [&](const luxir::api::ExprOp& exprOp) -> SearchOp* {
        AggregateExprOptions options{req.schema.get(), exprOp.vars, name};
        AggregateProgram* program =
            AggregateExprParser(options, req.arena).parse(exprOp.expr);
        return luxir::arenaCreate<ExprStatsOp>(req.arena, req, name, *program);
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

  FacetReq* createFieldFacetReq(
      std::string_view facetName, const luxir::api::FieldFacet& facetReq,
      OpsPlacement placement,
      const CanonicalValueSet* preparedSelected = nullptr) {
    validateSelectionEnvelope(facetName, facetReq, placement);
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
                           selectionContext, facetName, &req.warnings);
      ownedSelected = builder.canonicalizeFieldValues(
          facetField, selectedValues, true);
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
        facet = luxir::arenaCreate<IntFacetReq>(req.arena, req, facetReq,
            facetField, facetName, limit, minCount, missing, range.min,
            range.max, range.useVector, selectedInts);
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
        if (!facetReq.ops.empty() || !facetReq.sorts.empty()) {
          throw std::runtime_error("facet '" + std::string(facetName) + "': sub-ops/sorts are not yet supported for text field facets");
        }
        facet = luxir::arenaCreate<FullTextFacetReq>(req.arena, req, facetReq,
            facetField, facetName, limit, minCount, missing, selectedStrings);
        break;
      default: ;
    }
    if (facet == nullptr) {
      throw std::runtime_error("Unknown facet field type: " + std::string(facetField));
    }
    return facet;
  }

  FacetReq* createRangeFacetReq(
      std::string_view facetName, const luxir::api::RangeFacet& facetReq,
      OpsPlacement placement) {
    validateSelectionEnvelope(facetName, facetReq, placement);
    std::string_view facetField = facetReq.field;
    if (!facetReq.start.has_value() || !facetReq.end.has_value()) {
      throw std::runtime_error("facet '" + std::string(facetName)
          + "': range start and end are required");
    }
    if (std::holds_alternative<std::monostate>(facetReq.gap_kind)) {
      throw std::runtime_error("facet '" + std::string(facetName)
          + "': range gap or calendar_gap is required");
    }
    if (!facetReq.sorts.empty()) {
      throw std::runtime_error("facet '" + std::string(facetName)
          + "': sorts are not yet supported for range facets");
    }

    auto& fieldType = req.schema->getFieldTypeEx(facetField);
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

    int64_t minCount = facetReq.mincount.value_or(0);
    if (minCount < 0) {
      throw std::runtime_error("facet '" + std::string(facetName)
          + "': mincount must be >= 0");
    }

    // With sub-ops, every bucket carries a domain builder per segment plus
    // per-bucket response slots, so the cap sits far below the plain-counting
    // bucket limit.
    auto checkSubOpBucketCap = [&](const ParsedFences& parsed) {
      if (!facetReq.ops.empty()
          && parsed.values.size() - 1 > MAX_SUBOP_RANGE_BUCKETS) {
        throw std::runtime_error("facet '" + std::string(facetName)
            + "': range facets with sub-ops are limited to "
            + std::to_string(MAX_SUBOP_RANGE_BUCKETS) + " buckets");
      }
    };

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
      double start = floatingValue(*facetReq.start, "start");
      double end = floatingValue(*facetReq.end, "end");
      if (start >= end) {
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
      ParsedFences parsed = makeFloatingFences(
          facetName, fieldType->type(), start, end, gap);
      checkSubOpBucketCap(parsed);
      auto selected = resolveSelectedBuckets(parsed, [&](const api::Val& value) {
        double canonical = floatingValue(value, "selected value");
        return fieldType->type() == FieldType::Type::FLOAT
            ? (int64_t)floatToSortableInt32((float)canonical)
            : doubleToSortableInt64(canonical);
      });
      return luxir::arenaCreate<IntFacetRangeReq>(
          req.arena, req, facetReq, facetField, facetName, parsed.values,
          parsed.affine, parsed.affineGap, fieldType->type(), minCount,
          facetReq.missing, selected);
    }

    int64_t start = fieldType->type() == FieldType::Type::DATE
        ? dateBound(*facetReq.start)
        : fieldType->coerceColInt64(*facetReq.start, facetField, context);
    int64_t end = fieldType->type() == FieldType::Type::DATE
        ? dateBound(*facetReq.end)
        : fieldType->coerceColInt64(*facetReq.end, facetField, context);
    if (start >= end) {
      throw std::runtime_error("facet '" + std::string(facetName)
          + "': range start must be less than end");
    }

    ParsedFences parsed;
    if (const auto* gapValue = std::get_if<api::Val>(&facetReq.gap_kind)) {
      int64_t gap;
      if (fieldType->type() == FieldType::Type::DATE) {
        const auto* millis = std::get_if<int64_t>(&gapValue->kind);
        if (millis == nullptr) {
          throw std::runtime_error("facet '" + std::string(facetName)
              + "': a DATE fixed gap must be integer milliseconds");
        }
        gap = *millis;
      } else {
        gap = fieldType->coerceColInt64(*gapValue, facetField, context);
      }
      if (gap <= 0) {
        throw std::runtime_error("facet '" + std::string(facetName)
            + "': range gap must be > 0");
      }
      parsed = makeAffineFences(facetName, start, end, gap);
    } else {
      parsed = makeCalendarFences(facetName, start, end,
          std::get<api::CalendarGap>(facetReq.gap_kind), zone);
    }
    checkSubOpBucketCap(parsed);

    auto selected = resolveSelectedBuckets(parsed, [&](const api::Val& value) {
      return fieldType->type() == FieldType::Type::DATE
          ? dateBound(value)
          : fieldType->coerceColInt64(value, facetField, context);
    });

    return luxir::arenaCreate<IntFacetRangeReq>(
        req.arena, req, facetReq, facetField, facetName, parsed.values,
        parsed.affine, parsed.affineGap, fieldType->type(), minCount,
        facetReq.missing, selected);
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
        auto fieldTypePtr = req.schema->getFieldTypeEx(root.text);
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
    return luxir::arenaCreate<QueryFacetReq>(
        req.arena, req, facetReq, facetName, plan.bucketQueries, weights);
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
                             parseContext.coerceContext, key, &req.warnings);
        CanonicalValueSet canonical = builder.canonicalizeFieldValues(
            fieldProto->field, selected, true);
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
            repeated SortSpec sorts = 7;
    }
    */

    // Place the Query in the requestPool since it uses things like string_view that directly reference
    // the request.
    ParseContext parseContext{
      req.requestPool, *req.schema, req.arena,
      CoerceContext{req.dateMathNowEpochMillis, *req.timeZone}, name, &req.warnings};
    ProtobufQueryParser parser(parseContext);
    // An absent query selects all documents: the domain is then whatever the
    // filters carve out (browse / filter-only search). Boolean normalization
    // eliminates the match-all when filters fold in beside it.
    Query* query = topDocsReq.query.has_value()
      ? parser.parse(*topDocsReq.query)
      : req.requestPool.make<AllQuery>();
    Query* domainQuery = query;
    int64_t offset = topDocsReq.offset;
    unused(offset); // TODO
    int64_t specifiedLimit = topDocsReq.limit.has_value() ? *topDocsReq.limit : 10;
    // limit to actual number of docs in the index (or all if limit == -1)
    int64_t limit = specifiedLimit < 0 ? req.reader->maxDoc() : std::min(specifiedLimit, req.reader->maxDoc());

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

    auto parsedSorts = parseSorts(topDocsReq.sorts);
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
        && (topDocsReq.get_scores || parsedSorts.rankNeedsScores)) {
      requestFlags |= Query::NEED_SCORES;
    }
    // Competitive-score pruning requires a score-ranked heap. Sub-ops permit
    // pruning because a separate exhaustive windowed pass produces their
    // complete match domain and exact count before the ranking pass. Match-all
    // reuses the incoming domain and its cardinality instead.
    bool allowPruning = requirements.needRankedDocs
        && !parsedSorts.useFieldSort
        && (!requirements.needExactCount || requirements.needExactDomain);
    if (allowPruning) {
      requestFlags |= Query::ALLOW_PRUNING;
    }
    Query::ScoreProfile scoreProfile = luxir::scoreProfile(*query);
    bool exactCountTopK = requirements.needRankedDocs
        && requirements.needExactCount && !requirements.needExactDomain
        && !parsedSorts.useFieldSort
        && parsedSorts.rankNeedsScores;
    bool pureCountShape = !QueryPrep::disableWholeMembershipPlanForTests
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
    bool canOmitForExactDomain = !routedFilters
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
    bool fieldSortRoutesPreplanned = false;
    std::span<uint8_t> cacheFirstFieldSortRoutes;
    bool cacheFirstFieldSortShape =
        placement == TopDocsPlacement::ROOT_OP
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
              *query, *planningContext, *req.reader, parsedSorts, limit,
              true);
      cacheFirstMembershipUse = fieldSortPreflight.acceptedUse;
      cacheFirstFieldSortRoutes = fieldSortPreflight.routes;
      fieldSortRoutesPreplanned = fieldSortPreflight.decided;
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
            *weight, *req.reader, limit);
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
    std::span<uint8_t> wholeFieldSortCacheRoutes =
        cacheFirstFieldSortRoutes;
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
          for (auto& segment : req.reader->segments()) {
            DocSet* rootDomain = segment.liveDocs() == nullptr
                ? nullptr : &segment.liveDocs()->docset();
            if (!wholeMembershipWeight->constantCount(
                    segment, rootDomain).has_value()) {
              everySegmentConstant = false;
              break;
            }
          }
        }
        bool fieldSortFullyRouted = false;
        if (!everySegmentConstant && cache != nullptr && cache->enabled()) {
          bool acquireUse = true;
          if (wholeFieldSort && !readerStableWhole) {
            if (!fieldSortRoutesPreplanned) {
              wholeFieldSortCacheRoutes = req.requestPool.make_span<uint8_t>(
                  req.reader->segments().size());
              acquireUse = TopDocsReq::planFieldSortWholeMembershipRoutes(
                  *query, *wholeMembershipWeight, *req.reader, parsedSorts,
                  limit, wholeFieldSortCacheRoutes);
            } else {
              acquireUse = std::any_of(
                  wholeFieldSortCacheRoutes.begin(),
                  wholeFieldSortCacheRoutes.end(),
                  [](uint8_t routed) { return routed != 0; });
            }
            fieldSortFullyRouted = !acquireUse;
          }
          // An accepted reader-stable value replaces the ANN preparation pass.
          // Its bounded kNN membership is also a valid ladder fallback, while
          // numeric best-first still rechecks exact cardinality per segment.
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
                *countFreeRankingWeight, *req.reader, limit);
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
      countWeight, rankingWeight, limit, std::move(parsedSorts),
      requirements, wholeMembershipWeight, wholeRankingWeight,
      wholeMembershipUse, wholeFieldSortCacheRoutes,
      wholeConstantRanking, scoreProfile,
      (requestFlags & Query::NEED_SCORES) != 0,
      filters, filterWeights, std::move(domainVariants),
      domainQuery, domainQueryWeight,
      domainFilterWeights, residentExactDomainUses);

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
    // Fusion.sources is a map whose values are TopDocs directly (not
    // indirect), so srcProto is the message itself.
    std::vector<TopDocsReq*> sources;
    sources.reserve(fusionProto.sources.size());
    for (auto& [srcName, srcProto] : lastWins(fusionProto.sources)) {  // dedup duplicate source names, last-wins
      size_t idx = sources.size();
      auto* src = parseTopDocs(
          srcName, *srcProto, TopDocsPlacement::FUSION_SOURCE);
      src->rankingSink = [idx](TopDocsReq::Calc& calc, MergeableCollector* mc) {
        static_cast<FusionOp::Calc*>(calc.getParent())->acceptSourceRanking(idx, mc);
      };
      sources.push_back(src);
    }

    ParseContext parseContext{
      req.requestPool, *req.schema, req.arena,
      CoerceContext{req.dateMathNowEpochMillis, *req.timeZone}, name, &req.warnings};
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
    int64_t limit = specifiedLimit < 0 ? req.reader->maxDoc() : std::min(specifiedLimit, req.reader->maxDoc());

    auto* fusion = luxir::arenaCreate<FusionOp>(
      req.arena, req, name, fusionProto, std::move(sources), limit,
      sharedFilters, sharedFilterWeights, rrfK);

    // Sources are children of this FusionOp in the SearchOp tree but not
    // in `subOps` (different lifecycle - they deliver via rankingSink
    // rather than emit).  Set parent so any downstream code that walks up
    // the tree finds the right ancestor.
    for (auto* src : fusion->sources) {
      src->parent = fusion;
    }

    addSubs(*fusion, fusionProto.ops, depth, OpsPlacement::FUSION);
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
