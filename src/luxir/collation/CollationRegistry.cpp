// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include "luxir/collation/CollationRegistry.h"

#include <stdexcept>

#include <glaze/glaze.hpp>

namespace luxir {

CollationRegistry& CollationRegistry::global() {
  static CollationRegistry instance;
  return instance;
}

void CollationRegistry::add(std::string name, Maker maker) {
  if (isFrozen) throw std::logic_error("collation registry is frozen; collations must be added during startup");
  if (name.empty()) throw std::invalid_argument("collation name must not be empty");
  for (char c : name) {
    if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_')) {
      throw std::invalid_argument("invalid collation name '" + name + "'; use [a-z0-9_]+");
    }
  }
  if (!maker) throw std::invalid_argument("collation '" + name + "' has no maker");
  if (makers.contains(name)) throw std::invalid_argument("duplicate collation '" + name + "'");
  makers.emplace(std::move(name), std::move(maker));
}

std::shared_ptr<const Collation> CollationRegistry::make(const api::AnalyzerComponent& def) const {
  auto valid = [this] {
    std::string out;
    for (const auto& name : names()) {
      if (!out.empty()) out += ", ";
      out += name;
    }
    return out.empty() ? std::string("none") : out;
  };
  if (def.name.empty()) {
    throw std::invalid_argument("collation has no name; valid collations: " + valid());
  }
  auto it = makers.find(def.name);
  if (it == makers.end()) {
    throw std::invalid_argument("unknown collation '" + std::string(def.name) + "'; valid collations: " + valid());
  }
  auto factory = it->second(def);
  if (!factory) throw std::invalid_argument("collation '" + std::string(def.name) + "' made no factory");
  // Sort the parameter objects so equal definitions have equal signatures.
  std::string json;
  if (!api::write_json(def, json)) throw std::runtime_error("Cannot serialize collation");
  glz::generic_sorted_u64 value;
  if (glz::read_json(value, json)) throw std::runtime_error("Cannot canonicalize collation");
  if (glz::write_json(value, json)) throw std::runtime_error("Cannot serialize canonical collation");
  return std::make_shared<const Collation>(std::string(def.name), std::move(json), std::move(factory));
}

std::vector<std::string> CollationRegistry::names() const {
  std::vector<std::string> out;
  out.reserve(makers.size());
  for (const auto& entry : makers) out.push_back(entry.first);  // std::map: already sorted
  return out;
}

}  // namespace luxir
