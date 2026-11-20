// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <map>
#include <mutex>
#include <tuple>
#include "Schema.h"
#include "luxir/store/Directory.h"
#include "luxir/store/StorageMemory.h"

namespace luxir {

// One cache per node/backend. Local schema locations include the incarnation;
// collection names are not identities. Weak entries retain no unused schemas.
class SchemaCache {
  using Key = std::tuple<std::string, std::string, std::string, api::SchemaVersion>;
  std::mutex mutex;
  std::map<Key, std::weak_ptr<Schema>> entries;
  size_t additions = 0;
  std::shared_ptr<StorageMemory> memory = std::make_shared<StorageMemory>();

  std::shared_ptr<Schema> remember(const Key& key, std::shared_ptr<Schema> schema);
  static Key key(std::string_view incarnation, const SchemaBinding& binding);
public:
  std::shared_ptr<Schema> load(Directory& dir, const api::IndexInfo& info);
  std::shared_ptr<Schema> publish(Directory& dir, std::string_view incarnation, std::shared_ptr<Schema> schema);
  // Estimated owned payload, separate from RAM backing-file charges. The last
  // schema reference releases its tenant and node charge, even after teardown.
  uint64_t bytes() const { return memory->bytes(); }
  uint64_t tenantBytes(std::string_view tenant) { return memory->child(std::string(tenant))->bytes(); }
};

}
