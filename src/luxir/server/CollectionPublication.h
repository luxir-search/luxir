// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "luxir/index/CommitSnapshot.h"
#include "CollectionEvents.h"

namespace luxir {
// State captured only at the ordered collection feed, never by a name lookup.
struct CollectionPublication {
  std::weak_ptr<Collection> collection;
  std::optional<CommitId> commit;
  uint64_t digest = 0; // of commit's manifest
  std::string error;

  void refresh();
};
}
