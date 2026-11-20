// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "ObjectFields.h"

namespace luxir {

struct NestedPath {
  struct Family {
    int32_t parent;
    int32_t end;
    Family* next = nullptr;
  };
  PackedTerm name;
  NestedPath* parent;
  uint32_t depth;
  SpaceOrdinal ordinal = SpaceOrdinal::ROOT; // Reserved on the first row.
  int32_t rows = 0;
  Family* first = nullptr;
  Family* last = nullptr;
  ObjectFields fields;
  boost::unordered_flat_map<std::string_view, PackedTerm, PackedTermHash, PackedTermEqual> names;

  NestedPath(Inverter& inverter, std::string_view name, NestedPath* parent)
      : name(inverter.pool, name), parent(parent), depth(parent ? parent->depth + 1 : 1) {}

  class Names {
    Inverter& inverter;
    NestedPath& path;
  public:
    Names(Inverter& inverter, NestedPath& path) : inverter(inverter), path(path) {}
    std::optional<std::string_view> resolve(std::string_view name) const;
  };
};

// One append-only row counter and sparse family log per encountered path.
class NestedState {
  Inverter& inverter;
  boost::unordered_flat_map<std::string_view, u_ptr<NestedPath>, PackedTermHash, PackedTermEqual> paths;

public:
  static constexpr uint32_t MAX_DEPTH = 32;
  NestedPath* active = nullptr;

  explicit NestedState(Inverter& inverter) : inverter(inverter) {}
  NestedPath& path(std::string_view name);
  void index(NestedPath& path, const IndexVal& val);
  void finish();
};

namespace handler {

class NestedHandler final : public Inverter::IndexHandler {
  NestedState& state;
  NestedPath& path;
public:
  NestedHandler(Inverter& inverter, std::string_view name, const std::shared_ptr<FieldType>& type,
                SpaceOrdinal space, NestedState& state, NestedPath& path)
      : IndexHandler(PackedTerm(inverter.pool, name), type, space), state(state), path(path) {}
  void index(Inverter&, const IndexVal& val) override { state.index(path, val); }
  void flush(Inverter&) override {}
};

} // namespace handler
} // namespace luxir
