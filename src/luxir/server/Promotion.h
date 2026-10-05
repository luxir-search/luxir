// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <map>
#include <string>
#include <string_view>
#include "luxir/store/DirectoryFactory.h"

namespace luxir {

// Copies a snapshot of incarnation `from` into a fresh incarnation, linking its
// immutable files and writing a durable root under the new identity at a
// generation above any root `from` held. Returns the new incarnation; CURRENT
// is unchanged. Promotion and recovery both start a new identity this way.
std::string copySnapshot(CollectionStorage& storage, std::string_view from, const Manifest& manifest);

struct ReplicationState;

// Gives every selected collection of a follower data directory a new identity.
// Resumable: each target is recorded in `state` (persisted to `metadata`) before
// CURRENT selects it, and a recorded target is reused only if its newest root and
// every file it references are present. Returns per-collection failures; on
// success the follower binding is removed.
std::map<std::string, std::string> promote(DirectoryFactory& factory, Directory& metadata, ReplicationState& state);

}
