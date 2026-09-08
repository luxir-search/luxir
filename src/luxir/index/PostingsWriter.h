// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <assert.h>
#include <iostream>
#include <mutex>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>
#include "luxir/index/TrieBuilder.h"
#include "luxir/index/IndexRamBudget.h"
#include "luxir/util/MemPool.h"
#include "luxir/util/StrRef.h"
#include "luxir/store/OutputStream.h"
#include "luxir/store/Directory.h"
#include "luxir/codec/StreamVByte.h"
#include "luxir/reader/PostingsReader.h"
#include "luxir/reader/FieldReader.h"
#include "ScreamingBuilder.h"
#include "luxir/codec/Codec.h"
#include "luxir/schema/FieldType.h"
#include "luxir/util/screaming.h"
#include "luxir/util/Signal.h"
#include "LiveDocsWriter.h"


namespace luxir {

/**
 * PostingsWriter is used to write the postings for a segment.
 * A single Inverter uses/owns a PostingsWriter.
 * PostingsWriter is currently single-threaded, but will be made multithreaded in the future for faster flushing.
 *
 * The high level strategy is to write the lowest levels first since higher level information needs/points to that info.
 *
 * For all terms in a field:
 *   For all documents containing that term:
 *     For all positions in that document:
 *       - Write all the positions and remember the number as termfreq
 *     - Write the termdoc info (doc,termfreq,pointer_to_positions) and remember the number as docfreq
 *   - Write the term info (term,docfreq,pointer_to_docs)
 */
class PostingsWriter {
public:
  static constexpr uint64_t SMALL_SEGMENT_BYTES = 64ULL << 20;
  static constexpr size_t RAM_SPILL_BYTES = 4ULL << 20;

  // IndexFieldInfo adds extra info needed at index time to SegFieldInfo.
  struct IndexFieldInfo : public SegFieldInfo {
  };

private:
  Directory& directory;
  int32_t maxDoc;  // set by caller
  int64_t sizeInBytes = 0; // total size of all files written

  // Files available for use, sorted so largest are at the back. Reusing the
  // largest keeps residual streams small enough to fold into file 0 at finish.
  std::vector<OutputStream*> freeFiles;
  std::string segStr;

  // Actual RAM retained by filesystem delegating files is a separate forced
  // reservation from inverter or merge working memory. Writes never wait, but
  // reservation growth still wakes the existing pressure machinery.
  std::mutex delegatedRamMutex;
  IndexRamBudget::Guard delegatedRamGuard;
  int64_t delegatedRamBytes = 0;
  bool ramDelegatingFiles = false;

  struct DataFile {
    OutputStream out;
    std::unique_ptr<File> file;
    uint32_t fileNum;
    bool collapseProtected = false;
    bool collapsed = false;
  };

  struct FileRelocation {
    uint32_t fileNum;
    uint64_t baseOffset;
  };
  static constexpr uint32_t DROPPED_FILE = UINT32_MAX;
  std::deque<DataFile> files;
  // These are dequeues so elements don't move
  // TODO: put these in the pool?
  std::deque<IndexFieldInfo> fieldInfos;
  // Guards the shared writer state that concurrent field-merge tasks touch: the
  // free-stream pool (freeFiles/files), the field registry (fieldInfos), and pool
  // (via copyTerm).  Checked-out OutputStreams are exclusively owned and need no lock.
  std::mutex mutex;
public:
  MemPool pool;  // during concurrent writing, access only under mutex (see copyTerm)
  uint64_t segId;

public:
  PostingsWriter(Directory& dir, uint64_t segId, int32_t maxDoc = -1,
                 IndexRamBudget* ramBudget = nullptr,
                 bool ramDelegatingFiles = false)
      : directory(dir), maxDoc(maxDoc),
        delegatedRamGuard(ramBudget == nullptr
            ? IndexRamBudget::Guard() : IndexRamBudget::Guard(*ramBudget, 0)),
        ramDelegatingFiles(ramDelegatingFiles), segId(segId)
  {
    segStr = Postings::getSortableString(segId);
  }

  void configureRamDelegation(bool enabled,
                              IndexRamBudget* ramBudget = nullptr) {
    {
      const std::lock_guard<std::mutex> lock(delegatedRamMutex);
      if (ramBudget != nullptr && !delegatedRamGuard) {
        assert(delegatedRamBytes == 0);
        delegatedRamGuard = IndexRamBudget::Guard(*ramBudget, 0);
      }
      ramDelegatingFiles = enabled;
    }
    if (!enabled) {
      // Called only after indexing has quiesced or before a merge starts.
      // Existing candidate streams must spill too; changing only future file
      // creation would retain threshold times stream-count RAM on a big flush.
      for (auto& file : files) file.out.disableRamBuffering();
    }
  }

  Directory& getDirectory() {
    return directory;
  }

  // Thread-safe; fields may be registered concurrently and in any order
  // (finish() sorts the field table by name).
  IndexFieldInfo& addField(PackedTerm fieldName) {
    const std::lock_guard<std::mutex> lock(mutex);
    fieldInfos.emplace_back(); // we should get default-initialization with this for the SegFieldInfo members
    fieldInfos.back().fieldname = fieldName;
    assert(fieldInfos.back().monoLoc.offset() == 0 && fieldInfos.back().monoMetaOff == 0 && fieldInfos.back().columnMetaOff == 0);
    assert(fieldInfos.back().pointsMetaOff == 0);
    assert(fieldInfos.back().mono2Loc.offset() == 0 && fieldInfos.back().mono2MetaOff == 0);
    assert(fieldInfos.back().valDocLoc.offset() == 0 && fieldInfos.back().valDocMetaOff == 0);
    return fieldInfos.back();
  }

  // copies the fieldName into the pool associated with this PostingsWriter.
  IndexFieldInfo& addField(std::string_view fieldName) {
    return addField(copyTerm(fieldName));
  }

  // Thread-safe copy of a term into this writer's pool; the copy lives until the
  // writer is destroyed.
  PackedTerm copyTerm(std::string_view s) {
    const std::lock_guard<std::mutex> lock(mutex);
    return PackedTerm(pool, s);
  }


  // make sure that numFiles can be obtained, and if not create more.
  void reserveFiles(size_t numFiles) {
    const std::lock_guard<std::mutex> lock(mutex);
    _reserveFiles(numFiles);
  }

private:
  // caller holds mutex
  void _reserveFiles(size_t numFiles) {
    while (freeFiles.size() < numFiles) {
      uint32_t fnum = (uint32_t)files.size();
      // TODO: in the future, if this does IO, we may not want to lock?
      Directory::FileCreateOptions options;
      options.ramDelegating = ramDelegatingFiles;
      options.ramSpillBytes = RAM_SPILL_BYTES;
      options.ramBytesChanged = [this](int64_t delta) noexcept {
        adjustDelegatedRam(delta);
      };
      std::unique_ptr<File> file = directory.createFile(
          Postings::getIndexFileName(segStr, fnum), std::move(options));
      files.emplace_back(DataFile{OutputStream{},std::move(file), fnum});
      files.back().out.setFile( files.back().file.get());
      files.back().out.streamNumber = fnum;
      // writing something at the start of the file acts as a sanity check, and also makes file locations of 0
      // invalid (and thus distinguishable from default-initialized).
      // TODO: think about embedding other info such as the segment id and file number?
      files.back().out.writeBytes(Postings::LUXIR_HEADER);
      // insert at front of free list to maintain sorted order.
      freeFiles.insert(freeFiles.begin(), &files.back().out);
    }
  }

public:
  // Custom deleter that holds a reference to the factory (PostingsWriter) that was used to obtain it.
  class OutputStreamDeleter {
  public:
    explicit OutputStreamDeleter(PostingsWriter* factory) : factory(factory) {}
    OutputStreamDeleter() : factory(nullptr) {}

    void operator()(OutputStream* os) const {
      if (os != nullptr) {
        factory->releaseOutputStream(os);
      }
    }

  private:
    PostingsWriter* factory;
  };

  typedef std::unique_ptr<OutputStream, OutputStreamDeleter> OutputStreamPtr;

  // Thread-safe.  A checked-out stream is exclusively owned until released.
  OutputStreamPtr getOutputStream() {
    return std::move(getOutputStreams<1>()[0]);
  }

  // Thread-safe.  Checked-out streams are exclusively owned until released.
  template <std::size_t N>
  std::array<OutputStreamPtr, N> getOutputStreams() {
    const std::lock_guard<std::mutex> lock(mutex);
    _reserveFiles(N);
    std::array<OutputStreamPtr, N> os;
    for (size_t i = 0; i < N; ++i) {
      os[i] = OutputStreamPtr(freeFiles.back(), OutputStreamDeleter(this));
      freeFiles.pop_back();
    }
    return os;
  }

  // Thread-safe (called by OutputStreamDeleter from whichever thread drops the stream).
  void releaseOutputStream(OutputStream* os) {
    const std::lock_guard<std::mutex> lock(mutex);
    auto compareBySize = [](const OutputStream* a, const OutputStream* b) {
      return a->size() < b->size();
    };
    auto it = std::upper_bound(freeFiles.begin(), freeFiles.end(), os, compareBySize);
    freeFiles.insert(it, os);
  }

  void releaseOutputStreams(std::span<OutputStreamPtr> streams) {
    for (auto& streamPtr : streams) {
      releaseOutputStream(streamPtr.release());
    }
  }

  // A TermRangeRow is already embedded in the shared terms stream by segment
  // finalize.  Pin every file it names so collapse never has to patch an
  // append-only row in place.  Survivor filenums remain stable because file 0
  // stores a sparse physical-file list.
  void protectOutputFiles(std::span<const uint32_t> fileNums) {
    const std::lock_guard<std::mutex> lock(mutex);
    for (uint32_t fileNum : fileNums) {
      assert(fileNum < (uint32_t) files.size());
      files[fileNum].collapseProtected = true;
    }
  }


  uint64_t getSegId() const {
    return segId;
  }

  // returns true if anything was written.
  // If filenames is non-null, appends the names of files written (for fsync at commit time).
  bool finish(std::vector<std::string>* filenames = nullptr) {

    if (fieldInfos.empty()) {
      return false;  // already called, or no data added.
    }

    // make sure we have at least one file
    reserveFiles(1);

    // Make sure that there are no outstanding files.
    // Do this for non-debug mode as well?
    assert(freeFiles.size() == files.size());
    Signal::emit("postingsBeforeCollapse", this);
    std::vector<uint32_t> survivorFileNums = collapseFiles();
    writeFieldIndex();
    writeSegmentInfo(survivorFileNums);

    // The StreamVByte AVX tail decoder used by postings iteration, and the
    // LinearPack bulk decoder used by ord columns, may read up to
    // SVB_OVERREAD_PAD bytes past the encoded data. Reserve that slack at the end of
    // every pure-data file so the read stays in bounds (ord columns write no pad
    // of their own). File 0 ends with the field index and segment info just
    // written above, which already provide far more trailing slack, so it is
    // skipped. Padding past its segment-info size marker would also break the
    // end-relative read in PostingsReader.
    if (survivorFileNums.size() > 1) {
      static const char svbPad[SVB_OVERREAD_PAD] = {};
      for (uint32_t fileNum : survivorFileNums) {
        if (fileNum != 0) files[fileNum].out.write(svbPad, SVB_OVERREAD_PAD);
      }
    }

    if (filenames) {
      filenames->reserve(filenames->size() + survivorFileNums.size());
    }
    for (uint32_t fileNum : survivorFileNums) {
      auto& dataFile = files[fileNum];
      sizeInBytes += dataFile.out.size();
      dataFile.out.close();
      directory.finishFile(*dataFile.file);
      if (filenames) {
        filenames->emplace_back(dataFile.file->name());
      }
    }

    freeFiles.clear();
    fieldInfos.resize(0);
    files.resize(0);
    assert(delegatedRamBytes == 0);
    return true;
  }

  void setMaxDoc(int max) {
    maxDoc = max;
  }

  int32_t getMaxDoc() {
    return maxDoc;
  }

  int64_t getSizeInBytes() {
    return sizeInBytes;
  }

  // Write liveDocs bitmap for documents that were marked as deleted during indexing
  // Returns the liveGen (generation number) used for the file, or 0 if no file was written
  uint64_t writeLiveDocs(const screaming::FixedBitSet& liveBits, int32_t numLiveDocs);

private:
  void adjustDelegatedRam(int64_t delta) noexcept {
    const std::lock_guard<std::mutex> lock(delegatedRamMutex);
    assert(delta >= -delegatedRamBytes);
    delegatedRamBytes += delta;
    assert(delegatedRamBytes >= 0);
    if (delegatedRamGuard) delegatedRamGuard.forceResize(delegatedRamBytes);
  }

  std::vector<uint32_t> collapseFiles() {
    assert(!files.empty());
    std::vector<FileRelocation> relocations;
    relocations.reserve(files.size());
    for (uint32_t fileNum = 0; fileNum < (uint32_t) files.size(); fileNum++) {
      relocations.push_back({fileNum, 0});
    }

    // File 0 is mandatory for field and segment metadata, so it is the only
    // target that guarantees a tiny segment becomes one physical file.  Keep
    // survivor filenums sparse instead of densely renumbering them: a stream
    // that already spilled can retain its storage key, avoiding rename/copy
    // work on filesystems and future object stores.
    // A stream holding only the LUXIR002 header carried no data.  If nothing
    // references it, drop it outright instead of folding eight bytes plus
    // alignment pad into file 0.  A zero-length structure can still park a
    // FieldInfo base at a header-only file's tail, so referenced ones are
    // appended like any other stream (the reader must be able to open the
    // location's file even to read zero bytes).
    std::vector<bool> referenced(files.size(), false);
    for (auto& fieldInfo : fieldInfos) {
      fieldInfo.relocateLocations([&](seg_location location) {
        if (location.filenum() < referenced.size()) {
          referenced[location.filenum()] = true;
        }
        return location;
      });
    }

    OutputStream& target = files[0].out;
    for (uint32_t fileNum = 1; fileNum < (uint32_t) files.size(); fileNum++) {
      DataFile& source = files[fileNum];
      if (source.collapseProtected) continue;
      if (!referenced[fileNum] && source.out.isRelocatable()
          && source.out.size() == Postings::LUXIR_HEADER.size()) {
        // Buffered bytes are discarded at destruction (FSFile never flushes on
        // destroy; a delegating file releases its RAM accounting there too).
        source.collapsed = true;
        relocations[fileNum] = {DROPPED_FILE, 0};
        continue;
      }
      uint64_t baseOffset = 0;
      if (target.tryAppendRelocatable(source.out, MAX_ALIGN, baseOffset)) {
        source.collapsed = true;
        relocations[fileNum] = {0, baseOffset};
      }
    }

    auto relocate = [&](seg_location location) {
      uint32_t fileNum = location.filenum();
      if (fileNum >= relocations.size()) {
        throw std::logic_error("FieldInfo references an unknown segment file");
      }
      const FileRelocation& relocation = relocations[fileNum];
      if (relocation.fileNum == DROPPED_FILE) {
        throw std::logic_error("FieldInfo references a dropped header-only segment file");
      }
      if (location.offset() > seg_location::maxOffset() - relocation.baseOffset) {
        throw std::length_error("relocated segment offset exceeds seg_location");
      }
      return seg_location(relocation.fileNum,
                          relocation.baseOffset + location.offset());
    };
    for (auto& fieldInfo : fieldInfos) {
      fieldInfo.relocateLocations(relocate);
    }

    std::vector<uint32_t> survivors;
    survivors.reserve(files.size());
    for (uint32_t fileNum = 0; fileNum < (uint32_t) files.size(); fileNum++) {
      if (!files[fileNum].collapsed) survivors.push_back(fileNum);
    }
    assert(!survivors.empty() && survivors[0] == 0);
    return survivors;
  }

  void writeSegmentInfo(std::span<const uint32_t> survivorFileNums) {
    // TODO: if maxDoc==0, does this cause issues elsewhere?
    // assert(maxDoc >= 1);
    OutputStream& out = files[0].out;
    auto outStart = out.size();
    out.writeVint(maxDoc);
    // Physical filenums are intentionally sparse.  Dense renumbering would
    // require renaming a file after it had started spilling, which becomes an
    // object-store copy.  Readers keep a filenum-indexed table with holes.
    out.writeVint((uint32_t) survivorFileNums.size());
    for (uint32_t fileNum : survivorFileNums) out.writeVint(fileNum);
    auto segInfoSize = out.size() - outStart;
    out.writeInt(segInfoSize);
  }

  void writeFieldIndex() {
    //
    // This format is position independent.  One just needs a pointer to the end of the final structure.
    //
    // Format:
    // List of field metadata, followed by an array of offsets for each field, followed by the number of fields.
    //

    // IDEA: for few fields, we could just skip the compression and memcpy the whole struct.

    OutputStream& fieldOutput = files[0].out;
    std::vector<uint32_t> fieldOffs;  // location of each field in fieldFile (TODO: what is the max number of fields we will support?)
    fieldOffs.reserve(fieldInfos.size());

    auto fieldsStart = fieldOutput.size();  // where this index starts

    // FieldReader::seek does a binary search over the field offsets array, so
    // fields must be written in sorted name order.  Callers that flush fields
    // in sorted order already satisfy this, but some resources (like
    // stored-fields) are appended at the very end of flush; sort defensively
    // here to support arbitrary addField ordering.
    std::vector<IndexFieldInfo*> sortedInfos;
    sortedInfos.reserve(fieldInfos.size());
    for (auto& fi : fieldInfos) sortedInfos.push_back(&fi);
    std::sort(sortedInfos.begin(), sortedInfos.end(),
              [](const IndexFieldInfo* a, const IndexFieldInfo* b) {
                return a->fieldname < b->fieldname;
              });

    for (auto* finfoPtr : sortedInfos) {
      auto& finfo = *finfoPtr;
      auto fieldLoc = fieldOutput.size();
      fieldOffs.push_back(fieldLoc - fieldsStart);  // make the location relative so we can append this to a large file if necessary

      fieldOutput.writePackedTerm(finfo.fieldname);
      fieldOutput.writeVint(finfo.type);
      fieldOutput.writeVint(finfo.flags);
      fieldOutput.writeVlong(finfo.docsWithField);

      //  nocommit     if ((finfo.flags & 0x01) != 0) {
      // TODO: we could perhaps also tell by something like nTerms > 0?
      if ((finfo.flags & FieldType::INDEX_DOCS) != 0) {
        fieldOutput.writeVal(finfo.termBlockIndexLoc);
        fieldOutput.writeVal(finfo.termsLoc);  // TODO: If we change termBlockOffsets to be relative to the start of that index, we can remove termsLoc
        fieldOutput.writeVal(finfo.docsLoc);
        fieldOutput.writeVal(finfo.posLoc);
        fieldOutput.writeVlong(finfo.nTerms);
        fieldOutput.writeVlong(finfo.sumDocFreq - finfo.nTerms);            // sumDocFreq >= nTerms
        fieldOutput.writeVlong(finfo.sumTotalTermFreq - finfo.sumDocFreq);  // sumTotalTermFreq >= sumDocFreq
        fieldOutput.writeVal(finfo.trieLoc);
        fieldOutput.writeVlong(finfo.trieRootOff);
        if ((finfo.flags & FieldType::TERM_RANGES) != 0) {
          fieldOutput.writeVal(finfo.rangeTableLoc);
        }
      }

      // Things that have an int col: text fields (for norms), int col, float col, double col, string col (for ords)
      // Things that would not have an int col in the future - index only non-text fields, or text fields w/o norms,
      // or stored-only fields in a column family.  For now, just assume there is always a column.
      // if ((finfo.flags & 0x02) != 0) {
      fieldOutput.writeVal(finfo.docsWithFieldEndLoc);
      fieldOutput.writeVal(finfo.columnLoc);
      fieldOutput.writeVlong(finfo.columnMetaOff);
      fieldOutput.writeVlong(finfo.numValues);
      fieldOutput.writeVint((uint32_t)finfo.ordFormat);
      fieldOutput.writeVint((uint32_t)finfo.ordIndexing);
      fieldOutput.writeVint((uint32_t)finfo.ordBits);
      fieldOutput.writeVal(finfo.pointsLoc);
      fieldOutput.writeVlong(finfo.pointsMetaOff);
      fieldOutput.writeVint((uint32_t)finfo.normsFormat);
      fieldOutput.writeVal(finfo.normsLoc);
      fieldOutput.writeVlong(finfo.normsLen);

      // for now, always write mono col info.  If we want to make it optional, we need a flag for it.
      fieldOutput.writeVal(finfo.monoLoc);
      fieldOutput.writeVlong(finfo.monoMetaOff);
      fieldOutput.writeVal(finfo.mono2Loc);
      fieldOutput.writeVlong(finfo.mono2MetaOff);
      fieldOutput.writeVal(finfo.valDocLoc);
      fieldOutput.writeVlong(finfo.valDocMetaOff);
    }

    // Now write the start of each fieldInfo
    // Now make field offsets relative to the start of the locations array instead of the beginning of fields.
    // It's minor, but allows us to remove another pointer (to the start of the fields)
    fieldOutput.align(4);
    auto locationsOff = fieldOutput.size() - fieldsStart;
    for (auto& loc : fieldOffs) {
      loc = locationsOff - loc;
    }

    if (fieldOffs.size() > 0) {
      fieldOutput.write(&(fieldOffs[0]), fieldOffs.size() * sizeof(fieldOffs[0]));
    }
    // write the size of the array at the end so we can use it to find the start when reading
    fieldOutput.writeInt((int32_t)fieldOffs.size());
  }

};


using OutputStreamPtr = PostingsWriter::OutputStreamPtr;  // for convenience


class TextNormsView {
  std::span<const uint8_t> ordinalNorms;
  const screaming::BitSet* docsBitset = nullptr;
  mutable std::optional<screaming::BitSet::Iterator> docsIter;
  mutable int32_t lastDoc = -1;

public:
  TextNormsView() = default;

  TextNormsView(std::span<const uint8_t> ordinalNorms, const screaming::BitSet* docsBitset)
      : ordinalNorms(ordinalNorms), docsBitset(docsBitset) {
  }

  bool empty() const {
    return ordinalNorms.empty();
  }

  uint32_t normForDoc(int32_t docid) const {
    if (ordinalNorms.empty()) {
      return 0;
    }
    if (docsBitset == nullptr) {
      assert(docid >= 0 && (size_t)docid < ordinalNorms.size());
      return (uint32_t) ordinalNorms[(size_t)docid];
    }

    if (!docsIter.has_value() || docid <= lastDoc) {
      docsIter.emplace(*docsBitset);
    }
    [[maybe_unused]] int32_t found = docsIter->advance(docid);
    assert(found == docid);
    lastDoc = docid;
    int32_t ord = docsIter->rank();
    assert(ord >= 0 && (size_t)ord < ordinalNorms.size());
    return (uint32_t) ordinalNorms[(size_t)ord];
  }
};


// TODO: we need a specialization of this for when positions are not required (indexed string fields)
class TextWriter {
public:
  static constexpr int32_t L1_PERIOD = 32;
  static constexpr int32_t kCheckpointStride = 8;
  static inline int32_t checkpointStrideForTests = kCheckpointStride;

  struct RangeResult {
    std::vector<uint64_t> termBlockOffsets;
    std::vector<std::string> separatorKeys;
    std::string firstTerm;
    std::string lastTerm;
    int64_t sumDocFreq = 0;
    int64_t sumTotalTermFreq = 0;
    uint64_t docsBytes = 0;
    uint64_t posBytes = 0;
    int64_t nTerms = 0;
    int32_t nBlocks = 0;
  };

  static std::string separatorKey(std::string_view previous, std::string_view first) {
    uint32_t minLen = (uint32_t) std::min(previous.size(), first.size());
    uint32_t mismatch = 0;
    while (mismatch < minLen
           && (uint8_t) previous[mismatch] == (uint8_t) first[mismatch]) {
      mismatch++;
    }
    assert(mismatch < first.size());
    if (mismatch < previous.size()) {
      assert((uint8_t) previous[mismatch] < (uint8_t) first[mismatch]);
    }
    return std::string(first.substr(0, mismatch + 1));
  }

private:
  PostingsWriter& postingsWriter;
  PostingsWriter::IndexFieldInfo* fieldInfo;

  std::array<OutputStreamPtr, 3> streams; // holders for the streams we are using.
  OutputStream& termOutput;    // output stream for termFile
  OutputStream& docOutput;     // output stream for docFile
  OutputStream& posOutput;     // output stream for posFile


  //
  // variable naming:
  // *loc* refers to absolute locations in a file, usually obtained via OutputStream::size()
  // *off* / *offset* refers to offsets relative to something else (i.e. a location)
  //

  // needed to build each block
  std::vector<TermRef> termList;  // list of terms in the current term block, pointing into termBytes
  // Block-local term storage: startTerm() copies each term here because flushTerms()
  // re-reads the block's terms for prefix encoding, so callers need not keep term
  // bytes alive past the startTerm() call.  Slot i backs termList[i].
  std::array<char, Postings::TERMS_BLOCK_SIZE * PackedTerm::MAX_BYTES> termBytes;
  std::vector<uint64_t> termDocsEnd;  // cumulative trailer-free docs-region end offsets
  std::vector<uint32_t> termPackedBlocks;
  std::vector<uint32_t> termDocFreqs;
  std::vector<uint64_t> termTtfCodes;
  std::vector<uint64_t> termPosOffsets;
  std::vector<uint32_t> pulsed; // pulsed doc/pos values for terms with a single occurrence
  uint32_t termPulsedMask = 0;

  // needed for each term
  std::vector<int32_t> docs; // list of documents containing a term
  std::vector<int32_t> tfreqs; // term freqs - number of times the term appears in each document (parallel vector to "docs")
  std::vector<int32_t> posdeltas; // list of position deltas for the current term (for all documents... per-document positions are not delimited)

  // base (starting) values int the associated output streams to calculate offsets from
  int64_t termsLoc=0;
  int64_t docsLoc;
  int64_t posLoc;

  int64_t locOfPositionsForTermBlock;
  int64_t locOfDocsForTermBlock;
  int64_t locOfPositionsForTerm;
  int64_t locOfDocsForTerm;

  // Absolute posOutput location of the position block that holds the current
  // doc block's first position.  Captured when a doc block starts (startDoc
  // with an empty docs buffer): at that moment posOutput holds exactly the
  // fully flushed position blocks, so its size is the in-progress block's
  // start - the block the next position lands in.  Consumed by appendL0Block.
  int64_t pendingBlockPosByteOff = 0;

  int32_t docsFlushed;  /// number of documents flushed for the current term so far
  uint32_t termPackedBlockCount = 0;
  int32_t curTf = 0;    /// occurrences (term freq) seen so far for the current doc
  int64_t ttfAcc = 0;   /// total term freq (sum of tf over docs) accumulated for the current term
  uint32_t prevDocBlockLast = 0;  /// last doc id of the previous flushed doc block (cross-block delta base); reset per term
  uint32_t prevL1GroupLast = 0;   /// last doc id of the previous flushed L1 group; reset per term
  uint32_t l1GroupLastDoc = 0;    /// last doc id in the buffered L1 group
  int32_t l1GroupBlockCount = 0;
  uint32_t l1GroupPackedBlockCount = 0;
  int32_t l1GroupDocCount = 0;
  uint64_t l1GroupTfSum = 0;
  uint32_t l1GroupMaxTf = 0;
  uint32_t l1GroupMinNorm = 0;
  std::array<uint32_t, L1_PERIOD> l1BlockLastDocs{};
  std::array<uint64_t, L1_PERIOD> l1BlockBodyOffsets{};
  // Group-level (norm -> maxTf) surface for the buffered L1 group, merged from
  // each flushed block's frontier points.  The group header stores its Pareto
  // frontier (same staircase as L0 and the term dictionary) so group bounds
  // come from real (norm, tf) pairs, not a cross-doc (maxTf, minNorm) corner.
  std::array<uint32_t, 256> l1GroupMaxTfPerNorm{};

  // Index level for this field, decoded from the field flags in startField().  These gate
  // whether the freq stream and position stream are written at all.
  bool hasFreqs = false;
  bool hasPositions = false;
  bool hasNorms = false;
  TextNormsView norms;

  std::vector<uint64_t> termBlockOffsets;  // offset from termsOffset (for this field) for each term block
  TrieBuilder trieBuilder;
  std::vector<std::string> separatorKeys;
  std::string firstTermInRun;
  bool buildTrie = true;
  std::array<char, PackedTerm::MAX_BYTES> lastTermOfPrevBlock{};
  uint32_t lastTermOfPrevBlockLen = 0;
  bool hasLastTermOfPrevBlock = false;
  int64_t sumTotalTermFreq = 0; // updated in endTerm
  int64_t sumDocFreq = 0; // updated in endTerm
  int64_t numTerms; // currently only updated in flushTerms

  // TODO: pool allocate this
  std::vector<char> compressed_output;
  std::vector<char> header_output;
  std::vector<char> group_output;
  std::array<uint32_t, 256> maxTfPerNorm;
  std::vector<uint32_t> frontierNorms;
  std::vector<uint32_t> frontierTfs;
  // Term-level (maxTf, norm) surface accumulated across the whole term, so the
  // term dictionary can store the term's impact frontier: a scorer's global
  // max score must come from stored data, never a walk of the postings.
  std::array<uint32_t, 256> termMaxTfPerNorm;
  std::vector<char> termImpactRun;  // per-term encoded frontiers for the current term block
  static inline bool disableL0CheckpointWrite =
      std::getenv("LUXIR_DISABLE_L0_CHECKPOINT_WRITE") != nullptr;

private:  // some internal utility methods... not for use by indexers
  void anchorPositionedDocBlock() {
    if (hasPositions && docs.empty()) {
      pendingBlockPosByteOff = posOutput.size();
    }
  }

  void appendPositionDeltasRaw(std::span<const int32_t> deltas) {
    while (!deltas.empty()) {
      size_t count = std::min(
          deltas.size(), Postings::POSITIONS_BLOCK_SIZE - posdeltas.size());
      posdeltas.insert(posdeltas.end(), deltas.begin(), deltas.begin() + count);
      deltas = deltas.subspan(count);
      if (posdeltas.size() == Postings::POSITIONS_BLOCK_SIZE) {
        flushPositions();
      }
    }
  }

  static void appendVint(std::vector<char>& out, uint32_t val) {
    while (val > 0x7f) {
      out.push_back((char) (val | 0x80));
      val >>= 7;
    }
    out.push_back((char) val);
  }

  static void appendVlong(std::vector<char>& out, uint64_t val) {
    while (val > 0x7f) {
      out.push_back((char) (val | 0x80));
      val >>= 7;
    }
    out.push_back((char) val);
  }

  static void appendShortLE(std::vector<char>& out, uint16_t val) {
    out.push_back((char) val);
    out.push_back((char) (val >> 8));
  }

  static void appendIntLE(std::vector<char>& out, uint32_t val) {
    out.push_back((char) val);
    out.push_back((char) (val >> 8));
    out.push_back((char) (val >> 16));
    out.push_back((char) (val >> 24));
  }

  static void appendVint15(std::vector<char>& out, uint32_t val) {
    if ((val & ~0x7fffu) == 0) {
      appendShortLE(out, (uint16_t) val);
    } else {
      appendShortLE(out, (uint16_t) (0x8000u | (val & 0x7fffu)));
      appendVint(out, val >> 15);
    }
  }

  static void appendVlong15(std::vector<char>& out, uint64_t val) {
    if ((val & ~0x7fffull) == 0) {
      appendShortLE(out, (uint16_t) val);
    } else {
      appendShortLE(out, (uint16_t) (0x8000u | (val & 0x7fffull)));
      appendVlong(out, val >> 15);
    }
  }

  static uint32_t commonPrefixLen(const char* a, uint32_t alen, const char* b, uint32_t blen) {
    uint32_t len = std::min(alen, blen);
    uint32_t prefixLen = 0;
    while (prefixLen < len && a[prefixLen] == b[prefixLen]) {
      prefixLen++;
    }
    return prefixLen;
  }

  uint32_t normForDoc(int32_t docid) const {
    return norms.normForDoc(docid);
  }

  uint32_t buildImpactFrontier(uint32_t docCount) {
    frontierNorms.resize(0);
    frontierTfs.resize(0);
    if (!hasNorms) {
      return 0;
    }

    uint32_t minNorm = 255;
    if (!hasFreqs) {
      for (uint32_t i = 0; i < docCount; i++) {
        minNorm = std::min(minNorm, normForDoc(docs[i]));
      }
      return minNorm;
    }

    maxTfPerNorm.fill(0);
    for (uint32_t i = 0; i < docCount; i++) {
      uint32_t norm = normForDoc(docs[i]);
      assert(norm <= 255);
      uint32_t tf = (uint32_t) tfreqs[i];
      minNorm = std::min(minNorm, norm);
      maxTfPerNorm[norm] = std::max(maxTfPerNorm[norm], tf);
      termMaxTfPerNorm[norm] = std::max(termMaxTfPerNorm[norm], tf);
    }

    uint32_t runningMaxTf = 0;
    for (uint32_t norm = 0; norm < maxTfPerNorm.size(); norm++) {
      uint32_t tf = maxTfPerNorm[norm];
      if (tf > runningMaxTf) {
        frontierNorms.push_back(norm);
        frontierTfs.push_back(tf);
        runningMaxTf = tf;
      }
    }
    return minNorm;
  }

  // Encode the Pareto frontier of a (norm -> maxTf) surface as
  // [count vint][(norm u8, tf-delta vint)*], the same staircase the L0
  // headers use.  count == 0 only for fields without freqs+norms.
  static void appendImpactFrontier(std::vector<char>& out,
                                   const std::array<uint32_t, 256>& surface) {
    uint32_t count = 0;
    uint32_t runningMaxTf = 0;
    for (uint32_t norm = 0; norm < surface.size(); norm++) {
      if (surface[norm] > runningMaxTf) {
        count++;
        runningMaxTf = surface[norm];
      }
    }
    appendVint(out, count);
    runningMaxTf = 0;
    for (uint32_t norm = 0; norm < surface.size(); norm++) {
      uint32_t tf = surface[norm];
      if (tf > runningMaxTf) {
        out.push_back((char) (uint8_t) norm);
        appendVint(out, tf - runningMaxTf);
        runningMaxTf = tf;
      }
    }
  }

  static void appendL1ImpactFrontier(std::vector<char>& out,
                                     const std::array<uint32_t, 256>& surface,
                                     uint32_t maxTf) {
    uint32_t count = 0;
    uint32_t runningMaxTf = 0;
    for (uint32_t norm = 0; norm < surface.size(); norm++) {
      uint32_t tf = surface[norm];
      if (tf > runningMaxTf) {
        count++;
        runningMaxTf = tf;
      }
    }
    assert(count > 0);
    bool useU32 = maxTf > 65535u;
    appendVint(out, (count << 1) | (useU32 ? 1u : 0u));

    runningMaxTf = 0;
    for (uint32_t norm = 0; norm < surface.size(); norm++) {
      uint32_t tf = surface[norm];
      if (tf > runningMaxTf) {
        assert(norm <= 255);
        out.push_back((char) (uint8_t) norm);
        runningMaxTf = tf;
      }
    }

    runningMaxTf = 0;
    for (uint32_t norm = 0; norm < surface.size(); norm++) {
      uint32_t tf = surface[norm];
      if (tf > runningMaxTf) {
        if (useU32) {
          appendIntLE(out, tf);
        } else {
          assert(tf <= 65535u);
          appendShortLE(out, (uint16_t) tf);
        }
        runningMaxTf = tf;
      }
    }
  }

  void appendL0Header(std::vector<char>& out, uint32_t lastDoc, uint32_t base,
                      uint64_t blockByteLen, uint32_t docCount, uint64_t tfSum,
                      uint32_t maxTf, uint32_t minNorm, uint64_t posByteOff) {
    header_output.resize(0);
    assert(lastDoc >= base);
    appendVint15(header_output, lastDoc - base);
    appendVlong15(header_output, blockByteLen);
    if (hasPositions) {
      assert(tfSum >= docCount);
      appendVint(header_output, (uint32_t) (tfSum - docCount));
      // Byte offset (from the term's position start) of the position block
      // holding this doc block's first position.  The ord within that block is
      // not stored: it is cumTf-before-block % POSITIONS_BLOCK_SIZE, which the
      // reader already tracks via the tfSum chain.
      appendVlong(header_output, posByteOff);
    }
    if (hasFreqs && hasNorms) {
      assert(!frontierTfs.empty());
      assert(frontierTfs.size() == frontierNorms.size());
      appendVint(header_output, (uint32_t) frontierTfs.size());
      uint32_t prevTf = 0;
      for (size_t i = 0; i < frontierTfs.size(); i++) {
        uint32_t norm = frontierNorms[i];
        uint32_t tf = frontierTfs[i];
        assert(norm <= 255);
        assert(i == 0 || norm > frontierNorms[i - 1]);
        assert(tf > prevTf);
        // norm is a SmallFloat byte (0-255): store it raw, not vint-encoded.  tf is
        // unbounded, so keep its delta (the staircase is tf-ascending) vint-encoded.
        header_output.push_back((char) (uint8_t) norm);
        appendVint(header_output, tf - prevTf);
        prevTf = tf;
      }
    } else if (hasFreqs) {
      assert(maxTf > 0);
      appendVint(header_output, maxTf);
    } else if (hasNorms) {
      assert(minNorm <= 255);
      appendVint(header_output, minNorm);
    }
    appendVint(out, (uint32_t) header_output.size());
    appendBytes(out, header_output.data(), header_output.size());
  }

  void appendBytes(std::vector<char>& out, const void* data, size_t len) {
    const char* src = (const char*) data;
    out.insert(out.end(), src, src + len);
  }

  void appendVlongRun(std::vector<char>& out, const std::vector<uint64_t>& values) {
    for (uint64_t val : values) {
      appendVlong(out, val);
    }
  }

  void appendVlongDeltaRun(std::vector<char>& out, const std::vector<uint64_t>& values) {
    uint64_t prev = 0;
    for (uint64_t val : values) {
      assert(val >= prev);
      appendVlong(out, val - prev);
      prev = val;
    }
  }

  void appendSVBRun(std::vector<char>& out, const std::vector<uint32_t>& values) {
    if (values.empty()) {
      return;
    }
    uint32_t count = (uint32_t) values.size();
    uint32_t keyBytes = svbKeyBytes(count);
    std::vector<uint8_t> keys(keyBytes);
    std::vector<uint8_t> data((size_t) count * sizeof(uint32_t));
    uint8_t* dataEnd = svb_encode_scalar(values.data(), keys.data(), data.data(), count);
    appendBytes(out, keys.data(), keys.size());
    appendBytes(out, data.data(), (size_t) (dataEnd - data.data()));
  }

  void writeMetadataRunLen(size_t byteLen) {
    assert(byteLen <= (size_t) (UINT32_MAX >> 1));
    termOutput.writeVint(((uint32_t) byteLen) << 1u);
  }

  static uint32_t termMaskForCount(int nTerms) {
    assert(nTerms > 0 && nTerms <= Postings::TERMS_BLOCK_SIZE);
    return nTerms == 32 ? UINT32_MAX : ((1u << (uint32_t) nTerms) - 1u);
  }

  static bool allUInt64Equal(const std::vector<uint64_t>& values, uint64_t target) {
    for (uint64_t value : values) {
      if (value != target) {
        return false;
      }
    }
    return true;
  }

  static bool allUInt32Equal(const std::vector<uint32_t>& values, uint32_t target) {
    for (uint32_t value : values) {
      if (value != target) {
        return false;
      }
    }
    return true;
  }

  void addTrieSeparatorForCurrentBlock(uint32_t blockOrd) {
    if (blockOrd == 0) {
      return;
    }

    // Block 0 uses the trie's implicit empty separator.  For later blocks, the
    // separator is the first term truncated one byte past its first mismatch
    // with the previous block's last term.  That key is strictly greater than
    // every term in the previous block and <= the first term in this block, so
    // floorBlock routes between-block gaps to the later block.
    auto [firstData, firstLen] = termList[0].unpack();
    assert(hasLastTermOfPrevBlock);
    std::string key = separatorKey(
        std::string_view(lastTermOfPrevBlock.data(), lastTermOfPrevBlockLen),
        std::string_view(firstData, firstLen));
    if (buildTrie) {
      trieBuilder.add(key, blockOrd);
    } else {
      separatorKeys.push_back(std::move(key));
    }
  }

  void rememberLastTermOfCurrentBlock() {
    auto [lastData, lastLen] = termList.back().unpack();
    assert(lastLen <= PackedTerm::MAX_LEN);
    memcpy(lastTermOfPrevBlock.data(), lastData, lastLen);
    lastTermOfPrevBlockLen = lastLen;
    hasLastTermOfPrevBlock = true;
  }

  void appendL0Block(uint32_t lastDoc, uint32_t base, uint64_t blockByteLen,
                     uint32_t docCount, uint64_t tfSum, uint32_t maxTf, uint32_t minNorm) {
    assert(l1GroupBlockCount >= 0 && l1GroupBlockCount < L1_PERIOD);
    l1BlockLastDocs[(size_t) l1GroupBlockCount] = lastDoc;
    l1BlockBodyOffsets[(size_t) l1GroupBlockCount] =
        (uint64_t) group_output.size();
    // Position anchor for this doc block, captured at its first startDoc.
    assert(pendingBlockPosByteOff >= locOfPositionsForTerm);
    uint64_t posByteOff = hasPositions
        ? (uint64_t) (pendingBlockPosByteOff - locOfPositionsForTerm) : 0;
    appendL0Header(group_output, lastDoc, base, blockByteLen, docCount, tfSum, maxTf, minNorm,
                   posByteOff);
    appendBytes(group_output, compressed_output.data(), blockByteLen);
    l1GroupLastDoc = lastDoc;
    l1GroupBlockCount++;
    l1GroupDocCount += (int32_t) docCount;
    if (hasPositions) {
      l1GroupTfSum += tfSum;
    }
    if (hasFreqs) {
      l1GroupMaxTf = std::max(l1GroupMaxTf, maxTf);
    }
    if (hasNorms) {
      assert(minNorm <= 255);
      l1GroupMinNorm = l1GroupBlockCount == 1 ? minNorm : std::min(l1GroupMinNorm, minNorm);
    }
    if (hasFreqs && hasNorms) {
      // Merge this block's frontier (still live from buildImpactFrontier) into
      // the group surface; dominated points fall out when the group frontier is
      // re-extracted at flush.
      assert(frontierTfs.size() == frontierNorms.size());
      for (size_t i = 0; i < frontierNorms.size(); i++) {
        uint32_t norm = frontierNorms[i];
        l1GroupMaxTfPerNorm[norm] = std::max(l1GroupMaxTfPerNorm[norm], frontierTfs[i]);
      }
    }
    if (l1GroupBlockCount == L1_PERIOD) {
      flushL1Group();
    }
  }

  void flushL1Group() {
    if (l1GroupBlockCount == 0) {
      return;
    }
    header_output.resize(0);
    assert(l1GroupLastDoc >= prevL1GroupLast);
    appendVint15(header_output, l1GroupLastDoc - prevL1GroupLast);
    appendVlong15(header_output, group_output.size());
    assert(l1GroupPackedBlockCount <= (uint32_t) L1_PERIOD);
    header_output.push_back((char) l1GroupPackedBlockCount);
    assert(checkpointStrideForTests > 0);
    uint8_t entryCount = disableL0CheckpointWrite
        ? 0
        : (uint8_t) ((l1GroupBlockCount - 1) / checkpointStrideForTests);
    header_output.push_back((char) entryCount);
    if (!disableL0CheckpointWrite) {
      [[maybe_unused]] uint32_t previousKey = 0;
      for (int32_t b = checkpointStrideForTests; b < l1GroupBlockCount;
           b += checkpointStrideForTests) {
        uint32_t key = l1BlockLastDocs[(size_t) b - 1];
        uint64_t bodyOffset = l1BlockBodyOffsets[(size_t) b];
        assert(b == checkpointStrideForTests || key > previousKey);
        assert(bodyOffset < (1u << 24));
        assert(b > 0 && b < L1_PERIOD);
        uint64_t entry = ((uint64_t) key << 32)
            | ((uint64_t) (uint32_t) b << 24) | bodyOffset;
        static_assert(std::endian::native == std::endian::little);
        size_t offset = header_output.size();
        header_output.resize(offset + sizeof(entry));
        memcpy(header_output.data() + offset, &entry, sizeof(entry));
        previousKey = key;
      }
    }
    if (hasPositions) {
      assert(l1GroupTfSum >= (uint64_t) l1GroupDocCount);
      appendVint(header_output, (uint32_t) (l1GroupTfSum - (uint64_t) l1GroupDocCount));
    }
    if (hasFreqs && hasNorms) {
      assert(l1GroupMaxTf > 0);
      appendL1ImpactFrontier(header_output, l1GroupMaxTfPerNorm, l1GroupMaxTf);
    } else if (hasFreqs) {
      assert(l1GroupMaxTf > 0);
      appendVint(header_output, l1GroupMaxTf);
    } else if (hasNorms) {
      assert(l1GroupMinNorm <= 255);
      appendVint(header_output, l1GroupMinNorm);
    }
    docOutput.writeVint((uint32_t) header_output.size());
    docOutput.write(header_output.data(), header_output.size());
    docOutput.write(group_output.data(), group_output.size());

    prevL1GroupLast = l1GroupLastDoc;
    group_output.resize(0);
    l1GroupBlockCount = 0;
    l1GroupPackedBlockCount = 0;
    l1GroupDocCount = 0;
    l1GroupTfSum = 0;
    l1GroupMaxTf = 0;
    l1GroupMinNorm = 0;
    l1BlockLastDocs.fill(0);
    l1BlockBodyOffsets.fill(0);
    l1GroupMaxTfPerNorm.fill(0);
  }

  // number of docs for the current term
  int32_t getDocFreq() const {
    return docsFlushed + docs.size();
  }

  // total term freq (sum of tf over all docs) for the current term.  Equals the
  // total position count for fields that index positions.
  int64_t getTotalTermFreq() const {
    return ttfAcc;
  }

  // the size the docfile takes
  int32_t getDocFileSize() const {
    auto sz = docOutput.size() - locOfDocsForTerm;
    assert(sz <= UINT_MAX);
    return sz;
  }

public:
  TextWriter(PostingsWriter& postingsWriter)
  : postingsWriter(postingsWriter), streams(postingsWriter.getOutputStreams<3>()), termOutput(*streams[0]), docOutput(*streams[1]), posOutput(*streams[2])
  {
  }

  TextWriter(PostingsWriter& postingsWriter, OutputStream& termOutput,
             OutputStream& docOutput, OutputStream& posOutput)
  : postingsWriter(postingsWriter), termOutput(termOutput), docOutput(docOutput),
    posOutput(posOutput), buildTrie(false) {
  }

  // TODO: FIXME: this is for tests, but it doesn't set the flags / type properly!
  void startField(const std::string& fieldName) {
    PostingsWriter::IndexFieldInfo* finfo = &postingsWriter.addField(fieldName);
    finfo->type = FieldType::TEXT;
    finfo->flags = FieldType::INDEX_DOCS_FREQS_POSITIONS;
    startField(finfo);
  }

  void startField(PostingsWriter::IndexFieldInfo* finfo) {
    if (termsLoc == -1) {
      throw std::runtime_error("startField called twice!");
    }
    fieldInfo = finfo;
    hasFreqs = FieldType::hasFreqs(finfo->flags);
    hasPositions = FieldType::hasPositions(finfo->flags);
    hasNorms = hasPositions;
    termsLoc = termOutput.size();
    docsLoc = docOutput.size();
    posLoc = posOutput.size();
    numTerms = 0;
    // nocommit fieldInfo->flags |= 0x01;  // text field

    termBlockOffsets.resize(0);
    trieBuilder.reset();
    separatorKeys.clear();
    firstTermInRun.clear();
    lastTermOfPrevBlockLen = 0;
    hasLastTermOfPrevBlock = false;

    _startTermBlock(false);
  }

  // Currently only called for a full block of positions.
  // TODO: move to .cpp unless we template this class... allows for removal of the associated include files
  void flushPositions() {
    if (posdeltas.empty()) {
      return;
    }
    compressed_output.resize(Postings::POSITIONS_BLOCK_SIZE * sizeof(int32_t) + 1024);
    uint32_t compressedSize = compressed_output.size(); // this gets changed to the actual size
    IndexCodec::posCodec.encodeBlock(reinterpret_cast<uint32_t *>(posdeltas.data()), posdeltas.size(), compressed_output.data(),
                                  compressedSize);
    posOutput.write(compressed_output.data(), compressedSize);

    posdeltas.resize(0);
  }

  // TODO: move to .cpp unless we template this class
  void flushDocs() {
    assert(docs.size() == tfreqs.size());

    if (docs.empty()) {
      return;
    }

    assert(docs.back() < postingsWriter.getMaxDoc()); // sanity check to ensure we didn't go over provided numDocs
    assert(docs.size() == Postings::DOCS_BLOCK_SIZE);

    // NOTE: LuxirPFORd (docCodec) applies the adjacent delta itself, in place,
    // so we pass the raw (monotonic) doc ids. encodeBlock handles exactly one
    // DOCS_BLOCK_SIZE block.  The block's first id is coded as a delta from the
    // previous block's last id (cross-block base); capture this block's last id
    // first, since encodeBlock deltas `docs` in place.
    const uint32_t base = prevDocBlockLast;
    const uint32_t lastDoc = (uint32_t) docs.back();
    uint64_t tfSum = 0;
    uint32_t maxTf = 0;
    if (hasFreqs) {
      for (auto tf : tfreqs) {
        if (hasPositions) {
          tfSum += (uint32_t) tf;
        }
        maxTf = std::max(maxTf, (uint32_t) tf);
      }
    }
    uint32_t minNorm = buildImpactFrontier(Postings::DOCS_BLOCK_SIZE);

    // Doc-part encoding decision (token byte + body; see Postings::DOC_BLOCK_*
    // and the postings reader). Dense blocks store doc ids as a bitset over (base, lastDoc]
    // instead of PFor deltas, and a fully consecutive block stores nothing at
    // all. Storage rule as in Lucene: the bitset wins whenever it costs no more
    // than the next-larger packed width, biasing ties toward the bitset (its
    // word-OR count fill and cheaper decode are worth it).
    // docBase = the doc id bit 0 of the bitset form maps to. The term's first
    // block has no previous doc and docid 0 is legal, so its span starts AT the
    // (zero) base; later blocks start one past the previous block's last id.
    // A flushed block's last doc is >= DOCS_BLOCK_SIZE - 1 > 0, so base == 0
    // identifies the first block (mirrors the reader's docOrd == 0 test).
    const uint32_t docBase = base + (base == 0 ? 0 : 1);
    const uint32_t spanBits = lastDoc - docBase + 1;
    assert(spanBits >= (uint32_t) Postings::DOCS_BLOCK_SIZE);
    uint32_t deltaOr = (uint32_t) docs[0] - base;
    for (size_t i = 1; i < docs.size(); i++) {
      deltaOr |= (uint32_t) (docs[i] - docs[i - 1]);
    }
    const uint32_t bitsPerValue = 32 - (uint32_t) std::countl_zero(deltaOr | 1);
    const uint32_t numWords = (spanBits + 63) / 64;

    compressed_output.resize(2 * (Postings::DOCS_BLOCK_SIZE * sizeof(int32_t) + 1024));
    uint32_t blockByteLen;
    if (spanBits == (uint32_t) Postings::DOCS_BLOCK_SIZE) {
      compressed_output[0] = Postings::DOC_BLOCK_CONTIGUOUS;
      blockByteLen = 1;
    } else if (std::min(32u, bitsPerValue + 1) * (uint32_t) Postings::DOCS_BLOCK_SIZE <= numWords * 64) {
      compressed_output[0] = Postings::DOC_BLOCK_PACKED;
      termPackedBlockCount++;
      l1GroupPackedBlockCount++;
      uint32_t compressedSize = (uint32_t) compressed_output.size() - 1; // this gets changed to the actual size
      IndexCodec::docCodec.encodeBlock(reinterpret_cast<uint32_t *>(docs.data()), docs.size(), compressed_output.data() + 1,
                                    compressedSize, base);
      blockByteLen = 1 + compressedSize;
    } else {
      assert(numWords <= 63);
      compressed_output[0] = (char) (int8_t) -(int32_t) numWords;
      uint64_t words[64];
      memset(words, 0, numWords * 8);
      for (auto d : docs) {
        const uint32_t s = (uint32_t) d - docBase;
        words[s >> 6] |= 1ULL << (s & 63);
      }
      memcpy(compressed_output.data() + 1, words, numWords * 8);
      blockByteLen = 1 + numWords * 8;
    }

    //
    // now the term freqs (omitted entirely for DOCS-only fields)
    //
    if (hasFreqs) {
      uint32_t compressedSize = (uint32_t) compressed_output.size() - blockByteLen; // this gets changed to the actual size
      IndexCodec::tfreqCodec.encodeBlock(reinterpret_cast<uint32_t *>(tfreqs.data()), tfreqs.size(), compressed_output.data() + blockByteLen,
                                      compressedSize);
      blockByteLen += compressedSize;
    }

    appendL0Block(lastDoc, base, blockByteLen, Postings::DOCS_BLOCK_SIZE, tfSum, maxTf, minNorm);
    prevDocBlockLast = lastDoc;

    docsFlushed += docs.size();
    docs.resize(0);
    tfreqs.resize(0);
  }


  // Have a different set of methods depending on the field type?
  // For example things could be much simpler if not indexing positions.


  // called starting a new field, or after flushing a term block.
  void _startTermBlock(bool endingField) {
    unused(endingField);
    termList.resize(0);
    termDocsEnd.resize(0);
    termPackedBlocks.resize(0);
    termDocFreqs.resize(0);
    termTtfCodes.resize(0);
    termPosOffsets.resize(0);
    termImpactRun.resize(0);
    pulsed.resize(0);
    termPulsedMask = 0;
    locOfPositionsForTermBlock = posOutput.size();
    locOfDocsForTermBlock = docOutput.size();

    termBlockOffsets.push_back( termOutput.size() - termsLoc);
  }

  // Term block on-disk format.  This writer is the authority; see
  // TermsEnum::readTermBlock/readNextTermInBlock for the matching reader.
  //
  // A field's terms stream contains a sequence of fixed-size term blocks
  // (except the last block).  Each block is laid out as:
  //
  //   header:
  //     [firstTerm: PackedTerm]
  //       The first term is stored whole.  Terms 1..n-1 are front-coded below.
  //     [docsLocDelta: vlong]
  //       Offset of this block's docs region from the field docsLoc.
  //     [posLocDelta: vlong]
  //       Offset of this block's positions region from the field posLoc.
  //     [blockPrefixLen: u8]
  //       lcp(first term, last term).  These bytes are factored out of every
  //       in-block suffix and restored from the previous materialized term.
  //     [pulsedMask: u32 little-endian]
  //       Bit i is set when term i is pulsed, meaning its single doc, and
  //       optional position, are stored in the pulsed metadata run instead of
  //       occupying bytes in the docs stream.
  //     [suffixBytesTotal: vint]
  //       Total bytes in the dense suffix blob.  This lets the reader find the
  //       metadata section on block entry without summing suffix lengths.
  //
  //   term scan section:
  //     [hashes: n x u8]
  //       One low byte of XXH3 per term, used to reject most seekExact misses
  //       before materializing term bytes.
  //     [prefixLen: (n-1) x raw u8]
  //     [suffixLen: (n-1) x raw u8]
  //       For term i > 0, prefixLen is measured against the previous term
  //       after removing blockPrefixLen from both terms.  suffixLen is the
  //       remaining byte count.  The first term is the header firstTerm.
  //     [suffix blob: suffixBytesTotal bytes]
  //       Dense concatenation of suffix bytes for terms 1..n-1.
  //
  //   metadata section:
  //     Each sub-run begins with a vint code: (byteLen << 1) | encodingFlag.
  //     The normal v1 encoding writes encodingFlag 0 and a nonzero byteLen.
  //     Code 0 (byteLen 0, flag 0) is claimed as an all-default run marker;
  //     otherwise the low flag bit remains reserved for a future alternate
  //     encoding.  The byte-length vints are written first for every run,
  //     followed by the non-default run bytes in the same order:
  //
  //     [docsEnd lengths/code][packedBlocks lengths/code][df lengths/code]
  //     [ttfCode lengths/code?]
  //     [posOff lengths/code?][termImpact lengths/code?][pulsed lengths/code]
  //     [docsEnd: cumulative vlong run]
  //       Trailer-free slice ends within this block's docs region.  For term i,
  //       docsSize_i = docsEnd_i - docsEnd_(i-1), with docsEnd_(-1) = 0.
  //       Pulsed terms repeat the previous end, so their docsSize is 0.
  //       Default: every slice is zero-size, which is legal only when every
  //       term in the block is pulsed.
  //     [packedBlocks: StreamVByte run]
  //       Number of full doc blocks encoded with DOC_BLOCK_PACKED for every
  //       term in the block.  Pulsed terms and StreamVByte tail blocks are 0.
  //       Default: every value is 0.
  //     [df: StreamVByte run]
  //       docFreq for every term in the block.
  //       Default: every value is 1.
  //     [ttfCode: vlong run, only when the field indexes freqs]
  //       totalTermFreq - docFreq for every term.
  //       Default: every value is 0.
  //     [posOff: delta vlong run, only when the field indexes positions]
  //       Position-stream start offsets within this block's positions region.
  //       Pulsed terms repeat the running value; readers ignore the offset for
  //       pulsed terms.
  //       Default: every delta is 0, so every offset is 0.
  //     [termImpact: frontier run, only when the field indexes freqs+norms]
  //       Per term, the whole-term (norm, maxTf) Pareto frontier as
  //       [count vint][(norm u8, tf-delta vint)*] - the stored source for a
  //       scorer's global max score.  Never default-coded.
  //     [pulsed: StreamVByte run]
  //       Values for pulsed terms only, in term order.  Each pulsed term stores
  //       doc, and also pos when positions are indexed.  The value index for
  //       term i is popcount(pulsedMask below bit i), scaled by the per-term
  //       value count.
  //
  // The layout is column-stride on purpose.  Term scans, seekCeil, fuzzy
  // enumeration, and most miss paths need only hashes, lengths, and suffix
  // bytes.  Term stats decode as tier 1 (df/ttfCode), and postings-open
  // metadata decodes as tier 2 (docsEnd/packedBlocks/posOff/pulsed), both
  // lazily in TermsEnum.  Per-doc tf values are not stored here: they are
  // scoring data block-encoded alongside doc ids in the docs stream and
  // remain part of the postings payload.
  void flushTerms(bool endingField) {
    if (termList.empty()) {
      termBlockOffsets.pop_back();  // last block has no terms in it.
      return;
    }

    uint32_t blockOrd = (uint32_t)termBlockOffsets.size() - 1;
    addTrieSeparatorForCurrentBlock(blockOrd);

    numTerms += termList.size();

    int nTerms = termList.size();
    assert((int)termDocsEnd.size() == nTerms);
    assert((int)termPackedBlocks.size() == nTerms);
    assert((int)termDocFreqs.size() == nTerms);
    assert((int)termTtfCodes.size() == nTerms);
    assert(!hasPositions || (int)termPosOffsets.size() == nTerms);

    TermRef reference = termList[0];
    auto [firstData, firstLen] = reference.unpack();
    auto [lastData, lastLen] = termList.back().unpack();
    // numTerms was just bumped by this block, so equality means this is the
    // run's first non-empty block: capture the run's first term for stitching.
    if (numTerms == nTerms) {
      firstTermInRun.assign(firstData, firstLen);
    }
    uint32_t blockPrefixLen = commonPrefixLen(firstData, firstLen, lastData, lastLen);
    assert(blockPrefixLen <= UINT8_MAX);

    std::vector<char> prefixLens;
    std::vector<char> suffixLens;
    std::vector<char> suffixBlob;
    prefixLens.reserve((size_t) std::max(0, nTerms - 1));
    suffixLens.reserve((size_t) std::max(0, nTerms - 1));
    suffixBlob.reserve((size_t) nTerms * 8);

    auto [refdata, reflen] = reference.unpack();
    for (int i=1; i<nTerms; i++) {
      auto term = termList[i];
      auto [tdata, tlen] = term.unpack();
      assert(blockPrefixLen <= reflen);
      assert(blockPrefixLen <= tlen);
      assert(memcmp(firstData, tdata, blockPrefixLen) == 0);

      const char* refSuffix = refdata + blockPrefixLen;
      const char* termSuffix = tdata + blockPrefixLen;
      uint32_t refSuffixLen = reflen - blockPrefixLen;
      uint32_t termSuffixLen = tlen - blockPrefixLen;
      uint32_t prefixLen = commonPrefixLen(refSuffix, refSuffixLen, termSuffix, termSuffixLen);
      uint32_t suffixLen = termSuffixLen - prefixLen;
      assert(suffixLen > 0);
      assert(prefixLen <= UINT8_MAX);
      assert(suffixLen <= UINT8_MAX);
      assert(prefixLen == refSuffixLen || refSuffix[prefixLen] != termSuffix[prefixLen]);

      prefixLens.push_back((char) prefixLen);
      suffixLens.push_back((char) suffixLen);
      appendBytes(suffixBlob, termSuffix + prefixLen, suffixLen);

      refdata = tdata;
      reflen = tlen;
    }

    // Header: first term, postings block offsets, block-prefix length,
    // pulsed mask, and total suffix blob bytes.  The total lets the reader
    // locate metadata lazily without summing suffixLens on miss-only visits.
    termOutput.writePackedTerm(reference);
    termOutput.writeVlong(locOfDocsForTermBlock - docsLoc);
    termOutput.writeVlong(locOfPositionsForTermBlock - posLoc);
    termOutput.write((char) blockPrefixLen);
    termOutput.write(&termPulsedMask, sizeof(termPulsedMask));
    termOutput.writeVint((uint32_t) suffixBlob.size());

    // now write hashes of the terms
    for (int i=0; i<nTerms; i++) {
      auto term = termList[i];
      termOutput.write((char)XXH3_64bits(term.data(), term.size()));
    }

    termOutput.write(prefixLens.data(), prefixLens.size());
    termOutput.write(suffixLens.data(), suffixLens.size());
    termOutput.write(suffixBlob.data(), suffixBlob.size());

    std::vector<char> docsEndRun;
    std::vector<char> packedBlocksRun;
    std::vector<char> dfRun;
    std::vector<char> ttfRun;
    std::vector<char> posOffRun;
    std::vector<char> pulsedRun;
    appendVlongRun(docsEndRun, termDocsEnd);
    appendSVBRun(packedBlocksRun, termPackedBlocks);
    appendSVBRun(dfRun, termDocFreqs);
    if (hasFreqs) {
      appendVlongRun(ttfRun, termTtfCodes);
    }
    if (hasPositions) {
      appendVlongDeltaRun(posOffRun, termPosOffsets);
    }
    appendSVBRun(pulsedRun, pulsed);
    bool hasTermImpacts = hasFreqs && hasNorms;

    bool docsEndDefault = allUInt64Equal(termDocsEnd, 0);
    if (docsEndDefault) {
      assert(termPulsedMask == termMaskForCount(nTerms));
    }
    bool packedBlocksDefault = allUInt32Equal(termPackedBlocks, 0);
    bool dfDefault = allUInt32Equal(termDocFreqs, 1);
    bool ttfDefault = allUInt64Equal(termTtfCodes, 0);
    bool posOffDefault = hasPositions && allUInt64Equal(termPosOffsets, 0);

    writeMetadataRunLen(docsEndDefault ? 0 : docsEndRun.size());
    writeMetadataRunLen(packedBlocksDefault ? 0 : packedBlocksRun.size());
    writeMetadataRunLen(dfDefault ? 0 : dfRun.size());
    if (hasFreqs) {
      writeMetadataRunLen(ttfDefault ? 0 : ttfRun.size());
    }
    if (hasPositions) {
      writeMetadataRunLen(posOffDefault ? 0 : posOffRun.size());
    }
    if (hasTermImpacts) {
      writeMetadataRunLen(termImpactRun.size());
    }
    writeMetadataRunLen(pulsedRun.size());

    if (!docsEndDefault) {
      termOutput.write(docsEndRun.data(), docsEndRun.size());
    }
    if (!packedBlocksDefault) {
      termOutput.write(packedBlocksRun.data(), packedBlocksRun.size());
    }
    if (!dfDefault) {
      termOutput.write(dfRun.data(), dfRun.size());
    }
    if (hasFreqs && !ttfDefault) {
      termOutput.write(ttfRun.data(), ttfRun.size());
    }
    if (hasPositions && !posOffDefault) {
      termOutput.write(posOffRun.data(), posOffRun.size());
    }
    if (hasTermImpacts) {
      termOutput.write(termImpactRun.data(), termImpactRun.size());
    }
    termOutput.write(pulsedRun.data(), pulsedRun.size());

    [[maybe_unused]] uint32_t pulsedTerms = std::popcount(termPulsedMask);
    assert(pulsed.size() == (size_t)pulsedTerms * (hasPositions ? 2u : 1u));
    rememberLastTermOfCurrentBlock();

    if (!endingField) {
      _startTermBlock(endingField);
    }
  }


  /// The term is copied into block-local storage and only needs to be valid for the
  /// duration of this call.
  // returns 1-based ordinal of term in this field
  int64_t startTerm(TermRef term) {
    docsFlushed = 0;
    termPackedBlockCount = 0;
    ttfAcc = 0;
    prevDocBlockLast = 0;  // each term's first doc block starts from base 0
    prevL1GroupLast = 0;
    l1GroupLastDoc = 0;
    l1GroupBlockCount = 0;
    l1GroupPackedBlockCount = 0;
    l1GroupDocCount = 0;
    l1GroupTfSum = 0;
    l1GroupMaxTf = 0;
    l1GroupMinNorm = 0;
    l1BlockLastDocs.fill(0);
    l1BlockBodyOffsets.fill(0);
    l1GroupMaxTfPerNorm.fill(0);
    group_output.resize(0);
    termMaxTfPerNorm.fill(0);
    locOfPositionsForTerm = posOutput.size();
    pendingBlockPosByteOff = locOfPositionsForTerm;
    locOfDocsForTerm = docOutput.size();
    PackedTerm stored(termBytes.data() + termList.size() * PackedTerm::MAX_BYTES);
    term.copyTo(stored);
    termList.push_back(stored);
    return numTerms + (int64_t)termList.size();
  }

  void endTerm(TermRef term) {
    unused(term);
    auto docfreq = getDocFreq();
    // For fields that don't index freqs, ttf is *defined* as docfreq (each doc counts
    // once); the real occurrence count isn't stored and the reader reports docfreq, so
    // term-level and field-level (sumTotalTermFreq) stats must use the same definition.
    auto totalTermFreq = hasFreqs ? ttfAcc : (int64_t) docfreq;
    sumTotalTermFreq += totalTermFreq;
    if (docs.size() > 0) {
      assert(docs.back() < postingsWriter.getMaxDoc()); // sanity check to ensure we didn't go over provided numDocs
    }
    if (totalTermFreq == 0) {
      // term with no documents, remove term we just added
      termList.pop_back();
      return;
    }
    if (totalTermFreq == 1) {
      assert(getDocFileSize()==0 && docs.size()==1 && tfreqs.size()==1);
      assert(posdeltas.size() == (hasPositions ? 1u : 0u));
      int32_t blockTermOrd = (int32_t) termList.size() - 1;
      assert(blockTermOrd >= 0 && blockTermOrd < Postings::TERMS_BLOCK_SIZE);
      termPulsedMask |= 1u << (uint32_t) blockTermOrd;
      sumDocFreq += 1;
      if (hasFreqs && hasNorms) {
        // single-doc term: exact one-point frontier from its norm
        termMaxTfPerNorm.fill(0);
        uint32_t norm = normForDoc(docs[0]);
        assert(norm <= 255);
        termMaxTfPerNorm[norm] = 1;
        appendImpactFrontier(termImpactRun, termMaxTfPerNorm);
      }
      termPackedBlocks.push_back(0);
      termDocFreqs.push_back(1);
      termTtfCodes.push_back(0);
      termDocsEnd.push_back((uint64_t) (docOutput.size() - locOfDocsForTermBlock));
      if (hasPositions) {
        termPosOffsets.push_back((uint64_t) (posOutput.size() - locOfPositionsForTermBlock));
      }
      // the doc (+ position, if indexed) will be remembered to be included directly in the term dictionary (i.e. pulsing)
      pulsed.push_back((uint32_t) docs[0]);
      if (hasPositions) {
        pulsed.push_back((uint32_t) posdeltas.back());
        posdeltas.pop_back();
      }

      docsFlushed += docs.size();
      docs.resize(0);
      tfreqs.resize(0);

    } else {
      // Finish positions that were not block encoded (only when positions are indexed)
      // TODO: possibly block encode positions across terms?  For that we would want ttf encoded in the terms dict (or could also store sum of all prev in docs file)
      //       Downside to this is that it could hurt block compression to mix terms (small + big deltas mixed)
      if (hasPositions) {
        for (auto posDelta : posdeltas) {
          // TODO: try group varint
          posOutput.writeVint(posDelta);
        }
      }
      posdeltas.resize(0);

      // Finish docs (and, for fields that index them, term freqs) that were not block encoded.
      // StreamVByte tail layout (see DocsEnumImpl::nextDoc):
      //   [docKeys][docData] followed, for fields with freqs, by [tfreqKeys][tfreqData].
      // Docs are d1 encoded; freqs are plain StreamVByte values.
      // TODO: when ttf==docfreq (all tf==1) the freq stream is constant 1 and can be dropped for
      //   scoring fields too (plan A); needs the freq blocks deferred/separated since flushDocs is eager.
      assert(docs.size() == tfreqs.size());
      const uint32_t n = (uint32_t) docs.size();
      if (n > 0) {
        const uint32_t kb = svbKeyBytes(n);
        uint8_t dkeys[Postings::DOCS_BLOCK_SIZE / 4 + 1];
        uint8_t ddata[Postings::DOCS_BLOCK_SIZE * 4];
        // d1 base: continue the cross-block delta from the last full block (0 when
        // the term is a single partial block), matching the docs codec convention.
        const uint32_t base = prevDocBlockLast;
        const uint32_t lastDoc = (uint32_t) docs.back();
        uint64_t tfSum = 0;
        uint32_t maxTf = 0;
        if (hasFreqs) {
          for (auto tf : tfreqs) {
            if (hasPositions) {
              tfSum += (uint32_t) tf;
            }
            maxTf = std::max(maxTf, (uint32_t) tf);
          }
        }
        uint32_t minNorm = buildImpactFrontier(n);
        uint8_t* ddEnd = svb_encode_scalar_d1_init((const uint32_t*) docs.data(), dkeys, ddata, n, base);
        compressed_output.resize(0);
        appendBytes(compressed_output, dkeys, kb);
        appendBytes(compressed_output, ddata, (size_t)(ddEnd - ddata));
        if (hasFreqs) {
          uint8_t tkeys[Postings::DOCS_BLOCK_SIZE / 4 + 1];
          uint8_t tdata[Postings::DOCS_BLOCK_SIZE * 4];
          uint8_t* tdEnd = svb_encode_scalar((const uint32_t*) tfreqs.data(), tkeys, tdata, n);
          appendBytes(compressed_output, tkeys, kb);
          appendBytes(compressed_output, tdata, (size_t)(tdEnd - tdata));
        }
        appendL0Block(lastDoc, base, compressed_output.size(), n, tfSum, maxTf, minNorm);
        prevDocBlockLast = lastDoc;
      }
      docsFlushed += docs.size();
      docs.resize(0);
      tfreqs.resize(0);
      flushL1Group();


      sumDocFreq += docfreq;  // docfreq computed at the top of endTerm

      uint64_t ttfCode = 0;
      if (hasFreqs) {
        ttfCode = (uint64_t) (totalTermFreq - docfreq);
      }
      uint64_t posOffset = 0;
      if (hasPositions) {
        // offset from start of positions in term dict block
        posOffset = (uint64_t) (locOfPositionsForTerm - locOfPositionsForTermBlock);
      }

      termDocFreqs.push_back((uint32_t) docfreq);
      termTtfCodes.push_back(ttfCode);
      if (hasPositions) {
        termPosOffsets.push_back(posOffset);
      }
      if (hasFreqs && hasNorms) {
        appendImpactFrontier(termImpactRun, termMaxTfPerNorm);
      }
      termPackedBlocks.push_back(termPackedBlockCount);
      termDocsEnd.push_back((uint64_t) (docOutput.size() - locOfDocsForTermBlock));
    }

    if (termList.size() == Postings::TERMS_BLOCK_SIZE) {
      flushTerms(false);
    }
  }

  RangeResult finishTermRun() {
    flushTerms(true);

    RangeResult result;
    result.termBlockOffsets = std::move(termBlockOffsets);
    result.separatorKeys = std::move(separatorKeys);
    result.firstTerm = std::move(firstTermInRun);
    if (numTerms != 0) {
      result.lastTerm.assign(lastTermOfPrevBlock.data(), lastTermOfPrevBlockLen);
    }
    result.sumDocFreq = sumDocFreq;
    result.sumTotalTermFreq = sumTotalTermFreq;
    result.docsBytes = (uint64_t) (docOutput.size() - docsLoc);
    result.posBytes = (uint64_t) (posOutput.size() - posLoc);
    result.nTerms = numTerms;
    result.nBlocks = (int32_t) result.termBlockOffsets.size();
    assert((result.nTerms == 0) == (result.nBlocks == 0));
    return result;
  }

  void finalizeField(RangeResult&& result) {
    assert(buildTrie);

    // write index into the blocks of the terms dict
    // TODO: use a more efficient encoding for this array
    //   - make offsets be from the start of this index array... 32 bit normally fine, but not always for huge field?
    //   - sequence will be monotonically increasing (or decreasing)... interpolate?
    // Indexing RAM OPT: for fields with huge number of terms, we could stream this to separate file.  That would also facilitate alignment if it's important.
    fieldInfo->nTerms = result.nTerms;
    fieldInfo->sumDocFreq = result.sumDocFreq;
    fieldInfo->sumTotalTermFreq = result.sumTotalTermFreq;
    fieldInfo->flags &= ~FieldType::TERM_RANGES;
    fieldInfo->rangeTableLoc = {0, 0};

    if (result.nTerms == 0) {
      assert(result.termBlockOffsets.empty());
      fieldInfo->termBlockIndexLoc = {0, 0};
      fieldInfo->termsLoc = {0, 0};
      fieldInfo->docsLoc = {0, 0};
      fieldInfo->posLoc = {0, 0};
      fieldInfo->trieLoc = {0, 0};
      fieldInfo->trieRootOff = 0;
      return;
    }

    // The reader addresses the block-offset table as a uint64_t array straight
    // out of the mapping, so it has to start 8-aligned.
    termOutput.align(MAX_ALIGN);
    fieldInfo->termBlockIndexLoc = seg_location(termOutput.streamNumber, termOutput.size());
    assert((int)result.termBlockOffsets.size()
           == ((result.nTerms - 1) / Postings::TERMS_BLOCK_SIZE) + 1);
    // Append the trie after the fixed block-offset array.  trieRootOff is
    // relative to trieLoc, while child links inside the trie are backward
    // deltas within this appended byte region.
    fieldInfo->trieRootOff = (int64_t)trieBuilder.finish();
    const std::vector<char>& trieBytes = trieBuilder.bytes();
    // Dict metadata SVB runs are embedded in the terms stream.  The last run may
    // be read with the AVX decoder's tail overread, so the block-offset table
    // plus trie bytes that follow the final block must provide the same slack as
    // pure postings files.
    assert(result.termBlockOffsets.size() * sizeof(result.termBlockOffsets[0])
           + trieBytes.size() >= SVB_OVERREAD_PAD);
    termOutput.write(result.termBlockOffsets.data(),
                     result.termBlockOffsets.size() * sizeof(result.termBlockOffsets[0]));
    fieldInfo->trieLoc = seg_location(termOutput.streamNumber, termOutput.size());
    termOutput.write(trieBytes.data(), trieBytes.size());

    fieldInfo->termsLoc = seg_location(termOutput.streamNumber, termsLoc);
    fieldInfo->docsLoc = seg_location(docOutput.streamNumber, docsLoc);
    fieldInfo->posLoc = seg_location(posOutput.streamNumber, posLoc);
  }

  // This should only be called once. Multiple fields are not handled any longer.
  void endField() {
    finalizeField(finishTermRun());
  }


  void startDoc(int32_t doc) {
    unused(doc);
    curTf = 0;

    // This doc starts a new doc block: anchor the block to the position stream
    // before any of the doc's positions are added.  (All positioned paths -
    // Inverter's pushDocs and the merger's addDocsPos - route through here.)
    anchorPositionedDocBlock();
  }

  // Record a doc with a known term freq and no positions.  Positionless fields (string/id,
  // freq-only, and the positionless merge path) call this directly, since they already know
  // the freq and don't need the per-occurrence startDoc/addPositionDelta/endDoc dance.
  // endDoc() routes the position-streaming path here with its accumulated occurrence count.
  // tf is stored only when the field indexes freqs; for DOCS-only fields any tf>0 just
  // records the doc (its ttf is defined as docfreq).
  void addDoc(int32_t docid, int32_t tf) {
    if (tf == 0) {
      // TODO: revisit if this can happen... it depends on where deleted docs are checked.
      return;
    }
    docs.push_back(docid);
    tfreqs.push_back(tf);  // kept parallel to docs; only written when hasFreqs
    ttfAcc += tf;
    if (docs.size() == Postings::DOCS_BLOCK_SIZE) {
      flushDocs();
    }
  }

  void endDoc(int32_t doc) {
    addDoc(doc, curTf);
  }

  void endDoc(int32_t doc, int32_t expectedTf) {
    assert(curTf == expectedTf);
    endDoc(doc);
  }

  // Register one occurrence of the term in the current doc that carries a position.
  // The position is only stored when the field indexes positions; either way it
  // contributes to the term freq.
  void addPositionDelta(int32_t posDelta) {
    curTf++;
    if (hasPositions) {
      posdeltas.push_back(posDelta);
      if (posdeltas.size() == Postings::POSITIONS_BLOCK_SIZE) {
        flushPositions();
      }
    }
  }

  void appendPositionDeltas(std::span<const int32_t> deltas) {
    assert(hasPositions);
    curTf += (int32_t) deltas.size();
    appendPositionDeltasRaw(deltas);
  }

  // Copy positioned docs in source order while retaining ownership of output
  // doc-block anchors.  DeltaPull accepts a maximum count and may return
  // multiple spans before satisfying each output chunk's tf sum.
  template <typename DeltaPull>
  void addDocsWithPositions(std::span<const int32_t> docids,
                            std::span<const int32_t> tfValues,
                            DeltaPull&& deltaPull) {
    assert(hasPositions);
    assert(docids.size() == tfValues.size());
    size_t offset = 0;
    while (offset < docids.size()) {
      int32_t room = Postings::DOCS_BLOCK_SIZE - (int32_t) docs.size();
      int32_t count = std::min(room, (int32_t) (docids.size() - offset));
      assert(count > 0);

      anchorPositionedDocBlock();
      int64_t expectedDeltas = 0;
      for (int32_t i = 0; i < count; i++) {
        int32_t tf = tfValues[offset + (size_t) i];
        assert(tf > 0);
        expectedDeltas += (uint32_t) tf;
      }

      int64_t appendedDeltas = 0;
      while (appendedDeltas < expectedDeltas) {
        auto deltas = deltaPull(expectedDeltas - appendedDeltas);
        assert(!deltas.empty());
        assert((int64_t) deltas.size() <= expectedDeltas - appendedDeltas);
        appendPositionDeltasRaw(deltas);
        appendedDeltas += (int64_t) deltas.size();
      }
      assert(appendedDeltas == expectedDeltas);

      auto docChunk = docids.subspan(offset, (size_t) count);
      auto tfChunk = tfValues.subspan(offset, (size_t) count);
      docs.insert(docs.end(), docChunk.begin(), docChunk.end());
      tfreqs.insert(tfreqs.end(), tfChunk.begin(), tfChunk.end());
      ttfAcc += expectedDeltas;
      assert(docs.size() == tfreqs.size());
      assert(docs.size() <= Postings::DOCS_BLOCK_SIZE);
      if (docs.size() == Postings::DOCS_BLOCK_SIZE) {
        flushDocs();
      }
      offset += (size_t) count;
    }
  }

  void setNorms(TextNormsView norms) {
    this->norms = norms;
  }

};


class DocsWriter {
  friend class DocsWithValWriter;
  ScreamingBuilder builder;
public:

  /// This writer currently *always* writes at least 2 bytes (the number of buckets) in the screaming bitset.
  /// Decisions should be made at a higher level to not use this for 0 or all-bits-set scenarios.
  /// This writer allocates from "pool" but does not rewind.  It is safe to release after finish() is called.
  DocsWriter(MemPool& pool, OutputStream& output) : builder(pool, output) {
  }

  // TODO: optionally use a different encoding for few numbers of docs or low maxdoc... ScreamingBitset is
  // really only competitive when maxdoc is high.  That could be decided at finish(), or could even
  // be built into the ScreamingBitset format as well...  The limit could also be a function of maxdoc, so
  // we could avoid using for small indexes / segments altogether.  DocsReader could encapsulate a bitset or
  // a list of decoded docs (and we could populate that from different formats such as the lowest-bit-is-tfreq)
  // used in termdoc postings.

  // For direct column building we want something that waits for the first gap before starting to write.
  // It's also a shame to use up an OutputStream if what we would have to buffer in memory is small.

  // Docs should be added in order!
  void addDoc(int32_t docid) {
    builder.add(docid);
  }

  // Same as addDoc... it's named startDoc target for DocStream.pushDocs.
  void startDoc(int32_t docid) {
    builder.add(docid);
  }

  int32_t finish() {
    builder.flush();
    return builder.cardinality();
  }
};


class DocsWithValWriter {
  OutputStreamPtr idOutput;
  DocsWriter docsWriter;
  PostingsWriter& postingsWriter;
  // int64_t startLoc;
  PostingsWriter::IndexFieldInfo& fieldInfo; // don't have to store if we pass it to finish

public:
  // TODO: fixme... this obtains an outputStream and hence should not be used if the field is dense.
  DocsWithValWriter(MemPool& pool, PostingsWriter& postingsWriter, PostingsWriter::IndexFieldInfo& fieldInfo)
  : idOutput(postingsWriter.getOutputStream()), docsWriter(pool,*idOutput), postingsWriter(postingsWriter), fieldInfo(fieldInfo)
  {
    unused(this->postingsWriter);
    // startLoc = idOutput.size();
  }

  // Same as addDoc... it's named startDoc target for DocStream.pushDocs.
  void startDoc(int32_t docid) {
    docsWriter.startDoc(docid);
  }

  int32_t numAdded() const{
    return docsWriter.builder.cardinality();
  }

  void finish() {
    fieldInfo.docsWithField = docsWriter.finish();
    fieldInfo.docsWithFieldEndLoc = idOutput->slocation();
  }

  // Signal that the column has all docs present. No docs should be added in this case, but the count
  // in fieldInfo should be filled in.
  void finishDense(int32_t numDocs) {
    fieldInfo.docsWithField = numDocs;
    // this is a valid location, so use numDocs and see if it matches numDocs of segment to tell if there is data to read
    fieldInfo.docsWithFieldEndLoc = {0,0};
  }
};



} // end namespace
