// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <span>
#include <string>
#include <string_view>
#include <vector>
#include "luxir/store/CollectionId.h"

namespace luxir {

// What a follower copies: every collection of each listed tenant, plus each
// listed collection. Both empty copies everything. Collections match by name,
// so a source rename can move data in or out.
struct ReplicationSubscription {
  std::vector<std::string> tenants;      // sorted, unique
  std::vector<CollectionId> collections; // sorted, unique; never of a listed tenant

  bool all() const { return tenants.empty() && collections.empty(); }
  // Whether anything of the tenant is copied, so its catalog is watched.
  bool includesTenant(std::string_view tenant) const;
  bool includes(const CollectionId& id) const;
  // Collection labels, always "tenant/name".
  std::vector<std::string> labels() const;

  // Validates (InvalidCollectionNameError), sorts and deduplicates.
  static ReplicationSubscription of(std::span<const std::string> tenants, std::span<const std::string> collections);
  // Comma-separated lists, as in configuration.
  static ReplicationSubscription parse(std::string_view tenants, std::string_view collections);
  // "tenant/name", or "name" for the default tenant.
  static CollectionId parseLabel(std::string_view label);

  bool operator==(const ReplicationSubscription&) const = default;
};

} // namespace luxir
