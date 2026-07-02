// HAND-WRITTEN concrete classes for solux_types.proto (Phase 1 spike).
// Mirrors the non-owning instantiation (gen_real_templated/solux_types.msg.hpp), de-templatized:
// std::string_view / std::span / map_view / optional_indirect_view / indirect_view / std::optional,
// enums in Foo_ namespaces, no <Traits>, no unknown_fields_. Struct ORDER is a strict topological
// sort: span<const X> and (optional_)indirect_view<X> tolerate an incomplete X (break cycles), but
// std::optional<X>, by-value X (variant arms), and map_view<K,X-by-value> need X complete first.
// The generated solux_types.pb.cpp / .json.cpp bind pb_meta + glz::meta to these by pointer.
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

#include <hpp_proto/indirect_view.hpp>
#include <hpp_proto/optional_indirect.hpp>
#include <hpp_proto/json.hpp> // for the google::protobuf::NullValue glz below (WKT)

// ----- map_view: span-of-pairs with a map read API (last-wins find) -----
namespace solux::api {
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
} // namespace solux::api

// ----- WKT: google.protobuf.NullValue (Val.null arm) + its JSON glz (renders null) -----
namespace google::protobuf {
enum class NullValue { NULL_VALUE = 0 };
} // namespace google::protobuf
namespace glz {
template <>
struct to<JSON, google::protobuf::NullValue> {
  template <auto Opts>
  GLZ_ALWAYS_INLINE static void op(auto && /*value*/, auto &&...args) {
    serialize<JSON>::template op<Opts>(std::monostate{}, std::forward<decltype(args)>(args)...);
  }
};
template <>
struct from<JSON, google::protobuf::NullValue> {
  template <auto Opts>
  GLZ_ALWAYS_INLINE static void op(auto &value, auto &&...args) {
    parse<JSON>::template op<Opts>(std::monostate{}, std::forward<decltype(args)>(args)...);
    value = google::protobuf::NullValue::NULL_VALUE;
  }
};

// optional_indirect_view<T> is the non-owning singular optional message field (TopDocs.query,
// Match.val, ...). hpp-proto only ships glz for the optional_indirect_view_REF wrapper; the
// generated glz binds the raw member, so add from/to for the raw view here (arena-allocate on
// read, mirroring hpp-proto's optional_indirect_view_ref handler).
template <typename Type>
struct from<JSON, ::hpp_proto::optional_indirect_view<Type>> {
  template <auto Opts>
  static void op(auto &value, ::hpp_proto::concepts::is_non_owning_context auto &ctx, auto &it, auto &end) {
    if (!util::parse_null<Opts>(value, ctx, it, end)) {
      void *addr = ctx.memory_resource().allocate(sizeof(Type), alignof(Type));
      auto *obj = new (addr) Type; // NOLINT(cppcoreguidelines-owning-memory)
      parse<JSON>::template op<Opts>(*obj, ctx, it, end);
      value = obj;
    }
  }
};
template <typename Type>
struct to<JSON, ::hpp_proto::optional_indirect_view<Type>> {
  template <auto Opts, class... Args>
  GLZ_ALWAYS_INLINE static void op(auto &&value, Args &&...args) noexcept {
    if (value.has_value()) {
      to<JSON, Type>::template op<Opts>(*value, std::forward<Args>(args)...);
    }
  }
};
} // namespace glz

namespace solux::api {

// ---- forward declarations (all messages) ----
struct Target; struct SearchRequest; struct SearchOp; struct GenOp; struct TopDocs;
struct Fusion; struct RrfFusion; struct SortSpec; struct Query; struct ForcePrepareQuery;
struct ConstantScoreQuery; struct KnnQuery; struct Match; struct NamedQuery; struct BooleanQuery;
struct PrefixQuery; struct FuzzyQuery; struct PhraseQuery; struct FieldFacet; struct RangeFacet;
struct Domain; struct SearchResponse; struct DocList; struct FacetResult; struct Bucket;
struct CommitParams; struct UpdateRequest; struct UpdateResponse; struct NamedValue; struct Map;
struct Columns; struct Val; struct ArrVal; struct ArrStr; struct ArrInt; struct ArrFloat;
struct ArrDouble; struct ArrBin; struct ArrArrStr; struct ArrArrInt; struct ArrArrFloat;
struct ArrArrDouble; struct ArrArrBin; struct Vector; struct ArrVector; struct ArrInt32;
struct ColStr; struct Column; struct ColVector; struct MultiVector; struct ColInt;
struct ColFloat; struct ColDouble; struct ColMap; struct IndexInfo; struct AuxIndexInfo;
struct SegmentInfo; struct AnalyzerDef; struct FieldDef; struct VectorParams; struct SchemaDef;
struct SchemaRequest; struct SchemaResponse;
namespace UpdateResponse_ { struct Error; }

// ---- nested enums (Foo_ namespace; matches generated metadata refs) ----
namespace SortSpec_ { enum class SortDir { UNKNOWN = 0, ASC = 1, ASCENDING = 1, DESC = 2, DESCENDING = 2 }; }
namespace Match_ { enum class Operator { OPERATOR_UNSPECIFIED = 0, OR = 1, AND = 2 }; }
namespace UpdateResponse_ { enum class Status { UNKNOWN = 0, OK = 1, PARTIAL = 2, ERROR = 3 }; }
namespace FieldDef_ {
enum class FieldClass { STRING = 0, TEXT = 1, INT = 2, FLOAT = 3, DOUBLE = 4, BIN = 5, ID = 6, VECTOR = 7, DATE = 8 };
}
namespace VectorParams_ { enum class Metric { NONE = 0, L2 = 1, IP = 2, COSINE = 3 }; }
namespace SchemaRequest_ { enum class Mode { MERGE = 0, REPLACE = 1 }; }

// ===================== message definitions (strict topological order) =====================

struct Target { std::span<const std::string_view> name; };
struct RrfFusion { std::int32_t k = {}; };
struct SortSpec {
  using SortDir = solux::api::SortSpec_::SortDir;
  std::string_view field;
  SortDir dir = SortDir::UNKNOWN;
};
struct PrefixQuery { std::string_view field; std::string_view prefix; };
struct FuzzyQuery {
  std::string_view field;
  std::string_view term;
  std::optional<std::int32_t> max_edits;
  std::optional<std::int32_t> prefix_length;
  std::int32_t max_expansions = {};
};
struct PhraseQuery {
  std::string_view field;
  std::string_view text;
  std::span<const std::string_view> words;
  std::span<const std::string_view> terms;
  std::span<const ::hpp_proto::bytes_view> terms_bin;
  std::span<const std::int32_t> positions;
};
struct CommitParams {
  std::uint64_t commit_within_us = {};
  std::span<const std::string_view> build_aux_indexes;
  bool wait_for_merges = {};
};

namespace UpdateResponse_ {
struct Error { std::string_view id; std::string_view error_message; std::int32_t index = {}; };
} // namespace UpdateResponse_

struct UpdateResponse {
  using Status = solux::api::UpdateResponse_::Status;
  using Error = solux::api::UpdateResponse_::Error;
  std::string_view request_id;
  std::uint64_t update_version = {};
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
struct ColInt { std::int64_t missing_val = {}; std::span<const std::int64_t> v; };
struct ColFloat { float missing_val = {}; std::span<const float> v; };
struct ColDouble { double missing_val = {}; std::span<const double> v; };

struct AuxIndexInfo {
  std::string_view kind;
  std::string_view field;
  std::string_view name;
  std::uint64_t gen = {};
  std::uint64_t commit_time = {};
  std::span<const std::string_view> files;
  ::hpp_proto::bytes_view opaque_meta;
  std::uint64_t built_core_gen = {};
};
struct AnalyzerDef { std::string_view tokenizer; std::span<const std::string_view> filters; };
struct VectorParams {
  using Metric = solux::api::VectorParams_::Metric;
  std::int32_t dims = {};
  Metric metric = Metric::NONE;
  std::optional<bool> normalized;
  std::optional<bool> normalize_on_write;
};
struct FieldDef {
  using FieldClass = solux::api::FieldDef_::FieldClass;
  std::string_view name;
  std::string_view parent;
  std::optional<AnalyzerDef> analyzer;                          // align 8
  std::string_view stored_resource;
  std::optional<FieldClass> field_class;                        // align 4 (enum)
  std::optional<VectorParams> vector;                           // align 4
  bool abstract = {};                                           // align 1 (bools + optional<bool>)
  std::optional<bool> indexed;
  std::optional<bool> column_stored;
  std::optional<bool> multi_valued;
  std::optional<bool> stored;
};
struct SegmentInfo {
  std::uint64_t seg_id = {};
  std::uint64_t live_gen = {};
  std::uint64_t min_version = {};
  std::uint64_t max_version = {};
  std::uint64_t commit_time = {};
  std::uint64_t schema_gen = {};
  std::span<const AuxIndexInfo> overlays;
  std::int32_t max_doc = {};
  std::int32_t live_docs = {};
};
struct IndexInfo {
  std::uint64_t version = {};
  std::uint64_t commit_time = {};
  std::uint64_t index_gen = {};
  std::uint64_t core_gen = {};
  std::uint64_t update_version = {};
  std::uint64_t schema_gen = {};
  std::span<const SegmentInfo> segments;
  std::span<const AuxIndexInfo> aux_indexes;
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
  std::int32_t k = {};
  std::int32_t nprobe = {};
  std::int32_t refine_candidates = {};
  float min_scan_fraction = {};
  bool exact = {};
};
struct SchemaDef { std::span<const FieldDef> fields; };
struct ColVector { std::span<const Vector> v; };
struct ArrVector { std::span<const Vector> v; };
struct SchemaResponse { std::optional<SchemaDef> schema; };      // needs SchemaDef
struct SchemaRequest {                                           // needs Target, SchemaDef
  using Mode = solux::api::SchemaRequest_::Mode;
  std::optional<Target> collection;
  std::optional<SchemaDef> schema;
  Mode mode = Mode::MERGE;                                      // align 4 (enum)
};
struct MultiVector { std::span<const ArrVector> v; };
struct ColMap { std::span<const Map> v; };                       // span<incomplete Map> OK
struct ArrVal { std::span<const Val> v; };                       // span<incomplete Val> OK

struct Column {                                                  // variant arms all complete above
  std::variant<std::monostate, ColStr, ColInt, ColFloat, ColDouble, ArrArrStr, ArrArrInt,
               ArrArrFloat, ArrArrDouble, ColMap, ArrVal, ColVector, MultiVector>
      kind;
};
struct Columns { map_view<std::string_view, Column> columns; };  // needs Column

struct BooleanQuery {                                            // span<incomplete Query> OK
  std::span<const Query> filter;
  std::span<const Query> required;
  std::span<const Query> optional;
  std::span<const Query> prohibited;
  std::int32_t min_match = {};
};
struct TopDocs {                                                 // all indirect/span/opt-scalar
  ::hpp_proto::optional_indirect_view<Query> query;
  std::span<const NamedQuery> filter;
  std::int64_t offset = {};
  std::optional<std::int64_t> limit;
  std::span<const std::string_view> fields;
  std::span<const SortSpec> sorts;
  map_view<std::string_view, ::hpp_proto::indirect_view<SearchOp>> ops;
  std::int32_t batch_size = {};
  bool get_number = {};
  bool get_scores = {};
};
struct Fusion {                                                  // needs TopDocs, RrfFusion
  map_view<std::string_view, TopDocs> sources;
  std::span<const NamedQuery> filter;
  std::optional<std::int64_t> limit;
  std::int64_t offset = {};
  std::span<const std::string_view> fields;
  map_view<std::string_view, ::hpp_proto::indirect_view<SearchOp>> ops;
  std::optional<RrfFusion> rrf;                                  // align 4 (RrfFusion is one int32)
  std::int32_t batch_size = {};
  bool get_number = {};
  bool get_scores = {};
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
  bool missing = {};
};
struct RangeFacet {
  std::string_view field;
  std::optional<std::int64_t> start;
  std::optional<std::int64_t> end;
  std::int64_t gap = {};
  std::optional<std::int64_t> mincount;
  std::span<const SortSpec> sorts;
  map_view<std::string_view, ::hpp_proto::indirect_view<SearchOp>> ops;
  bool missing = {};
};
struct SearchOp {                                               // needs TopDocs,Fusion,FieldFacet,RangeFacet,GenOp
  std::variant<std::monostate, TopDocs, Fusion, FieldFacet, RangeFacet, GenOp> kind;
};
struct SearchRequest {                                          // needs Target
  std::string_view request_id;
  std::optional<Target> collection;
  map_view<std::string_view, ::hpp_proto::indirect_view<SearchOp>> ops;
  std::uint64_t freshness_us = {};
};
struct Map { map_view<std::string_view, ::hpp_proto::indirect_view<Val>> fields; };
struct Bucket {
  ::hpp_proto::optional_indirect_view<Val> bucket_id;
  map_view<std::string_view, ::hpp_proto::indirect_view<Val>> ops;
};
struct DocList {                                                // needs Column (map by value)
  std::optional<std::int64_t> matches;
  map_view<std::string_view, Column> columns;
  std::int64_t offset = {};
  map_view<std::string_view, ::hpp_proto::indirect_view<Val>> ops;
  std::optional<float> max_score;                              // align 4
  bool more = {};
};
struct FacetResult {                                           // needs Column (optional)
  std::optional<std::int64_t> total_buckets;
  std::optional<Column> bucket_ids;
  std::span<const std::int64_t> counts;
  std::optional<std::int64_t> missing;
  map_view<std::string_view, ::hpp_proto::indirect_view<Val>> ops;
  std::int64_t offset = {};
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
struct SearchResponse {
  std::string_view request_id;
  map_view<std::string_view, ::hpp_proto::indirect_view<Val>> ops;
  std::string_view error;
  bool more = {};
};
struct NamedValue { std::string_view name; ::hpp_proto::optional_indirect_view<Val> val; };
struct Match {
  using Operator = solux::api::Match_::Operator;
  std::string_view field;
  ::hpp_proto::optional_indirect_view<Val> val;
  Operator operator_ = Operator::OPERATOR_UNSPECIFIED;
  std::int32_t min_match = {};
};
struct ConstantScoreQuery {
  ::hpp_proto::optional_indirect_view<Query> query;
  std::optional<float> score;
};
struct ForcePrepareQuery { ::hpp_proto::optional_indirect_view<Query> query; };
struct Query {                                                 // needs Match,BooleanQuery,Phrase,Knn,ConstantScore,Prefix,Fuzzy,ForcePrepare
  std::variant<std::monostate, Match, BooleanQuery, bool, std::string_view, PhraseQuery, KnnQuery,
               ConstantScoreQuery, PrefixQuery, FuzzyQuery, ForcePrepareQuery>
      kind;
};
struct NamedQuery { std::string_view name; ::hpp_proto::optional_indirect_view<Query> query; };
struct UpdateRequest {                                         // needs Target,Columns,CommitParams
  std::string_view request_id;
  std::int64_t stream_id = {};
  std::optional<Target> collection;
  std::span<const Map> docs;
  std::optional<Columns> columns;
  std::span<const std::string_view> delete_ids;
  std::optional<CommitParams> commit;
  bool allow_dups = {};
  bool all_or_none = {};
  bool return_ids = {};
};

// ===================== trivial-destructibility checks =====================
#define SOLUX_TD(M) static_assert(std::is_trivially_destructible_v<M>);
SOLUX_TD(Target) SOLUX_TD(SearchRequest) SOLUX_TD(SearchOp) SOLUX_TD(GenOp) SOLUX_TD(TopDocs)
SOLUX_TD(Fusion) SOLUX_TD(RrfFusion) SOLUX_TD(SortSpec) SOLUX_TD(Query) SOLUX_TD(ForcePrepareQuery)
SOLUX_TD(ConstantScoreQuery) SOLUX_TD(KnnQuery) SOLUX_TD(Match) SOLUX_TD(NamedQuery) SOLUX_TD(BooleanQuery)
SOLUX_TD(PrefixQuery) SOLUX_TD(FuzzyQuery) SOLUX_TD(PhraseQuery) SOLUX_TD(FieldFacet) SOLUX_TD(RangeFacet)
SOLUX_TD(Domain) SOLUX_TD(SearchResponse) SOLUX_TD(DocList) SOLUX_TD(FacetResult) SOLUX_TD(Bucket)
SOLUX_TD(CommitParams) SOLUX_TD(UpdateRequest) SOLUX_TD(UpdateResponse) SOLUX_TD(NamedValue) SOLUX_TD(Map)
SOLUX_TD(Columns) SOLUX_TD(Val) SOLUX_TD(ArrVal) SOLUX_TD(ArrStr) SOLUX_TD(ArrInt) SOLUX_TD(ArrFloat)
SOLUX_TD(ArrDouble) SOLUX_TD(ArrBin) SOLUX_TD(ArrArrStr) SOLUX_TD(ArrArrInt) SOLUX_TD(ArrArrFloat)
SOLUX_TD(ArrArrDouble) SOLUX_TD(ArrArrBin) SOLUX_TD(Vector) SOLUX_TD(ArrVector) SOLUX_TD(ArrInt32)
SOLUX_TD(ColStr) SOLUX_TD(Column) SOLUX_TD(ColVector) SOLUX_TD(MultiVector) SOLUX_TD(ColInt)
SOLUX_TD(ColFloat) SOLUX_TD(ColDouble) SOLUX_TD(ColMap) SOLUX_TD(IndexInfo) SOLUX_TD(AuxIndexInfo)
SOLUX_TD(SegmentInfo) SOLUX_TD(AnalyzerDef) SOLUX_TD(FieldDef) SOLUX_TD(VectorParams) SOLUX_TD(SchemaDef)
SOLUX_TD(SchemaRequest) SOLUX_TD(SchemaResponse) SOLUX_TD(UpdateResponse_::Error)
#undef SOLUX_TD

// ===================== out-of-line codec entry-point declarations =====================
// decode() uses hpp_proto::padded_input. Callers must pass a payload-only span
// with 16 readable bytes past data.end(), and the first one must be zero.
#define SOLUX_ENTRY(M)                                                                      \
  bool decode(M &, std::span<const std::byte> data, std::pmr::memory_resource &arena);      \
  bool encode(const M &, std::vector<std::byte> &out);                                      \
  bool write_json(const M &, std::string &out);                                             \
  bool read_json(M &, std::string_view json, std::pmr::memory_resource &arena,              \
                 std::string *error = nullptr);
SOLUX_ENTRY(Target) SOLUX_ENTRY(SearchRequest) SOLUX_ENTRY(SearchOp) SOLUX_ENTRY(GenOp)
SOLUX_ENTRY(TopDocs) SOLUX_ENTRY(Fusion) SOLUX_ENTRY(RrfFusion) SOLUX_ENTRY(SortSpec)
SOLUX_ENTRY(Query) SOLUX_ENTRY(ForcePrepareQuery) SOLUX_ENTRY(ConstantScoreQuery)
SOLUX_ENTRY(KnnQuery) SOLUX_ENTRY(Match) SOLUX_ENTRY(NamedQuery) SOLUX_ENTRY(BooleanQuery)
SOLUX_ENTRY(PrefixQuery) SOLUX_ENTRY(FuzzyQuery) SOLUX_ENTRY(PhraseQuery) SOLUX_ENTRY(FieldFacet)
SOLUX_ENTRY(RangeFacet) SOLUX_ENTRY(Domain) SOLUX_ENTRY(SearchResponse) SOLUX_ENTRY(DocList)
SOLUX_ENTRY(FacetResult) SOLUX_ENTRY(Bucket) SOLUX_ENTRY(CommitParams) SOLUX_ENTRY(UpdateRequest)
SOLUX_ENTRY(UpdateResponse) SOLUX_ENTRY(NamedValue) SOLUX_ENTRY(Map) SOLUX_ENTRY(Columns)
SOLUX_ENTRY(Val) SOLUX_ENTRY(ArrVal) SOLUX_ENTRY(ArrStr) SOLUX_ENTRY(ArrInt) SOLUX_ENTRY(ArrFloat)
SOLUX_ENTRY(ArrDouble) SOLUX_ENTRY(ArrBin) SOLUX_ENTRY(ArrArrStr) SOLUX_ENTRY(ArrArrInt)
SOLUX_ENTRY(ArrArrFloat) SOLUX_ENTRY(ArrArrDouble) SOLUX_ENTRY(ArrArrBin) SOLUX_ENTRY(Vector)
SOLUX_ENTRY(ArrVector) SOLUX_ENTRY(ArrInt32) SOLUX_ENTRY(ColStr) SOLUX_ENTRY(Column)
SOLUX_ENTRY(ColVector) SOLUX_ENTRY(MultiVector) SOLUX_ENTRY(ColInt) SOLUX_ENTRY(ColFloat)
SOLUX_ENTRY(ColDouble) SOLUX_ENTRY(ColMap) SOLUX_ENTRY(IndexInfo) SOLUX_ENTRY(AuxIndexInfo)
SOLUX_ENTRY(SegmentInfo) SOLUX_ENTRY(AnalyzerDef) SOLUX_ENTRY(FieldDef) SOLUX_ENTRY(VectorParams)
SOLUX_ENTRY(SchemaDef) SOLUX_ENTRY(SchemaRequest) SOLUX_ENTRY(SchemaResponse)
#undef SOLUX_ENTRY

} // namespace solux::api

// Solux JSON dialect: hand from/to<JSON> overrides of the generated glz::meta
// (untagged Val, flattened Map, bare-array Vector, ...). Included here so every TU
// that can instantiate glaze over these types (in practice only the generated
// .json.cpp) agrees on the dialect.
#include "json_dialect.h"
