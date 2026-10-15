// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "TopTerms.h"
#include "luxir/api/luxir_index.hpp"
#include "luxir/reader/AuxReader.h"
#include "luxir/store/Directory.h"

namespace luxir {

class OrdMap;

/// Global-ordinal maps persisted for one segment composition: the
/// index-level aux entry of kind "ordmaps" (format: OrdMapDirectory in
/// protos/luxir_index.proto). IndexWriter builds it when a commit changes the
/// segment set and carries it forward otherwise, keyed by built_core_gen.
/// A reader whose core matches loads a field's map from it on first use
/// instead of merging every segment's terms dictionary; anything missing,
/// stale or corrupt falls back to building, so the store is only a latency
/// hint.
class OrdMapStore : public AuxReader {
public:
  struct Segment {
    uint64_t segId = 0;
    int32_t maxDoc = 0;
    uint64_t digest = 0;
    bool operator==(const Segment&) const = default;
  };

private:
  struct Entry {
    int64_t numOrds = 0;
    int32_t singleSegment = 0;
    uint64_t offset = 0;
    uint64_t length = 0;
    uint64_t xxh3 = 0;
    TopTerms topTerms;
  };
  std::string name;
  std::shared_ptr<InputFile> file;
  bool valid = false;
  uint64_t coreGen = 0;
  std::vector<Segment> segments;
  std::map<std::string, Entry, std::less<>> entries;

  void parse();

public:
  static constexpr std::string_view KIND = "ordmaps";
  static constexpr std::string_view NAME = "ordmaps";
  static constexpr uint32_t FORMAT_VERSION = 1;

  // Digest of a segment's data files; live docs are ignored, so deletes do
  // not change it.
  static uint64_t segmentDigest(std::span<const FileDescriptor> files);

  // "s_ordmaps_<core_gen>_00", following the index-level aux file naming.
  static std::string fileName(uint64_t coreGen);

  // Writes the non-null `maps` (sorted by field) as an ordmaps file, not
  // synced, and returns its descriptor.
  static FileDescriptor write(Directory& dir, const std::string& name, uint64_t coreGen,
                              std::span<const Segment> segments,
                              std::span<const std::pair<std::string, std::shared_ptr<OrdMap>>> maps);

  // Where the directory starts in an ordmaps file's bytes, or nullopt.
  static std::optional<uint64_t> directoryOffset(std::string_view bytes);

  // Null only for a missing file with missingFileOK; a corrupt file opens as
  // an empty store (logged).
  static std::shared_ptr<OrdMapStore> open(Directory& dir, const api::AuxIndexInfo& info, bool missingFileOK);

  OrdMapStore(std::string name, uint64_t gen, uint64_t builtCoreGen, std::shared_ptr<InputFile> file);

  std::string_view getKind() const override { return KIND; }
  std::string_view getName() const override { return name; }

  // Whether the store is intact and was built for exactly these segments.
  bool matches(uint64_t coreGen, std::span<const Segment> segments) const;

  bool contains(std::string_view field) const { return entries.contains(field); }

  // The persisted map of `field`, identical to OrdMap::build over the
  // matching segments; null when the field is absent or its payload fails
  // its digest (logged).
  std::shared_ptr<OrdMap> load(std::string_view field) const;
};

} // namespace luxir
