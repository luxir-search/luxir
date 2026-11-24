// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0
#include "ReplicationSubscription.h"

#include <algorithm>
#include <stdexcept>
#include "Collections.h"

namespace luxir {

bool ReplicationSubscription::includesTenant(std::string_view tenant) const {
  return all() || std::ranges::binary_search(tenants, tenant, std::less<>{}) ||
         std::ranges::any_of(collections, [&](const auto& id) { return id.tenant == tenant; });
}

bool ReplicationSubscription::includes(const CollectionId& id) const {
  return all() || std::ranges::binary_search(tenants, id.tenant, std::less<>{}) ||
         std::ranges::binary_search(collections, id);
}

std::vector<std::string> ReplicationSubscription::labels() const {
  std::vector<std::string> result;
  for (const auto& id : collections) result.push_back(id.tenant + "/" + id.name);
  return result;
}

CollectionId ReplicationSubscription::parseLabel(std::string_view label) {
  auto slash = label.find('/');
  CollectionId id = slash == std::string_view::npos
      ? CollectionId::of(label)
      : CollectionId{std::string(label.substr(0, slash)), std::string(label.substr(slash + 1))};
  Collections::validate(id);
  return id;
}

ReplicationSubscription ReplicationSubscription::of(std::span<const std::string> tenants,
                                                    std::span<const std::string> collections) {
  ReplicationSubscription result;
  for (const auto& tenant : tenants) {
    Collections::validateName(tenant, "tenant");
    result.tenants.push_back(tenant);
  }
  std::ranges::sort(result.tenants);
  result.tenants.erase(std::unique(result.tenants.begin(), result.tenants.end()), result.tenants.end());
  for (const auto& label : collections) {
    auto id = parseLabel(label);
    if (!std::ranges::binary_search(result.tenants, id.tenant)) result.collections.push_back(std::move(id));
  }
  std::ranges::sort(result.collections);
  result.collections.erase(std::unique(result.collections.begin(), result.collections.end()), result.collections.end());
  return result;
}

static std::vector<std::string> split(std::string_view list) {
  std::vector<std::string> result;
  if (list.empty()) return result;
  for (;;) { // a trailing comma leaves an empty entry, which validation rejects
    auto comma = list.find(',');
    result.emplace_back(list.substr(0, comma));
    if (comma == std::string_view::npos) break;
    list.remove_prefix(comma + 1);
  }
  return result;
}

ReplicationSubscription ReplicationSubscription::parse(std::string_view tenants, std::string_view collections) {
  return of(split(tenants), split(collections));
}

} // namespace luxir
