// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <memory_resource>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "luxir/api/luxir_types.hpp"
#include "luxir/schema/FieldType.h"
#include "luxir/util/ParamReader.h"
#include "luxir/util/geo.h"

namespace luxir {

// One built-in sub-field a plugin field type derives from each authored value.
// The physical field is <field>$<suffix>: hidden from catalogs and not a
// document key, but queried, sorted, faceted and retrieved by that name, and
// indexed, merged, deleted and replicated like any other field.
struct DerivedField {
  enum class Kind { INT, DATE, STRING, TEXT, GEO_POINT };
  // The longest suffix, so <field>$<suffix> fits the 127-byte name limit with
  // a root of at least 127 - 1 - MAX_SUFFIX_LEN bytes.
  static constexpr size_t MAX_SUFFIX_LEN = 32;

  std::string suffix = {};  // [a-z0-9_]+, at most MAX_SUFFIX_LEN bytes
  Kind kind = Kind::STRING;
  // More than one value per document. INT, DATE and STRING keep a sorted set
  // (repeats dropped); TEXT and GEO_POINT keep every value in order. Each
  // multi field is its own set: nothing correlates the i-th value of one
  // derived field with the i-th value of another, and neither the order nor
  // the repeats index() emitted survive.
  bool multi = false;
  // STRING and TEXT: the term index (term, prefix and term range queries).
  // INT, DATE and GEO_POINT: the points index (range and geo queries), which
  // needs the column; without it those queries scan the column.
  bool index = true;
  // A per-document column: sorts, facets, value expressions and project().
  // TEXT has none. A STRING column without an index holds arbitrary bytes.
  bool column = true;
  // TEXT only: the registered analyzer text() values go through ("english", a
  // plugin's analyzer); empty is the whitespace tokenizer. token() bypasses it.
  std::string analyzer = {};
  DateUnit unit = DateUnit::MILLIS;  // DATE only: the unit of integer() values
  // The values are the type's private encoding (packed bytes for project()):
  // retrieving <field>$<suffix> by name is an error. Queries, sorts and facets
  // still read it.
  bool opaque = false;
};

class DerivedSink;

// A document's column values of the derived fields, for project(). values[i]
// belongs to fields[i] (PluginFieldType::fields): an int64 for INT and DATE
// (epoch units), a string for STRING, an ArrInt or ArrStr when multi (sorted
// sets, see DerivedField::multi). TEXT and GEO_POINT fields and fields without
// a column are always absent. An indexed STRING column holds the indexed term,
// which for a value over 255 bytes is fitted (a prefix plus a hash), so it is
// not the emitted value; exact(i) tells. A column-only STRING holds its bytes
// exactly; keep what project() decodes there.
class DerivedValues {
public:
  std::span<const api::Val> values;
  std::span<const DerivedField> fields;

  // Null when the document has no value for field i.
  const api::Val* get(size_t i) const { return values[i].isNull() ? nullptr : &values[i]; }
  // False when field i's value (any of them when multi) may be a fitted term
  // rather than the emitted bytes.
  bool exact(size_t i) const;
};

// A field type a plugin registers: maps one authored value to values of the
// derived fields. Immutable and shared by every field and thread using it.
class PluginFieldType {
public:
  // The derived fields, fixed when the maker returns. Sink and DerivedValues
  // calls address them by index.
  std::vector<DerivedField> fields;

  virtual ~PluginFieldType() = default;

  // Indexes one document's value, whatever was sent (object, array, string,
  // number; never null, which is no value), by emitting values into the sink.
  // Throw std::invalid_argument (or DocumentError) for a value the type cannot
  // read: the document is rejected naming the field, or, with ignore_malformed,
  // the field is skipped and the rest of the document indexed. Nothing reaches
  // the index unless index() returns normally.
  virtual void index(const api::Val& value, DerivedSink& sink) const = 0;

  // Whether retrieving the authored field calls project().
  virtual bool projects() const { return false; }

  // The suffix of the derived field that stands in for the authored field
  // when a schema's index_sort names it, for example a shape's centroid
  // GEO_POINT for {"field": "shape", "order": "hilbert"}. Empty: the field
  // cannot be an index sort.
  virtual std::string_view sortProxy() const { return {}; }

  // The derived data's encoding version, part of every derived field's
  // signature. Raise it when index() starts writing different values for the
  // same params, so segments written before are a different representation.
  virtual uint32_t version() const { return 0; }

  // The effective params as a JSON object, defaults filled in and values
  // normalized, so equivalent definitions ({} and {"separator": ":"} when ":"
  // is the default) share a signature. Empty: the authored params.
  virtual std::string canonicalParams() const { return {}; }

  // The authored field's returned value, rebuilt from the document's derived
  // column values; leave out unset for none. mr backs any strings, arrays or
  // maps out points at. Called only when some derived value is present.
  virtual void project(const DerivedValues& values, api::Val& out, std::pmr::memory_resource& mr) const {
    (void)values, (void)out, (void)mr;
  }
};

// A plugin field type as the schema compiled it: the type plus the canonical
// definition (name, params, version) in its derived fields' signatures. A
// changed definition reports a new representation of every derived field in
// the resolved schema; segments written before keep their values, which are
// read under the new definition.
class RegisteredFieldType {
public:
  const std::string name;       // registry name, e.g. "geo_shape"
  // {"name", "params" (canonicalParams() or the authored ones, sorted) and
  // "version" when not 0}, as JSON
  const std::string canonical;
  const std::unique_ptr<const PluginFieldType> type;

  RegisteredFieldType(std::string name, std::string canonical, std::unique_ptr<const PluginFieldType> type)
      : name(std::move(name)), canonical(std::move(canonical)), type(std::move(type)) {}
};

// Collects the values PluginFieldType::index emits for one document, per
// derived field. The engine indexes them once index() returns. Not
// thread-safe; one per field per inverter.
class DerivedSink {
public:
  struct Values {
    std::string bytes;              // STRING and TEXT values, back to back
    std::vector<uint32_t> ends;     // each value's end offset in bytes
    std::vector<int32_t> positions; // TEXT tokens: each token's position
    std::vector<int64_t> ints;
    std::vector<GeoPoint> points;

    size_t count() const { return ends.size() + ints.size() + points.size(); }
  };

  const RegisteredFieldType& owner;
  std::vector<Values> values;  // one per PluginFieldType::fields entry

  explicit DerivedSink(const RegisteredFieldType& owner);

  // STRING: one value, a term when indexed and a column value when columned
  // (arbitrary bytes when column-only).
  void string(size_t field, std::string_view value);
  // TEXT: one value for the field's analyzer. Values of a multi field are
  // separated by the usual position gap.
  void text(size_t field, std::string_view value);
  // TEXT: one analyzed token at a position (>= 0, non-decreasing within the
  // document), bypassing the analyzer. A document uses text() or token() for
  // a field, not both.
  void token(size_t field, std::string_view term, int32_t position);
  // INT, or DATE in the field's unit.
  void integer(size_t field, int64_t value);
  // GEO_POINT: degrees; out of range throws std::invalid_argument.
  void point(size_t field, double latitude, double longitude);

  void clear();
  size_t retainedBytes() const;

private:
  // The field's values, checking the call matches its kind and, when `one`
  // adds a value, that a single-valued field has none yet.
  Values& add(size_t field, DerivedField::Kind kind, std::string_view call, bool one = true);
};

// The schema FieldType of a field whose type a plugin registered: the authored
// field. It has no physical structures; its values live in the derived fields,
// which the schema registers as <field>$<suffix>.
class CompositeFieldType : public FieldType {
public:
  std::shared_ptr<const RegisteredFieldType> plugin;
  // Skip a value the plugin rejects and index the rest of the document.
  bool ignoreMalformed;

  CompositeFieldType(std::string_view name, std::shared_ptr<const RegisteredFieldType> plugin, bool ignoreMalformed)
      : FieldType(name, FieldType::NONE, 0), plugin(std::move(plugin)), ignoreMalformed(ignoreMalformed) {}

  const PluginFieldType& impl() const { return *plugin->type; }

  // The separator between a field and its derived suffix. Field names cannot
  // contain it, so a derived name is never a document key.
  static constexpr char SEPARATOR = '$';
  static std::string derivedName(std::string_view field, std::string_view suffix) {
    std::string name(field);
    name += SEPARATOR;
    name += suffix;
    return name;
  }
};

// The field type registry: name -> maker. The process-wide instance is
// FieldTypeRegistry::global(), read through globalFieldTypeRegistry() by
// code that does not link the plugin headers. Filled during initPlugins(), then
// frozen; lookups after the freeze read an immutable table without locking.
//
// A maker reads the authored params (ParamReader: typed reads whose errors
// name the type and the parameter; a param it does not read is refused after
// it returns) and returns the immutable type with its derived fields declared.
// Makers throw std::invalid_argument or RequestError for bad params, with a
// message that teaches (what was wrong, what is valid).
class FieldTypeRegistry {
public:
  static FieldTypeRegistry& global();

  using Maker = std::function<std::unique_ptr<const PluginFieldType>(const ParamReader& params)>;

  // The name is [a-z0-9_]+ and not a built-in type name or "plugin". A
  // duplicate throws std::invalid_argument; any add after freeze() throws
  // std::logic_error.
  void add(std::string name, Maker maker);

  // Runs the maker and checks the declared fields. An unknown name throws
  // std::invalid_argument listing the built-in and registered types.
  std::shared_ptr<const RegisteredFieldType> make(const api::AnalyzerComponent& def) const;

  bool contains(std::string_view name) const { return makers.contains(name); }
  std::vector<std::string> names() const;

  // The built-in field type names, as the schema JSON spells them.
  static std::span<const std::string_view> builtinNames();

  void freeze() { isFrozen = true; }
  bool frozen() const { return isFrozen; }

private:
  std::map<std::string, Maker, std::less<>> makers;
  bool isFrozen = false;
};

// The frozen process-wide registry (FieldTypeRegistry::global()).
const FieldTypeRegistry& globalFieldTypeRegistry();

}  // namespace luxir
