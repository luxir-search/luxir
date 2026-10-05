// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0
#include "CollectionPublication.h"
#include "LuxirNode.h"

namespace luxir {
void CollectionPublication::refresh() {
  auto owner = collection.lock();
  if (!owner) return;
  error = owner->getUnavailableReason();
  auto shard = owner->getShard();
  auto snapshot = shard ? shard->getSnapshots().snapshot() : nullptr;
  if (snapshot && (!commit || snapshot->id.incarnation != commit->incarnation
      || snapshot->id.index_gen >= commit->index_gen)) commit = snapshot->id;
}
}
