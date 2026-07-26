#pragma once

#include "solux/util/proto.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <variant>
#include <vector>

#include "SearchRequest.h"
#include "ops/RootOp.h"
#include "ops/SearchOp.h"
#include "ops/FacetOp.h"
#include "ops/StrFacetOp.h"
#include "ops/StatsOp.h"
#include "ops/FusionOp.h"
#include "ops/TopDocsReq.h"
#include "solux/query/AllQuery.h"
#include "solux/query/BooleanQuery.h"
#include "solux/query/ForcePrepareQuery.h"
#include "solux/query/ProtobufQueryParser.h"
#include "solux/schema/ValCoerce.h"
#include "solux/util/NumericUtils.h"
#include "solux/util/Overloaded.h"
#include "solux/value/ValueExprParser.h"

namespace solux {

class ProtobufSearchParser {
  SearchRequest& req;
  TopDocsReq* firstQuery = nullptr;
  static constexpr size_t MAX_RANGE_BUCKETS = 100000;

  struct ParsedFences {
    std::span<const int64_t> values;
    int64_t affineGap = 0;
    bool affine = false;
  };

  // The request's named-op maps (SearchRequest.ops, TopDocs.ops, FieldFacet.ops,
  // RangeFacet.ops) all share this non-owning type: a span of (name, SearchOp
  // view) pairs over the request bytes (solux::api::map_view<sv, indirect_view<SearchOp>>).
  using OpsMap = decltype(ReqProto::ops);

  void warnOnce(std::string_view code, const std::string& message) {
    for (const api::Warning& warning : req.warnings) {
      if (warning.code == code && warning.message == message) return;
    }
    char* copy = req.requestPool.alloc(message.size());
    std::memcpy(copy, message.data(), message.size());
    req.warnings.push_back({code, std::string_view(copy, message.size())});
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
        return createRangeFacetReq(name, facetReq);
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

  FacetReq* createRangeFacetReq(
      std::string_view facetName, const solux::api::RangeFacet& facetReq) {
    std::string_view facetField = facetReq.field;
    if (!facetReq.start.has_value() || !facetReq.end.has_value()) {
      throw std::runtime_error("facet '" + std::string(facetName)
          + "': range start and end are required");
    }
    if (std::holds_alternative<std::monostate>(facetReq.gap_kind)) {
      throw std::runtime_error("facet '" + std::string(facetName)
          + "': range gap or calendar_gap is required");
    }
    if (!facetReq.ops.empty() || !facetReq.sorts.empty()) {
      throw std::runtime_error("facet '" + std::string(facetName)
          + "': sub-ops/sorts are not yet supported for range facets");
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
      return solux::arenaCreate<IntFacetRangeReq>(
          req.arena, req, facetReq, facetField, facetName, parsed.values,
          parsed.affine, parsed.affineGap, fieldType->type(), minCount,
          facetReq.missing);
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

    return solux::arenaCreate<IntFacetRangeReq>(
        req.arena, req, facetReq, facetField, facetName, parsed.values,
        parsed.affine, parsed.affineGap, fieldType->type(), minCount,
        facetReq.missing);
  }

  // Build the copyable sort plan. Schema lookups that may throw are resolved
  // here before the plan is passed to TopDocsReq.
  SortPlan parseSorts(std::span<const solux::api::SortSpec> sorts) {
    SortPlan out;
    if (sorts.empty()) return out;
    out.useFieldSort = true;
    out.rankNeedsScores = false;
    for (const auto& sortSpec : sorts) {
      ValueExprOptions options{req.schema.get(), sortSpec.vars};
      ValueProgram* program = ValueExprParser(options, req.arena).parse(sortSpec.expr);
      const ValueNode& root = program->root();
      bool scoreRoot = root.kind == ValueNodeKind::SCORE;
      bool defaultDesc = sortSpec.dir == solux::api::SortSpec_::SortDir::UNKNOWN
        && scoreRoot;
      SortField::SortOrder order =
        sortSpec.dir == solux::api::SortSpec_::SortDir::DESC || defaultDesc
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
              "min(...), max(...), or avg(...)",
              sortSpec.expr, valueTypeName(root.type)));
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

  // Build Weights for a filter list.  Weight ctors that may throw (e.g. KnnQuery
  // dim validation) are resolved here; the resulting span is passed to the op ctor.
  std::span<Query::Weight*> buildFilterWeights(
      std::span<std::pair<std::string_view, Query*>> filters,
      Query::Context& qcontext, int32_t requestFlags) {
    if (filters.empty()) return {};
    // Filters constrain matches but never contribute to score. Preserve any
    // future request flags, but clear NEED_SCORES.
    int32_t filterFlags = requestFlags
        & ~(Query::NEED_SCORES | Query::ALLOW_PRUNING);
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
      req.requestPool, *req.schema, req.arena,
      CoerceContext{req.dateMathNowEpochMillis, *req.timeZone}, name, &req.warnings};
    ProtobufQueryParser parser(parseContext);
    // An absent query selects all documents: the domain is then whatever the
    // filters carve out (browse / filter-only search). Boolean normalization
    // eliminates the match-all when filters fold in beside it.
    Query* query = topDocsReq.query.has_value()
      ? parser.parse(*topDocsReq.query)
      : req.requestPool.make<AllQuery>();
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
    bool countClauseDisabled =
        BooleanQuery::disableFilterClauseCountForTests
        && limit == 0 && topDocsReq.get_number && !topDocsReq.get_scores;
    bool foldFilters = !filters.empty()
        && !TopDocsReq::disableTopDocsFilterFoldForTests
        && !countClauseDisabled;
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
      {}, &req.warnings,
      FilterKeyContext{.schemaGen = req.schema->gen_,
                       .timeZone = req.proto.time_zone},
      req.filterUses);
    // Flags for this request's main query. Filters inherit these after
    // buildFilterWeights clears NEED_SCORES.
    //
    // NEED_SCORES is purely computational (fuzzy match semantics no longer
    // hang off it - the expansion cap is a property of the query).  A
    // count-/domain-only request (limit 0, no get_scores) reads no score, so
    // it skips norms/impacts/BM25 and lets boolean prep pick non-scoring
    // iterators. Ranked requests need scores only when their normalized sort
    // plan consumes score (default ranking, SCORE, or a score-dependent EXPR).
    int32_t requestFlags = 0;
    if (topDocsReq.get_scores || (limit > 0 && parsedSorts.rankNeedsScores)) {
      requestFlags |= Query::NEED_SCORES;
    }
    // Competitive-score pruning requires a score-ranked heap. Sub-ops permit
    // pruning because a separate exhaustive windowed pass produces their
    // complete match domain and exact count before the ranking pass. Match-all
    // reuses the incoming domain and its cardinality instead.
    bool allowPruning = limit > 0 && !parsedSorts.useFieldSort
        && (topDocsReq.ops.empty() ? !topDocsReq.get_number : true);
    if (allowPruning) {
      requestFlags |= Query::ALLOW_PRUNING;
    }
    auto* weight = query->createWeight(*qcontext, requestFlags);
    auto filterWeights = foldFilters
      ? std::span<Query::Weight*>{}
      : buildFilterWeights(filters, *qcontext, requestFlags);

    auto* qr = solux::arenaCreate<TopDocsReq>(
      req.arena, req, name, topDocsReq, *qcontext, query, weight, limit,
      std::move(parsedSorts),
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
      req.requestPool, *req.schema, req.arena,
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
