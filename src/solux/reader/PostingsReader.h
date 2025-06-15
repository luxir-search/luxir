#pragma once

#include <cstdint>
#include <assert.h>
#include <iostream>
#include <array>
#include <ostream>
#include <sstream>
#include <unordered_map>
#include <vector>
#include <string>
#include <charconv>
#include <solux/util/screaming.h>
#include <filesystem>
#include <solux/schema/FieldType.h>
#include "solux/store/Directory.h"
#include "solux/store/InputStream.h"
#include "solux/util/MemPool.h"
#include "solux/util/StrRef.h"
#include "solux/store/OutputStream.h"
#include "solux/codec/Codec.h"

namespace solux {

class PostingsReader;
class FieldReader;
class TermsEnum;
class DocsEnum;

// Some stuff that the postings reader and writer need to share.
class Postings {
public:
  static constexpr int32_t TERMS_BLOCK_SIZE = 32;
  static constexpr int32_t POSITIONS_BLOCK_SIZE = SoluxPFOR::BLOCK_SIZE;
  static constexpr int32_t DOCS_BLOCK_SIZE =  SoluxPFOR::BLOCK_SIZE;
  static constexpr int32_t NUMERIC_BLOCK_SIZE = 16384;

  // using PositionsCodec = IntegerCODECTypeWrapper<SIMDCompressionLib::FastPFor<4, false>>;
  using PositionsCodec = SoluxPFOR;
  // using DocsCodec = IntegerCODECTypeWrapper<SIMDCompressionLib::SIMDFastPFor<4, SIMDCompressionLib::RegularDeltaSIMD>>;
  using DocsCodec = SoluxPFORd;
  using TFreqCodec = PositionsCodec; // same type, but should also share instances for better performance
  using NumericCodec = SoluxSIMDFor;

  // These could be static if we made them thread safe...
  static DocsCodec docCodec;
  static PositionsCodec posCodec;
  static TFreqCodec& tfreqCodec;
  static NumericCodec numericCodec;

  // Filename related utilities.  We try to keep filenames short for many reasons, including
  // being able to fit in short-string optimization.

  static constexpr std::string_view INDEX_INFO_FILE = "s.olux"; // lists all segments in the index
  static constexpr std::string_view PREFIX_FNAME = "s";         // prefix for all data files
  static constexpr std::string_view SOLUX_HEADER = "SOLUX001";  // every data file starts with this header


  // Create a sortable string from a number.  It's currently
  // a base36 representation prefixed with the number of digits-1 to make it sort correctly.
  // Example: getSortableString(0)->"00", getSortableString(10)->"0a", getSortableString(36)->"110"
  static std::string getSortableString(uint64_t val) {
    std::array<char, 14> arr; // Need 13 digits (log(2**64)/log(36)==12.3) plus one for the length prefix.
    auto start = arr.begin() + 1;  // leave room to write the prefix
    auto[end, ec] = std::to_chars(start, arr.end(), val, 36);
    uint8_t extraDigits = end - start - 1;
    arr[0] = extraDigits <= 9 ? ('0' + extraDigits) : ('a' + (extraDigits - 10));  // base36 prefix
    return std::string(arr.begin(), end);
  }

  static std::string getIndexFileName(const std::string_view gen, const std::string_view suffix) {
    return std::string(PREFIX_FNAME).append(gen).append(suffix);
  }

  static std::string getIndexFileNamePrefix(uint64_t segId) {
    return std::string(PREFIX_FNAME).append(getSortableString(segId));
  }

  /// Filename for the segment given the segment gen/number and the file number.
  /// Example: the 3rd file in the 4th segment produced would be "s04_03"
  static std::string getIndexFileName(const std::string_view gen, uint32_t filenum) {
    std::string s = std::string(PREFIX_FNAME).append(gen);
    s += '_';
    s.append(getSortableString(filenum));
    return s;
  }

  /// A file that contains deletes for the segment.  deleteGen==0 implies no deletes.
  static std::string getDeleteFileName(const std::string_view gen, uint64_t deleteGen) {
    std::string s = std::string(PREFIX_FNAME).append(gen);
    s += '_';
    s += '_'; // use double underscore to prevent clashes with other index filenames
    s.append(getSortableString(deleteGen));
    return s;
  }

};




class IntColStats {
  int64_t minval = std::numeric_limits<int64_t>::max();
  int64_t maxval = std::numeric_limits<int64_t>::min();
  int32_t nvals = 0;  // this may be redundant (i.e. roaring bitset for docids also knows.

public:
  void add(int64_t val) {
    nvals++;
    if (val < minval) {
      minval = val;
    }
    if (val > maxval) {
      maxval = val;
    }
  }

  int64_t minVal() {
    return minval;
  }

  int64_t maxVal() {
    return maxval;
  }

  int32_t numVals() {
    return nvals;
  }
};



// Lowest level postings reader class that needs to correspond to the PostingsWriter class that created the data.
// PostingsReader should be thread-safe at the top level, but any iterators it supplies would not be.
// This does not contain deleted docs, so instances can be shared by different index versions.
class PostingsReader {
  std::vector<std::shared_ptr<InputFile>> files;  // keeps files live while this PostingsReader is live.
  std::vector<InputStream> inputStreams;
  int64_t segInfoOffset;  // after this is segInfo, before this is the field index
  int32_t maxdoc;
public:
  InputStream firstIS;

  // used as a sentinel value for docs and positions iterators in a single segment.
  static constexpr int32_t END = std::numeric_limits<int32_t>::max();

  explicit PostingsReader(Directory& dir, uint64_t segId) {

    std::string segStr = Postings::getSortableString(segId);
    auto segInfoFile = Postings::getIndexFileName(segStr, 0);
    files.emplace_back(dir.openFile(segInfoFile));
    if (files.back().get() == nullptr) {
      // TODO FIXME: this is the only place in the codebase where we throw an exception
      // for a non-error condition (we don't synchronize with the writer, so a merge may have deleted
      // the segment file we were trying to open).  For debugging purposes, it would be nice to
      // migrate away from exceptions so that exceptions should never happen unless testing error scenarios.
      throw std::filesystem::filesystem_error(
              std::format("Can't find/open first segment file '{}'", segInfoFile),
              std::make_error_code(std::errc::no_such_file_or_directory));
    }
    inputStreams.emplace_back(files[0]->getInputStream());
    firstIS = inputStreams[0];

    // seek to the end of firstIs and read the size of the segmentInfo
    // See PostingsWriter.writeSegmentInfo
    firstIS.seek(firstIS.size() - sizeof(int32_t));
    auto segInfoSize = firstIS.readInt();
    segInfoOffset = firstIS.size() - sizeof(int32_t) - segInfoSize;
    firstIS.seek(segInfoOffset);
    maxdoc = firstIS.readVint();
    int nFiles = firstIS.readVint();

    files.reserve(nFiles);
    inputStreams.reserve(nFiles);

    for (int i=1; i<nFiles; i++) {
      files.emplace_back(dir.openFile(Postings::getIndexFileName(segStr, i)));
      if (files.back().get() == nullptr) {
        throw std::filesystem::filesystem_error(
                std::format("Can't find/open segment file '{}'", Postings::getIndexFileName(segStr, i)),
                std::make_error_code(std::errc::no_such_file_or_directory));
      }
      inputStreams.emplace_back(files.back()->getInputStream());
    }
  }

  int32_t numDocs() const noexcept {
    return maxdoc;
  }

  InputFile* getFile(uint32_t fnum) {
    assert(fnum < files.size());
    return files[fnum].get();
  }

  // We can't get & cache the InputStream in PostingsReader unless we create them all in the constructor (for thread safety)
  // But if we're using mmap, that's probably fine?  Would not be fine if we need to read everything in the constructor.
  // TODO: could also have a mode that opens on demand (and hence synchronizes)... that would be good for something like IndexWriter
  // that needs to only read the ID field to handle overwrites / deletions.  That file *might* already be open by another IndexReader
  // though?  How to coordinate?
  InputStream getInputStream(uint32_t fnum) {
    assert(fnum < inputStreams.size());
    return inputStreams[fnum];
  }

  // return an InputStream positioned on the current location specified
  InputStream getInputStreamSeek(seg_location sloc) {
    InputStream is = getInputStream(sloc.filenum());
    is.seek(sloc.offset());
    return is;
  }

  friend std::ostream& operator<< (std::ostream &out, const PostingsReader &reader) {
    out << "PostingsReader: numDocs=" << reader.numDocs() << " files=" << reader.files;
    return out;
  }

  friend class FieldReader;
};

//
// IDEA: have something top-level, like a PostingsReader, that is not thread-safe (i.e. per-session/thread)
// that can cache some things (block encoders, pool, fast terms cache, or whatever)
// That could just be FieldReader, but we will probably have a higher level than that eventually.
//



struct SegFieldInfo {
  PackedTerm fieldname;
  FieldType::Type type;  // really only need a byte here
  int32_t flags;  // from FieldType
  seg_location termBlockIndexLoc;  // location of index into the terms blocks
  seg_location termsLoc;
  seg_location docsLoc;
  seg_location posLoc;
  int32_t nTerms;
  int32_t docsWithField;

  // I don't know if things like nTerms, sumDocFreq, sumTotalTermFreq will stay in fieldInfo
  // or perhaps be moved into the terms section of the postings (i.e. pushed down so one needs
  // a TermsEnum to read them).  To be safe, we should only access through TermsEnum for now.
  // Only reason to keep at this level would be if they are sometimes needed even without a TermsEnum.
  // They could be written right before the termBlock offsets that termBlockIndexLoc points to.
  // One advantage of keeping this stuff here is it makes the TermBlockOffsets fixed size.
  int64_t sumDocFreq;
  int64_t sumTotalTermFreq;

  // column
  seg_location docsWithFieldEndLoc;
  seg_location columnLoc;    // location of the start of the column
  int64_t columnMetaOff;     // offset from the start of the column to the metadata
  int64_t numValues;         // numValues in the column. for singleValued fields, docsWithField == numValues

  seg_location monoLoc;   // location of the monotonic column
  int64_t monoMetaOff;    // offset from the start of the mono column to the metadata
};





/// Not thread safe
class FieldReader {
  friend class DocsEnum;
  friend class TermsEnum;
  friend class IntColReader;

  InputStream fieldIS;
  int32_t nFields;
  int32_t currField = -1;
  uint32_t* fieldOffsets;  // the array of field offsets, written by PostingsWriter::writeFieldIndex()
  int64_t fieldOffsetsLoc; // location in the file of the above array

  PackedTerm fieldname{nullptr};
  bool fieldInfoRead = false;      // has field metadata been read for this field?

  // TODO: field number?
public:
  FieldReader(MemPool& pool, PostingsReader& postingsReader) {
    unused(pool);
    fieldIS = postingsReader.getInputStream(0);
    fieldIS.seek(postingsReader.segInfoOffset - sizeof(int32_t));
    fieldOffsetsLoc = fieldIS.offset();
    auto fieldLocEnd = fieldIS.ptr();
    nFields = fieldIS.readInt();
    fieldOffsets = ((uint32_t*)(fieldLocEnd)) - nFields;
    fieldOffsetsLoc -= nFields * sizeof(uint32_t);
  }

  FieldReader(MemPool& pool, PostingsReader& postingsReader, const InputStream& is) : fieldIS(is) {
    unused(pool, postingsReader);
  };

  int32_t numFields() {
    return nFields;
  }

  // TODO: we could make a readFieldInfo(std::string_view fieldName) that is thread safe (doesn't modify the FieldReader)
  // Although it might just be simpler to make a copy?
  // IndexReader could return an array of const FieldReaders

  [[nodiscard]] bool seek(const std::string_view fieldName) {
    auto comparator = [&](const int32_t fieldOff, const std::string_view key) {
      auto fieldNameFound = fieldIS.readPackedTerm(fieldOffsetsLoc - fieldOff);
      return fieldNameFound < key;
    };
    auto endPtr = fieldOffsets + nFields;
    uint32_t* fieldOffPtr = std::lower_bound(fieldOffsets, endPtr, fieldName, comparator);
    if (fieldOffPtr != endPtr) {
      // This may be slightly repeated work (additional read term and compare), but it may not be worth it to eliminate.
      auto fieldNameFound = fieldIS.readPackedTerm(fieldOffsetsLoc - *fieldOffPtr);

      if (fieldNameFound == fieldName) {
        currField = fieldOffPtr - fieldOffsets - 1;  // back up to previous field since we will increment in readNextField
        return readNextField();
      }
    }
    return false;
  }


  // TODO: should this read into a different structure?  Only if we want to iterate over all fields but not read them?
  // one possible use case: a wildcard in field names (fast iteration would be a bonus)
  [[nodiscard]] bool readNextField() {
    if (currField+1 >= nFields) {
      return false;
    }
    ++currField;
    fieldIS.seek(fieldOffsetsLoc - fieldOffsets[currField]);

    //
    // See PostingsWriter.endField() for the format written.
    //
    fieldname = fieldIS.readPackedTerm();
    fieldInfoRead = false;
    return true;
  }

  // only valid after readNextField() returns true or seek() returns true.
  // The SegFieldInfo produced is independent of FieldReader.
  void readFieldInfo(SegFieldInfo& fieldInfo) {
    assert(fieldIS.left() > 0); // this triggers if this fieldReader is unpositioned.
    assert(!fieldname.isNull());
    assert(!fieldInfoRead);  // we could back up and re-read based on currField
    if (!fieldInfoRead) {
      fieldInfoRead = true;
      fieldInfo.fieldname = fieldname;
      fieldInfo.type = static_cast<FieldType::Type>(fieldIS.readVint());
      fieldInfo.flags = fieldIS.readVint();
      fieldInfo.docsWithField = fieldIS.readVint();
      if (fieldInfo.flags & FieldType::INDEX_DOCS) {
        fieldInfo.termBlockIndexLoc = fieldIS.readVal<seg_location>();
        fieldInfo.termsLoc = fieldIS.readVal<seg_location>();
        fieldInfo.docsLoc = fieldIS.readVal<seg_location>();
        fieldInfo.posLoc = fieldIS.readVal<seg_location>();
        fieldInfo.nTerms = fieldIS.readVint();
        fieldInfo.sumDocFreq = fieldInfo.nTerms + fieldIS.readVlong();
        fieldInfo.sumTotalTermFreq = fieldInfo.sumDocFreq + fieldIS.readVlong();
      }

      fieldInfo.docsWithFieldEndLoc = fieldIS.readVal<seg_location>();
      fieldInfo.columnLoc = fieldIS.readVal<seg_location>();
      fieldInfo.columnMetaOff = fieldIS.readVlong();
      fieldInfo.numValues = fieldIS.readVlong();

      fieldInfo.monoLoc = fieldIS.readVal<seg_location>();
      fieldInfo.monoMetaOff = fieldIS.readVlong();
    }
  }

  PackedTerm name() const noexcept { return fieldname; }

private:

};




class TermsEnum {
  friend class DocsEnum;

  InputStream termsIS;
  MemPool& pool;
  PostingsReader& postingsReader;

  const SegFieldInfo& fieldInfo;

  PackedTerm currTerm;
  int32_t ordInBlock = -1; // the term number local to the current block
  int32_t docsSize;
  int32_t pulsedDoc;
  int32_t pulsedPos;

  // block-level information

  PackedTerm startingTerm;
  int32_t termBlockIndex = -1; // what term block are we currently in
  int32_t startingOrd = 0;
  int32_t maxOrdInBlock = -1;
  int64_t locOfDocsForTermBlock;  // absolute location... field offset + block offset
  int64_t locOfPositionsForTermBlock;  // absolute location... field offset + block offset
  int64_t cumulativeDocsSize;
  const char* termHashes;

  // term index level
  const int64_t* termBlockOffsets;
  int32_t numTermBlocks;

public:
  // fieldInfo is not copied and should remain valid throughout the lifetime of this TermsEnum and any related classes such as DocsEnum
  TermsEnum(MemPool& pool, PostingsReader& postingsReader, const SegFieldInfo& fieldInfo) : pool(pool), postingsReader(postingsReader), fieldInfo(fieldInfo) {
    unused(this->pool, this->postingsReader);
    numTermBlocks = ((fieldInfo.nTerms-1) / Postings::TERMS_BLOCK_SIZE) + 1;
    termsIS = postingsReader.getInputStreamSeek(fieldInfo.termBlockIndexLoc);
    termBlockOffsets = reinterpret_cast<const int64_t*>(termsIS.ptr());  // offsets from termsLoc
    currTerm = PackedTerm(pool.alloc(PackedTerm::getMemSize(PackedTerm::MAX_LEN)));
  }

  int32_t numTerms() const {
    return fieldInfo.nTerms;
  }

  int32_t docsWithField() const {
    return fieldInfo.docsWithField;
  }

  /// sumDocFreq is a field-level stat:
  /// The docFreq of a term is the number of documents it appears in. sumDocFreq is the sum across all terms in this field.
  /// If sumDocFreq() == docsWithField() then every document contains only one term (or the same term repeated.)  May be
  /// useful for detecting single-valued fields even if they were not marked as such.
  int64_t sumDocFreq() const {
    return fieldInfo.sumDocFreq;
  }

  /// sumTotalTermFreq is a field-level stat:
  /// termFreq is the number of times the term appears in a single document.
  /// totalTermFreq is the number of times the term appears across all documents in this segment.
  /// sumTotalTermFreq is the sum of totalTermFreq for all terms in this field (i.e. number of tokens indexed)
  int64_t sumTotalTermFreq() const {
    return fieldInfo.sumTotalTermFreq;
  }

  // 0 based ords
  int32_t ord() const {
    return startingOrd + ordInBlock;
  }

  /// NOTE: the returned term is invalidated/changed if this TermsEnum is moved off this
  /// term (i.e. the moment next() or seek() is called). Make a copy if you wish to keep it!
  PackedTerm term() const {
    return currTerm;
  }

  // read the data that comes after each term
  void readTermMetadata() {
    // see PostingsWriter.flushTerms
    docsSize = termsIS.readVint();
    cumulativeDocsSize += docsSize;
    if (docsSize == 0) {
      pulsedDoc = termsIS.readVint();
      pulsedPos = termsIS.readVint();
    }
  }

  // seeks to termBlockIndex and reads the block metadata + first term
  void readTermBlock() {
    termsIS.seek(fieldInfo.termsLoc.offset() + termBlockOffsets[termBlockIndex]);
    startingOrd = termBlockIndex * Postings::TERMS_BLOCK_SIZE;  // we currently have fixed size blocks
    cumulativeDocsSize = 0;
    ordInBlock = 0;
    maxOrdInBlock = std::min(Postings::TERMS_BLOCK_SIZE - 1, fieldInfo.nTerms - startingOrd - 1);

    // see PostingsWriter.flushTerms
    startingTerm = termsIS.readPackedTerm();
    locOfDocsForTermBlock = fieldInfo.docsLoc.offset() + termsIS.readVlong();  // fieldOffset + blockOffset for docs
    locOfPositionsForTermBlock = fieldInfo.posLoc.offset() + termsIS.readVlong();

    memcpy(currTerm.ptr(), startingTerm.ptr(), startingTerm.memorySize());

    // remember, then skip over the term hashes... one byte per hash.
    termHashes = termsIS.ptr();
    termsIS.skip(maxOrdInBlock+1);  // ords are 0 based, so add 1 for the number of them. maxOrdInBlock is also inclusive (not one past the end)

    readTermMetadata();
  }

  bool nextTerm() {
    if (ordInBlock == maxOrdInBlock) {
      if (ord() + 1 >= fieldInfo.nTerms) {  // could also compare number of blocks to detect end.
        return false;
      }
      termBlockIndex++;
      readTermBlock();
      return true;
    }

    readNextTermInBlock();
    return true;
  }


  // reads the next term in the block with no checking if one runs off the end of the block.
  void readNextTermInBlock() {
    assert(ordInBlock < maxOrdInBlock);
    ordInBlock++;
    // read next suffix
    // see PostingsWriter.flushTerms
    uint8_t code = termsIS.readByte();
    auto prefixLen = code >> 5;
    auto suffixLen = (code & 0x1f) + 1;
    if (prefixLen == 7) {
      prefixLen = termsIS.readByte();
    }
    if (suffixLen == 31) {
      suffixLen = termsIS.readByte();
    }

    auto [data, len] = currTerm.unpack();
    // TODO: things to try:
    // - an explicit loop vs memcpy
    // - an explict loop of 8 bytes at a time... requires making sure there are extra bytes at the end of termIS file.
    //   - try it as a do-while loop... easier branch prediction?

    // try and catch unoptimal prefix compression (we had a bug before)
    assert(uint32_t(prefixLen) == len || data[prefixLen] != *termsIS.ptr());

    termsIS.read(const_cast<char*>(data + prefixLen), suffixLen);
    currTerm.setSize(prefixLen + suffixLen);

    readTermMetadata();
  }

  bool seek(std::string_view target) {
    auto termBlockEnd = termBlockOffsets + numTermBlocks;
    // Find the first block that is greater than the current term.
    // std::cout << "seek key=" << target << " numBlocks=" << fieldReader.numTermBlocks << std::endl;

    auto blockOffsetPtr = std::upper_bound(termBlockOffsets, termBlockEnd, target,
                                 [&](std::string_view key, const int64_t& blockOffset) {
      auto termAtBlock = termsIS.readPackedTerm(fieldInfo.termsLoc.offset() + blockOffset);
      auto ret = key < termAtBlock;
      // std::cout << "index=" << (&blockOffset-fieldReader.termBlockOffsets) << " termAtBlock=" << termAtBlock << " ret=" << ret << std::endl;
      return ret;
    }
    );

    // Since the block we found is after, we will find our target term in the
    // previous block (if at all)
    if (blockOffsetPtr > termBlockOffsets) {
      blockOffsetPtr--;
    };

    termBlockIndex = blockOffsetPtr - termBlockOffsets;
    readTermBlock();
    return seekInBlock(target);
    // return seekCeilInBlock(target); // use this version to skip comparing hashes
  }

  bool seekInBlock(std::string_view target) {
    // TODO: rather than hashing every segment, have an option to pass it in?
    char hash = (char)XXH3_64bits(target.data(), target.size());
    int lastOrd = ordInBlock - 1; // check the current term we are on.
    for(;;) {
      int matchOrd;
      // find the first matching hash
      for(matchOrd = lastOrd+1; matchOrd <= maxOrdInBlock; matchOrd++) {
        if (termHashes[matchOrd] == hash) break;
      }
      if (matchOrd > maxOrdInBlock) {
        return false;  // we got lucky and no more hashes matched!
      }

      // Hash code matched for the matchOrd term.
      // If we had a different term block structure, it might be easier to skip.  For now,
      // just call nextTerm and we get to skip the compare.
      while (ordInBlock < matchOrd) {
        readNextTermInBlock();
        // NOTE: there is one case where we do more work here than we should by skipping comparisons.
        // if the term does not exist, we could return earlier if we hit a term larger.
      }

      // Since the hash code matched, do actual comparison.  A test for equality would be faster if it's a match, but
      // we also want to handle the case where we went too far.
      auto cmp = term() <=> target;
      if (cmp == 0) return true;
      if (cmp > 0) return false;
      // if the term we just saw was smaller, continue where we left off
      lastOrd = ordInBlock;
    }
  }

  // 0-based ords
  void seekOrd(int32_t targetOrd) {
    assert(targetOrd >= 0 && targetOrd < fieldInfo.nTerms);
    if (targetOrd < ord() || targetOrd > startingOrd + maxOrdInBlock) {
      // even if we were in the right block, we don't have the capability to go backwards or rewind
      termBlockIndex = uint32_t(targetOrd) / Postings::TERMS_BLOCK_SIZE;
      readTermBlock();
    }
    while (ord() < targetOrd) {
      // nextTerm();
      readNextTermInBlock();
    }
    assert(ord() == targetOrd);
  }


  bool seekCeilInBlock(std::string_view target) {
    auto cmp = term() <=> target;
    // std::cout << " comparing with first " << term() << ": eq=" << (cmp==0) << " gt=" <<  (cmp>0) << std::endl;
    if (cmp == 0) return true;
    if (cmp > 0) return false;  // this will normally only happen on the *first* block

    // TODO: we could optimize this seeking by not actually building the term to compare.
    // We know from prefix encoding how much of the previous term is shared.
    // It could also possibly be faster to skip term metadata rather than reading it as well.
    while (ordInBlock < maxOrdInBlock) {
      nextTerm();
      cmp = term() <=> target;
      // std::cout << " comparing with next " << term() << ": eq=" << (cmp==0) << " gt=" <<  (cmp>0) << std::endl;
      if (cmp == 0) return true;
      if (cmp > 0) return false;
    }
    return false;
  }


    // TODO: a push interface that can more quickly/directly handle pulsed postings while allowing inlining?
  // That could also handle differences between block and doc
};

// TODO: templatize to be able to instrument, implement checkindex, etc...
// TODO: investigate writing a version of this based on continuations and see how it performs?
// TODO: some of this internal state could be removed... we only need some of it in the constructor?
class DocsEnum {
  // Position ordinals are indexes into the term-global positions list for non-pulsed positions.
  // Thus the max posOrd should thus be totalTermFreq (except in the case of a single pulsed term,
  // in which case there is nothing to read from the posFile anyway).
  int32_t* posBuf;       // the list of decoded position deltas (may be partial)
  int32_t* docBuf;       // the list of decoded docs (may be partial)
  int32_t* tfreqBuf;     // the list of decoded term freqs

  int64_t posOrd = 0;         // the ordinal of the current position we are on
  int64_t posOrdStart = 0;    // the starting position ordinal for the current doc
  int64_t cumulativeTermFreq; // Synonym for posOrdEnd.  Should be equal to posOrdStart + tfreq (i.e. a docs positions are [posOrdStart,cumulativeTermFreq)

  int32_t posBufIdx = 0; // index of the next value to read in the position buffer
  int32_t posBufEnd;     // one-past the last decoded position delta (but may be past the deltas for *this* doc
  int32_t posBufEndDoc;  // one-past the last decoded pos delta for *this* doc
  int32_t pos;           // current position

  int32_t docOrd = 0;    // the ordinal of the current document we are on for this term
  int32_t docBufIdx = 0; // index of the next value to read in the docs buffer
  int32_t docBufEnd;     // index of one-past the last valid element
  int32_t docid;         // current docid

  // For term freqs, since they are parallel to docs, we don't actually need
  // all of these variables.  But it sets the stage for separating the two
  // more and using different encodings / block sizes for them.
  int32_t tfreqOrd = 0;      // ordinal of the current term freq (parallel to docOrd)
  int32_t tfreqBufIdx = 0;   // index of the next valid value
  int32_t tfreqBufEnd;       // index of one-past the last valid element
  int32_t tfreq;


  InputStream docIS;
  InputStream posIS;
  PostingsReader& postingsReader;
  const SegFieldInfo& fieldInfo;
  MemPool* pool;
  int32_t docfreq; // number of docs containing this term
  int64_t ttf;    // totalTermFreq (sum of term freq across all docs for this term)

  int32_t docsSize;  // size of the postings in the doc file for the given term
  int64_t startOfDocs;

  int64_t cumulativeDocsSize;
  int64_t locOfDocsForTermBlock;
  int64_t locOfPositionsForTermBlock;


  int32_t db[Postings::DOCS_BLOCK_SIZE];  // temporary...
  int32_t pb[Postings::POSITIONS_BLOCK_SIZE];
  int32_t tb[Postings::POSITIONS_BLOCK_SIZE];

  // returns the ord of the last position in the last full block.. i.e. for 150 positions, it would return 127 (0-127 are in first block)
  // for 10, it would return -1 (there are no block encoded positions)
  static int64_t lastBlockEncodedPosOrd(uint64_t ttf) {
    // A ttf of 127 means we don't have a full block... so return -1.
    // A ttf of 128 through (128+127) means ords 0-127 are in first block and we would want to return 127.
    return (ttf & ~(Postings::POSITIONS_BLOCK_SIZE-1)) - 1;
  }

public:
  /// sentinel value used for both docs and positions
  static constexpr int32_t END = std::numeric_limits<int32_t>::max();

  // After this constructor has finished, this DocsEnum instance is independent of the TermsEnum instance.
  // This instance *does* rely on fieldInfo that was passed into the TermsEnum instance still being valid.
  DocsEnum(MemPool& pool, PostingsReader& postingsReader, TermsEnum& tenum,
           int32_t* docsScratch=nullptr, int32_t* posScratch=nullptr, int32_t* tfreqScratch=nullptr)
  : postingsReader(postingsReader), fieldInfo(tenum.fieldInfo), pool(&pool)
  {
    unused(docsScratch, posScratch, tfreqScratch);
    docBuf=db;
    posBuf=pb;
    tfreqBuf=tb;

    // Since the same terms enum will often be used for multiple docs enum, we should copy everything we need
    // from the terms enum that we need (that may change.)
    // TODO: package those dependencies in a struct that can be simply assigned?  Or if there is enough overlap, simply copy the complete tenum?
    // Or we could invert the responsibility and make the client copy the tenum if they are going to change it.
    docsSize = tenum.docsSize;
    docid = -1;

    // TODO: look into deferring filling the buffer until we need it, then we can avoid allocating the buffers for a shared DocsEnum.
    if (docsSize == 0) {
      // postings pulsed
      docfreq = 1;
      tfreq = 1;
      ttf = 1;
      // fill buffers with single pulsed doc+position
      docBuf[0] = tenum.pulsedDoc;
      docBufEnd = 1;
      tfreqBuf[0] = 1;
      tfreqBufEnd = 1;
      posBuf[0] = tenum.pulsedPos;
      posBufIdx = 0;
      posBufEndDoc = posBufEnd = 1;
      cumulativeTermFreq = 0;  // this will be incremented in nextDoc()
      // std::cout << "Pulsed posting: id=" << docid << "pos=" << posBuf[0] << std::endl;

    } else {
      pos = tfreq = -1;  // unnecessary initializations, but it makes some maybe-uninitialized warnings go away with -O3  // todo: revisit
      locOfDocsForTermBlock = tenum.locOfDocsForTermBlock;
      locOfPositionsForTermBlock = tenum.locOfPositionsForTermBlock;
      cumulativeDocsSize = tenum.cumulativeDocsSize;
      docIS = postingsReader.getInputStream(fieldInfo.docsLoc.filenum());

      // see the end of PostingsWriter.endTerm() for the term-specific metadata written there (docfreq, ttf, etc)

      // read last byte of docs to get the metadata size
      docIS.seek(locOfDocsForTermBlock + cumulativeDocsSize - 1);
      uint8_t metaSize = docIS.readByte();
      docIS.relativeSeek(-metaSize - 1); // move to start of metadata
      docfreq = docIS.readVint();
      ttf = docfreq + docIS.readVlong();
      docBufEnd = 0;

      auto posOffset = docIS.readVlong();

      // start of the actual docs is end of block - size
      startOfDocs = locOfDocsForTermBlock + cumulativeDocsSize - docsSize;
      docIS.seek(startOfDocs);

      posIS = postingsReader.getInputStream(fieldInfo.posLoc.filenum());
      posIS.seek(locOfPositionsForTermBlock + posOffset);
      posBufEndDoc = posBufEnd = 0; // no positions read yet
      cumulativeTermFreq = 0;
      // std::cout << "Normal posting" << std::endl;
    }
  }

  DocsEnum(const DocsEnum& other) = delete;

  DocsEnum(MemPool& pool, const DocsEnum& other) : postingsReader(other.postingsReader), fieldInfo(other.fieldInfo) {
    memcpy(this, &other, sizeof(DocsEnum));  // is there a better way to copy everything that can be copied so we don't forget anything?
    // re-point the internal pointers
    this->pool = &pool; // new pool (currently unused though since buffers are immediate)
    docBuf = db;
    posBuf = pb;
    tfreqBuf = tb;
    // If buffers cease to be immediate, we need to copy in the pulsed docs/positions/freqs (first element)
  }

  /// number of documents containing the term
  int32_t numDocs() {
    return docfreq;
  }

  /// sum of term freq across all documents (i.e. total number of appearances for this term)
  int32_t totalTermFreq() {
    return ttf;
  }

  /// the document this iterator is currently positions on
  int32_t docId() {
    return docid;
  }

  /// number of times the term appears in the current document
  int32_t termFreq() {
    return tfreq;
  }

  int32_t nextDocOld() {
    if (docsSize != 0) {
      // see PostingsWriter.endTerm() for format of non-block encoded docs/freqs
      uint32_t doccode = docIS.readVint();
      if ((doccode & 0x01)==1) {
        tfreq = 1;
      } else {
        tfreq = docIS.readVint();
      }
      posOrdStart = cumulativeTermFreq;
      cumulativeTermFreq += tfreq;
      auto docDelta = doccode >> 1;
      docid += docDelta;
    }
    return docid;
  }

  // we also have a next() to align with scorers
  int32_t next() {
    return nextDoc();
  }

  int32_t nextDoc() {
    if (docBufIdx >= docBufEnd) {
      auto leftToRead = docfreq - docOrd;
      // Boundary analysis: if docfreq==1 and docOrd==1 (meaning we already read ord 0, but not 1), we are done.
      if (leftToRead <= 0) {
        assert(leftToRead == 0);
        docid = PostingsReader::END;
        return docid;
      }

      // Since we only read whole blocks, simply comparing with number of docs left to read is sufficient.
      // If we start partial decoding of blocks (say because of skipping), then we would want something
      // like lastBlockEncodedPosOrd, but for docs.
      if (leftToRead >= Postings::DOCS_BLOCK_SIZE) {
        uint32_t outSz = Postings::DOCS_BLOCK_SIZE;
        auto bytesRead = Postings::docCodec.decodeBlock(docIS.ptr(), docIS.left(), (uint32_t*)docBuf, outSz);
        docIS.skip(bytesRead);
        assert(outSz == Postings::DOCS_BLOCK_SIZE);
        docBufIdx = 0;
        docBufEnd = Postings::DOCS_BLOCK_SIZE;
        // std::cout << "read doc block: " << std::endl;

        // TODO: we should really decode term freqs lazily in case they aren't needed... but this is far simpler for now.
        outSz = Postings::DOCS_BLOCK_SIZE;  // currently parallel to docs, so must be same block size
        bytesRead = Postings::tfreqCodec.decodeBlock(docIS.ptr(), docIS.left(), (uint32_t*)tfreqBuf, outSz);
        docIS.skip(bytesRead);
        assert(outSz == Postings::DOCS_BLOCK_SIZE);
        tfreqBufIdx = 0;
        tfreqBufEnd = Postings::DOCS_BLOCK_SIZE;
        // std::cout << "read tfreq block: " << std::endl;
      } else {
        // decode whole tail?
        // int32_t id = docid;  // PostingsWriter currently uses -1 for tail base, not lastDoc
        int32_t id = -1;
        for (int i=0; i<leftToRead; i++) {
          // see PostingsWriter.endTerm() for format of non-block encoded docs/freqs
          uint32_t doccode = docIS.readVint();
          int32_t tf;
          if ((doccode & 0x01) == 1) {
            tf = 1;
          } else {
            tf = docIS.readVint();
          }
          auto docDelta = ((uint32_t)doccode) >> 1;
          id += docDelta;
          docBuf[i] = id;
          tfreqBuf[i] = tf;
        }

        docBufIdx = 0;
        docBufEnd = leftToRead;
        tfreqBufIdx = 0;
        tfreqBufEnd = leftToRead;
        // std::cout << "read doc/tfreq tail: " << leftToRead << std::endl;

      } // end decode tail
    }

    // NOTE: because tfreq and cumulativeTermFreq are currently directly read used when reading positions,
    // keep these up-to-date for now instead of lazily calculating.
    assert(docBufIdx == tfreqBufIdx);
    assert(docOrd == tfreqOrd);

    docid = docBuf[docBufIdx++];
    docOrd++;

    tfreq = tfreqBuf[tfreqBufIdx++];
    tfreqOrd++;
    posOrdStart = cumulativeTermFreq;
    cumulativeTermFreq += tfreq;

    return docid;
  }


  int32_t advance(int32_t target) {
    while (docid < target) {
      nextDoc();
    }
    return docid;
  }

  void startPositions() {
    pos = -1;
    while (posOrd < posOrdStart) {
      // need to skip some positions.
      auto numToSkip = posOrdStart - posOrd;
      auto leftInBlock = posBufEnd - posBufIdx;

      if (numToSkip <= leftInBlock) {
        // std::cout << "skipping within position block: id=" << docid << " numToSkip=" << numToSkip << std::endl;
        // our start is within current decoded block
        posBufIdx += numToSkip;
        posOrd += numToSkip;
        posBufEndDoc = std::min(posBufIdx + tfreq, posBufEnd);
        return;
      }

      // skip to end of block first.
      // some of these calculations may be redundant, but it's just to make it easier to think about for now.
      // Hopefully optimizer can take care of inefficiencies.
      posOrd += leftInBlock;
      numToSkip -= leftInBlock;
      posBufIdx = posBufEnd;

      if (numToSkip >= Postings::POSITIONS_BLOCK_SIZE) {
        // TODO: skip whole block
        // std::cout << "skipping blocks: id=" << docid << " numToSkip=" << numToSkip << std::endl;
        // For now, just decode the whole block.  Optimize this later.
        uint32_t outSz = Postings::POSITIONS_BLOCK_SIZE;
        auto bytesRead = Postings::posCodec.decodeBlock(posIS.ptr(), posIS.left(), (uint32_t*)posBuf, outSz);
        posIS.skip(bytesRead);
        assert(outSz == Postings::POSITIONS_BLOCK_SIZE);
        posBufIdx = 0;
        posBufEnd = outSz;
        // posBufEndDoc = std::min(posBufEnd, tfreq);  // not needed, we will be skipping the block
        continue;
      }


      // At this point, we need to skip less than a block of positions, but we need to know
      // if it is block encoded or vInt encoded.  Compare to last block encoded ord to tell.
      // Boundary conditions: if posOrdStart=127 then it is the last in the block and we
      // do want to decode the block (hence do tail logic otherwise)
      if (posOrdStart > lastBlockEncodedPosOrd(ttf)) {
        // std::cout << "skipping in position tail: id=" << docid << " numToSkip=" << numToSkip << std::endl;
        for (int i=0; i<numToSkip; i++) {
          auto delta = posIS.readVint();
          unused(delta);
          // TODO: a faster skipVint (potentially)? inlining should already eliminate the dead code though.
        }
        posOrd += numToSkip;
        assert (posOrd == posOrdStart); // nocommit, trivial
        posBufIdx = posBufEnd = posBufEndDoc = 0;
        return;
      }

      // OK load block of positions.  This could be optimized by only loading the relevant part.
      // If this enum wants all positions, we should just decode everything.
      uint32_t outSz = Postings::POSITIONS_BLOCK_SIZE;
      auto bytesRead = Postings::posCodec.decodeBlock(posIS.ptr(), posIS.left(), (uint32_t*)posBuf, outSz);
      posIS.skip(bytesRead);
      assert(outSz == Postings::POSITIONS_BLOCK_SIZE);
      posBufIdx = 0;
      posBufEnd = outSz;
      posBufEndDoc = std::min(posBufEnd, tfreq);
    }
  }

  int32_t nextPosition() {
    if (posBufIdx >= posBufEndDoc) {
      // Reached the end of buffered pos deltas, either because
      // there are no more positions for this doc, or because we need
      // to reload more.

      int leftToRead = (int)(cumulativeTermFreq - posOrd); // should never be larger than 32 bit int

      if (leftToRead <= 0) {
        assert(posOrd == cumulativeTermFreq); // should not have gone past
        // TODO: set pos to something, or keep last valid position?
        pos = PostingsReader::END;
        return pos;
      }

      // we moved to a new doc, but still have positions decoded in the buffer.
      // TODO: it feels like we should move this case to startPositions()
      if (posBufEnd > posBufEndDoc) {
        // just update our new end pointer
        posBufEndDoc = std::min(posBufEnd, posBufIdx+leftToRead);
      } else {
        // if we have more positions to read, then there should be no more buffered
        assert(posBufEndDoc == posBufEnd);

        if (posOrd <= lastBlockEncodedPosOrd(ttf)) {
          // read a new block of positions
          // TODO: refactor reading a new block (not skipping) to one place?
          uint32_t outSz = Postings::POSITIONS_BLOCK_SIZE;
          auto bytesRead = Postings::posCodec.decodeBlock(posIS.ptr(), posIS.left(), (uint32_t*)posBuf, outSz);
          posIS.skip(bytesRead);
          assert(outSz == Postings::POSITIONS_BLOCK_SIZE);
          posBufIdx = 0;
          posBufEnd = outSz;
          posBufEndDoc = std::min(posBufEnd, tfreq);
        } else {
          // we are in the tail
          // we could read position-by-position at the tail if we wanted...
          // we could also handle pulsed positions here instead if sticking them in the buffer.
          posBufIdx = 0;
          posBufEnd = leftToRead; // only read enough for this doc
          posBufEndDoc = leftToRead;
          // std::cout << "FILL pos buffer docid=" << docid << " ords=" << posOrd << " through " << posOrd+tfreq-1 << std::endl;
          for (int i = posBufIdx; i < posBufEnd; i++) {
            posBuf[i] = posIS.readVint();
          }
        }
      }
    }
    // std::cout << "RETN pos buffer docid=" << docid << " posOrd=" << posOrd << " posBufIdx=" << posBufIdx << std::endl;
    auto delta = posBuf[posBufIdx++];
    // We could possibly move posOrd update and only update at block end.
    // Would need to adjust/account for where we started in a block though.
    // And given that there is no data dependency in this hot loop, it's unclear if it would help at all.
    posOrd++;
    pos += delta;
    return pos;
  }

  int32_t advancePosition(int32_t target) {
    while (pos < target) {
      nextPosition();
    }
    return pos;
  }

  // We could also think about exposing the position deltas?
  // Or, we could sum the positions in a block (for a doc) and then it would make bulk access
  // easier / more efficient.
  // Also think about bulk copying of positions when merging?  Would only work for first segment unless
  // we supported multiple position lists.  Hence prob only worth it when merging small segment into large.
  // How to do position skipping?  Could always pre-pend last position in a block?
};

class DocsReader {
  screaming::BitSet bits;
  int32_t ndocs;

public:

  // initialize from docsWithField for the field if it exists
  DocsReader(MemPool &pool, PostingsReader &postingsReader, const SegFieldInfo &fieldInfo) {
    unused(pool);
    ndocs = fieldInfo.docsWithField;

    if (ndocs != postingsReader.numDocs()) {
      InputStream docsWithValIs = postingsReader.getInputStreamSeek(fieldInfo.docsWithFieldEndLoc);
      bits.set( docsWithValIs.ptr() );
    }
  }

  int32_t numDocs() const noexcept {
    return ndocs;
  }

  bool hasBitset() const noexcept {
    return !bits.empty();
  }

  const screaming::BitSet& bitset() const noexcept {
    return bits;
  }
};



} // end namespace