#pragma once

#include <algorithm>
#include <gtl/phmap.hpp>
#include "solux/util/MemPool.h"
#include "solux/schema/Schema.h"
#include "PostingsWriter.h"

// so IndexHandler can consume protobuf types
#include "DeletesData.h"
#include "protos/solux_types.pb.h"

namespace solux {

using std::iter_swap; // for boost string_sort


class CommitInfo;

class Inverter {
private:
  int currDoc = -1;  // the current document being indexed
  std::vector<int> deleted; // use a docstream for this?

  std::function<std::shared_ptr<Schema>()> schemaProvider;

public:
  MemPool pool;

  // Having a handle to the postings writer means that we can start flushing whenever we want or
  // even directly write certain dense columns without uninverting first.
  // For now, we'll directly contain it, but in the future we may want to pass it in.
  PostingsWriter postingsWriter;

  std::shared_ptr<Schema> schema;

  // If the flush of this inverter is part of a commit, then this will point to the CommitInfo
  // It is set asynchronously and consumed by the IndexWriter and is not used by the Inverter itself.
  CommitInfo* commitInfo = nullptr;

  // The lowest and highest update numbers for this inverter, including deletes.
  // Should be updated by calls to updateVersions() after obtaining the inverter.
  uint64_t currVersion = 0;
  uint64_t minVersion = 0;
  uint64_t maxVersion = 0;

  std::unique_ptr<DeletesData> deletesData;

  // Populated after flush() if there were deleted documents
  uint64_t liveGen = 0;
  int32_t liveDocs = 0;

  Inverter(solux::Directory& dir, uint64_t segId, const std::function<std::shared_ptr<Schema>()>& schemaProvider = {}) : postingsWriter(dir, segId) {
    // this is a test schemaProvider for convenience
    if (!schemaProvider) {
      this->schemaProvider = [&]() {
        return Schema::createSchema();
      };
    }
  }

  PostingsWriter& getPostingsWriter() { return postingsWriter; }

  // for segmentVersions, adds, and deletes to be versioned correctly, this should be called after
  // obtaining the inverter
  void updateVersions(uint64_t version) {
    if (minVersion != 0) {
      minVersion = version;
    } else {
      minVersion = std::min(minVersion, version);
    }
    maxVersion = std::max(maxVersion, version);
    currVersion = version;
  }

  bool hasDeletions() {
    return deletesData.get() != nullptr;
  }

  // deletes are remembered for now and applied when the segment is flushed.
  // the version passed should be the version from the UpdateMessage sequence.
  void deleteId(std::string_view id, uint64_t version) {
    if (deletesData == nullptr) {
      deletesData = std::make_unique<DeletesData>();
    }
    deletesData.get()->deleteId(id, version);
  }

  /// This is what clients should call to index the fields of a document.
  class IndexHandler {
    protected:
    friend Inverter;

    PackedTerm fieldName;
    std::shared_ptr<FieldType> fieldType;

    IndexHandler(IndexHandler& other) = delete; // let's not move any of these
  public:
    IndexHandler(PackedTerm fieldName, const std::shared_ptr<FieldType>& fieldType)
    : fieldName(fieldName), fieldType(fieldType) {
    }
    virtual ~IndexHandler() = default;

    auto operator<=>(const IndexHandler& other) const {
      return this->fieldName <=> other.fieldName;
    }

    auto operator<=>(std::string_view sv) const {
      return this->fieldName <=> sv;
    }

    auto operator<=>(const PackedTerm& fname) const {
      return this->fieldName <=> fname;
    }

    auto operator==(std::string_view sv) const {
      return this->fieldName == sv;
    }

    auto operator==(const PackedTerm& fname) const {
      return this->fieldName == fname;
    }

    virtual void index(Inverter& inverter, std::string_view val) {
      unused(inverter, val);
    }
    // yuck.  this is to handle an array of strings in protobuf (without creating a new list)
    virtual void index(Inverter& inverter, std::span<std::string_view> vals) {
      unused(inverter,vals);
    }
    // yuck.  this is to handle an array of strings in protobuf (without creating a new list)
    virtual void index(Inverter& inverter, std::span<const std::string* const> vals) {
      unused(inverter,vals);
    }
    virtual void index(Inverter& inverter, int64_t val) {
      unused(inverter, val);
    }
    virtual void index(Inverter& inverter, std::span<const int64_t> vals) {
      unused(inverter, vals);
    }

    virtual void index(Inverter& inverter, const proto::Val& val) {
      if (val.has_s()) {
        index(inverter, val.s());
      } else if (val.has_arr_s()) {
        auto& arr = val.arr_s().v();
        std::span<const std::string* const> values(arr.data(), arr.size());
        index(inverter, values);
      }
    }

    virtual void flush(Inverter &inverter) = 0;

    friend std::ostream& operator<<(std::ostream &out, const IndexHandler &sf) {
      return out << "{IndexHandler field:" << sf.fieldName << "}";
    }
  };



  // OPTIMIZATION: since we only do additions and not deletions, a monotonic allocator that had destructor
  // support would be good here.  Or we could add to our MemPool and manually destruct later.
  // This could also be a Set with a little more work since the fieldname is already in the value.
  // We don't want the values to move since clients can cache and reuse when indexing.
  gtl::flat_hash_map<std::string, std::unique_ptr<IndexHandler>> indexHandlers;


  // The returned reference will be valid for the duration of indexing this block.
  // TODO: a version that gets a set at a time, so the inverter can pick the best columns to write directly?
  // What about adjusting number of files on postings writer? We should be able to make that dynamic up until a max.
  IndexHandler& getIndexHandler(const std::string_view name) {
    auto iter = indexHandlers.find(name);
    if (iter != indexHandlers.end()) {
      return *iter->second;
    }

    return createIndexHandler(name);
  }


  void setDoc(int32_t docid) {
    assert (docid >= currDoc);
    currDoc = docid;
  }

  void startDoc() {
    currDoc++;
  }

  void finishDoc() {
  }

  int32_t getMaxDoc() const {
    return currDoc + 1;
  }

  int32_t getDoc() {
    return currDoc;
  }

  int64_t getSegId() const {
    return postingsWriter.segId;
  }

  /// mark the doc as deleted if something went wrong indexing it.
  void deleteDoc(int docid) {
    deleted.push_back(docid);
  }

  size_t memSize() {
    // TODO: take into account more than just the pool
    return pool.size();
  }

  /// finishes indexing this segment (also calls finish on the underlying postings writer)
  /// returns true on success if anything was written.
  bool flush();

private:
  IndexHandler& createIndexHandler(const std::string_view name);


};

inline std::string format_as(const Inverter& inverter) {
  return fmt::format("(seg={} max={})",  Postings::getIndexFileNamePrefix(inverter.getSegId()), inverter.getMaxDoc());
}

} // end namespace

