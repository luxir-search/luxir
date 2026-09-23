// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

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
struct SearchRequest; struct SearchOp; struct ExprOp; struct TopDocs;
struct Fusion; struct RrfFusion; struct SortSpec; struct Query;
struct ExistsQuery; struct ConstantScoreQuery; struct BoostQuery; struct RescoreQuery; struct KnnQuery; struct Match; struct AnyOfQuery; struct Filter; struct BooleanQuery;
struct PrefixQuery; struct WildcardQuery; struct RegexQuery; struct FuzzyQuery; struct PhraseQuery; struct SimpleQuery; struct RangeQuery;
struct GeoBoxQuery; struct GeoDistanceQuery; struct ExprQuery; struct Warning; struct Error;
struct ExecutionProfile; struct ExecutionProfileOp; struct ExecutionProfilePiece;
struct FieldFacet; struct CalendarGap; struct RangeFacet; struct QueryBucket; struct QueryFacet;
struct Domain; struct SearchResponse; struct DocList; struct FacetResult;
struct CommitParams; struct UpdateRequest; struct UpdateResponse; struct Map;
struct Val; struct ArrVal; struct ArrStr; struct ArrInt; struct ArrFloat;
struct ArrDouble; struct ArrBin; struct ArrArrStr; struct ArrArrInt; struct ArrArrFloat;
struct ArrArrDouble; struct Vector; struct ArrVector;
struct ColStr; struct Column; struct ColVector; struct MultiVector; struct ColInt;
struct ColFloat; struct ColDouble; struct AnalyzerComponent; struct AnalyzerDef; struct FieldDef; struct SchemaDef;
struct FieldVariants; struct FieldDefaults; struct NormalizerDef;
struct SchemaRequest; struct SchemaResponse;
struct CreateCollectionRequest; struct CreateCollectionResponse;
struct DeleteCollectionRequest; struct DeleteCollectionResponse; struct ListCollectionsResponse;
struct StatsRequest; struct StatsResponse; struct StatsTotals; struct CollectionStats;
struct ShardStats; struct IndexStats; struct SegmentStats; struct AuxStats;
struct QueryCacheStats; struct IndexRamStats;
struct CacheControlRequest; struct CacheControlResponse; struct CacheEntryDump;
struct ShardCacheControl; struct CollectionCacheControl;
namespace UpdateResponse_ { struct DocError; }
namespace KnnQuery_ { struct Ivf; }

// ---- nested enums (Foo_ namespace; matches generated metadata refs) ----
namespace Error_ {
enum class Kind {
  UNKNOWN = 0, INVALID_REQUEST = 1, NOT_FOUND = 2, ALREADY_EXISTS = 3, FAILED_PRECONDITION = 4,
  RESOURCE_EXHAUSTED = 5, UNAVAILABLE = 6, INTERNAL = 7
};
}
namespace SortSpec_ { enum class SortDir { UNKNOWN = 0, ASC = 1, DESC = 2 }; }
namespace Match_ { enum class Operator { OPERATOR_UNSPECIFIED = 0, OR = 1, AND = 2 }; }
namespace CalendarGap_ {
enum class Unit { UNKNOWN = 0, DAY = 1, WEEK = 2, MONTH = 3, QUARTER = 4, YEAR = 5 };
}
namespace UpdateResponse_ { enum class Status { UNKNOWN = 0, OK = 1, PARTIAL = 2, ERROR = 3 }; }
enum class VectorMetric { NONE = 0, L2 = 1, IP = 2, COSINE = 3 };
enum class DocFormat { DEFAULT = 0, ROWS = 1, COLUMNS = 2 };
enum class ResponseFormat { ENVELOPE = 0, DOCS = 1 };
enum class SelectionMode { ANY = 0, ALL = 1 };
namespace FieldDef_ {
enum class FieldClass { STRING = 0, TEXT = 1, INT = 2, FLOAT = 3, DOUBLE = 4, ID = 6, VECTOR = 7, DATE = 8, GEO_POINT = 9 };
enum class IndexMode { NONE = 0, MATCH = 1, RANGE = 2 };
enum class LongTerms { TRUNCATE = 0, REJECT = 1, HASH128 = 2 };
}
namespace SchemaRequest_ { enum class Mode { SET = 0, REPLACE_ALL = 1 }; }

// ===================== message definitions (strict topological order) =====================

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
  uint64_t commit_within_ms = 0;
  std::span<const std::string_view> build_aux_indexes;
  bool wait_for_merges = false;
  uint32_t max_segments = 0;
};

struct Error {
  using Kind = luxir::api::Error_::Kind;
  std::string_view code;
  std::string_view message;
  Kind kind = Kind::UNKNOWN;                                    // align 4 (enum)
};

namespace UpdateResponse_ {
struct DocError { std::string_view id; std::optional<Error> error; int32_t index = 0; };
} // namespace UpdateResponse_

struct UpdateResponse {
  using Status = luxir::api::UpdateResponse_::Status;
  using DocError = luxir::api::UpdateResponse_::DocError;
  std::string_view commit;
  std::string_view request_id;
  uint64_t update_version = 0;
  std::span<const std::string_view> ids;
  std::span<const DocError> errors;
  int64_t total_errors = 0;
  std::optional<Error> error;
  Status status = Status::UNKNOWN;                              // align 4 (enum)
};

struct ArrStr { std::span<const std::string_view> v; };
struct ArrInt { std::span<const std::int64_t> v; };
struct ArrFloat { std::span<const float> v; };
struct ArrDouble { std::span<const double> v; };
struct ArrBin { std::span<const ::hpp_proto::bytes_view> v; };
struct ColStr { std::string_view missing_val; std::span<const std::string_view> v; };
struct ColInt { int64_t missing_val = 0; std::span<const std::int64_t> v; };
struct ColFloat { float missing_val = 0.0f; std::span<const float> v; };
struct ColDouble { double missing_val = 0.0; std::span<const double> v; };

struct AnalyzerComponent {
  std::string_view name;
  map_view<std::string_view, ::hpp_proto::indirect_view<Val>> params;
};
struct AnalyzerDef {                                              // needs AnalyzerComponent
  std::optional<AnalyzerComponent> tokenizer;
  std::span<const AnalyzerComponent> filters;
};
struct FieldVariants {
  map_view<std::string_view, ::hpp_proto::indirect_view<FieldDef>> entries;
};
struct FieldDefaults {
  std::optional<std::string_view> search;
  std::optional<std::string_view> value;
};
struct NormalizerDef {
  std::span<const AnalyzerComponent> filters;
};
struct FieldDef {
  using FieldClass = luxir::api::FieldDef_::FieldClass;
  using IndexMode = luxir::api::FieldDef_::IndexMode;
  using LongTerms = luxir::api::FieldDef_::LongTerms;
  using Metric = luxir::api::VectorMetric;
  std::string_view parent;
  std::optional<AnalyzerDef> analyzer;                          // align 8
  std::optional<std::string_view> stored_resource;
  std::optional<FieldVariants> variants;
  std::optional<FieldDefaults> defaults;
  std::optional<NormalizerDef> normalizer;
  std::optional<FieldClass> type;                               // align 4 (enum)
  std::optional<IndexMode> index;                               // align 4 (enum)
  std::optional<LongTerms> long_terms;                          // align 4 (enum)
  std::optional<std::int32_t> dims;                             // align 4
  std::optional<Metric> metric;                                 // align 4 (enum)
  std::optional<bool> column;                                   // align 1 (optional<bool>s)
  std::optional<bool> multi;
  std::optional<bool> stored;
  std::optional<bool> normalized;
  std::optional<bool> normalize_on_write;
};
struct AuxStats {
  std::string_view kind;
  std::string_view field;
  std::string_view name;
  std::string_view gen;
  uint64_t commit_time = 0;
  uint64_t built_core_gen = 0;
  uint64_t bytes = 0;
  std::span<const std::string_view> files;
};
struct QueryCacheStats {
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
  std::string_view seg;
  std::string_view live_gen;
  uint64_t schema_gen = 0;
  uint64_t min_update_version = 0;
  uint64_t max_update_version = 0;
  uint64_t first_commit_time = 0;
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
  uint64_t snapshot_pins = 0;
  uint64_t pin_retained_bytes = 0;
  uint64_t pin_idle_drops = 0;
  uint64_t pin_budget_drops = 0;
  std::span<const AuxStats> aux_indexes;
  QueryCacheStats query_cache;
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
  std::optional<Error> error;
};
struct StatsRequest { std::string_view collection; bool segments = false; };
struct FollowerStats {
  std::string_view follower;
  std::string_view collection;
  std::string_view commit;
  uint64_t last_seen = 0;
  std::optional<uint64_t> lag;
};
struct ReplicationStatus {
  std::span<const FollowerStats> followers;
};
struct StatsResponse {
  StatsTotals totals;
  std::span<const CollectionStats> collections;
  IndexRamStats indexing_ram;
};
struct CacheControlRequest {
  std::string_view collection;
  uint32_t dump_limit = 0;
  bool flush = false;
  bool reset_admission = false;
  bool reset_counters = false;
  bool dump = false;
};
struct CacheEntryDump {
  uint64_t key_hash = 0;
  uint64_t bytes = 0;
  uint64_t hits = 0;
  uint64_t last_used_epoch = 0;
  std::string_view key_text;
  std::string_view scope;
  uint32_t key_bytes = 0;
  uint32_t segments_resident = 0;
  bool reader_value = false;
};
struct ShardCacheControl {                                      // needs QueryCacheStats,CacheEntryDump
  QueryCacheStats stats;
  uint64_t entries_resident = 0;
  std::span<const CacheEntryDump> entries;
  uint32_t shard_id = 0;
};
struct CollectionCacheControl {                                 // needs ShardCacheControl
  std::string_view name;
  std::span<const ShardCacheControl> shards;
  std::optional<Error> error;
};
struct CacheControlResponse { std::span<const CollectionCacheControl> collections; };

struct Vector { std::optional<ArrFloat> f32; };                  // needs ArrFloat
struct ArrArrStr { std::span<const ArrStr> v; };
struct ArrArrInt { std::span<const ArrInt> v; };
struct ArrArrFloat { std::span<const ArrFloat> v; };
struct ArrArrDouble { std::span<const ArrDouble> v; };
namespace KnnQuery_ {
struct Ivf { int32_t nprobe = 0; float min_scan_fraction = 0.0f; };
} // namespace KnnQuery_
struct KnnQuery {                                                // needs Vector
  using Ivf = luxir::api::KnnQuery_::Ivf;
  std::string_view field;
  std::optional<Vector> query;
  std::optional<Ivf> ivf;
  int32_t k = 0;
  int32_t refine_candidates = 0;
  bool exact = false;
};
struct SchemaDef {
  map_view<std::string_view, ::hpp_proto::indirect_view<FieldDef>> fields;
  map_view<std::string_view, ::hpp_proto::indirect_view<FieldDef>> templates;
};
struct ColVector { std::span<const Vector> v; };
struct ArrVector { std::span<const Vector> v; };
struct SchemaResponse { std::optional<SchemaDef> schema; };      // needs SchemaDef
struct SchemaRequest {                                           // needs SchemaDef
  using Mode = luxir::api::SchemaRequest_::Mode;
  std::string_view collection;
  std::optional<SchemaDef> schema;
  Mode mode = Mode::SET;                                      // align 4 (enum)
};
struct CreateCollectionRequest { std::string_view name; std::optional<SchemaDef> schema; };
struct CreateCollectionResponse { std::string_view name; };
struct DeleteCollectionRequest { std::string_view name; };
struct DeleteCollectionResponse { std::string_view name; };
struct ListCollectionsResponse { std::span<const std::string_view> collections; };
struct MultiVector { std::span<const ArrVector> v; };
struct ArrVal { std::span<const Val> v; };                       // span<incomplete Val> OK

struct Column {                                                  // variant arms all complete above
  std::variant<std::monostate, ColStr, ColInt, ColFloat, ColDouble, ArrArrStr, ArrArrInt,
               ArrArrFloat, ArrArrDouble, ColVector, MultiVector>
      kind;
};

struct BooleanQuery {                                            // span<incomplete Query> OK
  std::span<const Query> filter;
  std::span<const Query> required;
  std::span<const Query> optional;
  std::span<const Query> prohibited;
  int32_t min_match = 0;
};
struct Filter {
  ::hpp_proto::optional_indirect_view<Query> query;
  std::span<const std::string_view> except_ops;
};
struct TopDocs {                                                 // all indirect/span/opt-scalar
  ::hpp_proto::optional_indirect_view<Query> query;
  std::span<const Filter> filter;
  int64_t offset = 0;
  std::optional<std::int64_t> limit;
  std::span<const std::string_view> fields;
  std::span<const SortSpec> sort;
  map_view<std::string_view, ::hpp_proto::indirect_view<SearchOp>> ops;
  int32_t batch_size = 0;
  DocFormat document_format = DocFormat::DEFAULT;                // align 4 (enum)
  bool get_number = false;
  bool get_scores = false;
};
struct Fusion {                                                  // needs TopDocs, RrfFusion
  map_view<std::string_view, TopDocs> sources;
  std::span<const Filter> filter;
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
  ::hpp_proto::optional_indirect_view<Query> query;
  std::span<const Query> filter;
  std::optional<bool> apply_parent_filters;
};
struct ExprOp {
  std::string_view expr;
  map_view<std::string_view, ::hpp_proto::indirect_view<Val>> vars;
};
struct FieldFacet {
  std::string_view field;
  std::optional<std::int64_t> limit;
  std::optional<std::int64_t> mincount;
  std::span<const SortSpec> sort;
  map_view<std::string_view, ::hpp_proto::indirect_view<SearchOp>> ops;
  ::hpp_proto::optional_indirect_view<Val> selected;
  SelectionMode selection_mode = SelectionMode::ANY;
  bool missing = false;
};
struct QueryBucket {
  std::string_view name;
  ::hpp_proto::optional_indirect_view<Query> query;
};
struct QueryFacet {
  std::span<const QueryBucket> buckets;
  map_view<std::string_view, ::hpp_proto::indirect_view<SearchOp>> ops;
  ::hpp_proto::optional_indirect_view<Val> selected;
  SelectionMode selection_mode = SelectionMode::ANY;
};
struct SearchRequest {
  std::string_view request_id;
  std::string_view collection;
  map_view<std::string_view, ::hpp_proto::indirect_view<SearchOp>> ops;
  uint64_t freshness_ms = 0;
  std::string_view time_zone;
  ResponseFormat response_format = ResponseFormat::ENVELOPE;
  bool profile = false;
  std::int32_t max_parallel = 0;
  // JSON parsing metadata, absent from the wire schema and canonical JSON.
  // Only a root TopDocs shorthand creates this implicit q; an explicit op
  // named q (including one modified by a URL overlay) keeps its wrapper.
  bool json_shorthand = false;
};
struct Map { map_view<std::string_view, ::hpp_proto::indirect_view<Val>> fields; };
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
  map_view<std::string_view, ::hpp_proto::indirect_view<SearchOp>> ops;
  std::string_view time_zone;
  ::hpp_proto::optional_indirect_view<Val> selected;
  SelectionMode selection_mode = SelectionMode::ANY;
  bool missing = false;
};
struct SearchOp {                                               // needs TopDocs,Fusion,FieldFacet,RangeFacet,QueryFacet,ExprOp,Domain
  std::variant<std::monostate, TopDocs, Fusion, FieldFacet, RangeFacet, QueryFacet, ExprOp> kind;
  std::optional<Domain> domain;
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
  std::optional<Error> error;
  std::span<const Warning> warnings;
  std::optional<ExecutionProfile> profile;
  bool more = false;
};
struct Match {
  using Operator = luxir::api::Match_::Operator;
  std::string_view field;
  ::hpp_proto::optional_indirect_view<Val> val;
  Operator operator_ = Operator::OPERATOR_UNSPECIFIED;
  int32_t min_match = 0;
};
struct AnyOfQuery {
  std::string_view field;
  ::hpp_proto::optional_indirect_view<Val> values;
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
struct Query {                                                 // needs Match,AnyOfQuery,BooleanQuery,Exists,Phrase,Knn,ConstantScore,Prefix,Fuzzy,Simple,Range,Expr,GeoBox,GeoDistance,Boost,Rescore
  std::variant<std::monostate, Match, BooleanQuery, bool, ExistsQuery, PhraseQuery, KnnQuery,
               ConstantScoreQuery, PrefixQuery, FuzzyQuery, SimpleQuery, RangeQuery, ExprQuery,
               GeoBoxQuery, GeoDistanceQuery, BoostQuery, RescoreQuery, WildcardQuery, RegexQuery,
               AnyOfQuery>
      kind;
};
struct UpdateRequest {                                         // needs CommitParams
  std::string_view request_id;
  std::string_view collection;
  std::span<const Map> docs;
  std::span<const std::string_view> delete_ids;
  std::optional<CommitParams> commit;
  // Input doc key -> schema field for this request's docs ("" target = drop the key).
  map_view<std::string_view, std::string_view> field_map;
  bool allow_dups = false;
  bool all_or_none = false;
  bool return_ids = false;
  bool drop_unmapped = false;
};

// ===================== trivial-destructibility checks =====================
#define LUXIR_TD(M) static_assert(std::is_trivially_destructible_v<M>);
LUXIR_TD(SearchRequest) LUXIR_TD(SearchOp) LUXIR_TD(ExprOp) LUXIR_TD(TopDocs)
LUXIR_TD(Fusion) LUXIR_TD(RrfFusion) LUXIR_TD(SortSpec) LUXIR_TD(Query)
LUXIR_TD(ExistsQuery)
LUXIR_TD(ConstantScoreQuery) LUXIR_TD(BoostQuery) LUXIR_TD(RescoreQuery) LUXIR_TD(KnnQuery) LUXIR_TD(Match) LUXIR_TD(AnyOfQuery) LUXIR_TD(Filter) LUXIR_TD(BooleanQuery)
LUXIR_TD(PrefixQuery) LUXIR_TD(WildcardQuery) LUXIR_TD(RegexQuery) LUXIR_TD(FuzzyQuery) LUXIR_TD(PhraseQuery) LUXIR_TD(SimpleQuery) LUXIR_TD(RangeQuery)
LUXIR_TD(GeoBoxQuery) LUXIR_TD(GeoDistanceQuery) LUXIR_TD(ExprQuery)
LUXIR_TD(Warning) LUXIR_TD(Error) LUXIR_TD(ExecutionProfile) LUXIR_TD(ExecutionProfileOp)
LUXIR_TD(ExecutionProfilePiece) LUXIR_TD(FieldFacet) LUXIR_TD(CalendarGap) LUXIR_TD(RangeFacet)
LUXIR_TD(QueryBucket) LUXIR_TD(QueryFacet)
LUXIR_TD(Domain) LUXIR_TD(SearchResponse) LUXIR_TD(DocList) LUXIR_TD(FacetResult)
LUXIR_TD(CommitParams) LUXIR_TD(UpdateRequest) LUXIR_TD(UpdateResponse) LUXIR_TD(Map)
LUXIR_TD(Val) LUXIR_TD(ArrVal) LUXIR_TD(ArrStr) LUXIR_TD(ArrInt) LUXIR_TD(ArrFloat)
LUXIR_TD(ArrDouble) LUXIR_TD(ArrBin) LUXIR_TD(ArrArrStr) LUXIR_TD(ArrArrInt) LUXIR_TD(ArrArrFloat)
LUXIR_TD(ArrArrDouble) LUXIR_TD(Vector) LUXIR_TD(ArrVector)
LUXIR_TD(ColStr) LUXIR_TD(Column) LUXIR_TD(ColVector) LUXIR_TD(MultiVector) LUXIR_TD(ColInt)
LUXIR_TD(ColFloat) LUXIR_TD(ColDouble)
LUXIR_TD(AnalyzerComponent) LUXIR_TD(AnalyzerDef) LUXIR_TD(FieldDef) LUXIR_TD(SchemaDef)
LUXIR_TD(FieldVariants) LUXIR_TD(FieldDefaults) LUXIR_TD(NormalizerDef)
LUXIR_TD(SchemaRequest) LUXIR_TD(SchemaResponse) LUXIR_TD(UpdateResponse_::DocError)
LUXIR_TD(KnnQuery_::Ivf)
LUXIR_TD(CreateCollectionRequest) LUXIR_TD(CreateCollectionResponse)
LUXIR_TD(DeleteCollectionRequest) LUXIR_TD(DeleteCollectionResponse) LUXIR_TD(ListCollectionsResponse)
LUXIR_TD(ReplicationStatus) LUXIR_TD(FollowerStats) LUXIR_TD(StatsRequest) LUXIR_TD(StatsResponse) LUXIR_TD(StatsTotals) LUXIR_TD(CollectionStats)
LUXIR_TD(ShardStats) LUXIR_TD(IndexStats) LUXIR_TD(SegmentStats) LUXIR_TD(AuxStats)
LUXIR_TD(QueryCacheStats) LUXIR_TD(IndexRamStats)
LUXIR_TD(CacheControlRequest) LUXIR_TD(CacheControlResponse) LUXIR_TD(CacheEntryDump)
LUXIR_TD(ShardCacheControl) LUXIR_TD(CollectionCacheControl)
#undef LUXIR_TD

// ===================== out-of-line codec entry-point declarations =====================
// decode() uses hpp_proto::padded_input. Callers must pass a payload-only span
// with 16 readable bytes past data.end(), and the first one must be zero.
#define LUXIR_ENTRY(M)                                                                      \
  bool decode(M &, std::span<const std::byte> data, std::pmr::memory_resource &arena);      \
  bool encode(const M &, std::vector<std::byte> &out);                                      \
  bool write_json(const M &, std::string &out);                                             \
  bool read_json(M &, std::string_view json, std::pmr::memory_resource &arena,              \
                 std::string *error = nullptr);                                              \
  bool merge_json(M &, std::string_view json, std::pmr::memory_resource &arena,             \
                  std::string *error = nullptr);
LUXIR_ENTRY(SearchRequest) LUXIR_ENTRY(SearchOp) LUXIR_ENTRY(ExprOp)
LUXIR_ENTRY(TopDocs) LUXIR_ENTRY(Fusion) LUXIR_ENTRY(RrfFusion) LUXIR_ENTRY(SortSpec)
LUXIR_ENTRY(Query) LUXIR_ENTRY(ExistsQuery) LUXIR_ENTRY(ConstantScoreQuery) LUXIR_ENTRY(BoostQuery) LUXIR_ENTRY(RescoreQuery)
LUXIR_ENTRY(KnnQuery) LUXIR_ENTRY(Match) LUXIR_ENTRY(AnyOfQuery) LUXIR_ENTRY(Filter) LUXIR_ENTRY(BooleanQuery)
LUXIR_ENTRY(PrefixQuery) LUXIR_ENTRY(WildcardQuery) LUXIR_ENTRY(RegexQuery) LUXIR_ENTRY(FuzzyQuery) LUXIR_ENTRY(PhraseQuery) LUXIR_ENTRY(SimpleQuery)
LUXIR_ENTRY(RangeQuery) LUXIR_ENTRY(GeoBoxQuery) LUXIR_ENTRY(GeoDistanceQuery) LUXIR_ENTRY(ExprQuery)
LUXIR_ENTRY(Warning) LUXIR_ENTRY(Error) LUXIR_ENTRY(ExecutionProfile) LUXIR_ENTRY(ExecutionProfileOp)
LUXIR_ENTRY(ExecutionProfilePiece) LUXIR_ENTRY(FieldFacet)
LUXIR_ENTRY(CalendarGap) LUXIR_ENTRY(RangeFacet) LUXIR_ENTRY(QueryBucket) LUXIR_ENTRY(QueryFacet)
LUXIR_ENTRY(Domain) LUXIR_ENTRY(SearchResponse) LUXIR_ENTRY(DocList)
LUXIR_ENTRY(FacetResult) LUXIR_ENTRY(CommitParams) LUXIR_ENTRY(UpdateRequest)
LUXIR_ENTRY(UpdateResponse) LUXIR_ENTRY(Map)
LUXIR_ENTRY(Val) LUXIR_ENTRY(ArrVal) LUXIR_ENTRY(ArrStr) LUXIR_ENTRY(ArrInt) LUXIR_ENTRY(ArrFloat)
LUXIR_ENTRY(ArrDouble) LUXIR_ENTRY(ArrBin) LUXIR_ENTRY(ArrArrStr) LUXIR_ENTRY(ArrArrInt)
LUXIR_ENTRY(ArrArrFloat) LUXIR_ENTRY(ArrArrDouble) LUXIR_ENTRY(Vector)
LUXIR_ENTRY(ArrVector) LUXIR_ENTRY(ColStr) LUXIR_ENTRY(Column)
LUXIR_ENTRY(ColVector) LUXIR_ENTRY(MultiVector) LUXIR_ENTRY(ColInt) LUXIR_ENTRY(ColFloat)
LUXIR_ENTRY(ColDouble) LUXIR_ENTRY(AnalyzerComponent) LUXIR_ENTRY(AnalyzerDef) LUXIR_ENTRY(FieldDef)
LUXIR_ENTRY(FieldVariants) LUXIR_ENTRY(FieldDefaults) LUXIR_ENTRY(NormalizerDef)
LUXIR_ENTRY(SchemaDef) LUXIR_ENTRY(SchemaRequest) LUXIR_ENTRY(SchemaResponse)
LUXIR_ENTRY(CreateCollectionRequest) LUXIR_ENTRY(CreateCollectionResponse)
LUXIR_ENTRY(DeleteCollectionRequest) LUXIR_ENTRY(DeleteCollectionResponse)
LUXIR_ENTRY(ListCollectionsResponse)
LUXIR_ENTRY(ReplicationStatus) LUXIR_ENTRY(FollowerStats) LUXIR_ENTRY(StatsRequest) LUXIR_ENTRY(StatsResponse) LUXIR_ENTRY(StatsTotals)
LUXIR_ENTRY(CollectionStats) LUXIR_ENTRY(ShardStats) LUXIR_ENTRY(IndexStats)
LUXIR_ENTRY(SegmentStats) LUXIR_ENTRY(AuxStats) LUXIR_ENTRY(QueryCacheStats)
LUXIR_ENTRY(IndexRamStats)
LUXIR_ENTRY(CacheControlRequest) LUXIR_ENTRY(CacheControlResponse) LUXIR_ENTRY(CacheEntryDump)
LUXIR_ENTRY(ShardCacheControl) LUXIR_ENTRY(CollectionCacheControl)
#undef LUXIR_ENTRY

} // namespace luxir::api
