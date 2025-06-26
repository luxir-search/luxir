#pragma once

#include <string>
#include <vector>

// Holds data about deletes during indexing that need to be applied to segments before
// a commit is made.  Instances start off on the Inverter and are transferred
// to the appropriate CommitInfo when a segment is flushed.
class DeletesData {
public:
  // FUTURE OPT: use a separate MemPool for the "id" field to avoid copying the id string in order to
  // transfer it to SegInfo and outlive this Inverter's lifetime.  Explicit deletes could also
  // go in that pool.  std::string is too big for this anyway (heap allocation aside)
  // Could make a stream API for Arena that is shared for all inverters that will be part of a commit.
  std::vector<std::string> deletedIds;
  std::vector<uint64_t> deletedVersions; // the versions of the Ids in the deletedIds field.  Could delta encode.

  // deletes are remembered for now and applied when the segment is flushed.
  // the version passed should be the version from the UpdateMessage sequence.
  void deleteId(std::string_view id, uint64_t version) {
    deletedIds.emplace_back(id);
    deletedVersions.emplace_back(version);
  }

  uint64_t getLargestVersion() const {
    if (deletedVersions.empty()) {
      return 0;
    }
    return deletedVersions.back();
  }

  uint64_t getSmallestVersion() const {
    if (deletedVersions.empty()) {
      return 0;
    }
    return deletedVersions.front();
  }

  std::string toString(size_t max=10) const {
    std::string s;
    s += "(sz=" + std::to_string(deletedIds.size()) + "; ids=";
    auto n = std::min(deletedIds.size(), max);
    s.reserve(deletedIds.size() * 20);
    for (size_t i = 0; i < n; i++) {
      if (i > 0) {
        s += ",";
      }
      s += deletedIds[i];
      s += ':';
      s += std::to_string(deletedVersions[i]);
    }
    s += ")";
    return s;
  }
};

// DeletesData from multiple inverters.
class MultiDeletesData {
public:
  std::vector<std::unique_ptr<DeletesData>> deletesData;

  uint64_t getLargestVersion() const {
    uint64_t largest = 0;
    for (const auto& data : deletesData) {
      largest = std::max(largest, data->getLargestVersion());
    }
    return largest;
  }

  uint64_t getSmallestVersion() const {
    uint64_t smallest = std::numeric_limits<uint64_t>::max();
    for (const auto& data : deletesData) {
      smallest = std::min(smallest, data->getSmallestVersion());
    }
    return smallest;
  }

  bool empty() const {
    return deletesData.empty();
  }
};
