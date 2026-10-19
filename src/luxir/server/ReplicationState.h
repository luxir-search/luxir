// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <map>
#include <string>
#include "luxir/store/Directory.h"
#include "luxir/store/CollectionId.h"

namespace luxir {

// Node-local follower discovery history. CURRENT only selects data.
struct ReplicationState {
  struct Collection {
    std::string advertised;
    std::string boot;
    bool replace_empty = true;
    // Durable promotion target, recorded before switching CURRENT. A restart
    // finishes that switch instead of minting another incarnation.
    std::string promoted;
  };
  std::string source;
  std::map<std::string, std::map<std::string, Collection>> tenants;

  Collection& collection(const CollectionId& id) { return tenants[id.tenant][id.name]; }

  static ReplicationState read(InputFile& file);
  void write(Directory& dir) const;
};
}
