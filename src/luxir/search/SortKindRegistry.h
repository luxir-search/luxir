// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once
#include <functional>
#include <map>
#include <memory>
#include <memory_resource>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "luxir/api/luxir_types.hpp"
#include "luxir/search/FieldComparator.h"
#include "luxir/search/IndexReader.h"

namespace luxir {

class FieldResolver;
class Schema;
struct ResolvedFieldHandle;

// The sort clause a claimed sort kind computes. It is immutable and shared by
// every collector a request creates (one per thread, collapse member folds,
// head sorts), so per-collection state lives in the comparators it makes.
class SortKindSource {
public:
  virtual ~SortKindSource() = default;

  // A comparator over `reader`'s segments in the clause's direction (the
  // maker saw it as SortKindContext::descending). It follows the
  // FieldComparator contract: compare* return < 0 when the first argument
  // ranks first, compareBottom(bottom, ., doc) > 0 when doc beats the bottom,
  // and docs without a value rank last in both directions, tied among
  // themselves. It may offer competitiveDocs (FieldComparator) to let the
  // collector jump over docs that cannot enter the heap.
  // ValueSortComparator implements everything but the per-doc read.
  virtual std::unique_ptr<FieldComparator> createComparator(IndexReader& reader) const = 0;

  // The value `doc` of `segment` ranks by, for the _sort_ return field and
  // collapse ranks; leave `out` unset for a doc without one. It must agree
  // with the comparator's key (same value, same docs without one): collapse
  // ranks group heads by it and clients page with it. `mr` backs any string
  // or array the value allocates.
  virtual void value(IndexReader& reader, const IndexReader::Segment& segment, int32_t doc,
                     api::Val& out, std::pmr::memory_resource& mr) const = 0;
};

// The reducer around a claimed call: name(...) alone, min(name(...)) or
// max(name(...)). What it means is the kind's: for a multi-valued field it
// usually selects the nearest or farthest value.
enum class SortKindMode { SINGLE, MIN, MAX };

// One argument of a claimed call. Arguments are literals, not expressions: a
// bare name (usually a field), a number, a quoted string, or a $variable from
// SortSpec.vars holding a number or a string.
struct SortKindArg {
  enum class Kind { NAME, NUMBER, STRING };
  Kind kind = Kind::NAME;
  // NAME: the identifier; STRING: the unescaped string; NUMBER: the literal
  // as written (empty for a variable).
  std::string_view text;
  // NUMBER: an int64 literal or variable (intValue holds it exactly); else a
  // double.
  bool integer = false;
  int64_t intValue = 0;
  double doubleValue = 0;  // NUMBER: the value, integers included
  // The $variable the value came from, without the '$'; empty for a literal.
  std::string_view variable;
};

// What a sort kind maker receives. The accessors throw RequestError naming
// the expression and the argument, which the request reports.
class SortKindContext {
public:
  std::string_view name;   // the claimed name
  std::string_view expr;   // the whole sort expression
  SortKindMode mode = SortKindMode::SINGLE;
  bool descending = false;
  std::span<const SortKindArg> args;
  const Schema& schema;
  FieldResolver& fields;

  SortKindContext(std::string_view name, std::string_view expr, SortKindMode mode, bool descending,
                  std::span<const SortKindArg> args, const Schema& schema, FieldResolver& fields)
      : name(name), expr(expr), mode(mode), descending(descending), args(args), schema(schema),
        fields(fields) {}

  // Throws unless the call has between min and max arguments.
  void arity(size_t min, size_t max) const;
  // Argument i as a field, resolved as a value expression resolves it (a
  // field's value variant, see schema.md); a NAME or a STRING.
  const ResolvedFieldHandle& field(size_t i) const;
  // Argument i as a number (a double, also for an integer).
  double number(size_t i) const;
  // Argument i as an int64; a double argument is an error.
  int64_t integer(size_t i) const;
  // Argument i as a quoted string or a string $variable.
  std::string_view string(size_t i) const;
  // Throws a RequestError "sort expression '<expr>': <message>".
  [[noreturn]] void fail(std::string_view message) const;
  // Adds a line to ?explain=resolved, for example what the kind resolved to.
  void note(std::string text) const;

private:
  const SortKindArg& arg(size_t i) const;
};

// Returns the clause's source; throws RequestError for bad arguments (the
// context's accessors already do).
using SortKindMaker = std::function<std::shared_ptr<const SortKindSource>(SortKindContext& ctx)>;

class SortKindRegistry {
  std::map<std::string, SortKindMaker, std::less<>> makers;
  bool isFrozen = false;

public:
  static SortKindRegistry& global();

  // The name is [a-z0-9_]+ and does not start with a digit; min, max and col
  // are the expression language's own. A claimed name may also be a value
  // function: the claim decides only a sort clause that is the call itself.
  // Throws std::invalid_argument on a bad, reserved or duplicate name or a
  // null maker, std::logic_error after freeze.
  void add(std::string name, SortKindMaker maker);
  const SortKindMaker* find(std::string_view name) const;
  std::vector<std::string> names() const;
  bool frozen() const { return isFrozen; }
  void freeze() { isFrozen = true; }
};

// SortKindRegistry::global(), for parsers that need no other registry.
const SortKindRegistry& globalSortKindRegistry();

// A sort expression whose root is a call to a claimed name, parsed.
struct SortKindCall {
  const SortKindMaker* maker = nullptr;
  std::string_view name;
  SortKindMode mode = SortKindMode::SINGLE;
  std::vector<SortKindArg> args;
};

// The claimed call `expr` is, or nullopt when its root is not a call to a
// name `registry` claims (the expression then sorts as a value expression).
// Throws RequestError for a claimed call whose arguments are not literals,
// names or defined $variables. Strings and literal texts view `expr` or `mr`.
std::optional<SortKindCall> parseSortKindCall(
    const SortKindRegistry& registry, std::string_view expr,
    const api::map_view<std::string_view, ::hpp_proto::indirect_view<api::Val>>& vars,
    std::pmr::memory_resource& mr);

// A FieldComparator over one numeric sort key per document, the usual shape
// of a plugin sort kind: the subclass reads the current segment's key of a
// doc (read), and this base keeps the slots, orders them in the clause's
// direction and ranks docs without a key last in both directions. T is
// int64_t or double; a NaN key is the subclass's to avoid.
template <class T>
class ValueSortComparator : public FieldComparator {
public:
  struct Key {
    T value{};
    bool present = false;
  };

protected:
  bool descending;
  std::vector<Key> slots;
  // The last key read, by segdoc: the collector compares a doc with the
  // bottom and then copies it.
  segdoc lastDoc = {-1, -1};
  Key lastKey;

  // Reads the current segment's key of `doc` into `out`; false when the doc
  // has none. Docs arrive in any order.
  virtual bool read(int32_t doc, T& out) = 0;

  const Key& load(segdoc doc) {
    if (doc != lastDoc) {
      lastKey.present = read(doc.docId(), lastKey.value);
      lastDoc = doc;
    }
    return lastKey;
  }

  // < 0 when a ranks first.
  int order(const Key& a, const Key& b) const {
    if (a.present != b.present) return a.present ? -1 : 1;
    if (!a.present) return 0;
    int cmp = (a.value > b.value) - (a.value < b.value);
    return descending ? -cmp : cmp;
  }

public:
  ValueSortComparator(IndexReader::FieldSegments fields, bool descending)
      : FieldComparator(fields), descending(descending) {}

  const Key& slot(int32_t slot) const { return slots[(size_t)slot]; }

  int compareBottom(int32_t bottomSlot, segdoc bottomDoc, segdoc newDoc) override {
    unused(bottomDoc);
    return order(slots[(size_t)bottomSlot], load(newDoc));
  }

  int compare(int32_t slotA, segdoc docA, int32_t slotB, segdoc docB) override {
    unused(docA, docB);
    return order(slots[(size_t)slotA], slots[(size_t)slotB]);
  }

  int compare(int32_t slotA, segdoc docA, FieldComparator& other, int32_t slotB,
              segdoc docB) override {
    unused(docA, docB);
    return order(slots[(size_t)slotA], static_cast<ValueSortComparator&>(other).slot(slotB));
  }

  void copy(int32_t slot, segdoc doc) override { slots[(size_t)slot] = load(doc); }

  void copy(int32_t slot, FieldComparator& other, int32_t otherSlot, segdoc otherDoc) override {
    unused(otherDoc);
    slots[(size_t)slot] = static_cast<ValueSortComparator&>(other).slot(otherSlot);
  }

  void growSlots(int32_t capacity) override {
    if (slots.size() < (size_t)capacity) slots.resize((size_t)capacity);
  }
};

}  // namespace luxir
