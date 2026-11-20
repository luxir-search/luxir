// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <charconv>
#include "luxir/api/luxir_index.hpp"
#include "luxir/reader/Postings.h"

namespace luxir {

// Owning counterpart of the wire reference. A local schema with no shared base
// uses major/minor zero. Epoch is file identity, not a field-history generation.
struct SchemaBinding {
  std::string scope;
  std::string name;
  api::SchemaVersion version;

  SchemaBinding() = default;
  explicit SchemaBinding(const api::SchemaReference& ref) : scope(ref.scope), name(ref.name), version(ref.version) {}
  api::SchemaReference wire() const { return {scope, name, version}; }
  auto operator<=>(const SchemaBinding&) const = default;
  uint64_t generation() const { return version.generation(); }

  static std::string fileName(const api::SchemaVersion& version) {
    return std::format("schema_{}.{}.{}.{}", version.major, version.minor, version.local,
                       Postings::epochString(version.epoch));
  }
  static bool isFile(std::string_view name) {
    if (!name.starts_with("schema_")) return false;
    name.remove_prefix(7);
    api::SchemaVersion v;
    for (auto* component : {&v.major, &v.minor, &v.local}) {
      auto dot = name.find('.');
      if (dot == std::string_view::npos) return false;
      auto text = name.substr(0, dot);
      auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), *component);
      if (error != std::errc() || end != text.data() + text.size() || std::to_string(*component) != text) return false;
      name.remove_prefix(dot + 1);
    }
    v.epoch = Postings::parseEpoch(name);
    return v.local && v.epoch && Postings::epochString(v.epoch) == name;
  }
};

}
