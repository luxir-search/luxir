#pragma once

#include "solux/util/MemPool.h"
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

  static constexpr int INDEX_DOCS = (1 << 0);
  static constexpr int INDEX_DOCS_FREQS = INDEX_DOCS | (1 << 1);
  static constexpr int INDEX_DOCS_FREQS_POSITIONS = INDEX_DOCS_FREQS | (1 << 2);
  static constexpr int NUM_TOKENS_APPROX = (1 << 3);
  static constexpr int NUM_TOKENS_EXACT = (1 << 4);
  static constexpr int MULTI_VALUED = (1 << 5);  // Set if the field can have multiple values per document
  static constexpr int FIELD_SPECIFIC_ANALYZER = (1 << 6);  // Set if different fields using the same type have different analyzers (i.e. don't cache across different fields)

  const std::string name_;
  const FieldType::Type type_;
  int flags_;

  // constructor
  FieldType(std::string_view name, FieldType::Type type, bool multiValued, int flags) :
    name_(name), type_(type), flags_(flags) {
    if (multiValued) {
      flags_ |= MULTI_VALUED;
    }
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


  // TODO: how to share analyzers (potentially expensive) among FieldTypes?
  // One way: Have a parent FieldType in the constructor.
};


class TextFieldType : public FieldType {
  // TODO: optional list of token filters, etc...
public:
  TextFieldType(std::string_view name, bool multiValued = false, int flags=INDEX_DOCS_FREQS_POSITIONS) : FieldType(name, FieldType::TEXT, multiValued, flags) {}

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
  StrFieldType(std::string_view name, bool multiValued=false, int flags=INDEX_DOCS) : FieldType(name, FieldType::STRING, multiValued, flags) {
  }
};

class IntFieldType : public FieldType {
public:
  IntFieldType(std::string_view name, bool multiValued=false, int flags=0) : FieldType(name, FieldType::INT, multiValued, flags) {
  }
};


} // end namespace