// HAND-WRITTEN concrete classes for luxir_types.proto (Phase 1 spike).
// Mirrors the non-owning instantiation (gen_real_templated/luxir_types.msg.hpp), de-templatized:
// std::string_view / std::span / map_view / optional_indirect_view / indirect_view / std::optional,
// enums in Foo_ namespaces, no <Traits>, no unknown_fields_. Struct ORDER is a strict topological
// sort: span<const X> and (optional_)indirect_view<X> tolerate an incomplete X (break cycles), but
// std::optional<X>, by-value X (variant arms), and map_view<K,X-by-value> need X complete first.
// The generated luxir_types.pb.cpp / .json.cpp bind pb_meta + glz::meta to these by pointer.
// Members within each struct are ordered by DESCENDING ALIGNMENT to minimize padding; pb_meta
// binds by member pointer + explicit tag, so member declaration order is free / independent of
// the wire tags (reordering does not change serialization).
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include <hpp_proto/field_types.hpp> // bytes_view
#include <hpp_proto/indirect_view.hpp>
#include <hpp_proto/optional_indirect.hpp>
// NOTE: this header is the wire MODEL only - no glaze, no <hpp_proto/json.hpp>. Almost every
// engine TU reaches it through Schema.h / Query.h, and the glaze stack costs ~1.5s of frontend
// per TU. JSON codec metadata (the glz from/to overrides + the dialect) lives in
// luxir_types_json.hpp; include that only in TUs that actually serialize.

// ----- map_view: span-of-pairs with a map read API (last-wins find) -----
namespace luxir::api {
template <class K, class V>
struct map_view : std::span<const std::pair<K, V>> {
  using base = std::span<const std::pair<K, V>>;
  using base::base;
  constexpr map_view(base s) noexcept : base(s) {}
  const V *find(const K &k) const {
    for (auto it = this->rbegin(); it != this->rend(); ++it) {
      if (it->first == k) {
        return &it->second;
      }
    }
    return nullptr;
  }
  bool contains(const K &k) const { return find(k) != nullptr; }
  const V &at(const K &k) const { return *find(k); }
};
} // namespace luxir::api

// ----- WKT: google.protobuf.NullValue (Val.null arm) -----
// Its JSON glz (renders null) is in luxir_types_json.hpp.
namespace google::protobuf {
enum class NullValue { NULL_VALUE = 0 };
} // namespace google::protobuf

namespace luxir::api {

// ---- forward declarations (all messages) ----
struct Target; struct SearchRequest; struct SearchOp; struct GenOp; struct TopDocs;
struct Fusion; struct RrfFusion; struct SortSpec; struct Query;
struct ExistsQuery; struct ConstantScoreQuery; struct BoostQuery; struct RescoreQuery; struct KnnQuery; struct Match; struct NamedQuery; struct BooleanQuery;
struct PrefixQuery; struct WildcardQuery; struct RegexQuery; struct FuzzyQuery; struct PhraseQuery; struct SimpleQuery; struct RangeQuery;
struct GeoBoxQuery; struct GeoDistanceQuery; struct ExprQuery; struct Warning;
struct ExecutionProfile; struct ExecutionProfileOp; struct ExecutionProfilePiece;
struct FieldFacet; struct CalendarGap; struct RangeFacet;
struct Domain; struct SearchResponse; struct DocList; struct FacetResult; struct Bucket;
struct CommitParams; struct UpdateRequest; struct UpdateResponse; struct NamedValue; struct Map;
struct Val; struct ArrVal; struct ArrStr; struct ArrInt; struct ArrFloat;
struct ArrDouble; struct ArrBin; struct ArrArrStr; struct ArrArrInt; struct ArrArrFloat;
struct ArrArrDouble; struct ArrArrBin; struct Vector; struct ArrVector; struct ArrInt32;
struct ColStr; struct Column; struct ColVector; struct MultiVector; struct ColInt;
struct ColFloat; struct ColDouble; struct ColMap; struct IndexInfo; struct AuxIndexInfo;
struct SegmentInfo; struct AnalyzerDef; struct FieldDef; struct SchemaDef;
struct SchemaRequest; struct SchemaResponse;
struct CreateCollectionRequest; struct CreateCollectionResponse;
struct DeleteCollectionRequest; struct DeleteCollectionResponse;
struct StatsRequest; struct StatsResponse; struct StatsTotals; struct CollectionStats;
struct ShardStats; struct IndexStats; struct SegmentStats; struct AuxStats;
struct FilterCacheStats; struct IndexRamStats;
namespace UpdateResponse_ { struct Error; }

// ---- nested enums (Foo_ namespace; matches generated metadata refs) ----
namespace SortSpec_ { enum class SortDir { UNKNOWN = 0, ASC = 1, DESC = 2 }; }
namespace Match_ { enum class Operator { OPERATOR_UNSPECIFIED = 0, OR = 1, AND = 2 }; }
namespace CalendarGap_ {
enum class Unit { UNKNOWN = 0, DAY = 1, WEEK = 2, MONTH = 3, QUARTER = 4, YEAR = 5 };
}
namespace UpdateResponse_ { enum class Status { UNKNOWN = 0, OK = 1, PARTIAL = 2, ERROR = 3 }; }
enum class VectorMetric { NONE = 0, L2 = 1, IP = 2, COSINE = 3 };
enum class DocFormat { DEFAULT = 0, ROWS = 1, COLUMNS = 2 };
enum class ResponseFormat { ENVELOPE = 0, DOCS = 1 };
namespace FieldDef_ {
enum class FieldClass { STRING = 0, TEXT = 1, INT = 2, FLOAT = 3, DOUBLE = 4, BIN = 5, ID = 6, VECTOR = 7, DATE = 8, GEO_POINT = 9 };
enum class IndexMode { NONE = 0, MATCH = 1, RANGE = 2 };
}
namespace SchemaRequest_ { enum class Mode { SET = 0, REPLACE_ALL = 1 }; }

// ===================== message definitions (strict topological order) =====================

struct Target { std::span<const std::string_view> name; };
struct RrfFusion { int32_t k = 0; };
struct CalendarGap {
  using Unit = luxir::api::CalendarGap_::Unit;
  int32_t n = 0;
  Unit unit = Unit::UNKNOWN;
};
struct SortSpec {
  using SortDir = luxir::api::SortSpec_::SortDir;
  std::string_view expr;
  map_view<std::string_view, ::hpp_proto::indirect_view<Val>> vars;
  SortDir dir = SortDir::UNKNOWN;
};
struct ExistsQuery { std::string_view field; };
struct PrefixQuery { std::string_view field; std::string_view prefix; };
struct WildcardQuery { std::string_view field; std::string_view pattern; };
struct RegexQuery { std::string_view field; std::string_view pattern; };
struct FuzzyQuery {
  std::string_view field;
  std::string_view term;
  std::optional<std::int32_t> max_edits;
  std::optional<std::int32_t> prefix_length;
  int32_t max_expansions = 0;
};
struct PhraseQuery {
  std::string_view field;
  std::string_view text;
  std::span<const std::string_view> words;
  std::span<const std::string_view> terms;
  std::span<const ::hpp_proto::bytes_view> terms_bin;
  int32_t slop = 0;
  std::span<const std::int32_t> positions;
};
struct CommitParams {
  uint64_t commit_within_us = 0;
  std::span<const std::string_view> build_aux_indexes;
  bool wait_for_merges = false;
  uint32_t max_segments = 0;
};

namespace UpdateResponse_ {
struct Error { std::string_view id; std::string_view error_message; int32_t index = 0; };
} // namespace UpdateResponse_

struct UpdateResponse {
  using Status = luxir::api::UpdateResponse_::Status;
  using Error = luxir::api::UpdateResponse_::Error;
  std::string_view request_id;
  uint64_t update_version = 0;
  std::span<const std::string_view> ids;
  std::span<const Error> errors;
  std::string_view error_message;
  Status status = Status::UNKNOWN;                              // align 4 (enum)
};

struct ArrStr { std::span<const std::string_view> v; };
struct ArrInt { std::span<const std::int64_t> v; };
struct ArrFloat { std::span<const float> v; };
struct ArrDouble { std::span<const double> v; };
struct ArrBin { std::span<const ::hpp_proto::bytes_view> v; };
struct ArrInt32 { std::span<const std::int32_t> v; };

struct ColStr { std::string_view missing_val; std::span<const std::string_view> v; };
struct ColInt { int64_t missing_val = 0; std::span<const std::int64_t> v; };
struct ColFloat { float missing_val = 0.0f; std::span<const float> v; };
struct ColDouble { double missing_val = 0.0; std::span<const double> v; };

struct AuxIndexInfo {
  std::string_view kind;
  std::string_view field;
  std::string_view name;
  uint64_t gen = 0;
  uint64_t commit_time = 0;
  std::span<const std::string_view> files;
  ::hpp_proto::bytes_view opaque_meta;
  uint64_t built_core_gen = 0;
};
struct AnalyzerDef { std::string_view tokenizer; std::span<const std::string_view> filters; };
struct FieldDef {
  using FieldClass = luxir::api::FieldDef_::FieldClass;
  using IndexMode = luxir::api::FieldDef_::IndexMode;
  using Metric = luxir::api::VectorMetric;
  std::string_view parent;
  std::optional<AnalyzerDef> analyzer;                          // align 8
  std::string_view stored_resource;
  std::optional<FieldClass> type;                               // align 4 (enum)
  std::optional<IndexMode> index;                               // align 4 (enum)
  std::optional<std::int32_t> dims;                             // align 4
  std::optional<Metric> metric;                                 // align 4 (enum)
  std::optional<bool> column;                                   // align 1 (optional<bool>s)
  std::optional<bool> multi;
  std::optional<bool> stored;
  std::optional<bool> normalized;
  std::optional<bool> normalize_on_write;
};
struct SegmentInfo {
  uint64_t seg_id = 0;
  uint64_t live_gen = 0;
  uint64_t min_version = 0;
  uint64_t max_version = 0;
  uint64_t commit_time = 0;
  uint64_t schema_gen = 0;
  std::span<const AuxIndexInfo> overlays;
  int32_t max_doc = 0;
  int32_t live_docs = 0;
};
struct IndexInfo {
  uint64_t version = 0;
  uint64_t commit_time = 0;
  uint64_t index_gen = 0;
  uint64_t core_gen = 0;
  uint64_t update_version = 0;
  uint64_t schema_gen = 0;
  std::span<const SegmentInfo> segments;
  std::span<const AuxIndexInfo> aux_indexes;
};

struct AuxStats {
  std::string_view kind;
  std::string_view field;
  std::string_view name;
  uint64_t gen = 0;
  uint64_t commit_time = 0;
  uint64_t built_core_gen = 0;
  uint64_t bytes = 0;
  std::span<const std::string_view> files;
};
struct FilterCacheStats {
  uint64_t max_bytes = 0;
  uint64_t resident_bytes = 0;
  uint64_t metadata_bytes = 0;
  uint64_t hits = 0;
  uint64_t misses = 0;
  uint64_t admissions = 0;
  uint64_t builds = 0;
  uint64_t byproduct_inserts = 0;
  uint64_t publish_rejects = 0;
  uint64_t evictions = 0;
  uint64_t purges = 0;
  uint64_t oversized_key_bypasses = 0;
  uint64_t reader_stable_hits = 0;
  uint64_t reader_stable_refreshes = 0;
  uint64_t reader_stable_retires = 0;
  bool enabled = false;
};
struct IndexRamStats { uint64_t limit_bytes = 0; uint64_t reserved_bytes = 0; };
struct StatsTotals {
  uint64_t collections = 0;
  uint64_t shards = 0;
  uint64_t segments = 0;
  uint64_t committed_segments = 0;
  uint64_t max_docs = 0;
  uint64_t live_docs = 0;
  uint64_t deleted_docs = 0;
  uint64_t bytes = 0;
};
struct SegmentStats {
  uint64_t seg_id = 0;
  uint64_t live_gen = 0;
  uint64_t min_update_version = 0;
  uint64_t max_update_version = 0;
  uint64_t first_commit_time = 0;
  uint64_t schema_gen = 0;
  uint64_t bytes = 0;
  std::span<const AuxStats> overlays;
  int32_t max_doc = 0;
  int32_t live_docs = 0;
  int32_t deleted_docs = 0;
  uint32_t merge_level = 0;
  bool committed = false;
  bool merging = false;
};
struct IndexStats {
  StatsTotals totals;
  uint64_t commit_time = 0;
  uint64_t index_gen = 0;
  uint64_t core_gen = 0;
  uint64_t update_version = 0;
  uint64_t schema_gen = 0;
  uint64_t active_merges = 0;
  std::span<const AuxStats> aux_indexes;
  FilterCacheStats filter_cache;
  std::span<const SegmentStats> segments;
};
struct ShardStats {
  IndexStats index;
  uint32_t shard_id = 0;
};
struct CollectionStats {
  std::string_view name;
  StatsTotals totals;
  uint64_t schema_gen = 0;
  std::span<const ShardStats> shards;
  std::string_view error;
};
struct StatsRequest { std::optional<Target> collection; bool segments = false; };
struct StatsResponse {
  StatsTotals totals;
  std::span<const CollectionStats> collections;
  IndexRamStats index_ram;
};

struct Vector { std::optional<ArrFloat> f32; };                  // needs ArrFloat
struct ArrArrBin { std::span<const ArrBin> v; };
struct ArrArrStr { std::span<const ArrStr> v; };
struct ArrArrInt { std::span<const ArrInt> v; };
struct ArrArrFloat { std::span<const ArrFloat> v; };
struct ArrArrDouble { std::span<const ArrDouble> v; };
struct KnnQuery {                                                // needs Vector
  std::string_view field;
  std::optional<Vector> query;
  int32_t k = 0;
  int32_t nprobe = 0;
  int32_t refine_candidates = 0;
  float min_scan_fraction = 0.0f;
  bool exact = false;
};
struct SchemaDef {
  map_view<std::string_view, FieldDef> fields;
  map_view<std::string_view, FieldDef> templates;
};
struct ColVector { std::span<const Vector> v; };
struct ArrVector { std::span<const Vector> v; };
struct SchemaResponse { std::optional<SchemaDef> schema; };      // needs SchemaDef
struct SchemaRequest {                                           // needs Target, SchemaDef
  using Mode = luxir::api::SchemaRequest_::Mode;
  std::optional<Target> collection;
  std::optional<SchemaDef> schema;
  Mode mode = Mode::SET;                                      // align 4 (enum)
};
struct CreateCollectionRequest { std::string_view name; std::optional<SchemaDef> schema; };
struct CreateCollectionResponse { std::string_view name; };
struct DeleteCollectionRequest { std::string_view name; };
struct DeleteCollectionResponse { std::string_view name; };
struct MultiVector { std::span<const ArrVector> v; };
struct ColMap { std::span<const Map> v; };                       // span<incomplete Map> OK
struct ArrVal { std::span<const Val> v; };                       // span<incomplete Val> OK

struct Column {                                                  // variant arms all complete above
  std::variant<std::monostate, ColStr, ColInt, ColFloat, ColDouble, ArrArrStr, ArrArrInt,
               ArrArrFloat, ArrArrDouble, ColMap, ArrVal, ColVector, MultiVector>
      kind;
};

struct BooleanQuery {                                            // span<incomplete Query> OK
  std::span<const Query> filter;
  std::span<const Query> required;
  std::span<const Query> optional;
  std::span<const Query> prohibited;
  int32_t min_match = 0;
};
struct TopDocs {                                                 // all indirect/span/opt-scalar
  ::hpp_proto::optional_indirect_view<Query> query;
  std::span<const NamedQuery> filter;
  int64_t offset = 0;
  std::optional<std::int64_t> limit;
  std::span<const std::string_view> fields;
  std::span<const SortSpec> sorts;
  map_view<std::string_view, ::hpp_proto::indirect_view<SearchOp>> ops;
  int32_t batch_size = 0;
  DocFormat document_format = DocFormat::DEFAULT;                // align 4 (enum)
  bool get_number = false;
  bool get_scores = false;
};
struct Fusion {                                                  // needs TopDocs, RrfFusion
  map_view<std::string_view, TopDocs> sources;
  std::span<const NamedQuery> filter;
  std::optional<std::int64_t> limit;
  int64_t offset = 0;
  std::span<const std::string_view> fields;
  map_view<std::string_view, ::hpp_proto::indirect_view<SearchOp>> ops;
  std::optional<RrfFusion> rrf;                                  // align 4 (RrfFusion is one int32)
  int32_t batch_size = 0;
  DocFormat document_format = DocFormat::DEFAULT;                // align 4 (enum)
  bool get_number = false;
  bool get_scores = false;
};
struct Domain {
  std::string_view op_name;
  ::hpp_proto::optional_indirect_view<Query> root;
  std::span<const std::string_view> include_filter;
  std::span<const std::string_view> exclude_filter;
  std::span<const NamedQuery> filter;
};
struct GenOp { std::string_view name; std::span<const Val> args; };
struct FieldFacet {
  std::string_view field;
  std::optional<std::int64_t> limit;
  std::optional<std::int64_t> mincount;
  std::span<const SortSpec> sorts;
  map_view<std::string_view, ::hpp_proto::indirect_view<SearchOp>> ops;
  bool missing = false;
};
struct SearchRequest {                                          // needs Target
  std::string_view request_id;
  std::optional<Target> collection;
  map_view<std::string_view, ::hpp_proto::indirect_view<SearchOp>> ops;
  uint64_t freshness_us = 0;
  std::string_view time_zone;
  ResponseFormat response_format = ResponseFormat::ENVELOPE;
  bool profile = false;
  std::int32_t max_parallel = 0;
};
struct Map { map_view<std::string_view, ::hpp_proto::indirect_view<Val>> fields; };
struct Bucket {
  ::hpp_proto::optional_indirect_view<Val> bucket_id;
  map_view<std::string_view, ::hpp_proto::indirect_view<Val>> ops;
};
struct DocList {                                                // needs Column (map by value)
  std::optional<std::int64_t> found;
  map_view<std::string_view, Column> columns;
  // Per-document field maps: docs[i] holds document i's fields not in
  // columns (row_count entries when present).  See the .proto contract.
  std::span<const Map> docs;
  int64_t offset = 0;
  map_view<std::string_view, ::hpp_proto::indirect_view<Val>> ops;
  int32_t row_count = 0;                                       // align 4
  bool more = false;
};
struct FacetResult {                                           // needs Column (optional)
  std::optional<std::int64_t> total_buckets;
  std::optional<Column> bucket_ids;
  std::span<const std::int64_t> counts;
  std::optional<std::int64_t> missing;
  map_view<std::string_view, ::hpp_proto::indirect_view<Val>> ops;
  int64_t offset = 0;
};
struct Val {                                                   // needs Map,ArrVal,Arr*,Vector,ArrVector,DocList,FacetResult
  std::variant<std::monostate, google::protobuf::NullValue, std::string_view, std::int64_t,
               double, float, bool, ::hpp_proto::bytes_view, Map, ArrVal, ArrStr, ArrInt,
               ArrFloat, ArrDouble, ArrBin, Vector, ArrVector, DocList, FacetResult>
      kind;
  // Named accessors for the common result arms. The variant `kind` is itself a fine public
  // surface for everything else (std::get_if<T>(&kind) / std::holds_alternative<T>(kind)).
  const DocList *docList() const { return std::get_if<DocList>(&kind); }
  const FacetResult *facetResult() const { return std::get_if<FacetResult>(&kind); }
  bool isNull() const {
    return std::holds_alternative<std::monostate>(kind) ||
           std::holds_alternative<google::protobuf::NullValue>(kind);
  }
  std::int64_t asInt() const { return std::get<std::int64_t>(kind); }
  double asDouble() const { return std::get<double>(kind); }
  float asFloat() const { return std::get<float>(kind); }
  bool asBool() const { return std::get<bool>(kind); }
  std::string_view asString() const { return std::get<std::string_view>(kind); }
};
struct RangeFacet {                                             // needs Val,CalendarGap
  std::variant<std::monostate, Val, CalendarGap> gap_kind;
  std::string_view field;
  ::hpp_proto::optional_indirect_view<Val> start;
  ::hpp_proto::optional_indirect_view<Val> end;
  std::optional<std::int64_t> mincount;
  std::span<const SortSpec> sorts;
  map_view<std::string_view, ::hpp_proto::indirect_view<SearchOp>> ops;
  std::string_view time_zone;
  bool missing = false;
};
struct SearchOp {                                               // needs TopDocs,Fusion,FieldFacet,RangeFacet,GenOp
  std::variant<std::monostate, TopDocs, Fusion, FieldFacet, RangeFacet, GenOp> kind;
};
struct Warning { std::string_view code; std::string_view message; };
struct ExecutionProfilePiece {
  std::string_view kind;
  std::string_view strategy;
  std::span<const std::string_view> details;
  std::optional<std::int64_t> cardinality;
  std::optional<std::int64_t> domain_size;
  std::int64_t thread_id = 0;
  std::uint64_t elapsed_us = 0;
  std::int32_t segment = 0;
  std::int32_t max_doc = 0;
};
struct ExecutionProfileOp {
  std::string_view name;
  std::span<const ExecutionProfilePiece> pieces;
};
struct ExecutionProfile { std::span<const ExecutionProfileOp> ops; };
struct SearchResponse {
  std::string_view request_id;
  map_view<std::string_view, ::hpp_proto::indirect_view<Val>> ops;
  std::string_view error;
  std::span<const Warning> warnings;
  std::optional<ExecutionProfile> profile;
  bool more = false;
};
struct NamedValue { std::string_view name; ::hpp_proto::optional_indirect_view<Val> val; };
struct Match {
  using Operator = luxir::api::Match_::Operator;
  std::string_view field;
  ::hpp_proto::optional_indirect_view<Val> val;
  Operator operator_ = Operator::OPERATOR_UNSPECIFIED;
  int32_t min_match = 0;
};
struct ConstantScoreQuery {
  ::hpp_proto::optional_indirect_view<Query> query;
  std::optional<float> score;
};
struct BoostQuery {
  ::hpp_proto::optional_indirect_view<Query> query;
  std::optional<float> boost;
};
struct RescoreQuery {
  ::hpp_proto::optional_indirect_view<Query> query;
  std::string_view expr;
  map_view<std::string_view, ::hpp_proto::indirect_view<Val>> vars;
};
struct SimpleQuery {
  using Operator = luxir::api::Match_::Operator;
  std::string_view q;
  std::span<const std::string_view> fields;
  std::span<const std::string_view> allowed_fields;
  Operator operator_ = Operator::OPERATOR_UNSPECIFIED;
  int32_t min_match = 0;
};
struct RangeQuery {                                            // needs Val
  std::string_view field;
  ::hpp_proto::optional_indirect_view<Val> gte;
  ::hpp_proto::optional_indirect_view<Val> gt;
  ::hpp_proto::optional_indirect_view<Val> lte;
  ::hpp_proto::optional_indirect_view<Val> lt;
};
struct GeoBoxQuery {
  std::string_view field;
  double min_lat = 0.0;
  double max_lat = 0.0;
  double min_lon = 0.0;
  double max_lon = 0.0;
};
struct GeoDistanceQuery {
  std::string_view field;
  double lat = 0.0;
  double lon = 0.0;
  double radius_meters = 0.0;
};
struct ExprQuery {                                             // map value indirect: Val may be incomplete, but is complete here anyway
  std::string_view q;
  map_view<std::string_view, ::hpp_proto::indirect_view<Val>> vars;
};
struct Query {                                                 // needs Match,BooleanQuery,Exists,Phrase,Knn,ConstantScore,Prefix,Fuzzy,Simple,Range,Expr,GeoBox,GeoDistance,Boost,Rescore
  std::variant<std::monostate, Match, BooleanQuery, bool, ExistsQuery, PhraseQuery, KnnQuery,
               ConstantScoreQuery, PrefixQuery, FuzzyQuery, SimpleQuery, RangeQuery, ExprQuery,
               GeoBoxQuery, GeoDistanceQuery, BoostQuery, RescoreQuery, WildcardQuery, RegexQuery>
      kind;
};
struct NamedQuery { std::string_view name; ::hpp_proto::optional_indirect_view<Query> query; };
struct UpdateRequest {                                         // needs Target,Column,CommitParams
  std::string_view request_id;
  int64_t stream_id = 0;
  std::optional<Target> collection;
  // Same pair, same contract as DocList: document i is columns row i merged
  // with docs[i]; a field name never appears in both.
  std::span<const Map> docs;
  map_view<std::string_view, Column> columns;
  std::span<const std::string_view> delete_ids;
  std::optional<CommitParams> commit;
  bool allow_dups = false;
  bool all_or_none = false;
  bool return_ids = false;
};

// ===================== trivial-destructibility checks =====================
#define LUXIR_TD(M) static_assert(std::is_trivially_destructible_v<M>);
LUXIR_TD(Target) LUXIR_TD(SearchRequest) LUXIR_TD(SearchOp) LUXIR_TD(GenOp) LUXIR_TD(TopDocs)
LUXIR_TD(Fusion) LUXIR_TD(RrfFusion) LUXIR_TD(SortSpec) LUXIR_TD(Query)
LUXIR_TD(ExistsQuery)
LUXIR_TD(ConstantScoreQuery) LUXIR_TD(BoostQuery) LUXIR_TD(RescoreQuery) LUXIR_TD(KnnQuery) LUXIR_TD(Match) LUXIR_TD(NamedQuery) LUXIR_TD(BooleanQuery)
LUXIR_TD(PrefixQuery) LUXIR_TD(WildcardQuery) LUXIR_TD(RegexQuery) LUXIR_TD(FuzzyQuery) LUXIR_TD(PhraseQuery) LUXIR_TD(SimpleQuery) LUXIR_TD(RangeQuery)
LUXIR_TD(GeoBoxQuery) LUXIR_TD(GeoDistanceQuery) LUXIR_TD(ExprQuery)
LUXIR_TD(Warning) LUXIR_TD(ExecutionProfile) LUXIR_TD(ExecutionProfileOp)
LUXIR_TD(ExecutionProfilePiece) LUXIR_TD(FieldFacet) LUXIR_TD(CalendarGap) LUXIR_TD(RangeFacet)
LUXIR_TD(Domain) LUXIR_TD(SearchResponse) LUXIR_TD(DocList) LUXIR_TD(FacetResult) LUXIR_TD(Bucket)
LUXIR_TD(CommitParams) LUXIR_TD(UpdateRequest) LUXIR_TD(UpdateResponse) LUXIR_TD(NamedValue) LUXIR_TD(Map)
LUXIR_TD(Val) LUXIR_TD(ArrVal) LUXIR_TD(ArrStr) LUXIR_TD(ArrInt) LUXIR_TD(ArrFloat)
LUXIR_TD(ArrDouble) LUXIR_TD(ArrBin) LUXIR_TD(ArrArrStr) LUXIR_TD(ArrArrInt) LUXIR_TD(ArrArrFloat)
LUXIR_TD(ArrArrDouble) LUXIR_TD(ArrArrBin) LUXIR_TD(Vector) LUXIR_TD(ArrVector) LUXIR_TD(ArrInt32)
LUXIR_TD(ColStr) LUXIR_TD(Column) LUXIR_TD(ColVector) LUXIR_TD(MultiVector) LUXIR_TD(ColInt)
LUXIR_TD(ColFloat) LUXIR_TD(ColDouble) LUXIR_TD(ColMap) LUXIR_TD(IndexInfo) LUXIR_TD(AuxIndexInfo)
LUXIR_TD(SegmentInfo) LUXIR_TD(AnalyzerDef) LUXIR_TD(FieldDef) LUXIR_TD(SchemaDef)
LUXIR_TD(SchemaRequest) LUXIR_TD(SchemaResponse) LUXIR_TD(UpdateResponse_::Error)
LUXIR_TD(CreateCollectionRequest) LUXIR_TD(CreateCollectionResponse)
LUXIR_TD(DeleteCollectionRequest) LUXIR_TD(DeleteCollectionResponse)
LUXIR_TD(StatsRequest) LUXIR_TD(StatsResponse) LUXIR_TD(StatsTotals) LUXIR_TD(CollectionStats)
LUXIR_TD(ShardStats) LUXIR_TD(IndexStats) LUXIR_TD(SegmentStats) LUXIR_TD(AuxStats)
LUXIR_TD(FilterCacheStats) LUXIR_TD(IndexRamStats)
#undef LUXIR_TD

// ===================== out-of-line codec entry-point declarations =====================
// decode() uses hpp_proto::padded_input. Callers must pass a payload-only span
// with 16 readable bytes past data.end(), and the first one must be zero.
#define LUXIR_ENTRY(M)                                                                      \
  bool decode(M &, std::span<const std::byte> data, std::pmr::memory_resource &arena);      \
  bool encode(const M &, std::vector<std::byte> &out);                                      \
  bool write_json(const M &, std::string &out);                                             \
  bool read_json(M &, std::string_view json, std::pmr::memory_resource &arena,              \
                 std::string *error = nullptr);
LUXIR_ENTRY(Target) LUXIR_ENTRY(SearchRequest) LUXIR_ENTRY(SearchOp) LUXIR_ENTRY(GenOp)
LUXIR_ENTRY(TopDocs) LUXIR_ENTRY(Fusion) LUXIR_ENTRY(RrfFusion) LUXIR_ENTRY(SortSpec)
LUXIR_ENTRY(Query) LUXIR_ENTRY(ExistsQuery) LUXIR_ENTRY(ConstantScoreQuery) LUXIR_ENTRY(BoostQuery) LUXIR_ENTRY(RescoreQuery)
LUXIR_ENTRY(KnnQuery) LUXIR_ENTRY(Match) LUXIR_ENTRY(NamedQuery) LUXIR_ENTRY(BooleanQuery)
LUXIR_ENTRY(PrefixQuery) LUXIR_ENTRY(WildcardQuery) LUXIR_ENTRY(RegexQuery) LUXIR_ENTRY(FuzzyQuery) LUXIR_ENTRY(PhraseQuery) LUXIR_ENTRY(SimpleQuery)
LUXIR_ENTRY(RangeQuery) LUXIR_ENTRY(GeoBoxQuery) LUXIR_ENTRY(GeoDistanceQuery) LUXIR_ENTRY(ExprQuery)
LUXIR_ENTRY(Warning) LUXIR_ENTRY(ExecutionProfile) LUXIR_ENTRY(ExecutionProfileOp)
LUXIR_ENTRY(ExecutionProfilePiece) LUXIR_ENTRY(FieldFacet)
LUXIR_ENTRY(CalendarGap) LUXIR_ENTRY(RangeFacet) LUXIR_ENTRY(Domain) LUXIR_ENTRY(SearchResponse) LUXIR_ENTRY(DocList)
LUXIR_ENTRY(FacetResult) LUXIR_ENTRY(Bucket) LUXIR_ENTRY(CommitParams) LUXIR_ENTRY(UpdateRequest)
LUXIR_ENTRY(UpdateResponse) LUXIR_ENTRY(NamedValue) LUXIR_ENTRY(Map)
LUXIR_ENTRY(Val) LUXIR_ENTRY(ArrVal) LUXIR_ENTRY(ArrStr) LUXIR_ENTRY(ArrInt) LUXIR_ENTRY(ArrFloat)
LUXIR_ENTRY(ArrDouble) LUXIR_ENTRY(ArrBin) LUXIR_ENTRY(ArrArrStr) LUXIR_ENTRY(ArrArrInt)
LUXIR_ENTRY(ArrArrFloat) LUXIR_ENTRY(ArrArrDouble) LUXIR_ENTRY(ArrArrBin) LUXIR_ENTRY(Vector)
LUXIR_ENTRY(ArrVector) LUXIR_ENTRY(ArrInt32) LUXIR_ENTRY(ColStr) LUXIR_ENTRY(Column)
LUXIR_ENTRY(ColVector) LUXIR_ENTRY(MultiVector) LUXIR_ENTRY(ColInt) LUXIR_ENTRY(ColFloat)
LUXIR_ENTRY(ColDouble) LUXIR_ENTRY(ColMap) LUXIR_ENTRY(IndexInfo) LUXIR_ENTRY(AuxIndexInfo)
LUXIR_ENTRY(SegmentInfo) LUXIR_ENTRY(AnalyzerDef) LUXIR_ENTRY(FieldDef)
LUXIR_ENTRY(SchemaDef) LUXIR_ENTRY(SchemaRequest) LUXIR_ENTRY(SchemaResponse)
LUXIR_ENTRY(CreateCollectionRequest) LUXIR_ENTRY(CreateCollectionResponse)
LUXIR_ENTRY(DeleteCollectionRequest) LUXIR_ENTRY(DeleteCollectionResponse)
LUXIR_ENTRY(StatsRequest) LUXIR_ENTRY(StatsResponse) LUXIR_ENTRY(StatsTotals)
LUXIR_ENTRY(CollectionStats) LUXIR_ENTRY(ShardStats) LUXIR_ENTRY(IndexStats)
LUXIR_ENTRY(SegmentStats) LUXIR_ENTRY(AuxStats) LUXIR_ENTRY(FilterCacheStats)
LUXIR_ENTRY(IndexRamStats)
#undef LUXIR_ENTRY

} // namespace luxir::api
