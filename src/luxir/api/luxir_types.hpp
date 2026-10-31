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

#include <boost/describe/enum.hpp>
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
struct Diversify; struct Collapse; struct GroupComponent; struct GroupKey; struct GroupTable;
struct Fusion; struct RrfFusion; struct LinearFusion; struct SortSpec; struct Query;
struct ExistsQuery; struct ConstantScoreQuery; struct BoostQuery; struct RescoreQuery; struct KnnQuery; struct Match; struct AnyOfQuery; struct Filter; struct BooleanQuery;
struct PrefixQuery; struct WildcardQuery; struct RegexQuery; struct FuzzyQuery; struct PhraseQuery; struct SimpleQuery; struct RangeQuery;
struct GeoBoxQuery; struct GeoDistanceQuery; struct ExprQuery; struct WeightedField; struct PhraseOptions; struct MultiMatchQuery;
struct DisMaxQuery; struct PluginQuery; struct PluginOp;
struct Warning; struct Error;
struct ExecutionProfile; struct ExecutionProfileOp; struct ExecutionProfilePiece;
struct FieldFacet; struct CalendarGap; struct RangeFacet; struct QueryBucket; struct QueryFacet;
struct Domain; struct SearchResponse; struct DocList; struct FacetResult;
struct CommitParams; struct UpdateRequest; struct UpdateResponse; struct Map;
struct Val; struct ArrVal; struct ArrStr; struct ArrInt; struct ArrFloat;
struct ArrDouble; struct ArrBin; struct ArrArrStr; struct ArrArrInt; struct ArrArrFloat;
struct ArrArrDouble; struct Vector; struct ArrVector;
struct ColStr; struct Column; struct ColVal; struct ColVector; struct MultiVector; struct ColInt;
struct ColFloat; struct ColDouble; struct AnalyzerComponent; struct AnalyzerDef; struct FieldDef; struct SchemaDef;
struct FieldVariants; struct FieldDefaults; struct NormalizerDef; struct VectorAnnDef; struct StoredResourceDef;
struct SchemaRequest; struct SchemaResponse;
struct CreateCollectionRequest; struct CreateCollectionResponse;
struct DeleteCollectionRequest; struct DeleteCollectionResponse; struct ListCollectionsResponse; struct ListTenantsResponse;
struct StatsRequest; struct StatsResponse; struct StatsTotals; struct CollectionStats;
struct ShardStats; struct IndexStats; struct SegmentStats; struct AuxStats;
struct QueryCacheStats; struct IndexRamStats; struct ArenaStats; struct JemallocStats; struct FieldMetadataStats; struct MemoryStats; struct ScrubStats;
struct HistogramStats; struct ActivityStats; struct TransportStats; struct ServiceStats; struct SystemStats;
struct HostStats; struct OutcomeStats; struct CollectionActivityStats; struct ProcessIoStats;
struct ProcessStats; struct CpuTimes; struct HostCpuStats; struct HostMemoryStats; struct NetworkInterfaceStats;
struct TcpStats; struct DiskStats; struct FilesystemStats; struct PressureStats; struct CgroupStats;
struct CgroupMemoryStats; struct CgroupCpuStats; struct CgroupTaskStats;
struct CacheControlRequest; struct CacheControlResponse; struct CacheEntryDump;
struct ShardCacheControl; struct CollectionCacheControl;
namespace UpdateResponse_ { struct DocError; }
namespace KnnQuery_ { struct Ivf; }
namespace VectorAnnDef_ { struct RaBitQ; struct Pq; }

// ---- nested enums (Foo_ namespace; matches generated metadata refs) ----
namespace Error_ {
enum class Kind {
  UNKNOWN = 0, INVALID_REQUEST = 1, NOT_FOUND = 2, ALREADY_EXISTS = 3, FAILED_PRECONDITION = 4,
  RESOURCE_EXHAUSTED = 5, UNAVAILABLE = 6, INTERNAL = 7
};
}
namespace LinearFusion_ { enum class Normalization { MIN_MAX = 0 }; }
namespace Collapse_ { enum class MissingPolicy { EXPAND = 0, GROUP = 1, DROP = 2 }; }
namespace FieldFacet_ { enum class CountUnit { DOCS = 0, GROUPS = 1 }; }
namespace RangeFacet_ { enum class Bounds { EXPLICIT = 0, DATA = 1 }; }
namespace GroupComponent_ { enum class Type { STRING = 0, ID = 1, INT = 2, DATE = 3 }; }
namespace SortSpec_ { enum class SortDir { UNKNOWN = 0, ASC = 1, DESC = 2 }; }
// Enums an expr callable takes as an argument also describe the values it
// accepts (ExprFunctions.h); keep each list beside its enum.
namespace Match_ {
enum class Operator { OPERATOR_UNSPECIFIED = 0, OR = 1, AND = 2 };
BOOST_DESCRIBE_ENUM(Operator, AND, OR)
}
namespace MultiMatchQuery_ {
enum class Mode { COMBINED = 0, PER_FIELD = 1 };
BOOST_DESCRIBE_ENUM(Mode, COMBINED, PER_FIELD)
}
namespace CalendarGap_ {
enum class Unit { UNKNOWN = 0, DAY = 1, WEEK = 2, MONTH = 3, QUARTER = 4, YEAR = 5 };
}
namespace UpdateResponse_ { enum class Status { UNKNOWN = 0, OK = 1, PARTIAL = 2, ERROR = 3 }; }
enum class VectorMetric { NONE = 0, L2 = 1, IP = 2, COSINE = 3 };
enum class DocFormat { DEFAULT = 0, ROWS = 1, COLUMNS = 2, ADAPTIVE = 3 };
enum class RequestFlagBit { DATE_MODE_LOW_BIT = 0, DATE_MODE_HIGH_BIT = 1, INT64_STR_BIT = 2 };
enum class ResponseFormat { ENVELOPE = 0, DOCS = 1 };
enum class SelectionMode { ANY = 0, ALL = 1 };
namespace FieldDef_ {
enum class FieldClass { STRING = 0, TEXT = 1, INT = 2, FLOAT = 3, DOUBLE = 4, ID = 6, VECTOR = 7, DATE = 8, GEO_POINT = 9 };
enum class MultiMode { SINGLE = 0, DEFAULT = 1, SET = 2, ARRAY = 3 };
enum class IndexMode { NONE = 0, MATCH = 1, RANGE = 2 };
enum class LongTerms { TRUNCATE = 0, REJECT = 1, HASH128 = 2 };
enum class DateUnit { MS = 0, NS = 1 };
}
namespace VectorAnnDef_ { enum class Codec { RABITQ = 0, PQ = 1 }; }
namespace StoredResourceDef_ {
enum class Codec { LZ4 = 0, ZSTD = 1 };
enum class Layout { INLINE = 0, SEPARATED = 1 };
}
namespace SchemaRequest_ { enum class Mode { SET = 0, REPLACE_ALL = 1 }; }

// ===================== message definitions (strict topological order) =====================

struct RrfFusion { std::optional<int32_t> k; };
struct LinearFusion {
  using Normalization = luxir::api::LinearFusion_::Normalization;
  Normalization normalization = Normalization::MIN_MAX;
};
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
struct PrefixQuery {
  std::string_view field;
  std::string_view prefix;
  bool case_insensitive = false;
};
struct WildcardQuery {
  std::string_view field;
  std::string_view pattern;
  bool case_insensitive = false;
};
struct RegexQuery {
  std::string_view field;
  std::string_view pattern;
  bool case_insensitive = false;
};
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
struct AllReplicas {};
struct ReplicaRequirement { std::variant<std::monostate, uint32_t, AllReplicas> kind; };
struct CommitParams {
  uint64_t commit_within_ms = 0;
  std::span<const std::string_view> build_aux_indexes;
  bool wait_for_merges = false;
  uint32_t max_segments = 0;
  std::optional<ReplicaRequirement> wait_for_replicas;
  std::optional<uint64_t> wait_for_replicas_timeout_ms;
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

namespace ReplicaResult_ { enum class Outcome { SATISFIED = 0, TIMED_OUT = 1, CANCELLED = 2 }; }
struct ReplicaResult {
  using Outcome = ReplicaResult_::Outcome;
  std::optional<uint32_t> wanted;
  std::optional<uint32_t> serving;
  std::optional<Outcome> outcome;
};

struct CollectionCommit {
  std::string_view commit;
  std::optional<ReplicaResult> replicas;
  std::optional<Error> error;
};

struct UpdateResponse {
  using Status = luxir::api::UpdateResponse_::Status;
  using DocError = luxir::api::UpdateResponse_::DocError;
  std::string_view commit;
  std::optional<ReplicaResult> replicas;
  map_view<std::string_view, CollectionCommit> commits;
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
  std::span<const AnalyzerComponent> char_filters;
  std::string_view name;
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
namespace VectorAnnDef_ {
struct RaBitQ { std::optional<int32_t> bits; std::optional<int32_t> query_bits; };
struct Pq { std::optional<int32_t> m; std::optional<int32_t> bits; };
} // namespace VectorAnnDef_
struct VectorAnnDef {
  using Codec = luxir::api::VectorAnnDef_::Codec;
  using RaBitQ = luxir::api::VectorAnnDef_::RaBitQ;
  using Pq = luxir::api::VectorAnnDef_::Pq;
  std::optional<RaBitQ> rabitq;                                 // align 4
  std::optional<Pq> pq;
  std::optional<Codec> codec;
  std::optional<int32_t> nlist;
};
struct FieldDef {
  using FieldClass = luxir::api::FieldDef_::FieldClass;
  using MultiMode = luxir::api::FieldDef_::MultiMode;
  using IndexMode = luxir::api::FieldDef_::IndexMode;
  using LongTerms = luxir::api::FieldDef_::LongTerms;
  using DateUnit = luxir::api::FieldDef_::DateUnit;
  using Metric = luxir::api::VectorMetric;
  std::string_view parent;
  std::optional<AnalyzerDef> analyzer;                          // align 8
  std::optional<std::string_view> stored_resource;
  std::optional<FieldVariants> variants;
  std::optional<FieldDefaults> defaults;
  std::optional<NormalizerDef> normalizer;
  std::optional<VectorAnnDef> ann;                              // align 4
  std::optional<FieldClass> type;                               // align 4 (enum)
  std::optional<IndexMode> index;                               // align 4 (enum)
  std::optional<LongTerms> long_terms;                          // align 4 (enum)
  std::optional<DateUnit> unit;                                // align 4 (enum)
  std::optional<std::int32_t> dims;                             // align 4
  std::optional<std::int32_t> truncate_dims;                    // align 4
  std::optional<Metric> metric;                                 // align 4 (enum)
  std::optional<MultiMode> multi;
  std::optional<bool> column;                                   // align 1 (optional<bool>s)
  std::optional<bool> stored;
  std::optional<bool> normalized;
  std::optional<bool> normalize_on_write;
  std::optional<bool> ignore_malformed;
  std::optional<bool> ignore_z_value;
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
  uint64_t build_attempts = 0;
  uint64_t capacity_dead_builds = 0;
  uint64_t thrash_build_skips = 0;
  uint64_t reader_dead_builds = 0;
  uint64_t reader_thrash_build_skips = 0;
  bool enabled = false;
};
struct StorageRamStats { uint64_t used_bytes = 0; uint64_t limit_bytes = 0; };
struct IndexRamStats {
  uint64_t limit_bytes = 0;
  uint64_t reserved_bytes = 0;
  uint64_t draining_bytes = 0;
  uint64_t pending_merge_bytes = 0;
  uint64_t retained_delete_bytes = 0;
};
struct ArenaStats {
  uint64_t allocated_bytes = 0;
  uint64_t resident_bytes = 0;
};
struct JemallocStats {
  std::string_view version;
  uint64_t allocated_bytes = 0;
  uint64_t active_bytes = 0;
  uint64_t metadata_bytes = 0;
  uint64_t resident_bytes = 0;
  uint64_t mapped_bytes = 0;
  uint64_t retained_bytes = 0;
  uint64_t dirty_bytes = 0;
  ArenaStats indexing;
  ArenaStats big_buffer;
};
struct FieldMetadataStats {
  uint64_t bytes = 0;
  uint64_t directories = 0;
  uint64_t directory_bytes = 0;
  uint64_t pages = 0;
  uint64_t page_bytes = 0;
  uint64_t entries = 0;
  uint64_t entry_bytes = 0;
  uint64_t names = 0;
  uint64_t name_bytes = 0;
};
struct MemoryStats {
  IndexRamStats indexing_budget;
  StorageRamStats storage_ram;
  std::optional<JemallocStats> jemalloc;
  FieldMetadataStats field_metadata;
  uint64_t node_budget_bytes = 0;
  uint64_t search_request_limit_bytes = 0;
};
struct StatsTotals {
  uint64_t collections = 0;
  uint64_t shards = 0;
  uint64_t segments = 0;
  uint64_t committed_segments = 0;
  uint64_t max_docs = 0;
  uint64_t live_docs = 0;
  uint64_t deleted_docs = 0;
  std::optional<uint64_t> bytes;
  uint64_t committed_bytes = 0;
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
  uint64_t inverters_busy = 0;
  uint64_t inverters_idle = 0;
  uint64_t inverters_flushing = 0;
  uint64_t commits = 0;
  uint64_t retained_delete_bytes = 0;
  std::span<const AuxStats> aux_indexes;
  QueryCacheStats query_cache;
  std::span<const SegmentStats> segments;
};
struct ShardStats {
  IndexStats index;
  uint32_t shard_id = 0;
};
struct OutcomeStats {
  double seconds = 0;
  uint64_t ok = 0;
  uint64_t error = 0;
  uint64_t cancelled = 0;
};
struct CollectionActivityStats {
  OutcomeStats searches;
  OutcomeStats index_batches;
  uint64_t documents_accepted = 0;
  uint64_t documents_rejected = 0;
  uint64_t delete_ids_accepted = 0;
};
struct CollectionStats {
  std::optional<CollectionActivityStats> activity;
  uint64_t storage_ram_bytes = 0;
  std::string_view name;
  std::string_view tenant;
  StatsTotals totals;
  uint64_t schema_gen = 0;
  std::span<const ShardStats> shards;
  std::optional<Error> error;
};
struct StatsRequest {
  std::string_view collection;
  std::string_view tenant;
  bool segments = false;
  bool collections = false;
  bool files = false;
  bool system = false;
  bool host = false;
};
struct FollowerStatus {
  std::string_view follower;
  std::string_view tenant;
  std::string_view collection;
  std::string_view commit;
  uint64_t last_seen = 0;
  std::optional<uint64_t> lag;
};
struct ReplicationCatalogEntry {
  std::string_view manifest_xxh3;
  std::string_view commit;
  std::string_view tenant;
  std::string_view collection;
  bool available = false;
};
struct ReplicationCatalog {
  std::string_view boot;
  std::string_view cursor;
  std::span<const ReplicationCatalogEntry> collections;
};
struct ReplicationInstalled {
  std::string_view follower;
  std::string_view tenant;
  std::string_view collection;
  std::string_view commit;
};
namespace ReplicationCollectionStatus_ { enum class State { SYNCING = 0, SERVING = 1, WAITING = 2, STALE = 3, ORPHAN = 4, ERROR = 5 }; }
struct ReplicationCollectionStatus {
  using State = ReplicationCollectionStatus_::State;
  std::string_view name;
  std::string_view tenant;
  std::string_view source_commit;
  std::string_view serving_commit;
  std::optional<State> state;
  uint64_t bytes_downloaded = 0;
  uint64_t bytes_total = 0;
  std::string_view last_error;
  uint64_t next_retry = 0;
};
struct ReplicationStatus {
  std::span<const FollowerStatus> followers;
  std::string_view source;
  std::string_view follower;
  std::optional<bool> connected;
  uint64_t last_contact = 0;
  std::span<const ReplicationCollectionStatus> collections;
};
struct HistogramStats {
  std::span<const uint64_t> counts;
  double sum = 0;
  uint64_t count = 0;
};
struct ActivityStats {
  HistogramStats duration;
  int64_t in_flight = 0;
  uint64_t ok = 0;
  uint64_t error = 0;
  uint64_t cancelled = 0;
};
struct TransportStats {
  map_view<std::string_view, ActivityStats> operations;
  map_view<std::string_view, uint64_t> errors;
  uint64_t received_bytes = 0;
  uint64_t sent_bytes = 0;
  uint64_t stream_pauses = 0;
  uint64_t connections = 0;
  int64_t io_threads = 0;
};
struct ServiceStats {
  std::string_view version;
  std::string_view cpu_target;
  std::span<const double> request_duration_bounds;
  std::span<const double> background_duration_bounds;
  TransportStats http;
  TransportStats grpc;
  ActivityStats search;
  map_view<std::string_view, uint64_t> document_errors;
  HistogramStats search_execution;
  uint64_t search_resource_exhausted = 0;
  uint64_t search_warnings = 0;
  ActivityStats index_batches;
  uint64_t documents_accepted = 0;
  uint64_t documents_rejected = 0;
  uint64_t delete_ids_accepted = 0;
  ActivityStats flush;
  ActivityStats commit;
  ActivityStats delete_pass;
  ActivityStats merge;
  ActivityStats vector_build;
  ActivityStats reader_refresh;
  bool read_only = false;
};
struct ProcessStats {
  double cpu_seconds = 0;
  double start_time_seconds = 0;
  int64_t max_fds = 0;
  uint64_t resident_bytes = 0;
  uint64_t virtual_bytes = 0;
  uint64_t swap_bytes = 0;
  uint64_t resident_anonymous_bytes = 0;
  uint64_t resident_file_bytes = 0;
  uint64_t resident_shared_bytes = 0;
  uint64_t resident_peak_bytes = 0;
  uint64_t threads = 0;
  uint64_t voluntary_context_switches = 0;
  uint64_t involuntary_context_switches = 0;
  uint64_t minor_page_faults = 0;
  uint64_t major_page_faults = 0;
  uint64_t open_fds = 0;
};
struct ProcessIoStats {
  uint64_t disk_read_bytes = 0;
  uint64_t disk_written_bytes = 0;
  uint64_t io_read_bytes = 0;
  uint64_t io_written_bytes = 0;
  uint64_t io_read_calls = 0;
  uint64_t io_write_calls = 0;
};
struct CpuTimes {
  std::string_view cpu;
  double user = 0;
  double nice = 0;
  double system = 0;
  double idle = 0;
  double iowait = 0;
  double irq = 0;
  double softirq = 0;
  double steal = 0;
};
struct HostCpuStats {
  std::span<const CpuTimes> cpus;
  double boot_time_seconds = 0;
  double load1 = 0;
  double load5 = 0;
  double load15 = 0;
  uint64_t context_switches = 0;
  uint64_t forks = 0;
  uint64_t procs_running = 0;
  uint64_t procs_blocked = 0;
  uint64_t filefd_allocated = 0;
  uint64_t filefd_maximum = 0;
};
struct HostMemoryStats {
  uint64_t total_bytes = 0;
  uint64_t free_bytes = 0;
  uint64_t available_bytes = 0;
  uint64_t buffers_bytes = 0;
  uint64_t cached_bytes = 0;
  uint64_t swap_cached_bytes = 0;
  uint64_t swap_total_bytes = 0;
  uint64_t swap_free_bytes = 0;
  uint64_t dirty_bytes = 0;
  uint64_t writeback_bytes = 0;
  uint64_t mapped_bytes = 0;
  uint64_t shmem_bytes = 0;
  uint64_t slab_bytes = 0;
  uint64_t slab_reclaimable_bytes = 0;
  uint64_t slab_unreclaimable_bytes = 0;
  uint64_t page_tables_bytes = 0;
  uint64_t kernel_stack_bytes = 0;
  uint64_t anon_pages_bytes = 0;
  uint64_t page_faults = 0;
  uint64_t major_page_faults = 0;
  uint64_t swap_in_pages = 0;
  uint64_t swap_out_pages = 0;
  uint64_t oom_kills = 0;
};
struct NetworkInterfaceStats {
  std::string_view device;
  uint64_t receive_bytes = 0;
  uint64_t receive_packets = 0;
  uint64_t receive_errs = 0;
  uint64_t receive_drop = 0;
  uint64_t receive_fifo = 0;
  uint64_t receive_frame = 0;
  uint64_t receive_compressed = 0;
  uint64_t receive_multicast = 0;
  uint64_t transmit_bytes = 0;
  uint64_t transmit_packets = 0;
  uint64_t transmit_errs = 0;
  uint64_t transmit_drop = 0;
  uint64_t transmit_fifo = 0;
  uint64_t transmit_colls = 0;
  uint64_t transmit_carrier = 0;
  uint64_t transmit_compressed = 0;
};
struct TcpStats {
  uint64_t curr_estab = 0;
  uint64_t retrans_segs = 0;
  uint64_t in_errs = 0;
  uint64_t out_rsts = 0;
  uint64_t attempt_fails = 0;
};
struct DiskStats {
  std::string_view device;
  double read_time_seconds = 0;
  double write_time_seconds = 0;
  double io_time_seconds = 0;
  double io_time_weighted_seconds = 0;
  uint64_t reads_completed = 0;
  uint64_t reads_merged = 0;
  uint64_t read_bytes = 0;
  uint64_t writes_completed = 0;
  uint64_t writes_merged = 0;
  uint64_t written_bytes = 0;
  uint64_t io_now = 0;
};
struct FilesystemStats {
  std::string_view path;
  uint64_t size_bytes = 0;
  uint64_t free_bytes = 0;
  uint64_t avail_bytes = 0;
  uint64_t files = 0;
  uint64_t files_free = 0;
  bool readonly = false;
};
struct PressureStats {
  std::string_view resource;
  double some_seconds = 0;
  std::optional<double> full_seconds;
};
struct CgroupMemoryStats {
  uint64_t current_bytes = 0;
  int64_t max_bytes = 0;
  int64_t high_bytes = 0;
  uint64_t low_events = 0;
  uint64_t high_events = 0;
  uint64_t max_events = 0;
  uint64_t oom_events = 0;
  uint64_t oom_kill_events = 0;
};
struct CgroupCpuStats {
  double usage_seconds = 0;
  double throttled_seconds = 0;
  double quota_cores = 0;
  uint64_t periods = 0;
  uint64_t throttled_periods = 0;
};
struct CgroupTaskStats {
  uint64_t current = 0;
  int64_t max = 0;
};
struct CgroupStats {
  std::string_view path;
  std::optional<CgroupMemoryStats> memory;
  std::optional<CgroupCpuStats> cpu;
  std::optional<CgroupTaskStats> tasks;
};
struct SystemStats {
  std::optional<ProcessStats> process;
  std::optional<ProcessIoStats> process_io;
  std::optional<FilesystemStats> filesystem;
  std::optional<CgroupStats> cgroup;
  std::span<const std::string_view> available;
  std::span<const std::string_view> unavailable;
};
struct HostStats {
  std::optional<HostCpuStats> cpu;
  std::optional<HostMemoryStats> memory;
  std::span<const NetworkInterfaceStats> network;
  std::optional<TcpStats> tcp;
  std::span<const DiskStats> disks;
  std::span<const PressureStats> pressure;
  std::span<const std::string_view> available;
  std::span<const std::string_view> unavailable;
};
struct ScrubStats {
  uint64_t passes = 0;
  uint64_t files = 0;
  uint64_t bytes = 0;
  uint64_t mismatches = 0;
  uint64_t errors = 0;
  uint64_t last_pass_end = 0;
};
struct StatsResponse {
  std::optional<StatsTotals> totals;
  std::span<const CollectionStats> collections;
  uint64_t multiterm_expansion_refills = 0;
  std::optional<MemoryStats> memory;
  std::optional<ServiceStats> service;
  std::optional<SystemStats> system;
  std::optional<HostStats> host;
  double duration_seconds = 0;
  std::optional<ScrubStats> scrub;
};
struct CacheControlRequest {
  std::string_view collection;
  std::string_view tenant;
  std::optional<uint64_t> max_bytes;
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
  std::string_view tenant;
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
  std::optional<float> similarity;
  int32_t k = 0;
  int32_t refine_candidates = 0;
  bool exact = false;
  std::string_view unit;
};
struct StoredResourceDef {
  using Codec = luxir::api::StoredResourceDef_::Codec;
  using Layout = luxir::api::StoredResourceDef_::Layout;
  std::optional<std::int64_t> min_file_bytes;                   // align 8
  std::optional<double> min_live_ratio;
  std::span<const std::string_view> includes;
  std::span<const std::string_view> excludes;
  std::optional<Codec> codec;                                   // align 4 (enum)
  std::optional<Layout> layout;
  std::optional<std::int32_t> zstd_level;                       // align 4
  std::optional<std::int32_t> chunk_size_bytes;
  std::optional<std::int32_t> max_docs_per_chunk;
  std::optional<std::int32_t> zstd_dictionary_bytes;
  std::optional<std::int32_t> max_files;
  std::optional<bool> zstd_dictionary;                          // align 1
  std::optional<bool> enabled;
};
struct SchemaDef {                                                // needs StoredResourceDef
  map_view<std::string_view, ::hpp_proto::indirect_view<FieldDef>> fields;
  map_view<std::string_view, ::hpp_proto::indirect_view<FieldDef>> templates;
  map_view<std::string_view, StoredResourceDef> stored_resources;
};
struct ColVal { std::span<const Val> v; };                        // span<incomplete Val> OK
struct ColVector { std::span<const Vector> v; };
struct ArrVector { std::span<const Vector> v; };
struct SchemaResponse {
  std::string_view commit; std::optional<SchemaDef> schema; std::optional<ReplicaResult> replicas; };  // needs SchemaDef
struct SchemaRequest {                                           // needs SchemaDef
  using Mode = luxir::api::SchemaRequest_::Mode;
  std::string_view collection;
  std::string_view tenant;
  std::optional<SchemaDef> schema;
  std::optional<ReplicaRequirement> wait_for_replicas;
  std::optional<uint64_t> wait_for_replicas_timeout_ms;
  Mode mode = Mode::SET;                                      // align 4 (enum)
};
struct CreateCollectionRequest {
  std::string_view name; std::optional<SchemaDef> schema; std::string_view tenant;
  std::optional<ReplicaRequirement> wait_for_replicas; std::optional<uint64_t> wait_for_replicas_timeout_ms; };
struct CreateCollectionResponse {
  std::string_view commit; std::string_view name; std::optional<ReplicaResult> replicas; };
struct DeleteCollectionRequest { std::string_view name; std::string_view tenant; };
struct DeleteCollectionResponse { std::string_view name; };
struct ListCollectionsResponse { std::span<const std::string_view> collections; };
struct ListTenantsResponse { std::span<const std::string_view> tenants; };
struct MultiVector { std::span<const ArrVector> v; };
struct ArrVal { std::span<const Val> v; };                       // span<incomplete Val> OK

struct Column {                                                  // variant arms all complete above
  std::variant<std::monostate, ColStr, ColInt, ColFloat, ColDouble, ArrArrStr, ArrArrInt,
               ArrArrFloat, ArrArrDouble, ColVal, ColVector, MultiVector>
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
struct Collapse {
  std::span<const std::string_view> field;
  Collapse_::MissingPolicy missing = Collapse_::MissingPolicy::EXPAND;
  std::span<const SortSpec> head_sort;
  std::span<const SortSpec> sort;
  bool get_count = false;
  map_view<std::string_view, ::hpp_proto::indirect_view<SearchOp>> ops;
};
struct Diversify {
  std::span<const std::string_view> field;
  Collapse_::MissingPolicy missing = Collapse_::MissingPolicy::EXPAND;
  std::optional<int64_t> max_per_key;
};
struct TopDocs {                                                 // all indirect/span/opt-scalar
  ::hpp_proto::optional_indirect_view<Query> query;
  std::span<const Filter> filter;
  int64_t offset = 0;
  std::optional<std::int64_t> limit;
  std::optional<std::int64_t> count_limit;
  std::span<const std::string_view> fields;
  std::span<const SortSpec> sort;
  map_view<std::string_view, ::hpp_proto::indirect_view<SearchOp>> ops;
  std::optional<float> min_score;
  int32_t batch_size = 0;
  ::hpp_proto::optional_indirect_view<Collapse> collapse;
  ::hpp_proto::optional_indirect_view<Diversify> diversify;
  DocFormat document_format = DocFormat::DEFAULT;                // align 4 (enum)
  bool get_scores = false;
};
struct Fusion {                                                  // needs TopDocs, RrfFusion, LinearFusion
  map_view<std::string_view, TopDocs> sources;
  std::span<const Filter> filter;
  std::optional<std::int64_t> limit;
  std::optional<std::int64_t> count_limit;
  int64_t offset = 0;
  std::span<const std::string_view> fields;
  map_view<std::string_view, ::hpp_proto::indirect_view<SearchOp>> ops;
  map_view<std::string_view, double> weights;
  ::hpp_proto::optional_indirect_view<Collapse> collapse;
  std::variant<std::monostate, RrfFusion, LinearFusion> method;  // align 4
  int32_t batch_size = 0;
  DocFormat document_format = DocFormat::DEFAULT;                // align 4 (enum)
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
  std::optional<FieldFacet_::CountUnit> count_unit;
  ::hpp_proto::optional_indirect_view<Collapse> count_distinct;
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
  ::hpp_proto::optional_indirect_view<Collapse> count_distinct;
  std::optional<FieldFacet_::CountUnit> count_unit;
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
  std::string_view min_commit;
  std::optional<uint64_t> min_commit_timeout_ms;
  uint64_t flags = 0;
  uint64_t flags_set = 0;
  // JSON parsing metadata, absent from the wire schema and canonical JSON.
  // Only a root TopDocs shorthand creates this implicit q; an explicit op
  // named q (including one modified by a URL overlay) keeps its wrapper.
  bool json_shorthand = false;
};
struct Map { map_view<std::string_view, ::hpp_proto::indirect_view<Val>> fields; };
struct GroupComponent {
  std::variant<std::monostate, google::protobuf::NullValue, std::string_view, std::int64_t> value;
  GroupComponent_::Type type = GroupComponent_::Type::STRING;
};
struct GroupKey {
  std::span<const GroupComponent> component;
  std::optional<uint64_t> singleton;
};
struct GroupTable {
  std::span<const GroupKey> keys;
  std::span<const int64_t> counts;
  std::span<const ArrVal> ranks;
  map_view<std::string_view, ::hpp_proto::indirect_view<Val>> ops;
};
struct DocList {                                                // needs Column (map by value)
  std::optional<std::int64_t> found;
  std::optional<std::int64_t> found_docs;
  std::optional<std::int64_t> found_groups;
  ::hpp_proto::optional_indirect_view<GroupTable> groups;
  map_view<std::string_view, Column> columns;
  // Per-document field maps: docs[i] holds document i's fields not in
  // columns (row_count entries when present).  See the .proto contract.
  std::span<const Map> docs;
  int64_t offset = 0;
  map_view<std::string_view, ::hpp_proto::indirect_view<Val>> ops;
  int32_t row_count = 0;                                       // align 4
  bool more = false;
  bool found_gte = false;
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
  ::hpp_proto::optional_indirect_view<Collapse> count_distinct;
  std::optional<FieldFacet_::CountUnit> count_unit;
  std::variant<std::monostate, Val, CalendarGap> gap_kind;
  std::string_view field;
  ::hpp_proto::optional_indirect_view<Val> start;
  ::hpp_proto::optional_indirect_view<Val> end;
  std::optional<std::int64_t> mincount;
  map_view<std::string_view, ::hpp_proto::indirect_view<SearchOp>> ops;
  std::string_view time_zone;
  ::hpp_proto::optional_indirect_view<Val> selected;
  ::hpp_proto::optional_indirect_view<Val> origin;
  SelectionMode selection_mode = SelectionMode::ANY;
  RangeFacet_::Bounds bounds = RangeFacet_::Bounds::EXPLICIT;
  bool missing = false;
};
struct PluginOp {
  std::string_view name;
  map_view<std::string_view, ::hpp_proto::indirect_view<Val>> params;
  map_view<std::string_view, ::hpp_proto::indirect_view<SearchOp>> ops;
};
struct SearchOp {                                               // needs TopDocs,Fusion,FieldFacet,RangeFacet,QueryFacet,ExprOp,PluginOp,Domain
  std::variant<std::monostate, TopDocs, Fusion, FieldFacet, RangeFacet, QueryFacet, ExprOp, PluginOp> kind;
  std::optional<Domain> domain;
  uint64_t flags = 0;
  uint64_t flags_set = 0;
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
  std::string_view commit;
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
  bool case_insensitive = false;
};
struct AnyOfQuery {
  std::string_view field;
  ::hpp_proto::optional_indirect_view<Val> values;
};
struct PluginQuery {
  std::string_view name;
  map_view<std::string_view, ::hpp_proto::indirect_view<Val>> params;
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
  std::optional<float> min_score;
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
struct WeightedField {
  std::string_view field;
  std::optional<float> weight;
};
struct PhraseOptions {
  int32_t slop = 0;
};
struct MultiMatchQuery {
  using Operator = luxir::api::Match_::Operator;
  using Mode = luxir::api::MultiMatchQuery_::Mode;
  std::string_view text;
  std::span<const WeightedField> fields;
  Mode mode = Mode::COMBINED;
  Operator operator_ = Operator::OPERATOR_UNSPECIFIED;
  int32_t min_match = 0;
  std::optional<float> tie;
  std::optional<PhraseOptions> phrase;
};
struct DisMaxQuery {                                             // span<incomplete Query> OK
  std::span<const Query> queries;
  std::optional<float> tie;
};
struct ExprQuery {                                             // map value indirect: Val may be incomplete, but is complete here anyway
  std::string_view q;
  map_view<std::string_view, ::hpp_proto::indirect_view<Val>> vars;
};
struct Query {                                                 // needs Match,AnyOfQuery,BooleanQuery,Exists,Phrase,Knn,ConstantScore,Prefix,Fuzzy,Simple,Range,Expr,GeoBox,GeoDistance,Boost,Rescore
  std::variant<std::monostate, Match, BooleanQuery, bool, ExistsQuery, PhraseQuery, KnnQuery,
               ConstantScoreQuery, PrefixQuery, FuzzyQuery, SimpleQuery, RangeQuery, ExprQuery,
               GeoBoxQuery, GeoDistanceQuery, BoostQuery, RescoreQuery, WildcardQuery, RegexQuery,
               AnyOfQuery, MultiMatchQuery, DisMaxQuery, PluginQuery>
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
  // Source includes/excludes for this request's docs, replacing the collection's.
  std::span<const std::string_view> source_includes;
  std::span<const std::string_view> source_excludes;
  bool allow_dups = false;
  bool all_or_none = false;
  bool return_ids = false;
  bool drop_unmapped = false;
};

// ===================== trivial-destructibility checks =====================
#define LUXIR_TD(M) static_assert(std::is_trivially_destructible_v<M>);
LUXIR_TD(SearchRequest) LUXIR_TD(SearchOp) LUXIR_TD(ExprOp) LUXIR_TD(TopDocs)
LUXIR_TD(Diversify) LUXIR_TD(Collapse) LUXIR_TD(GroupComponent) LUXIR_TD(GroupKey) LUXIR_TD(GroupTable)
LUXIR_TD(Fusion) LUXIR_TD(RrfFusion) LUXIR_TD(LinearFusion) LUXIR_TD(SortSpec) LUXIR_TD(Query)
LUXIR_TD(ExistsQuery)
LUXIR_TD(ConstantScoreQuery) LUXIR_TD(BoostQuery) LUXIR_TD(RescoreQuery) LUXIR_TD(KnnQuery) LUXIR_TD(Match) LUXIR_TD(AnyOfQuery) LUXIR_TD(Filter) LUXIR_TD(BooleanQuery)
LUXIR_TD(PrefixQuery) LUXIR_TD(WildcardQuery) LUXIR_TD(RegexQuery) LUXIR_TD(FuzzyQuery) LUXIR_TD(PhraseQuery) LUXIR_TD(SimpleQuery) LUXIR_TD(RangeQuery)
LUXIR_TD(GeoBoxQuery) LUXIR_TD(GeoDistanceQuery) LUXIR_TD(ExprQuery) LUXIR_TD(WeightedField) LUXIR_TD(PhraseOptions) LUXIR_TD(MultiMatchQuery) LUXIR_TD(DisMaxQuery) LUXIR_TD(PluginQuery) LUXIR_TD(PluginOp)
LUXIR_TD(Warning) LUXIR_TD(Error) LUXIR_TD(ExecutionProfile) LUXIR_TD(ExecutionProfileOp)
LUXIR_TD(ExecutionProfilePiece) LUXIR_TD(FieldFacet) LUXIR_TD(CalendarGap) LUXIR_TD(RangeFacet)
LUXIR_TD(QueryBucket) LUXIR_TD(QueryFacet)
LUXIR_TD(Domain) LUXIR_TD(SearchResponse) LUXIR_TD(DocList) LUXIR_TD(FacetResult)
LUXIR_TD(CollectionCommit) LUXIR_TD(ReplicaResult) LUXIR_TD(CommitParams) LUXIR_TD(UpdateRequest) LUXIR_TD(UpdateResponse) LUXIR_TD(Map)
LUXIR_TD(Val) LUXIR_TD(ArrVal) LUXIR_TD(ArrStr) LUXIR_TD(ArrInt) LUXIR_TD(ArrFloat)
LUXIR_TD(ArrDouble) LUXIR_TD(ArrBin) LUXIR_TD(ArrArrStr) LUXIR_TD(ArrArrInt) LUXIR_TD(ArrArrFloat)
LUXIR_TD(ArrArrDouble) LUXIR_TD(Vector) LUXIR_TD(ArrVector)
LUXIR_TD(ColStr) LUXIR_TD(Column) LUXIR_TD(ColVal) LUXIR_TD(ColVector) LUXIR_TD(MultiVector) LUXIR_TD(ColInt)
LUXIR_TD(ColFloat) LUXIR_TD(ColDouble)
LUXIR_TD(AnalyzerComponent) LUXIR_TD(AnalyzerDef) LUXIR_TD(FieldDef) LUXIR_TD(SchemaDef)
LUXIR_TD(FieldVariants) LUXIR_TD(FieldDefaults) LUXIR_TD(NormalizerDef) LUXIR_TD(StoredResourceDef)
LUXIR_TD(VectorAnnDef) LUXIR_TD(VectorAnnDef_::RaBitQ) LUXIR_TD(VectorAnnDef_::Pq)
LUXIR_TD(SchemaRequest) LUXIR_TD(SchemaResponse) LUXIR_TD(UpdateResponse_::DocError)
LUXIR_TD(KnnQuery_::Ivf)
LUXIR_TD(CreateCollectionRequest) LUXIR_TD(CreateCollectionResponse)
LUXIR_TD(DeleteCollectionRequest) LUXIR_TD(DeleteCollectionResponse) LUXIR_TD(ListCollectionsResponse)
LUXIR_TD(ReplicationStatus) LUXIR_TD(ReplicationCollectionStatus) LUXIR_TD(FollowerStatus) LUXIR_TD(StatsRequest) LUXIR_TD(StatsResponse) LUXIR_TD(OutcomeStats) LUXIR_TD(CollectionActivityStats) LUXIR_TD(StatsTotals) LUXIR_TD(CollectionStats)
LUXIR_TD(ShardStats) LUXIR_TD(IndexStats) LUXIR_TD(SegmentStats) LUXIR_TD(AuxStats)
LUXIR_TD(QueryCacheStats) LUXIR_TD(StorageRamStats) LUXIR_TD(IndexRamStats) LUXIR_TD(ArenaStats)
LUXIR_TD(JemallocStats) LUXIR_TD(FieldMetadataStats) LUXIR_TD(MemoryStats) LUXIR_TD(ScrubStats)
LUXIR_TD(HistogramStats) LUXIR_TD(ActivityStats) LUXIR_TD(TransportStats) LUXIR_TD(ServiceStats) LUXIR_TD(HostStats) LUXIR_TD(SystemStats) LUXIR_TD(ProcessIoStats) LUXIR_TD(ProcessStats) LUXIR_TD(CpuTimes) LUXIR_TD(HostCpuStats) LUXIR_TD(HostMemoryStats) LUXIR_TD(NetworkInterfaceStats) LUXIR_TD(TcpStats) LUXIR_TD(DiskStats) LUXIR_TD(FilesystemStats) LUXIR_TD(PressureStats) LUXIR_TD(CgroupStats) LUXIR_TD(CgroupMemoryStats) LUXIR_TD(CgroupCpuStats) LUXIR_TD(CgroupTaskStats)
LUXIR_TD(CacheControlRequest) LUXIR_TD(CacheControlResponse) LUXIR_TD(CacheEntryDump)
LUXIR_TD(ShardCacheControl) LUXIR_TD(CollectionCacheControl)
LUXIR_TD(AllReplicas) LUXIR_TD(ReplicaRequirement) LUXIR_TD(ReplicationCatalog) LUXIR_TD(ReplicationCatalogEntry) LUXIR_TD(ReplicationInstalled)
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
LUXIR_ENTRY(Diversify) LUXIR_ENTRY(Collapse) LUXIR_ENTRY(GroupComponent)
LUXIR_ENTRY(GroupKey) LUXIR_ENTRY(GroupTable)
LUXIR_ENTRY(TopDocs) LUXIR_ENTRY(Fusion) LUXIR_ENTRY(RrfFusion) LUXIR_ENTRY(LinearFusion) LUXIR_ENTRY(SortSpec)
LUXIR_ENTRY(Query) LUXIR_ENTRY(ExistsQuery) LUXIR_ENTRY(ConstantScoreQuery) LUXIR_ENTRY(BoostQuery) LUXIR_ENTRY(RescoreQuery)
LUXIR_ENTRY(KnnQuery) LUXIR_ENTRY(Match) LUXIR_ENTRY(AnyOfQuery) LUXIR_ENTRY(Filter) LUXIR_ENTRY(BooleanQuery)
LUXIR_ENTRY(PrefixQuery) LUXIR_ENTRY(WildcardQuery) LUXIR_ENTRY(RegexQuery) LUXIR_ENTRY(FuzzyQuery) LUXIR_ENTRY(PhraseQuery) LUXIR_ENTRY(SimpleQuery)
LUXIR_ENTRY(RangeQuery) LUXIR_ENTRY(GeoBoxQuery) LUXIR_ENTRY(GeoDistanceQuery) LUXIR_ENTRY(ExprQuery)
LUXIR_ENTRY(WeightedField) LUXIR_ENTRY(PhraseOptions) LUXIR_ENTRY(MultiMatchQuery) LUXIR_ENTRY(DisMaxQuery) LUXIR_ENTRY(PluginQuery) LUXIR_ENTRY(PluginOp)
LUXIR_ENTRY(Warning) LUXIR_ENTRY(Error) LUXIR_ENTRY(ExecutionProfile) LUXIR_ENTRY(ExecutionProfileOp)
LUXIR_ENTRY(ExecutionProfilePiece) LUXIR_ENTRY(FieldFacet)
LUXIR_ENTRY(CalendarGap) LUXIR_ENTRY(RangeFacet) LUXIR_ENTRY(QueryBucket) LUXIR_ENTRY(QueryFacet)
LUXIR_ENTRY(Domain) LUXIR_ENTRY(SearchResponse) LUXIR_ENTRY(DocList)
LUXIR_ENTRY(FacetResult) LUXIR_ENTRY(CollectionCommit) LUXIR_ENTRY(ReplicaResult) LUXIR_ENTRY(CommitParams) LUXIR_ENTRY(UpdateRequest)
LUXIR_ENTRY(UpdateResponse) LUXIR_ENTRY(Map)
LUXIR_ENTRY(Val) LUXIR_ENTRY(ArrVal) LUXIR_ENTRY(ArrStr) LUXIR_ENTRY(ArrInt) LUXIR_ENTRY(ArrFloat)
LUXIR_ENTRY(ArrDouble) LUXIR_ENTRY(ArrBin) LUXIR_ENTRY(ArrArrStr) LUXIR_ENTRY(ArrArrInt)
LUXIR_ENTRY(ArrArrFloat) LUXIR_ENTRY(ArrArrDouble) LUXIR_ENTRY(Vector)
LUXIR_ENTRY(ArrVector) LUXIR_ENTRY(ColStr) LUXIR_ENTRY(Column)
LUXIR_ENTRY(ColVal) LUXIR_ENTRY(ColVector) LUXIR_ENTRY(MultiVector) LUXIR_ENTRY(ColInt) LUXIR_ENTRY(ColFloat)
LUXIR_ENTRY(ColDouble) LUXIR_ENTRY(AnalyzerComponent) LUXIR_ENTRY(AnalyzerDef) LUXIR_ENTRY(FieldDef)
LUXIR_ENTRY(FieldVariants) LUXIR_ENTRY(FieldDefaults) LUXIR_ENTRY(NormalizerDef) LUXIR_ENTRY(StoredResourceDef)
LUXIR_ENTRY(VectorAnnDef)
LUXIR_ENTRY(SchemaDef) LUXIR_ENTRY(SchemaRequest) LUXIR_ENTRY(SchemaResponse)
LUXIR_ENTRY(CreateCollectionRequest) LUXIR_ENTRY(CreateCollectionResponse)
LUXIR_ENTRY(DeleteCollectionRequest) LUXIR_ENTRY(DeleteCollectionResponse)
LUXIR_ENTRY(ListCollectionsResponse)
LUXIR_ENTRY(ListTenantsResponse)
LUXIR_ENTRY(ReplicationStatus) LUXIR_ENTRY(ReplicationCollectionStatus) LUXIR_ENTRY(FollowerStatus) LUXIR_ENTRY(StatsRequest) LUXIR_ENTRY(StatsResponse) LUXIR_ENTRY(OutcomeStats) LUXIR_ENTRY(CollectionActivityStats) LUXIR_ENTRY(StatsTotals)
LUXIR_ENTRY(CollectionStats) LUXIR_ENTRY(ShardStats) LUXIR_ENTRY(IndexStats)
LUXIR_ENTRY(SegmentStats) LUXIR_ENTRY(AuxStats) LUXIR_ENTRY(QueryCacheStats)
LUXIR_ENTRY(StorageRamStats) LUXIR_ENTRY(IndexRamStats) LUXIR_ENTRY(ArenaStats)
LUXIR_ENTRY(JemallocStats) LUXIR_ENTRY(FieldMetadataStats) LUXIR_ENTRY(MemoryStats) LUXIR_ENTRY(ScrubStats)
LUXIR_ENTRY(HistogramStats) LUXIR_ENTRY(ActivityStats) LUXIR_ENTRY(TransportStats) LUXIR_ENTRY(ServiceStats) LUXIR_ENTRY(HostStats) LUXIR_ENTRY(SystemStats) LUXIR_ENTRY(ProcessIoStats) LUXIR_ENTRY(ProcessStats) LUXIR_ENTRY(CpuTimes) LUXIR_ENTRY(HostCpuStats) LUXIR_ENTRY(HostMemoryStats) LUXIR_ENTRY(NetworkInterfaceStats) LUXIR_ENTRY(TcpStats) LUXIR_ENTRY(DiskStats) LUXIR_ENTRY(FilesystemStats) LUXIR_ENTRY(PressureStats) LUXIR_ENTRY(CgroupStats) LUXIR_ENTRY(CgroupMemoryStats) LUXIR_ENTRY(CgroupCpuStats) LUXIR_ENTRY(CgroupTaskStats)
LUXIR_ENTRY(CacheControlRequest) LUXIR_ENTRY(CacheControlResponse) LUXIR_ENTRY(CacheEntryDump)
LUXIR_ENTRY(ShardCacheControl) LUXIR_ENTRY(CollectionCacheControl)
LUXIR_ENTRY(AllReplicas) LUXIR_ENTRY(ReplicaRequirement) LUXIR_ENTRY(ReplicationCatalog) LUXIR_ENTRY(ReplicationCatalogEntry) LUXIR_ENTRY(ReplicationInstalled)
#undef LUXIR_ENTRY

} // namespace luxir::api
