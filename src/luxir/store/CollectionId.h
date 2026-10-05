// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <compare>
#include <functional>
#include <string>
#include <string_view>
#include <boost/container_hash/hash.hpp>

namespace luxir {

// A collection's identity: the tenant that owns it and its name there.
// Requests name only the collection; their scope supplies the tenant.
// Manifests and commit tokens never carry either.
struct CollectionId {
  static constexpr std::string_view kDefaultTenant = "default";
  std::string tenant;
  std::string name;

  CollectionId() = default;
  CollectionId(std::string tenant, std::string name) : tenant(std::move(tenant)), name(std::move(name)) {}
  // A collection of the default tenant.
  static CollectionId of(std::string_view name) { return {std::string(kDefaultTenant), std::string(name)}; }

  auto operator<=>(const CollectionId&) const = default;
  // For messages: the default tenant's collections keep their plain names.
  std::string label() const { return tenant == kDefaultTenant ? name : tenant + "/" + name; }

  friend std::size_t hash_value(const CollectionId& id) {
    std::size_t seed = 0;
    boost::hash_combine(seed, id.tenant);
    boost::hash_combine(seed, id.name);
    return seed;
  }
};

}

template <> struct std::hash<luxir::CollectionId> {
  std::size_t operator()(const luxir::CollectionId& id) const noexcept { return hash_value(id); }
};
