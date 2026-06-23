#pragma once

#include <cstdint>
#include <assert.h>
#include <iostream>
#include <sstream>
#include <unordered_map>
#include <vector>
#include "solux/util/MemPool.h"
#include "solux/util/StrRef.h"
#include "solux/store/OutputStream.h"
#include "solux/store/Directory.h"
#include "solux/codec/StreamVByte.h"
#include "solux/reader/PostingsReader.h"
#include "solux/reader/FieldReader.h"
#include "ScreamingBuilder.h"
#include "solux/codec/Codec.h"
#include "solux/schema/FieldType.h"
#include "solux/util/screaming.h"
#include "LiveDocsWriter.h"


namespace solux {

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
  // IndexFieldInfo adds extra info needed at index time to SegFieldInfo.
  struct IndexFieldInfo : public SegFieldInfo {
  };

private:
  Directory& directory;
  int32_t maxDoc;  // set by caller
  int64_t sizeInBytes = 0; // total size of all files written

  // files available for use, sorted so largest are at the back.  This is done to keep the smallest files small
  // so they can be just appended to the end of a large file (when we implement that functionallity.)
  std::vector<OutputStream*> freeFiles;
  std::string segStr;

  struct DataFile {
    OutputStream out;
    std::unique_ptr<File> file;
    uint32_t fileNum;
  };
  std::deque<DataFile> files;
  // These are dequeues so elements don't move
  // TODO: put these in the pool?
  std::deque<IndexFieldInfo> fieldInfos;
public:
  MemPool pool;  // perhaps migrate to googles Arena if segment writing becomes multi-threaded.
  uint64_t segId;

public:
  PostingsWriter(Directory& dir, uint64_t segId, int32_t maxDoc=-1) : directory(dir), maxDoc(maxDoc), segId(segId)
  {
    segStr = Postings::getSortableString(segId);
  }

  Directory& getDirectory() {
    return directory;
  }

  // not thread-safe
  IndexFieldInfo& addField(PackedTerm fieldName) {
    fieldInfos.emplace_back(); // we should get default-initialization with this for the SegFieldInfo members
    fieldInfos.back().fieldname = fieldName;
    assert(fieldInfos.back().monoLoc.offset() == 0 && fieldInfos.back().monoMetaOff == 0 && fieldInfos.back().columnMetaOff == 0);
    assert(fieldInfos.back().mono2Loc.offset() == 0 && fieldInfos.back().mono2MetaOff == 0);
    assert(fieldInfos.back().valDocLoc.offset() == 0 && fieldInfos.back().valDocMetaOff == 0);
    return fieldInfos.back();
  }

  // not thread-safe
  // copies the fieldName into the pool associated with this PostingsWriter.
  IndexFieldInfo& addField(std::string_view fieldName) {
    // making the PackedTerm in the pool also not thread safe
    return addField(PackedTerm(pool, fieldName));
  }


  // make sure that numFiles can be obtained, and if not create more.
  void reserveFiles(size_t numFiles) {
    while (freeFiles.size() < numFiles) {
      uint32_t fnum = (uint32_t)files.size();
      // TODO: in the future, if this does IO, we may not want to lock?
      std::unique_ptr<File> file = directory.createFile(Postings::getIndexFileName(segStr, fnum));
      files.emplace_back(DataFile{OutputStream{},std::move(file), fnum});
      files.back().out.setFile( files.back().file.get());
      files.back().out.streamNumber = fnum;
      // writing something at the start of the file acts as a sanity check, and also makes file locations of 0
      // invalid (and thus distinguishable from default-initialized).
      // TODO: think about embedding other info such as the segment id and file number?
      files.back().out.writeBytes(Postings::SOLUX_HEADER);
      // insert at front of free list to maintain sorted order.
      freeFiles.insert(freeFiles.begin(), &files.back().out);
    }
  }

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

  // TODO: make these thread safe before making flushing or merging multi-threaded.
  OutputStreamPtr getOutputStream() {
    return std::move(getOutputStreams<1>()[0]);
  }

  // TODO: make these thread safe before making flushing or merging multi-threaded.
  template <std::size_t N>
  std::array<OutputStreamPtr, N> getOutputStreams() {
    reserveFiles(N);
    std::array<OutputStreamPtr, N> os;
    for (size_t i = 0; i < N; ++i) {
      os[i] = OutputStreamPtr(freeFiles.back(), OutputStreamDeleter(this));
      freeFiles.pop_back();
    }
    return os;
  }

  // TODO: make these thread safe before making flushing or merging multi-threaded.
  void releaseOutputStream(OutputStream* os) {
    auto compareBySize = [](const OutputStream* a, const OutputStream* b) {
      return a->size() < b->size();
    };
    auto it = std::upper_bound(freeFiles.begin(), freeFiles.end(), os, compareBySize);
    freeFiles.insert(it, os);
  }

  // TODO: make these thread safe before making flushing or merging multi-threaded.
  void releaseOutputStreams(std::span<OutputStreamPtr> streams) {
    for (auto& streamPtr : streams) {
      releaseOutputStream(streamPtr.release());
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
    writeFieldIndex();
    writeSegmentInfo();
    // TODO: implement compound files for small files

    // The StreamVByte AVX tail decoder used by DocsEnum may read up to
    // SVB_OVERREAD_PAD bytes past the encoded data. Reserve that slack at the end of
    // every pure-data file so the read stays in bounds. File 0 ends with the field
    // index and segment info just written above, which already provide far more
    // trailing slack, so it is skipped. Padding past its segment-info size marker
    // would also break the end-relative read in PostingsReader.
    if (files.size() > 1) {
      static const char svbPad[SVB_OVERREAD_PAD] = {};
      for (size_t i = 1; i < files.size(); i++) {
        files[i].out.write(svbPad, SVB_OVERREAD_PAD);
      }
    }

    if (filenames) {
      filenames->reserve(filenames->size() + files.size());
    }
    for (auto& dataFile : files) {
      sizeInBytes += dataFile.out.size();
      dataFile.out.close();
      directory.finishFile(*dataFile.file);
      if (filenames) {
        filenames->emplace_back(dataFile.file->name());
      }
    }

    fieldInfos.resize(0);
    files.resize(0);
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
  void writeSegmentInfo() {
    // TODO: if maxDoc==0, does this cause issues elsewhere?
    // assert(maxDoc >= 1);
    OutputStream& out = files[0].out;
    auto outStart = out.size();
    out.writeVint(maxDoc);
    out.writeVint(files.size());
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
        fieldOutput.writeVint(finfo.nTerms);
        fieldOutput.writeVlong(finfo.sumDocFreq - finfo.nTerms);            // sumDocFreq >= nTerms
        fieldOutput.writeVlong(finfo.sumTotalTermFreq - finfo.sumDocFreq);  // sumTotalTermFreq >= sumDocFreq
      }

      // Things that have an int col: text fields (for norms), int col, float col, double col, string col (for ords)
      // Things that would not have an int col in the future - index only non-text fields, or text fields w/o norms,
      // or stored-only fields in a column family.  For now, just assume there is always a column.
      // if ((finfo.flags & 0x02) != 0) {
      fieldOutput.writeVal(finfo.docsWithFieldEndLoc);
      fieldOutput.writeVal(finfo.columnLoc);
      fieldOutput.writeVlong(finfo.columnMetaOff);
      fieldOutput.writeVlong(finfo.numValues);

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


// TODO: we need a specialization of this for when positions are not required (indexed string fields)
class TextWriter {
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
  std::vector<TermRef> termList;  // list of terms in the current term block
  std::vector<uint32_t> docFileSize;         // size of the data in the docs file for this term (TODO: can we guarantee that this isn't bigger than 2B or 4B?)
  std::vector<int32_t> pulsed; // if docFileSize==0, then the term has a single doc/pos that is pulsed, and those values are the next in this list.

  // needed for each term
  std::vector<int32_t> docs; // list of documents containing a term
  std::vector<int32_t> tfreqs; // term freqs - number of times the term appears in each document (parallel vector to "docs")
  std::vector<int32_t> posdeltas; // list of position deltas for the current term (for all documents... per-document positions are not delimited)

  static constexpr int32_t L1_PERIOD = 32;

  // base (starting) values int the associated output streams to calculate offsets from
  int64_t termsLoc=0;
  int64_t docsLoc;
  int64_t posLoc;

  int64_t locOfPositionsForTermBlock;
  int64_t locOfDocsForTermBlock;
  int64_t locOfPositionsForTerm;
  int64_t locOfDocsForTerm;

  int32_t docsFlushed;  /// number of documents flushed for the current term so far
  int32_t curTf = 0;    /// occurrences (term freq) seen so far for the current doc
  int64_t ttfAcc = 0;   /// total term freq (sum of tf over docs) accumulated for the current term
  uint32_t prevDocBlockLast = 0;  /// last doc id of the previous flushed doc block (cross-block delta base); reset per term
  uint32_t prevL1GroupLast = 0;   /// last doc id of the previous flushed L1 group; reset per term
  uint32_t l1GroupLastDoc = 0;    /// last doc id in the buffered L1 group
  int32_t l1GroupBlockCount = 0;
  int32_t l1GroupDocCount = 0;
  uint64_t l1GroupTfSum = 0;
  uint32_t l1GroupMaxTf = 0;

  // Index level for this field, decoded from the field flags in startField().  These gate
  // whether the freq stream and position stream are written at all.
  bool hasFreqs = false;
  bool hasPositions = false;

  std::vector<uint64_t> termBlockOffsets;  // offset from termsOffset (for this field) for each term block
  int64_t sumTotalTermFreq = 0; // updated in endTerm
  int64_t sumDocFreq = 0; // updated in endTerm
  int32_t numTerms; // currently only updated in flushTerms

  // TODO: pool allocate this
  std::vector<char> compressed_output;
  std::vector<char> header_output;
  std::vector<char> group_output;

private:  // some internal utility methods... not for use by indexers
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

  void appendL0Header(std::vector<char>& out, uint32_t lastDoc, uint32_t base,
                      uint64_t blockByteLen, uint32_t docCount, uint64_t tfSum,
                      uint32_t maxTf) {
    header_output.resize(0);
    assert(lastDoc >= base);
    appendVint15(header_output, lastDoc - base);
    appendVlong15(header_output, blockByteLen);
    if (hasPositions) {
      assert(tfSum >= docCount);
      appendVint(header_output, (uint32_t) (tfSum - docCount));
    }
    if (hasFreqs) {
      assert(maxTf > 0);
      appendVint(header_output, maxTf);
    }
    appendVint(out, (uint32_t) header_output.size());
    appendBytes(out, header_output.data(), header_output.size());
  }

  void appendBytes(std::vector<char>& out, const void* data, size_t len) {
    const char* src = (const char*) data;
    out.insert(out.end(), src, src + len);
  }

  void appendL0Block(uint32_t lastDoc, uint32_t base, uint64_t blockByteLen,
                     uint32_t docCount, uint64_t tfSum, uint32_t maxTf) {
    appendL0Header(group_output, lastDoc, base, blockByteLen, docCount, tfSum, maxTf);
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
    if (hasPositions) {
      assert(l1GroupTfSum >= (uint64_t) l1GroupDocCount);
      appendVint(header_output, (uint32_t) (l1GroupTfSum - (uint64_t) l1GroupDocCount));
    }
    if (hasFreqs) {
      assert(l1GroupMaxTf > 0);
      appendVint(header_output, l1GroupMaxTf);
    }
    docOutput.writeVint((uint32_t) header_output.size());
    docOutput.write(header_output.data(), header_output.size());
    docOutput.write(group_output.data(), group_output.size());

    prevL1GroupLast = l1GroupLastDoc;
    group_output.resize(0);
    l1GroupBlockCount = 0;
    l1GroupDocCount = 0;
    l1GroupTfSum = 0;
    l1GroupMaxTf = 0;
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
    termsLoc = termOutput.size();
    docsLoc = docOutput.size();
    posLoc = posOutput.size();
    numTerms = 0;
    // nocommit fieldInfo->flags |= 0x01;  // text field

    termBlockOffsets.resize(0);

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

    // NOTE: SoluxPFORd (docCodec) applies the adjacent delta itself, in place,
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

    compressed_output.resize(2 * (Postings::DOCS_BLOCK_SIZE * sizeof(int32_t) + 1024));
    uint32_t compressedSize = (uint32_t) compressed_output.size(); // this gets changed to the actual size
    IndexCodec::docCodec.encodeBlock(reinterpret_cast<uint32_t *>(docs.data()), docs.size(), compressed_output.data(),
                                  compressedSize, base);
    uint32_t blockByteLen = compressedSize;

    //
    // now the term freqs (omitted entirely for DOCS-only fields)
    //
    if (hasFreqs) {
      compressedSize = (uint32_t) compressed_output.size() - blockByteLen; // this gets changed to the actual size
      IndexCodec::tfreqCodec.encodeBlock(reinterpret_cast<uint32_t *>(tfreqs.data()), tfreqs.size(), compressed_output.data() + blockByteLen,
                                      compressedSize);
      blockByteLen += compressedSize;
    }

    appendL0Block(lastDoc, base, blockByteLen, Postings::DOCS_BLOCK_SIZE, tfSum, maxTf);
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
    docFileSize.resize(0);
    pulsed.resize(0);
    locOfPositionsForTermBlock = posOutput.size();
    locOfDocsForTermBlock = docOutput.size();

    termBlockOffsets.push_back( termOutput.size() - termsLoc);
  }

  void flushTerms(bool endingField) {
    if (termList.empty()) {
      termBlockOffsets.pop_back();  // last block has no terms in it.
      return;
    }

    numTerms += termList.size();

    // TODO: find common prefix (i.e. min_prefix_len) for all terms in block and strip it off (same as common prefix of first and last)
    // important for some things that share long prefixes, like URLs for example.
    // TODO: store min term size (or minimum suffix length) and then code the suffix lengths as additional to that? Would help indexing things like text uuids.
    // Store max term size to help optimize readers?

    TermRef reference = termList[0];
    auto [refdata, reflen] = reference.unpack();

    // Write the terms block header.
    termOutput.writeStr(refdata, reflen);
    termOutput.writeVlong(locOfDocsForTermBlock - docsLoc);
    termOutput.writeVlong(locOfPositionsForTermBlock - posLoc);

    int nTerms = termList.size();

    // now write hashes of the terms
    for (int i=0; i<nTerms; i++) {
      auto term = termList[i];
      termOutput.write((char)XXH3_64bits(term.data(), term.size()));
    }

    // now write the block:
    int32_t pulsedIdx = 0;  // index of next pulsed data
    for (int i=0; i<nTerms; i++) {
      auto term = termList[i];

      if (i > 0) {
        // If not the first term, find common prefix with previous term
        auto[tdata, tlen] = term.unpack();
        int minsize = std::min(tlen, reflen);
        int prefixLen = 0;
        // Is there a compiler intrinsic for this?  Or a SIMD version?  Seems like a SIMD subtract followed by find-first-nonzero would do it.
        // Even w/o simd, if registers are in big endian (see movbe instr), then subtract, find high bit, divide to convert to byte.
        while (prefixLen < minsize && tdata[prefixLen] == refdata[prefixLen]) {
          prefixLen++;
        }
        // No longer needed. PackedTerm is now limited to 0xff length
        // auto prefixLen = std::min(prefixLen,0x0ff);  // support a maximum prefix sharing of 255 to simplify coding.

        // encode shared prefix length + suffix length in a single byte.
        // 3 bits of prefix length starting at 0 (7 means this is followed by another byte encoding the prefix length)
        // ORIG FORMAT to support lengths to 32K: 5 bits of suffix length starting at 1 (32 means this is followed by
        // another vInt encoding the suffix length (and add 32) we start at 1 for the suffix since that is the min
        // suffix length (otherwise it would be the same term))
        // NEW: term lengths are limited to one byte, so 32 means just read the second byte for the exact suffix len.
        auto suffixLen = tlen - prefixLen;
        auto prefCode = (prefixLen < 7) ? (prefixLen << 5u) : (7u << 5u);
        auto suffCode = (suffixLen < 32) ? (suffixLen - 1) : (32 - 1);
        termOutput.write((char) (prefCode | suffCode));
        if (prefixLen >= 7) {
          termOutput.write((char) prefixLen);
        }
        if (suffixLen >= 32) {
          // termOutput.writeVint(suffixLen - 32);
          termOutput.write((char)suffixLen);
        }

        // now write the suffix of the current term
        termOutput.write(tdata + prefixLen, suffixLen);

        // update what we are prefix encoding relative to
        refdata = tdata;
        reflen = tlen;
      }

      // Write the term metadata that belongs in the term dictionary.
      // We need pointer into the docs file.  Currently coded as the size in the docs file that *this* term takes up.  Hence
      // One needs the doc pointer for the previous term to know the start of the docs block for this term.
      // That's not good if we want to add skipping to the terms list... but maybe that's OK since we always access
      // a doc block from its tail anyway.  We could save a little space (smaller doc skipping index) if we didn't need
      // to encode the start of the block there though.

      // This is also where we "pulse" (directly include) a term that only has a single doc and position.
      // A doc block will have a minimum size... hence we cloud use (FUTURE) a small docBlockSize to encode the size of pulsed data.
      auto docsSize = docFileSize[i];
      if (docsSize == 0) {
        auto doc = pulsed[pulsedIdx++];
        // TODO: optimize this wasteful encoding.
        // We could add enough to the minimum size of a docs block so that we could use the low 4 bits as a group
        // varint encoding.  This would also speed up skipping over a pulsed term.  We could also put pulsed terms
        // in a separate block... but we really want primary key lookup to be fast!
        // We could also find something else to encode and always group encode 2 integers (like term freq)
        // We could make the term freq or doc even if it's real or odd if it's a pulsed position.
        termOutput.write(0);
        termOutput.writeVint(doc);  // for now, just write vints (slower to skip though)
        if (hasPositions) {
          auto pos = pulsed[pulsedIdx++];
          termOutput.writeVint(pos);
        }
      } else {
        termOutput.writeVint(docsSize);
      }
    }

    assert(pulsedIdx == (int)pulsed.size());  // we should have read all pulsed docs/pos;

    if (!endingField) {
      _startTermBlock(endingField);
    }
  }


  /// NOTE! The provided term ref should be valid for the lifetime of this TextWriter (or at least until endField())
  // TODO: We could do better for merging... instead of having to keep all terms around in memory until the field
  // ends, we could provide a callback or another signal (perhaps a bool return from endField()) to release
  // the term storage.  We could also have a startTerm(std::string_view) and an associated pool that we could
  // roll back after we flush a term block.
  // returns 1-based ordinal of term in this field
  int32_t startTerm(TermRef term) {
    docsFlushed = 0;
    ttfAcc = 0;
    prevDocBlockLast = 0;  // each term's first doc block starts from base 0
    prevL1GroupLast = 0;
    l1GroupLastDoc = 0;
    l1GroupBlockCount = 0;
    l1GroupDocCount = 0;
    l1GroupTfSum = 0;
    l1GroupMaxTf = 0;
    group_output.resize(0);
    locOfPositionsForTerm = posOutput.size();
    locOfDocsForTerm = docOutput.size();
    termList.push_back(term);  // we don't really need the term name at this point (could add in endTerm), but it might be nice for debugging / exceptions?
    return numTerms + termList.size();
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
      sumDocFreq += 1;
      docFileSize.push_back(0);
      // the doc (+ position, if indexed) will be remembered to be included directly in the term dictionary (i.e. pulsing)
      pulsed.push_back(docs[0]);
      if (hasPositions) {
        pulsed.push_back(posdeltas.back());
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
      // StreamVByte tail layout (see DocsEnum::nextDoc):
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
        appendL0Block(lastDoc, base, compressed_output.size(), n, tfSum, maxTf);
        prevDocBlockLast = lastDoc;
      }
      docsFlushed += docs.size();
      docs.resize(0);
      tfreqs.resize(0);
      flushL1Group();


      // The reader can find the start or end of a doc block from the terms dictionary (since blocks are all adjacent)
      // So we can store info at the end of the block as well (but need to encode backwards, or have a single byte metadata
      // length at the end to enable backing up.)
      auto metadataStart = docOutput.size();

      sumDocFreq += docfreq;  // docfreq computed at the top of endTerm

      // Per-term metadata is level-dependent: docfreq always; ttfCode (ttf-docfreq)
      // only when freqs are indexed; posOffset only when positions are indexed.
      // TODO: encode as group, and can replace the metadataSize byte with the control byte.
      docOutput.writeVint(docfreq);
      if (hasFreqs) {
        auto ttfCode = totalTermFreq - docfreq;
        docOutput.writeVlong(ttfCode);
      }
      if (hasPositions) {
        // offset from start of positions in term dict block
        auto posOffset = locOfPositionsForTerm - locOfPositionsForTermBlock;
        docOutput.writeVlong(posOffset);
      }

      auto metadataSize = docOutput.size() - metadataStart;
      docOutput.write((char)metadataSize);

      docFileSize.push_back(getDocFileSize());
    }

    if (termList.size() == Postings::TERMS_BLOCK_SIZE) {
      flushTerms(false);
    }
  }

  // This should only be called once.  Multiple fields are not handled any longer.
  void endField() {
    flushTerms(true);

    // write index into the blocks of the terms dict
    // TODO: use a more efficient encoding for this array
    //   - make offsets be from the start of this index array... 32 bit normally fine, but not always for huge field?
    //   - sequence will be monotonically increasing (or decreasing)... interpolate?
    // Indexing RAM OPT: for fields with huge number of terms, we could stream this to separate file.  That would also facilitate alignment if it's important.
    fieldInfo->nTerms = numTerms;
    fieldInfo->sumDocFreq = sumDocFreq;
    fieldInfo->sumTotalTermFreq = sumTotalTermFreq;

    fieldInfo->termBlockIndexLoc = seg_location(termOutput.streamNumber, termOutput.size());
    // one way this assert can fail is if numTerms==0, but I think so far this always represents a bug elsewhere.
    assert((int)termBlockOffsets.size() == ((numTerms-1) / Postings::TERMS_BLOCK_SIZE) + 1);
    termOutput.write(&(termBlockOffsets[0]), termBlockOffsets.size() * sizeof(termBlockOffsets[0]) );

    fieldInfo->termsLoc = seg_location(termOutput.streamNumber, termsLoc);
    fieldInfo->docsLoc = seg_location(docOutput.streamNumber, docsLoc);
    fieldInfo->posLoc = seg_location(posOutput.streamNumber, posLoc);
  }


  void startDoc(int32_t doc) {
    unused(doc);
    curTf = 0;

    // Do we need to know the current doc?

    // We don't keep track of positions for the doc... we block encode all positions for a term together.
    // locationOfPositionsForDoc = posOutput->size();
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
