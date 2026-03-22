#pragma once

#include "solux/analysis/Analyzer.h"

#include <memory>
#include <vector>

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
    DOUBLE,
    ID        // unique id field
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

  bool isAbstract() { return (bool) (flags_ & ABSTRACT); }

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
public:
  std::string tokenizer_;                // e.g., "whitespace", "nocopy_whitespace", "keyword"
  std::vector<std::string> filters_;     // e.g., {"lowercase"}

  TextFieldType(std::string_view name, int flags=INDEX_DOCS_FREQS_POSITIONS,
                std::string_view tokenizer = "whitespace", std::vector<std::string> filters = {})
    : FieldType(name, FieldType::TEXT, flags), tokenizer_(tokenizer), filters_(std::move(filters)) {}

  // Create a new non-thread-safe analyzer for this text field type
  std::unique_ptr<TokenChain> createAnalyzer(std::string_view fieldName) {
    unused(fieldName);

    // Create tokenizer by name
    std::unique_ptr<Tokenizer> tok;
    if (tokenizer_ == "nocopy_whitespace") {
      tok = std::make_unique<NoCopyWhitespaceTokenizer>();
    } else if (tokenizer_ == "keyword") {
      tok = std::make_unique<KeywordTokenizer>();
    } else {
      // default: "whitespace"
      tok = std::make_unique<WhitespaceTokenizer>();
    }

    auto& headRef = *tok;
    std::unique_ptr<TokenStream> tail = std::move(tok);

    // Apply filters in order
    for (const auto& filter : filters_) {
      if (filter == "lowercase") {
        tail = std::make_unique<LowercaseFilter>(std::move(tail));
      }
      // easy to add more filters here
    }

    return std::make_unique<TokenChain>(headRef, std::move(tail));
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

// Unique id field
class IdFieldType : public FieldType {
public:
  IdFieldType(std::string_view name, int flags=INDEX_DOCS | COLUMN_STORED) : FieldType(name, FieldType::ID, flags) {
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