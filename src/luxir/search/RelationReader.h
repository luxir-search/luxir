// Copyright 2026 Luxir contributors
// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "IndexReader.h"
#include <map>

namespace luxir {
// Snapshot-local topology. A family token names one parent incarnation; old
// children cannot attach to a recreated parent, even across segment merges.
class RelationReader {
public:
  struct Parent { int32_t segment = -1; int32_t doc = -1; };
  struct Segment {
    std::vector<Parent> parents;
    std::vector<uint32_t> relation;
    std::vector<int32_t> children;
    std::unique_ptr<DocSet> roots;
  };
  std::vector<Segment> segments;
  std::map<std::string, uint32_t, std::less<>> relationOrdinals;
  explicit RelationReader(IndexReader& reader);
  static std::vector<std::string> strings(IndexReader& reader, int32_t segment,
      std::string_view field, std::span<const int32_t> docs);
};
}
