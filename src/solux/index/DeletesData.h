#pragma once

#include <string>
#include <vector>
#include <memory>

#include "solux/util/MemPool.h"
#include "solux/util/encoding.h"
#include "solux/store/InputStream.h"

namespace solux {

// Holds data about deletes during indexing that need to be applied to segments before
// a commit is made.  Instances start off on the Inverter and are transferred
// to the appropriate CommitInfo when a segment is flushed.
//
// Delete entries are packed in a MemPool for compactness and cache locality.
// Record format: [vlong (zigzag(versionDelta) + 1)][uint8_t idLen][char id[idLen]]
//   Code 0 is a sentinel meaning "end of buffer, hop to next".
//   versionDelta is zigzag-encoded so small deltas (positive or negative) use 1 byte.
//   The +1 shift reserves code 0 for the sentinel.
class DeletesData {
  MemPool pool;
  int32_t count_ = 0;
  uint64_t minVersion_ = 0;  // the lowest version number in the list
  uint64_t maxVersion_ = 0;   // the highest version number in the list
  uint64_t prevVersion_ = 0;   // last version written, for delta encoding

public:
  // Record a delete of the given id at the given version.
  void deleteId(std::string_view id, uint64_t version) {
    assert(id.size() <= 255);

    int64_t delta = (int64_t)version - (int64_t)prevVersion_;

    // vlong (zigzag(delta)+1) + 1 byte idLen + id bytes + 1 byte reserved for future sentinel
    int maxSize = MAX_VLONG_SIZE + 1 + (int)id.size() + 1;

    // Save state to detect block change
    char* prevBlockStart = pool.blockStart();
    char* prevPtr = pool.ptr();

    char* p = pool.alloc(maxSize);

    // If alloc moved to a new block, write sentinel in the old block
    if (pool.blockStart() != prevBlockStart) {
      *prevPtr = 0;  // sentinel
    }

    // Write record: [vlong (zigzag(delta) + 1)][uint8_t idLen][id bytes]
    char* start = p;
    p = writeVLong(p, zigzagEncode(delta) + 1);
    *p++ = (uint8_t)id.size();
    std::memcpy(p, id.data(), id.size());
    p += id.size();

    // Give back unused bytes
    pool.shrink(maxSize - (int)(p - start));

    if (count_ == 0) {
      minVersion_ = version;
      maxVersion_ = version;
    } else {
      if (version < minVersion_) minVersion_ = version;
      if (version > maxVersion_) maxVersion_ = version;
    }
    prevVersion_ = version;
    count_++;
  }

  int32_t count() const { return count_; }

  uint64_t getLargestVersion() const {
    return maxVersion_;
  }

  uint64_t getSmallestVersion() const {
    return minVersion_;
  }

  struct Entry {
    std::string_view id;
    uint64_t version;
  };

  class Iterator {
    const MemPool& pool_;
    InputStream is_;
    int bufIdx_;
    int32_t remaining_;
    uint64_t prevVersion_ = 0;

    void setBuf(int idx) {
      bufIdx_ = idx;
      const char* start = pool_.buffers[idx] + MemPool::HEADER_SIZE;
      const char* end = pool_.buffers[idx] + pool_.bufferSize(pool_.buffers[idx]);
      is_ = InputStream(start, end);
    }

  public:
    Iterator(const DeletesData& data)
      : pool_(data.pool)
      , bufIdx_(0)
      , remaining_(data.count_)
    {
      if (remaining_ > 0) {
        setBuf(0);
      }
    }

    bool hasNext() const { return remaining_ > 0; }

    Entry next() {
      uint64_t code = is_.readVlong();

      // Code 0 is a sentinel: hop to next buffer and read again
      while (code == 0) {
        setBuf(bufIdx_ + 1);
        code = is_.readVlong();
      }

      int64_t delta = zigzagDecode(code - 1);
      prevVersion_ = (uint64_t)((int64_t)prevVersion_ + delta);
      PackedTerm term = is_.readPackedTerm();

      remaining_--;
      return {(std::string_view)term, prevVersion_};
    }
  };

  std::string toString(size_t max=10) const {
    std::string s;
    s += "(sz=" + std::to_string(count_) + "; ids=";
    Iterator it(*this);
    for (size_t i = 0; i < max && it.hasNext(); i++) {
      auto entry = it.next();
      if (i > 0) {
        s += ",";
      }
      s += entry.id;
      s += ':';
      s += std::to_string(entry.version);
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

} // namespace solux
