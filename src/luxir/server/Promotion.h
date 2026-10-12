// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <filesystem>
#include <iosfwd>
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

// Offline promotion of a follower data directory to an independent writer:
// every selected collection gets a new identity. Resumable: each target is
// recorded in the follower binding before CURRENT selects it, and a recorded
// target is reused only if its newest root and every file it references are
// present. The binding is removed only when every collection succeeded, and a
// writer refuses to start while it exists. Prints one line per collection and a
// summary; returns false on any failure.
bool promote(const std::filesystem::path& dataDir, std::ostream& output);

}
