// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

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
  using Open = std::function<std::shared_ptr<AuxReader>(Directory&, const api::AuxIndexInfo&)>;
  Maker maker;
  Open open;
  SegmentOverlayPolicy policy{.onDemand = true, .advisory = true};
};

// One entry of a collection's segment_overlays, owning its strings.
class SegmentOverlaySpec {
  std::pmr::monotonic_buffer_resource arena;

public:
  api::SegmentOverlayDef def;  // views into this spec

  explicit SegmentOverlaySpec(const api::SegmentOverlayDef& source);
  SegmentOverlaySpec(const SegmentOverlaySpec&) = delete;
  SegmentOverlaySpec& operator=(const SegmentOverlaySpec&) = delete;

  // AuxIndexInfo.name of this overlay: "<kind>.<name>".
  std::string overlayName() const;
  // The physical fields of `segment` this overlay covers, sorted.
  std::vector<std::string> resolveFields(PostingsReader& segment) const;
};

// Overlay kinds registered by plugins. Filled during startup (initPlugins),
// then frozen.
class SegmentOverlayRegistry {
  std::map<std::string, SegmentOverlayKindInfo, std::less<>> kinds;
  bool isFrozen = false;

public:
  static SegmentOverlayRegistry& global();

  // Throws std::invalid_argument on a bad or duplicate name, std::logic_error
  // after the freeze. Names are [a-z0-9_]+.
  void add(std::string kind, SegmentOverlayKindInfo info);
  const SegmentOverlayKindInfo* find(std::string_view kind) const;

  // Checks a schema entry: its name, kind, fields and params. Throws
  // std::invalid_argument or ParamError.
  void validate(const api::SegmentOverlayDef& def) const;
  std::unique_ptr<SegmentOverlayKind> make(const SegmentOverlaySpec& spec) const;

  // Sorted.
  std::vector<std::string> names() const;
  bool frozen() const { return isFrozen; }
  void freeze() { isFrozen = true; }
};

}  // namespace luxir
