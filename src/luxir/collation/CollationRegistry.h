// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "luxir/api/luxir_types.hpp"

namespace luxir {

// Turns a string into a binary sort key: comparing two keys as unsigned bytes
// (memcmp, then length) orders their values by the collation. Values the
// collation treats as equal may or may not get equal keys. One instance per
// use; not thread-safe, and it may keep scratch state between calls.
class Collator {
public:
  virtual ~Collator() = default;
  // Appends the key of `value` to `key`. The caller owns `key` and may have put
  // other bytes in it; the collator only appends.
  virtual void appendKey(std::string_view value, std::string& key) = 0;
};

// Immutable and thread-safe: built once per authored collation (locale data, a
// compiled rule set) and shared by every field using it.
class CollatorFactory {
public:
  virtual ~CollatorFactory() = default;
  virtual std::unique_ptr<Collator> create() const = 0;
};

// A collation as the schema compiled it: the factory plus the canonical
// name and parameters that signature the field in segment metadata. Keys from
// different canonical definitions are not comparable, so a changed definition
// is a new field representation.
class Collation {
public:
  const std::string name;       // registry name, e.g. "icu"
  const std::string canonical;  // the definition with its parameters sorted, as JSON
  const std::unique_ptr<const CollatorFactory> factory;

  Collation(std::string name, std::string canonical, std::unique_ptr<const CollatorFactory> factory)
      : name(std::move(name)), canonical(std::move(canonical)), factory(std::move(factory)) {}

  std::unique_ptr<Collator> create() const { return factory->create(); }
};

// The collation registry: name -> maker. The process-wide instance is
// Registries::global().collation, read through globalCollationRegistry() by
// code that does not link the plugin headers. Plugins register through
// Plugin::registerComponents during initPlugins(), which then freezes it; after
// the freeze, lookups only read an immutable table (no locking).
//
// A maker receives the authored component (name + typed params, the same shape
// an analysis component has), validates the params, and returns an immutable
// factory. Makers throw std::invalid_argument for bad params; the message
// should teach (what was wrong, what is valid).
class CollationRegistry {
public:
  using Maker = std::function<std::unique_ptr<const CollatorFactory>(const api::AnalyzerComponent&)>;

  // The name is [a-z0-9_]+. A duplicate throws std::invalid_argument; any add
  // after freeze() throws std::logic_error.
  void add(std::string name, Maker maker);

  // Throws std::invalid_argument with a teaching message ("unknown collation
  // 'x'; valid collations: a, b, c"); names are listed sorted.
  std::shared_ptr<const Collation> make(const api::AnalyzerComponent& def) const;

  std::vector<std::string> names() const;

  void freeze() { isFrozen = true; }
  bool frozen() const { return isFrozen; }

private:
  std::map<std::string, Maker, std::less<>> makers;
  bool isFrozen = false;
};

// The frozen process-wide registry (Registries::global().collation).
const CollationRegistry& globalCollationRegistry();

}  // namespace luxir
