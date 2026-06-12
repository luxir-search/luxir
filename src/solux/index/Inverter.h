#pragma once

#include <algorithm>
#include <limits>
#include <boost/unordered/unordered_flat_map.hpp>
#include "solux/util/MemPool.h"
#include "solux/schema/Schema.h"
#include "PostingsWriter.h"
#include "StoredFieldsWriter.h"

// so IndexHandler can consume protobuf types
#include "SortedDeletes.h"
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
  // minVersion starts at the max sentinel ("no updates seen yet") so the first
  // updateVersions() call adopts the real minimum; obtainInverter() always calls
  // it before the inverter is used or flushed, so the sentinel never reaches a
  // consumer. A version of 0 (the obtainInverter default / unversioned) is the
  // absorbing element of min and sticks, keeping the inverter an always-candidate
  // for commits and deletes.
  uint64_t currVersion = 0;
  uint64_t minVersion = std::numeric_limits<uint64_t>::max();
  uint64_t maxVersion = 0;

  // When true, indexing an id field will automatically queue a delete for previous
  // versions and index the _version_ field. Set via setOverwrite() before indexing docs.
  bool overwrite = false;

  // Populated by IdHandler::flush() with sorted delete lists.
  std::unique_ptr<SortedDeletes> sortedDeletes;

  // Populated after flush() if there were deleted documents
  uint64_t liveGen = 0;
  int32_t liveDocs = 0;

  Inverter(solux::Directory& dir, uint64_t segId, const std::function<std::shared_ptr<Schema>()>& schemaProvider = {}) : postingsWriter(dir, segId) {
    if (schemaProvider) {
      this->schemaProvider = schemaProvider;
    } else {
      // fallback for tests that create Inverters directly
      this->schemaProvider = []() {
        return Schema::createDefaultSchema();
      };
    }
  }

  PostingsWriter& getPostingsWriter() { return postingsWriter; }

  // for segmentVersions, adds, and deletes to be versioned correctly, this should be called after
  // obtaining the inverter
  void updateVersions(uint64_t version) {
    minVersion = std::min(minVersion, version);
    maxVersion = std::max(maxVersion, version);
    currVersion = version;
  }

  bool hasDeletions() {
    return sortedDeletes != nullptr;
  }

  // Record an explicit delete-by-id. Routes through IdHandler's delete hash
  // so all deletes end up as sorted lists after flush.
  void deleteId(std::string_view id, uint64_t version);

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

    // index() implementations should validate a value before mutating any stream
    // state: a throw is recovered by marking the doc deleted, so streams must stay
    // appendable for subsequent docs.  Data already appended for the failed doc is
    // fine (the doc is dead); a throw mid-append of a single value is not.
    // Cross-doc constraints learned from values (e.g. vector dims) must only be
    // committed once a value is actually appended, so a failed doc does not
    // constrain later docs; see VectorHandler.
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



  // This could also be a Set with a little more work since the fieldname is already in the value.
  // We don't want the values to move since clients can cache and reuse when indexing.
  // Handlers are pool-allocated; u_ptr destroys them without freeing.
  boost::unordered_flat_map<std::string, u_ptr<IndexHandler>,
                            PackedTermHash, PackedTermEqual> indexHandlers;


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
  /// A partially indexed doc cannot be backed out of the append-only postings and
  /// column streams; deleting it via liveDocs is the recovery mechanism.
  void deleteDoc(int docid) {
    deleted.push_back(docid);
  }

  // Rollback support for failed documents / failed all_or_none requests.
  // A mark captures the undo state at a point in time; rollbackTo restores the
  // id map (termsHash / deleteHash mutations are undone) and un-marks docs
  // deleted after the mark (in-inverter overwrites mark superseded docs deleted;
  // a rolled-back update must resurrect them).  Postings and column data of
  // rolled-back docs stay in the segment; callers mark those docs deleted.
  // The undo scope is a single update message: IndexWriter::releaseInverter
  // clears the log, so marks must not be held across obtain/release.
  struct UndoMark {
    size_t idUndoSize = 0;
    size_t numDeleted = 0;
  };
  UndoMark undoMark() const;
  void rollbackTo(const UndoMark& mark);
  void clearUndoLog();

  size_t memSize() {
    // TODO: take into account more than just the pool
    return pool.size();
  }

  /// finishes indexing this segment (also calls finish on the underlying postings writer)
  /// returns true on success if anything was written.
  /// If filenames is provided, appends the names of all files written (for fsync at commit time).
  bool flush(std::vector<std::string>* filenames = nullptr);

private:
  IndexHandler* idHandler_ = nullptr;  // cached pointer to the IdHandler, set in createIndexHandler
  // Stored-fields writers, keyed by resource name.  Lazily populated when
  // fields with the STORED flag are first indexed.  PackedTermHash/Equal
  // give transparent lookup by string_view/PackedTerm/std::string.
  boost::unordered_flat_map<std::string, std::unique_ptr<StoredFieldsWriter>,
                            PackedTermHash, PackedTermEqual> storedFields_;

  // Get-or-create the StoredFieldsWriter for the named resource (column
  // family).  config may be null to use defaults.  Called from
  // createIndexHandler when wrapping a STORED field.
  StoredFieldsWriter& getOrCreateStoredFields(std::string_view resourceName,
                                              const StoredFieldType* config) {
    auto it = storedFields_.find(resourceName);
    if (it != storedFields_.end()) return *it->second;
    auto writer = std::make_unique<StoredFieldsWriter>(postingsWriter, resourceName, config);
    auto [newIt, _] = storedFields_.emplace(std::string(resourceName), std::move(writer));
    return *newIt->second;
  }

  IndexHandler& createIndexHandler(const std::string_view name);

};

inline std::string format_as(const Inverter& inverter) {
  return fmt::format("(seg={} max={})",  Postings::getIndexFileNamePrefix(inverter.getSegId()), inverter.getMaxDoc());
}

} // end namespace

