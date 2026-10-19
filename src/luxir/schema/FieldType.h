// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "luxir/analysis/Analyzer.h"
#include "luxir/reader/Postings.h"
#include "luxir/util/DateTime.h"
#include "luxir/util/StrRef.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace luxir {

namespace api { struct Val; }  // the wire value oneof (api/luxir_types.hpp)

// Request/update-scoped inputs that can affect value coercion. Updates use the
// default UTC frame; request-facing parser seams always pass the whole context.
struct CoerceContext {
  std::optional<int64_t> dateMathNowEpochMillis;
  TimeZone timeZone = TimeZone::utc();
};



// FieldType contains meta-data about a field, including all the information necessary
// to index that field.  It is independent of any given index segment, as it must exist
// before indexing any data.
class FieldType {
public:
  // NOTE: these ordinals are persisted as-is in the segment (PostingsWriter
  // writes IndexFieldInfo::type, FieldReader reads it back via static_cast).
  // Append new types at the end; reordering silently misreads existing segments.
  enum Type {
    NONE=0,
    STRING,   // unanalyzed string field
    TEXT,     // analyzed text field (if indexed)
    BIN,      // binary field
    INT,
    FLOAT,
    DOUBLE,
    ID,       // unique id field
    VECTOR,   // dense float vector; column-stored as fixed-size bytes
    DATE,     // timestamp; column-stored as int64 milliseconds since the Unix epoch
    GEO_POINT // latitude/longitude point; packed into one int64 column value
  };

  using flag_type = int32_t;

  static constexpr flag_type INDEX_DOCS = (1 << 0);
  static constexpr flag_type INDEX_DOCS_FREQS = INDEX_DOCS | (1 << 1);
  static constexpr flag_type INDEX_DOCS_FREQS_POSITIONS = INDEX_DOCS_FREQS | (1 << 2);
  static constexpr flag_type NUM_TOKENS_APPROX = (1 << 3);
  static constexpr flag_type NUM_TOKENS_EXACT = (1 << 4);
  static constexpr flag_type MULTI_VALUED = (1 << 5);  // Set if the field can have multiple values per document
  static constexpr flag_type FIELD_SPECIFIC_ANALYZER = (1 << 6);  // Set if different fields using the same type have different analyzers (i.e. don't cache across different fields)

  static constexpr flag_type COLUMN_STORED = (1 << 7);  // Set if the field has values stored in a columnar format
  static constexpr flag_type FIXED_SIZE = (1 << 8);     // if all values have the same size in bytes (for otherwise variable-length fields)
  static constexpr flag_type ABSTRACT = (1 << 9);       // Abstract fields are only usable via suffix matching or inheritance
  static constexpr flag_type STORED = (1 << 10);        // Set if the field's raw values are kept in the segment's stored-fields resource for per-doc retrieval
  // RANGE query contract. Numeric fields use a 1-D points index. GEO_POINT
  // flushes a 2-D BKD index; its query and merge consumers land in pass C.
  static constexpr flag_type INDEX_RANGE = (1 << 11);
  // Segment-format marker. It is never supplied by schema configuration.
  static constexpr flag_type TERM_RANGES = (1 << 12);
  static constexpr flag_type DERIVED = (1 << 13);  // named representation of a logical field
  // Segment-format marker, never supplied by schema configuration: some term
  // in this segment field is at least PackedTerm::MIN_FITTED_LEN bytes, so it
  // may be a truncated or hashed (fitted) form of a longer value.
  static constexpr flag_type MAY_HAVE_FITTED = (1 << 14);
  // Every segment-format marker. A writer derives them from what it writes,
  // so merges never carry them over from source segments.
  static constexpr flag_type SEGMENT_MARKERS = TERM_RANGES | MAY_HAVE_FITTED;

  const FieldType::Type type_;
  const std::string name_;
  flag_type flags_;
  // Determines indexed and query term bytes.
  TermPolicy longTerms = TermPolicy::HASH128;
  // When STORED is set, raw values are routed to the stored-fields resource
  // with this name (default: Postings::STORED_DEFAULT_RESOURCE).  Shared by
  // TEXT, STRING, and ID.  Ignored by field types that don't support STORED.
  std::string storedResource_ = std::string(Postings::STORED_DEFAULT_RESOURCE);

  // constructor
  FieldType(std::string_view name, FieldType::Type type, int flags) :
     type_(type), name_(name), flags_(flags) {
  }

  bool isSet(flag_type flag) {
    return (flags_ & flag);
  }
  bool notSet(flag_type flag) {
    return !(flags_ & flag);
  }

  virtual ~FieldType() = default;

  // Return the basic type of the field.
  FieldType::Type type() { return type_; }


  // The name of the field type (which is not necessarily the name of the field)
  std::string_view name() { return name_; }

  bool isAbstract() { return (bool) (flags_ & ABSTRACT); }
  bool isDerived() const { return (bool) (flags_ & DERIVED); }

  // Segment flags describe physical structures; schema roles stay in the schema.
  flag_type segmentFlags() const { return flags_ & ~(ABSTRACT | DERIVED); }

  // TODO: check standard on cast of int to bool (check generated code too)
  bool indexed() { return (bool) (flags_ & INDEX_DOCS); }

  bool rangeIndexed() const { return (bool) (flags_ & INDEX_RANGE); }

  // The index level is cumulative: DOCS < DOCS_FREQS < DOCS_FREQS_POSITIONS.
  // A level is present only when all of that level's bits are set; a plain
  // bit-and would report a DOCS-only field as having freqs/positions.
  static bool hasFreqs(flag_type flags) { return (flags & INDEX_DOCS_FREQS) == INDEX_DOCS_FREQS; }
  static bool hasPositions(flag_type flags) { return (flags & INDEX_DOCS_FREQS_POSITIONS) == INDEX_DOCS_FREQS_POSITIONS; }

  bool hasFreqs() { return hasFreqs(flags_); }

  bool hasPositions() { return hasPositions(flags_); }

  bool hasNumTokens() { return (bool) (flags_ & (NUM_TOKENS_APPROX | NUM_TOKENS_EXACT)); }

  bool multiValued() { return (bool) (flags_ & MULTI_VALUED); }

  // Whether a sort on a field of this type orders by its int64 column
  // values: raw integers for INT, epoch millis for DATE, sortable bits for
  // FLOAT and DOUBLE (SimpleNumericFieldComparator). Single-valued columns
  // of these types are the ones field-sort planning reads, so only they
  // persist a value sketch (NumColumnWriter).
  static bool sortsByIntColumn(Type type) {
    return type == INT || type == DATE || type == FLOAT || type == DOUBLE;
  }

  bool isAnalyzerFieldSpecific() { return (bool) (flags_ & FIELD_SPECIFIC_ANALYZER); }

  bool hasColumn() { return (bool) (flags_ & COLUMN_STORED); }

  bool isStored() { return (bool) (flags_ & STORED); }

  // ---- Val -> field-native coercion ----
  // One contract shared by ingest and query build so both sides coerce
  // identically (a doc ingested via one rule must be findable by querying the
  // same literal).  Concrete field types override the primitive matching
  // their native storage; the defaults throw.  The shared conversion core and
  // the rules live in schema/ValCoerce.h; definitions are in ValCoerce.cpp so
  // this widely-included header stays independent of the api types.

  // The encoded int64 the standard int column stores for this field type:
  // the raw integer for INT, sortable bits for FLOAT/DOUBLE (NumericUtils),
  // epoch millis for DATE (ISO-8601 strings parsed on the way in).  Throws on
  // impossible values (per-doc failure at ingest, request error at query
  // build).
  // context fixes NOW for the surrounding request/update. Other numeric types
  // ignore it; a missing value lets DATE capture the clock at this call.
  virtual int64_t coerceColInt64(
      const api::Val& val, std::string_view fieldName,
      const CoerceContext& context = {}) const;

  // Term/text bytes for term-backed fields (TEXT/STRING/ID).  Numeric and
  // bool arms render canonically into buf (>= coerce::TEXT_BUF_SIZE bytes)
  // and return a view of it; string/bytes arms pass through as views of the
  // original value.  Callers must consume the result before buf is reused.
  virtual std::string_view coerceTerm(const api::Val& val, std::string_view fieldName,
                                      std::span<char> buf) const;
};


class TextFieldType : public FieldType {
public:
  // The compiled chain (analysis/Analyzer.h), shared with every field that
  // resolves to the same authored definition.
  std::shared_ptr<const Analyzer> analyzer_;

  TextFieldType(std::string_view name, int flags, std::shared_ptr<const Analyzer> analyzer)
    : FieldType(name, FieldType::TEXT, flags), analyzer_(std::move(analyzer)) {}

  // Parameterless components by name (tests, benches, hand-built schemas);
  // throws like Analyzer::compile.  Defined in FieldType.cpp so this header
  // stays independent of the api types.
  TextFieldType(std::string_view name, int flags = INDEX_DOCS_FREQS_POSITIONS,
                std::string_view tokenizer = "whitespace", std::vector<std::string> filters = {});

  // Create a new non-thread-safe analyzer chain for this text field type
  std::unique_ptr<TokenChain> createAnalyzer(std::string_view fieldName) {
    unused(fieldName);
    return analyzer_->createChain();
  }

  std::string_view coerceTerm(const api::Val& val, std::string_view fieldName,
                              std::span<char> buf) const override;
};

class StrFieldType : public FieldType {
public:
  std::shared_ptr<const Analyzer> normalizer;

  StrFieldType(std::string_view name, int flags=INDEX_DOCS | COLUMN_STORED,
               std::shared_ptr<const Analyzer> normalizer = {})
    : FieldType(name, FieldType::STRING, flags), normalizer(std::move(normalizer)) {}

  // Hot paths (ingest and query literals) must retain a chain from normalizer->createChain().
  void normalize(std::string& value) const;

  std::string_view coerceTerm(const api::Val& val, std::string_view fieldName,
                              std::span<char> buf) const override;
};

class IntFieldType : public FieldType {
public:
  IntFieldType(std::string_view name, int flags=COLUMN_STORED) : FieldType(name, FieldType::INT, flags) {
  }

  int64_t coerceColInt64(
      const api::Val& val, std::string_view fieldName,
      const CoerceContext& context = {}) const override;
};

// FLOAT and DOUBLE fields store Lucene-style sortable bits in the standard
// int column (see util/NumericUtils.h): the encoded int64 orders the same as
// the floating point value, so sorting and min/max work on the column
// directly; consumers that need the numeric value decode at the edges.
class FloatFieldType : public FieldType {
public:
  FloatFieldType(std::string_view name, int flags=COLUMN_STORED) : FieldType(name, FieldType::FLOAT, flags) {
  }

  int64_t coerceColInt64(
      const api::Val& val, std::string_view fieldName,
      const CoerceContext& context = {}) const override;
};

class DoubleFieldType : public FieldType {
public:
  DoubleFieldType(std::string_view name, int flags=COLUMN_STORED) : FieldType(name, FieldType::DOUBLE, flags) {
  }

  int64_t coerceColInt64(
      const api::Val& val, std::string_view fieldName,
      const CoerceContext& context = {}) const override;
};

// DATE stores int64 milliseconds since the Unix epoch directly in the standard
// int column.  Signed millis already sorts in chronological order, so sort,
// range-facet, and min/max stats run on the raw column with no per-value
// transform (unlike FLOAT/DOUBLE, which store sortable bits).  The DATE tag
// drives string<->millis parsing on input and date-aware rendering on output.
class DateFieldType : public FieldType {
public:
  DateFieldType(std::string_view name, int flags=COLUMN_STORED) : FieldType(name, FieldType::DATE, flags) {
  }

  int64_t coerceColInt64(
      const api::Val& val, std::string_view fieldName,
      const CoerceContext& context = {}) const override;

  // The [lo, hiExclusive) epoch-millis window a query-side date literal
  // denotes at its own granularity: "2024-06-25" is the whole day, "2024-06"
  // the month, epoch millis a single instant.  Queries match/round by the
  // window; ingest and sorting use coerceColInt64 (the window start).
  DateRange coerceDateRange(
      const api::Val& val, std::string_view fieldName,
      const CoerceContext& context = {}) const;
};

class GeoPointFieldType : public FieldType {
public:
  GeoPointFieldType(std::string_view name, int flags=COLUMN_STORED)
    : FieldType(name, FieldType::GEO_POINT, flags) {
  }
};

// Unique id field
class IdFieldType : public FieldType {
public:
  IdFieldType(std::string_view name, int flags=INDEX_DOCS | COLUMN_STORED) : FieldType(name, FieldType::ID, flags) {
  }

  std::string_view coerceTerm(const api::Val& val, std::string_view fieldName,
                              std::span<char> buf) const override;
};

// Resolved ANN overlay settings of a vector field (api::VectorAnnDef with
// defaults applied: PQ unless explicitly selected otherwise). Build-time
// parameters are recorded in each overlay's VectorAuxMeta, so a change only
// affects overlays built afterwards;
// rabitqQueryBits is a search-time parameter read from the current schema.
struct VectorAnnConfig {
  enum Codec {
    RABITQ = 0,
    PQ = 1,
  };
  static constexpr int32_t DEFAULT_RABITQ_BITS = 1;
  static constexpr int32_t DEFAULT_RABITQ_QUERY_BITS = 4;
  static constexpr int32_t DEFAULT_PQ_BITS = 8;

  Codec codec = PQ;
  int32_t nlist = 0;  // 0 = sqrt(segment vectors), capped
  int32_t rabitqBits = DEFAULT_RABITQ_BITS;
  int32_t rabitqQueryBits = DEFAULT_RABITQ_QUERY_BITS;
  int32_t pqM = 0;    // 0 = derived from dims
  int32_t pqBits = DEFAULT_PQ_BITS;
};

// Dense float vector field.  Values are stored as fixed-size byte blobs
// through the standard binary-column path (see VectorHandler).  dims_ may be
// 0, in which case the first indexed value in a segment fixes the segment's
// per-vector size; a positive dims_ enforces validation at index time.
//
// metric_ controls whether the field is searchable by kNN / ANN.  NONE means
// storage-only; L2 / IP / COSINE pick the similarity metric for exact column
// scan and future aux-backed ANN engines.
class VectorFieldType : public FieldType {
public:
  enum Metric {
    METRIC_NONE = 0,
    METRIC_L2 = 1,
    METRIC_IP = 2,
    METRIC_COSINE = 3,
  };

  int32_t dims_;
  Metric metric_;
  // Caller asserts incoming vectors are unit-norm.  COSINE write and aux-build
  // paths trust the bytes as-is.  Ignored for non-COSINE metrics.
  bool normalized_;
  // COSINE fields normalize vectors before column storage by default.  Ignored
  // for non-COSINE metrics.
  bool normalizeOnWrite_;
  // ANN overlay codec and parameters (schema "ann").
  VectorAnnConfig ann_;

  VectorFieldType(std::string_view name, int32_t dims = 0,
                  int flags = COLUMN_STORED | FIXED_SIZE,
                  Metric metric = METRIC_NONE,
                  bool normalized = false,
                  bool normalizeOnWrite = true,
                  VectorAnnConfig ann = {})
    : FieldType(name, FieldType::VECTOR, flags),
      dims_(dims),
      metric_(metric),
      normalized_(normalized),
      normalizeOnWrite_((metric == METRIC_COSINE) && normalizeOnWrite && !normalized),
      ann_(ann) {
  }

  int32_t dims() const { return dims_; }
  Metric metric() const { return metric_; }
  bool normalized() const { return normalized_; }
  bool normalizeOnWrite() const { return normalizeOnWrite_; }
  const VectorAnnConfig& ann() const { return ann_; }

  // True iff this field supports kNN / ANN search.  Exact flat search can run
  // directly over the column; real ANN engines may also build aux artifacts.
  bool knnSearchable() const { return metric_ != METRIC_NONE; }
};

// How a stored-fields resource compresses its chunks.  codec and the two chunk
// limits are recorded in each segment's resource metadata; the level only
// affects compression.
struct StoredChunkFormat {
  // Recorded on disk as one byte.
  enum Codec : uint8_t { LZ4 = 0, ZSTD = 1 };
  static constexpr int DEFAULT_ZSTD_LEVEL = 3;
  static constexpr uint32_t DEFAULT_CHUNK_TARGET = 16 * 1024;
  static constexpr uint32_t DEFAULT_MAX_DOCS_PER_CHUNK = 128;
  // ZSTD is chosen for size, so its defaults trade some per-document read
  // cost for larger chunks.
  static constexpr uint32_t DEFAULT_ZSTD_CHUNK_TARGET = 64 * 1024;
  static constexpr uint32_t DEFAULT_ZSTD_MAX_DOCS_PER_CHUNK = 1024;

  Codec codec = LZ4;
  int zstdLevel = DEFAULT_ZSTD_LEVEL;
  // Target uncompressed bytes per chunk before flushing.
  uint32_t chunkTarget = DEFAULT_CHUNK_TARGET;
  // Hard cap on docs per chunk.  A chunk flushes when *either* the byte
  // target or this doc count is reached, whichever comes first.
  uint32_t maxDocsPerChunk = DEFAULT_MAX_DOCS_PER_CHUNK;

  // Same chunk bytes and boundaries: compressed chunks of one format can be
  // copied verbatim into a writer of the other.  The level is not part of it.
  bool sameLayout(const StoredChunkFormat& o) const {
    return codec == o.codec && chunkTarget == o.chunkTarget && maxDocsPerChunk == o.maxDocsPerChunk;
  }
  bool operator==(const StoredChunkFormat&) const = default;

  // The defaults for a codec.
  static StoredChunkFormat defaults(Codec codec) {
    if (codec == LZ4) return {};
    return {ZSTD, DEFAULT_ZSTD_LEVEL, DEFAULT_ZSTD_CHUNK_TARGET, DEFAULT_ZSTD_MAX_DOCS_PER_CHUNK};
  }
};

// Describes a stored-fields resource (a per-segment column of compressed
// whole-doc chunks).  Registered in the schema under the resource's own name
// (e.g. "_stored_" for the default, "_stored_paragraphs_" for a named family).
// FieldType::storedResource_ names which StoredFieldType a STORED field
// flushes into.  Built from SchemaDef.stored_resources.
class StoredFieldType : public FieldType {
public:
  StoredChunkFormat format_;

  explicit StoredFieldType(std::string_view name, StoredChunkFormat format = {})
    : FieldType(name, FieldType::BIN, FieldType::STORED),
      format_(format) {}
};

} // end namespace
