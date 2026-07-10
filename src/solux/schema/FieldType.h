#pragma once

#include "solux/analysis/Analyzer.h"
#include "solux/reader/Postings.h"

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace solux {

namespace api { struct Val; }  // the wire value oneof (api/solux_types.hpp)



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
    DATE      // timestamp; column-stored as int64 milliseconds since the Unix epoch
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
  static constexpr flag_type INDEX_RANGE = (1 << 11);   // Set if a numeric field has a points index

  const FieldType::Type type_;
  const std::string name_;
  flag_type flags_;
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
  virtual int64_t coerceColInt64(const api::Val& val, std::string_view fieldName) const;

  // Term/text bytes for term-backed fields (TEXT/STRING/ID).  Numeric and
  // bool arms render canonically into buf (>= coerce::TEXT_BUF_SIZE bytes)
  // and return a view of it; string/bytes arms pass through as views of the
  // original value.  Callers must consume the result before buf is reused.
  virtual std::string_view coerceTerm(const api::Val& val, std::string_view fieldName,
                                      std::span<char> buf) const;

  // TODO: how to share analyzers (potentially expensive) among FieldTypes?
  // One way: Have a parent FieldType in the constructor.
};


class TextFieldType : public FieldType {
public:
  std::string tokenizer_;                // e.g., "whitespace", "keyword"
  std::vector<std::string> filters_;     // e.g., {"lowercase"}

  TextFieldType(std::string_view name, int flags=INDEX_DOCS_FREQS_POSITIONS,
                std::string_view tokenizer = "whitespace", std::vector<std::string> filters = {})
    : FieldType(name, FieldType::TEXT, flags), tokenizer_(tokenizer), filters_(std::move(filters)) {}

  // Create a new non-thread-safe analyzer for this text field type
  std::unique_ptr<TokenChain> createAnalyzer(std::string_view fieldName) {
    unused(fieldName);

    // Create tokenizer by name. The old "whitespace"/"nocopy_whitespace" split
    // is gone: under the read-only borrow contract the tokenizer always views
    // the source, so there is a single WhitespaceTokenizer. ("nocopy_whitespace"
    // is still accepted as an alias until schemas are migrated.)
    std::unique_ptr<Tokenizer> tok;
    bool stateful = false;
    size_t firstFilter = 0;  // index of the first filter still to apply (some get fused into the tokenizer)
    if (tokenizer_ == "keyword") {
      tok = std::make_unique<KeywordTokenizer>();
    } else if (tokenizer_ == "unicode_word") {
      // UAX#29 word segmentation; carries a cursor, so the chain is stateful.
      stateful = true;
      // Optimization: unicode_word + nfkc_cf (the common default) fuses into one
      // StandardTokenizer - segmentation and NFKC_CF in a single stage. Any
      // remaining filters (e.g. fold) still apply on top.
      if (!filters_.empty() && filters_[0] == "nfkc_cf") {
        tok = makeStandardTokenizer();
        firstFilter = 1;
      } else {
        tok = makeUnicodeWordTokenizer();
      }
    } else {
      // default: "whitespace" (and the "nocopy_whitespace" alias)
      tok = std::make_unique<WhitespaceTokenizer>();
    }

    auto& headRef = *tok;
    std::unique_ptr<TokenStream> tail = std::move(tok);

    // Apply remaining filters in order
    for (size_t i = firstFilter; i < filters_.size(); i++) {
      const auto& filter = filters_[i];
      if (filter == "lowercase") {
        tail = std::make_unique<LowercaseFilter>(std::move(tail));
      } else if (filter == "nfkc_cf") {
        tail = makeNfkcCasefoldFilter(std::move(tail));
      } else if (filter == "fold") {
        tail = makeAccentFoldFilter(std::move(tail));
      }
      // easy to add more filters here
    }

    return std::make_unique<TokenChain>(headRef, std::move(tail), stateful);
  }

  std::string_view coerceTerm(const api::Val& val, std::string_view fieldName,
                              std::span<char> buf) const override;
};

class StrFieldType : public FieldType {
public:
  StrFieldType(std::string_view name, int flags=INDEX_DOCS | COLUMN_STORED) : FieldType(name, FieldType::STRING, flags) {
  }

  std::string_view coerceTerm(const api::Val& val, std::string_view fieldName,
                              std::span<char> buf) const override;
};

class IntFieldType : public FieldType {
public:
  IntFieldType(std::string_view name, int flags=COLUMN_STORED) : FieldType(name, FieldType::INT, flags) {
  }

  int64_t coerceColInt64(const api::Val& val, std::string_view fieldName) const override;
};

// FLOAT and DOUBLE fields store Lucene-style sortable bits in the standard
// int column (see util/NumericUtils.h): the encoded int64 orders the same as
// the floating point value, so sorting and min/max work on the column
// directly; consumers that need the numeric value decode at the edges.
class FloatFieldType : public FieldType {
public:
  FloatFieldType(std::string_view name, int flags=COLUMN_STORED) : FieldType(name, FieldType::FLOAT, flags) {
  }

  int64_t coerceColInt64(const api::Val& val, std::string_view fieldName) const override;
};

class DoubleFieldType : public FieldType {
public:
  DoubleFieldType(std::string_view name, int flags=COLUMN_STORED) : FieldType(name, FieldType::DOUBLE, flags) {
  }

  int64_t coerceColInt64(const api::Val& val, std::string_view fieldName) const override;
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

  int64_t coerceColInt64(const api::Val& val, std::string_view fieldName) const override;

  // The [lo, hiExclusive) epoch-millis window a query-side date literal
  // denotes at its own granularity: "2024-06-25" is the whole day, "2024-06"
  // the month, epoch millis a single instant.  Queries match/round by the
  // window; ingest and sorting use coerceColInt64 (the window start).
  std::pair<int64_t, int64_t> coerceDateRange(const api::Val& val,
                                              std::string_view fieldName) const;
};

// Unique id field
class IdFieldType : public FieldType {
public:
  IdFieldType(std::string_view name, int flags=INDEX_DOCS | COLUMN_STORED) : FieldType(name, FieldType::ID, flags) {
  }

  std::string_view coerceTerm(const api::Val& val, std::string_view fieldName,
                              std::span<char> buf) const override;
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

  VectorFieldType(std::string_view name, int32_t dims = 0,
                  int flags = COLUMN_STORED | FIXED_SIZE,
                  Metric metric = METRIC_NONE,
                  bool normalized = false,
                  bool normalizeOnWrite = true)
    : FieldType(name, FieldType::VECTOR, flags),
      dims_(dims),
      metric_(metric),
      normalized_(normalized),
      normalizeOnWrite_((metric == METRIC_COSINE) && normalizeOnWrite && !normalized) {
  }

  int32_t dims() const { return dims_; }
  Metric metric() const { return metric_; }
  bool normalized() const { return normalized_; }
  bool normalizeOnWrite() const { return normalizeOnWrite_; }

  // True iff this field supports kNN / ANN search.  Exact flat search can run
  // directly over the column; real ANN engines may also build aux artifacts.
  bool knnSearchable() const { return metric_ != METRIC_NONE; }
};

// Describes a stored-fields resource (a per-segment column of LZ4-compressed
// whole-doc chunks).  Registered in the schema under the resource's own name
// (e.g. "_stored_" for the default, "_stored_paragraphs_" for a named family).
// FieldType::storedResource_ names which StoredFieldType a STORED field
// flushes into.
class StoredFieldType : public FieldType {
public:
  // Codec name.  Only "lz4" is supported in v1; reserved slot for future
  // codecs (e.g. zstd with a trained dictionary).
  std::string codec_;
  // Target uncompressed bytes per chunk before flushing.
  size_t chunkTargetUncompressed_;
  // Hard cap on docs per chunk.  A chunk flushes when *either* the byte
  // target or this doc count is reached, whichever comes first.
  size_t maxDocsPerChunk_;

  explicit StoredFieldType(std::string_view name,
                           std::string_view codec = "lz4",
                           size_t chunkTargetUncompressed = 16 * 1024,
                           size_t maxDocsPerChunk = 128)
    : FieldType(name, FieldType::BIN, FieldType::STORED),
      codec_(codec),
      chunkTargetUncompressed_(chunkTargetUncompressed),
      maxDocsPerChunk_(maxDocsPerChunk) {}
};

// Special FieldType for score sorting
class ScoreFieldType : public FieldType {
public:
  ScoreFieldType() : FieldType("_score_", FieldType::FLOAT, 0) {}
};

// Special FieldType for document ID sorting
class DocFieldType : public FieldType {
public:
  DocFieldType() : FieldType("_docid_", FieldType::INT, 0) {}
};


} // end namespace
