#pragma once

#include <cstdint>
#include <assert.h>
#include <iostream>
#include <ostream>
#include <sstream>
#include <unordered_map>
#include <vector>
#include "solux/util/MemPool.h"
#include "solux/util/StrRef.h"
#include "solux/store/OutputStream.h"
#include "solux/store/Directory.h"
#include "solux/reader/PostingsReader.h"
#include "simdcomp/include/codecfactory.h"
#include "roaring.hh"
#include "ScreamingBuilder.h"


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

  // not thread-safe
  IndexFieldInfo& addField(PackedTerm fieldName) {
    fieldInfos.emplace_back(); // we should get default-initialization with this for the SegFieldInfo members
    fieldInfos.back().fieldname = fieldName;
    assert(fieldInfos.back().monoLoc.offset() == 0 && fieldInfos.back().monoMetaOff == 0 && fieldInfos.back().columnMetaOff == 0);
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
      files.back().out.writeStr(Postings::SOLUX_HEADER);
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
  bool finish() {

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

    for (auto& dataFile : files) {
      sizeInBytes += dataFile.out.size();
      dataFile.out.close();
      directory.finishFile(*dataFile.file);
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

    for (auto& finfo : fieldInfos) {
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

  // base (starting) values int the associated output streams to calculate offsets from
  int64_t termsLoc=0;
  int64_t docsLoc;
  int64_t posLoc;

  int64_t locOfPositionsForTermBlock;
  int64_t locOfDocsForTermBlock;
  int64_t locOfPositionsForTerm;
  int64_t locOfDocsForTerm;

  int64_t positionsHandled;  /// number of positions handled for the current term so far (everything except posdeltas)
  int32_t docsFlushed;  /// number of documents flushed for the current term so far
  int64_t totalTermFreqPrevDoc = 0; // total term freq up through the previous doc

  std::vector<uint64_t> termBlockOffsets;  // offset from termsOffset (for this field) for each term block
  int64_t sumTotalTermFreq = 0; // updated in endTerm
  int64_t sumDocFreq = 0; // updated in endTerm
  int32_t numTerms; // currently only updated in flushTerms

  // TODO: pool allocate this
  std::vector<char> compressed_output;

private:  // some internal utility methods... not for use by indexers
  // number of docs for the current term
  int32_t getDocFreq() const {
    return docsFlushed + docs.size();
  }

  // total number of positions for the current term
  int64_t getTotalTermFreq() const {
    return positionsHandled + posdeltas.size();
  }

  // total number of positions for the current doc
  int32_t getTermFreq() const {
    return getTotalTermFreq() - totalTermFreqPrevDoc;
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
    Postings::posCodec.encodeBlock(reinterpret_cast<uint32_t *>(posdeltas.data()), posdeltas.size(), compressed_output.data(),
                                  compressedSize);
    posOutput.write(compressed_output.data(), compressedSize);

    positionsHandled += posdeltas.size();
    posdeltas.resize(0);
  }

  // TODO: move to .cpp unless we template this class
  void flushDocs() {
    assert(docs.size() == tfreqs.size());

    if (docs.empty()) {
      return;
    }

    assert(docs.back() < postingsWriter.getMaxDoc()); // sanity check to ensure we didn't go over provided numDocs

    // NOTE: some codecs (like s4-fastpfor-d1) modify the input array to calculate deltas!
    // given that we (could) already have deltas, is there an easy way to bypass that part?
    // NOTE: SIMDCompressionAndIntersection puts 32 bit size at start!  Look at C version and see if it's easier to modify?
    // The simdcomp C library does have lower level interfaces that just handle a single 128 value block

    compressed_output.resize(Postings::DOCS_BLOCK_SIZE * sizeof(int32_t) + 1024);
    uint32_t compressedSize = compressed_output.size(); // this gets changed to the actual size
    Postings::docCodec.encodeBlock(reinterpret_cast<uint32_t *>(docs.data()), docs.size(), compressed_output.data(),
                                  compressedSize);
    docOutput.write(compressed_output.data(), compressedSize);

    //
    // now the term freqs
    //
    compressed_output.resize(Postings::TERMS_BLOCK_SIZE + 1024);
    compressedSize = compressed_output.size(); // this gets changed to the actual size
    Postings::tfreqCodec.encodeBlock(reinterpret_cast<uint32_t *>(tfreqs.data()), tfreqs.size(), compressed_output.data(),
                                    compressedSize);
    docOutput.write(compressed_output.data(), compressedSize);

    docsFlushed += docs.size();
    docs.resize(0);
    tfreqs.resize(0);

    // TODO: add data (or keep track of blocks) for docs skip list
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
        auto pos = pulsed[pulsedIdx++];
        // TODO: optimize this wasteful encoding.
        // We could add enough to the minimum size of a docs block so that we could use the low 4 bits as a group
        // varint encoding.  This would also speed up skipping over a pulsed term.  We could also put pulsed terms
        // in a separate block... but we really want primary key lookup to be fast!
        // We could also find something else to encode and always group encode 2 integers (like term freq)
        // We could make the term freq or doc even if it's real or odd if it's a pulsed position.
        termOutput.write(0);
        termOutput.writeVint(doc);  // for now, just write vints (slower to skip though)
        termOutput.writeVint(pos);
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
    positionsHandled = 0;
    locOfPositionsForTerm = posOutput.size();
    locOfDocsForTerm = docOutput.size();
    termList.push_back(term);  // we don't really need the term name at this point (could add in endTerm), but it might be nice for debugging / exceptions?
    return numTerms + termList.size();
  }

  void endTerm(TermRef term) {
    unused(term);
    auto totalTermFreq = getTotalTermFreq();
    sumTotalTermFreq += totalTermFreq;
    if (docs.size() > 0) {
      assert(docs.back() < postingsWriter.getMaxDoc()); // sanity check to ensure we didn't go over provided numDocs
    }
    // TODO: handle case when all docs were deleted for term (and term should no longer appear)
    if (totalTermFreq == 1) {
      assert(getDocFileSize()==0 && docs.size()==1 && posdeltas.size() == 1);
      sumDocFreq += 1;
      docFileSize.push_back(0);
      // the doc+position will be remembered to be included directly in the term dictionary (i.e. pulsing)
      pulsed.push_back(docs[0]);
      pulsed.push_back(posdeltas.back());
      posdeltas.pop_back();
      positionsHandled++;

      docsFlushed += docs.size();
      docs.resize(0);
      tfreqs.resize(0);

    } else {
      // Finish positions that were not block encoded
      // TODO: possibly block encode positions across terms?  For that we would want ttf encoded in the terms dict (or could also store sum of all prev in docs file)
      //       Downside to this is that it could hurt block compression to mix terms (small + big deltas mixed)
      for (auto posDelta : posdeltas) {
        // TODO: try group varint
        posOutput.writeVint(posDelta);
      }
      positionsHandled += posdeltas.size();
      posdeltas.resize(0);

      // Finish docs that were not block encoded.  In this case, we simply interleave docs and termfreqs
      // for more efficient incremental decode.
      // TODO: try some sort of group varint encoding once we have a good benchmark framework.
      // TODO: pull this out into codec?
      // TODO: for a list above a certain size, bisect with a skip?  Wait for good benchmarks to implement this.  It seems like
      //   it would only speed up rare/rare term conjunctions.  Might help common terms in small segments too though.
      // TODO: make first delta an actual delta from the last block... not from -1.  Not too important though given that that this is only sub-optimal
      //   when the docfreq is larger than the doc block size.
      int32_t lastdoc = -1;
      assert(docs.size() == tfreqs.size());
      for (int i=0; i<(int)docs.size(); i++) {
        assert(docs[i] > lastdoc || i==0);
        int docdelta = docs[i] - lastdoc;
        lastdoc = docs[i];

        auto tfreq = tfreqs[i];
        int doccode = docdelta << 1;  // the low bit will be used to signal a termfreq of 1 or not.
        if (tfreq == 1) {
          doccode |= 1u;  // low bit==1 means tfreq==1.
        }
        docOutput.writeVint(doccode);
        if (tfreq != 1) {
          docOutput.writeVint(tfreq);
        }
      }
      docsFlushed += docs.size();
      docs.resize(0);
      tfreqs.resize(0);


      // The reader can find the start or end of a doc block from the terms dictionary (since blocks are all adjacent)
      // So we can store info at the end of the block as well (but need to encode backwards, or have a single byte metadata
      // length at the end to enable backing up.)
      auto metadataStart = docOutput.size();

      auto docfreq = getDocFreq();
      sumDocFreq += docfreq;
      auto ttfCode = totalTermFreq - docfreq;
      // offset from start of positions in term dict block
      auto posOffset = locOfPositionsForTerm - locOfPositionsForTermBlock;

      // TODO: encode as group, and can replace the metadataSize byte with the control byte.
      docOutput.writeVint(docfreq);
      docOutput.writeVlong(ttfCode);
      docOutput.writeVlong(posOffset);

      auto metadataSize = docOutput.size() - metadataStart;
      docOutput.write((char)metadataSize);
      // TODO: generate/store skip index

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
    totalTermFreqPrevDoc = getTotalTermFreq();

    // Do we need to know the current doc?

    // We don't keep track of positions for the doc... we block encode all positions for a term together.
    // locationOfPositionsForDoc = posOutput->size();
  }

  void endDoc(int32_t doc) {
    auto tf = getTermFreq();
    if (tf == 0) {
      // TODO: revisit if this can happen... it depends on where deleted docs are checked.
      return;
    }
    docs.push_back(doc);
    tfreqs.push_back(tf);
    if (docs.size() == Postings::DOCS_BLOCK_SIZE) {
      flushDocs();
    }
  }

  void addPositionDelta(int32_t posDelta) {
    posdeltas.push_back(posDelta);
    if (posdeltas.size() == Postings::POSITIONS_BLOCK_SIZE) {
      flushPositions();
    }
  }

};


class DocsWriter {
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