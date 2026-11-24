// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <map>
#include <string>
#include <vector>
#include "luxir/store/Directory.h"
#include "luxir/store/CollectionId.h"

namespace luxir {

// Node-local follower discovery history. The tenant catalog selects data.
struct ReplicationState {
  struct Collection {
    std::string advertised;
    std::string boot;
    bool replace_empty = true;
    // Durable promotion target, recorded before catalog activation. A restart
    // finishes that switch instead of minting another incarnation.
    std::string promoted;
  };
  struct Tenant {
    std::string boot;
    uint64_t revision = 0;
    std::map<std::string, std::string> names;
    std::map<std::string, Collection> incarnations;
  };
  // The subscription in force, and the configuration it last came from. A
  // restart whose configuration differs from `configured` adopts it; otherwise
  // a runtime change stands.
  struct Subscription {
    std::string source;
    std::vector<std::string> tenants, collections;
    bool operator==(const Subscription&) const = default;
  };
  std::string source;
  std::vector<std::string> subscribed_tenants, subscribed_collections;
  Subscription configured;
  std::map<std::string, Tenant> tenants;

  Collection& incarnation(std::string_view tenant, std::string_view id) {
    return tenants[std::string(tenant)].incarnations[std::string(id)];
  }
  Collection& collection(const CollectionId& id) {
    auto& tenant = tenants.at(id.tenant);
    return tenant.incarnations.at(tenant.names.at(id.name));
  }

  static ReplicationState read(InputFile& file);
  void write(Directory& dir) const;
};
}
