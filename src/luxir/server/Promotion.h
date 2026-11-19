// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <filesystem>
#include <functional>
#include <iosfwd>
#include <memory_resource>
#include <map>
#include <string>
#include <string_view>
#include "TenantCatalog.h"
#include "luxir/store/Manifest.h"

namespace luxir {

// Copies a snapshot of incarnation `from` into a fresh incarnation, linking its
// immutable files and writing a durable root under the new identity at a
// generation above any root `from` held. With `verify` every linked file is
// read and checked against its manifest size and xxh3 first (a mismatch throws
// ChecksumMismatchError). Returns the reserved incarnation; activation is separate.
// Promotion and recovery both start a new identity this way. Markers naming
// files of the copied manifest are durable in the new incarnation on return.
// If supplied, claimed receives the new lifetime for opening its writer.
std::string copySnapshot(TenantCatalog& storage, std::string_view name, std::string_view from, const Manifest& manifest,
                          bool verify = false, IncarnationState* claimed = nullptr);

// Rewrites a copied root before its files are linked: it may drop segments
// and write files of its own into `target` (named for `writerEpoch`, the new
// lifetime). Strings it adds must live in `mr`.
using SnapshotEdit = std::function<void(api::IndexInfo& info, Directory& target, uint64_t writerEpoch,
                                        std::pmr::memory_resource& mr)>;

// As copySnapshot, into a fresh incarnation reserved for the free name `to`
// (incarnation `from` stays selected by its own name), rewritten by `edit`.
// Returns the reserved incarnation; the caller activates or removes it.
std::string copySnapshotAs(TenantCatalog& storage, std::string_view to, std::string_view from, const Manifest& manifest,
                           const SnapshotEdit& edit);

// Offline promotion of a follower data directory to an independent writer:
// every selected collection gets a new identity. Resumable: each target is
// recorded in the follower binding before the catalog activates it, and a recorded
// target is reused only if its newest root and every file it references are
// present. The binding is removed only when every collection succeeded, and a
// writer refuses to start while it exists. Prints one line per collection and a
// summary; returns false on any failure. `verify` is store.verify-on-open=full:
// the linked files are read and checked, and a damaged one fails its collection.
bool promote(const std::filesystem::path& dataDir, std::ostream& output, bool verify = false);

}
