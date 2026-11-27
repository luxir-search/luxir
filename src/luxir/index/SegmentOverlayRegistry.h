// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <memory_resource>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "luxir/api/luxir_index.hpp"
#include "luxir/api/luxir_types.hpp"
#include "luxir/reader/AuxReader.h"
#include "luxir/store/Directory.h"
#include "luxir/util/ParamReader.h"

namespace luxir {

class PostingsReader;

// What a kind's maker may read. Valid only for the duration of the call.
struct SegmentOverlayContext {
  const ParamReader& params;  // the overlay's params from the schema
};

// One segment to build an overlay for.
struct SegmentOverlayBuild {
  uint64_t segId;
  int32_t maxDoc;
  PostingsReader& segment;           // the finished segment
  std::vector<std::string> fields;   // the segment's physical fields the overlay covers, never empty
  Directory& dir;
  // The name of the overlay's file `fnum`. Every file the overlay writes takes
  // its name from here; on a failed build they are deleted.
  std::function<std::string(uint32_t fnum)> fileName;
};

struct SegmentOverlayOutput {
  std::vector<FileDescriptor> files;
  // Stored in the manifest entry (AuxIndexInfo.opaque_meta) and readable when
  // the overlay opens, with no I/O: keep it small.
  std::vector<std::byte> meta;
};

class SegmentOverlayKind {
public:
  virtual ~SegmentOverlayKind() = default;
  // Writes the segment's overlay; nullopt means this segment needs none.
  // Throwing leaves the segment without the overlay.
  virtual std::optional<SegmentOverlayOutput> build(const SegmentOverlayBuild& request) const = 0;
};

struct SegmentOverlayKindInfo {
  using Maker = std::function<std::unique_ptr<SegmentOverlayKind>(const SegmentOverlayContext&)>;
  // Opens the reader from the overlay's files, which the snapshot already opened,
  // in manifest order. Any exception it throws counts as invalid bytes: it
  // becomes a SegmentCorruptError naming the segment (so the plugin need not), and
  // under ADVISORY the overlay then reads as absent for the life of the snapshot,
  // under REQUIRED every use fails.
  using Open = std::function<std::shared_ptr<AuxReader>(std::span<const std::shared_ptr<InputFile>> files,
                                                        const api::AuxIndexInfo&)>;
  Maker maker;
  Open open;
  // ADVISORY: an overlay with invalid bytes reads as absent; REQUIRED fails its uses.
  OverlayPolicy policy = OverlayPolicy::ADVISORY;
  // The opened reader implements TermMembership, so term pruning consults it.
  // Requires ADVISORY: an overlay that cannot open or verify proves nothing.
  bool termMembership = false;
};

// One entry of a collection's segment_overlays, owning its strings.
class SegmentOverlaySpec {
  std::pmr::monotonic_buffer_resource arena;

public:
  api::SegmentOverlayDef def;  // views into this spec
  // Hex hash of what decides an overlay's content: kind, fields and params. Recorded
  // with each built overlay (AuxInfo::field); a segment whose overlay differs is rebuilt.
  std::string contentHash;

private:
  std::string overlayNameText;

public:

  explicit SegmentOverlaySpec(const api::SegmentOverlayDef& source);
  SegmentOverlaySpec(const SegmentOverlaySpec&) = delete;
  SegmentOverlaySpec& operator=(const SegmentOverlaySpec&) = delete;

  // AuxIndexInfo.name of this overlay: "<kind>.<name>".
  const std::string& overlayName() const { return overlayNameText; }
  // The physical fields of `segment` this overlay covers, sorted.
  std::vector<std::string> resolveFields(PostingsReader& segment) const;
};

// Overlay kinds registered by plugins. Filled during startup (initPlugins),
// then frozen.
class SegmentOverlayRegistry {
  std::map<std::string, SegmentOverlayKindInfo, std::less<>> kinds;
  std::atomic<bool> isFrozen = false;

public:
  static SegmentOverlayRegistry& global();

  // Startup only: throws std::invalid_argument on a bad or duplicate name,
  // std::logic_error after the freeze. Names are [a-z0-9_]+.
  void add(std::string kind, SegmentOverlayKindInfo info);
  const SegmentOverlayKindInfo* find(std::string_view kind) const;

  // Checks a schema entry: its name, kind, fields and params. Throws
  // std::invalid_argument or ParamError.
  void validate(const api::SegmentOverlayDef& def) const;
  std::unique_ptr<SegmentOverlayKind> make(const SegmentOverlaySpec& spec) const;

  // Sorted.
  std::vector<std::string> names() const;
  bool frozen() const { return isFrozen.load(std::memory_order_acquire); }
  void freeze() { isFrozen.store(true, std::memory_order_release); }
};

}  // namespace luxir
