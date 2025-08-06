#pragma once

#include "solux/analysis/Analyzer.h"

#include <memory>

namespace solux {



// FieldType contains meta-data about a field, including all the information necessary
// to index that field.  It is independent of any given index segment, as it must exist
// before indexing any data.
class FieldType {
public:
  enum Type {
    NONE=0,
    STRING,   // unanalyzed string field
    TEXT,     // analyzed text field (if indexed)
    BIN,      // binary field
    INT,
    FLOAT,
    DOUBLE
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

  const FieldType::Type type_;
  const std::string name_;
  flag_type flags_;

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

  // TODO: check standard on cast of int to bool (check generated code too)
  bool indexed() { return (bool) (flags_ & INDEX_DOCS_FREQS_POSITIONS); }

  bool hasFreqs() { return (bool) (flags_ & INDEX_DOCS_FREQS); }

  bool hasPositions() { return (bool) (flags_ & INDEX_DOCS_FREQS_POSITIONS); }

  bool hasNumTokens() { return (bool) (flags_ & (NUM_TOKENS_APPROX | NUM_TOKENS_EXACT)); }

  bool multiValued() { return (bool) (flags_ & MULTI_VALUED); }

  bool isAnalyzerFieldSpecific() { return (bool) (flags_ & FIELD_SPECIFIC_ANALYZER); }

  bool hasColumn() { return (bool) (flags_ & COLUMN_STORED); }

  // TODO: how to share analyzers (potentially expensive) among FieldTypes?
  // One way: Have a parent FieldType in the constructor.
};


class TextFieldType : public FieldType {
  // TODO: optional list of token filters, etc...
public:
  TextFieldType(std::string_view name, int flags=INDEX_DOCS_FREQS_POSITIONS) : FieldType(name, FieldType::TEXT, flags) {}

  // Right now, our analyzer only consists of a TokenChain.  We could either fold other analyzer methods into TextFieldType, or
  // fill out an Analyzer class (only needed if it needs state of its own?)

  // Create a new non-thread-safe analyzer for this text field type
  std::unique_ptr<TokenChain> createAnalyzer(std::string_view fieldName) {
    unused(fieldName);
    std::unique_ptr<TokenChain> tc;

    // hack to just drive off of the name for now
    if (name_ == "_w") {
      auto wsTok = std::make_unique<NoCopyWhitespaceTokenizer>();
      auto &headRef = *wsTok;
      tc = make_unique<TokenChain>(headRef, std::move(wsTok));  // ws only
    } else if (name_ == "_wl") {
      // auto wsTok = std::make_unique<WhitespaceTokenizer>();
      auto wsTok = std::make_unique<WhitespaceTokenizer>();
      auto &headRef = *wsTok;
      auto lowerFilt = std::make_unique<LowercaseFilter>(std::move(wsTok));
      tc = make_unique<TokenChain>(headRef, std::move(lowerFilt));
    }
    return tc;
  }

};

class StrFieldType : public FieldType {
public:
  StrFieldType(std::string_view name, int flags=INDEX_DOCS | COLUMN_STORED) : FieldType(name, FieldType::STRING, flags) {
  }
};

class IntFieldType : public FieldType {
public:
  IntFieldType(std::string_view name, int flags=COLUMN_STORED) : FieldType(name, FieldType::INT, flags) {
  }
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