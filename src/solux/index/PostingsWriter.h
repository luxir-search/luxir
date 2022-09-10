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
#include "solux/search/PostingsReader.h"
#include "simdcomp/include/codecfactory.h"
#include "roaring.hh"
#include "ScreamingBuilder.h"


/**
 * The high level strategy is to write the lowest levels first since higher level information needs/points to that info.
 *
 * For all terms in a field:
 *   For all documents containing that term:
 *     For all positions in that document:
 *       - Write all the positions and remember the number as termfreq
 *     - Write the termdoc info (doc,termfreq,pointer_to_positions) and remember the number as docfreq
 *   - Write the term info (term,docfreq,pointer_to_docs)
 */



// Reference for lucene's posting format:
// https://lucene.apache.org/core/8_3_0/core/org/apache/lucene/codecs/lucene50/Lucene50PostingsFormat.html
// https://lucene.apache.org/core/8_6_0/core/org/apache/lucene/codecs/

// TODO: consider using roaring bitmaps for high density docids?  Requires knowing the approx docfreq up front?
// Would be great for index-term based faceting!
// presumably not for positions since they would almost never be dense?  Although it would be good for parallel field query (color:blue size:large in same position)
// It might make it harder to seek to the positions for any document as well (depends on iteration speed since for a given docId, we need to find
// it's relative position in the ID list, so we can match that relative position in the positions list.  Maybe reserve for fields that don't
// index positions, or have it as an additional option for fields that do.
// https://github.com/RoaringBitmap/CRoaring   https://arxiv.org/abs/1709.07821
// http://db.ucsd.edu/wp-content/uploads/2017/03/sidm338-wangA.pdf (comparison between bitmaps and inverted list compression)
//   TODO: where is the source code for the alternate versions

// TODO: in debug mode, keep track of how much space everything takes?
// We should try to enable this in non-debug mode too... CheckIndex type of func.

// We can't rely on exact index stats (docfreq) from the input, since documents may be deleted.  We won't know until we iterate over the postings
// and match against deleted documents (when merging)


// How to handle different indexing styles (positions vs not?)
// templatize that somehow?



// Term Index Ideas:
// Do a simple binary search for term first?
// - strip off common prefix
// - pad all starts out to 4 bytes to do a quick compare?
//    - maybe even have a dense int[] of these...
//       - if adjacent bucket has same value, then we know we need to look further.  Could have an exception mechanism for additional chars,
//         or could simply look directly at the terms list.
//       - for that matter... we could start by doing a binary search directly on the terms dictionary w/o an index!
//         - just need a block number -> offset map.  How to compress the offsets?
//            -- ideas: align blocks.  interpolation.  can we assume term index will not be > 32 bits??? calculate max size of block and max number of terms.
//                  - important to use exceptions for long strings if we do this as well to cap the max size of a block.
//    - investigate a serialized hash table?
// - have an exception mechanism for long terms, so we get better locality?  where would they be written though?  Another file?
// - We could have some internal skipping if for every term that shares no prefix bytes with the previous term, we encode the offset of the next
// term like that.  Unclear how often it would be useful (i.e. only when the starting term mismatches with the seek term)
//     - actually... at the beginning of the block, we could also store the number of shared prefix bytes for the whole block N.  then we could
//       point to the term which mismatches at term N+1
//       - could this be generalized more? #terms that share N additional bytes (we can skip ahead if the seek term is greater than that number of bytes
//         - could we include enough info that it would be redundant with the number of bytes shared from the prev term?
//         - if we completely generalized it and had large block sizes... this seems very much like a trie/fst? ( esp if the number of shared terms is a delta
//           over the last.)
//         - could also optionally put a hash/jump table at the start of the block based on the next char/substring.
// - being able to skip forward sort of requires encoding backwards so you can encode offsets?
// - STORE a minimum term length.  All additional lengths are implied to be in excess of this.  This will help with fixed-length things like UUIDs!
// - could also add a second level to the terms (like a skip list)... encode every 8th or 16th term as if they were adjacent.
//    - or multi-level skip list...  store ords (0,64) then (0+32,64+32), then (0+16,0+32+16,64+16,64+32+16).  Each term would be encoded relative to
//      the term to it's left AT THE SAME LEVEL.  Also have an offset index (which could now actually be effectively utilized) to skip to the lowest level
//      block.  For example, if we've deduced that the term we want is greater than 64, but less than 64+16, how do we seek to term #65?
//      We also want to be able to skip the skip pointers themselves.
//      There is another way to do the skip pointers... depth first rather than breadth first (0,64), (0+32), (0+16), (leaf 1-15), (32+16), (leaf 17-31), ...
//      Depth first may be a little easier to understand, but breadth first may have better locality at the top of the tree.
//      An index per-doc for skip terms, and an index per mini-block start for leaf terms?  Or, with each skip term, could put an offset of where to go for the
//      term to it's right.  The term to it's left would always be adjacent (next), but since that hasn't been written yet, it would be hard to put inline.
//      For depth-first encoding, just encode all the terms in order (including skip terms), but on a skip term, encode it relative to the last on it's own level.
//      May be easiest to start with.  Encoding and full decoding will thus need a stack of states, but seeking would not.  It would slow down bulk decoding / scanning
//      a little, but still prob worth it given weight on lookups.
//      - pulsing: inline (with a good way of skipping it all) or reference the Nth pulsed term (or delta) and look up in separate space?
//        - for docs-only, might be a good place to use a 2 bit code to indicate number of additional bytes... then can do branchless skip.  Also doable
//          with doc+pos, but slightly more complicated.  What bit/code indicates pulsing though?  doc-position delta of 0?  We can make this delta smaller
//          using a small amount of alignment (4 byte alignment means single vint byte stores length to 512 instead of 128... likewise, can add a minimum
//          known block size.  Ensure padding so we can always read a full 4 bytes and mask it off rather than byte at a time.
//      - for mini-block offsets, try to find something that doesn't need to be decoded?
//      - defer other block metadata decoding in case the term isn't found (which will be the common case for IDs in a multi-segment index?)
//      - also record the max(prefix_from_prev_term)? feels like this could be used to optimize how much of previous term to copy when moving to next?
//         -- single byte if we move to 8 bit term length.
//      - things like docfreq, position offset, etc, can be in the docs file to make the terms more compact.... but the base can be in the terms block
//        - block encoding (pfordelta) would make for smaller index overall, but increase the size of the terms data and require decoding the block.
//          - we could also still encode position offsets, but put them in the positions file (after all of the positions)
//
// - TODO: make fixed block size adjustable and store in index as power of two (2^N)

// What is stored in TermBlockHeader:
//    full starting term, block_prefix_size, max_term_size?, min_suffix_size (useful? would reduce length bytes needed for many large terms (uncomressed uuid?)
//    starting_ordinal, number of terms in this block (needed for variable sized blocks only... starting ordinal could be repositioned to allow binary search)
//    docsFP, posFP, payFP        // postings-metadata:  even if we chose not to store metadata in the terms block, these can still be used as a base to delta-code.
//    docDeltaMin // this could be figured out at compile time (the theoretical minimum), or could be the minimum for this block.
// encoding: varint-GB (TODO: try SSE "stream vbyte")  OR provide a way to skip unneeded metadata (anything not needed for a miss)
// How to get to the index (or any other data written after the full block has been written?)
//   - could index from the end of the block.  Have some metadata at the start and some at the end?
//     Or could put all metadata at the end.  If the string still points at the start of the block, we could be passed the end of the block by looking
//     at the start of the next block.
//     Or we could just buffer the complete block. No terms block should be overly large.
//

// Docs file:
// Lucene interleaves blocks of documents and frequencies. Lucene does not encode a positions offset for each document... instead
// the termfreq is summed for all documents through the current document.  This is used to calculate how many positions need to be
// skipped (there are no delimiters between positions of different documents for the same term.)
// FUTURE: we could investigate putting positions from different terms together as well.


//   See Lucene84PostingsWriter.
// What if we wanted to use roaring bitset for docs?  How to find the freqs?
// don't need skip data for the docs, but still would for freq_start & position_start.
// buffer the whole freq output and put it after the docs?  Or put it in the positions file? Or somewhere else?
// FIRST ITERATION: use whatever we would for leftover small enough to not encode in a block.
//   <doc_delta_code>  // doc delta or

namespace solux {

class PostingsWriter {
  Directory& directory;
  std::string generation;
  int32_t maxDoc;  // set by caller (IndexWriter)
public:
  MemPool pool;

//
// variable naming:
// *loc* refers to absolute locations in a file, usually obtained via OutputStream::size()
// *off* / *offset* refers to offsets relative to something else (i.e. a location)
//

  // TODO: pool allocate this
  std::vector<char> compressed_output;

  OutputStream segOutput;     // output stream for segment info file
  OutputStream fieldOutput;   // output stream for field info
  OutputStream termOutput;    // output stream for termFile
  OutputStream docOutput;     // output stream for docFile
  OutputStream posOutput;     // output stream for posFile
  OutputStream colOutput;     // output stream for columns

  std::unique_ptr<File> segFile; // segment info
  std::unique_ptr<File> fieldFile; // list of fields
  std::unique_ptr<File> termFile; // terms for each field
  std::unique_ptr<File> docFile; // documents for each term
  std::unique_ptr<File> posFile; // positions for each term
  std::unique_ptr<File> colFile; // used for columns


  // needed to build each block
  std::vector<TermRef> termList;  // list of terms in the current term block
  std::vector<uint32_t> docFileSize;         // size of the data in the docs file for this term (TODO: can we guarantee that this isn't bigger than 2B or 4B?)
  std::vector<int32_t> pulsed; // if docFileSize==0, then the term has a single doc/pos that is pulsed, and those values are the next in this list.

  // needed for each term
  std::vector<int32_t> docs; // list of documents containing a term
  std::vector<int32_t> tfreqs; // term freqs - number of times the term appears in each document (parallel vector to "docs")
  std::vector<int32_t> posdeltas; // list of position deltas for the current term (for all documents... per-document positions are not delimited)


  int64_t locOfPositionsForTermBlock;
  int64_t locOfDocsForTermBlock;
  int64_t locOfPositionsForTerm;
  int64_t locOfDocsForTerm;

  int64_t positionsHandled;  /// number of positions handled for the current term so far (everything except posdeltas)
  int32_t docsFlushed;  /// number of documents flushed for the current term so far
  int64_t totalTermFreqPrevDoc = 0; // total term freq up through the previous doc

  std::vector<uint64_t> termBlockOffsets;  // offset from termsOffset (for this field) for each term block


    // Should this be refactored into a class?
  struct FieldInfo {
    std::string fieldName;
    int64_t termBlockIndexLoc;
    int64_t termsLoc;
    int64_t docsLoc;
    int64_t posLoc;
    int64_t sumDocFreq;
    int64_t sumTotalTermFreq;
    int numTerms;  // currently only updated in flushTerms()

    // column
    int64_t columnLoc;
    int64_t docsWithValueEndLoc;
    int32_t docsWithValue;

    int32_t flags;  // temporary... currently has type info. 0x01 text, 0x02 int col.  In the future, we should decompose and have separate sections for each type
  };
  FieldInfo* fieldInfo;

  std::vector<FieldInfo> fieldInfos;

private:  // some internal utility methods... not for use by indexers
  Postings postings; // contains limits and codecs

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
  PostingsWriter(Directory& dir, const std::string_view& gen, int32_t maxDoc) : directory(dir), generation(gen), maxDoc(maxDoc)
  {
    // TODO: defer file creation until needed, *or* use a RAMDelegatingFile that does so.
    // that does so.
    segFile    = directory.createFile(Postings::getIndexFileName(gen, Postings::SEGMENT_INFO_FNAME));
    fieldFile  = directory.createFile(Postings::getIndexFileName(gen, Postings::FIELDS_FNAME));
    termFile   = directory.createFile(Postings::getIndexFileName(gen, Postings::TERMS_FNAME));
    docFile    = directory.createFile(Postings::getIndexFileName(gen, Postings::DOCS_FNAME));
    posFile    = directory.createFile(Postings::getIndexFileName(gen, Postings::POS_FNAME));
    colFile    = directory.createFile(Postings::getIndexFileName(gen, Postings::COL_FNAME));

    segOutput.setFile(segFile.get());
    fieldOutput.setFile(fieldFile.get());
    termOutput.setFile(termFile.get());
    docOutput.setFile(docFile.get());
    posOutput.setFile(posFile.get());
    colOutput.setFile(colFile.get());

    // TODO: if we hit an error, should we clean up any files?
  }

  void finish() {
    writeFieldIndex();
    writeSegmentInfo();
    // TODO: implement compound files for small files

    // minor optimization here - we close the files in sorted order to trigger the
    // optimization in RAMDir
    colOutput.close();
    directory.finishFile(*colFile);
    docOutput.close();
    directory.finishFile(*docFile);
    fieldOutput.close();
    directory.finishFile(*fieldFile);
    posOutput.close();
    directory.finishFile(*posFile);
    segOutput.close();
    directory.finishFile(*segFile);   // TODO... should this be last, or is there a higher level sync mechanism to prevent reading seg file before other files are written?
    termOutput.close();
    directory.finishFile(*termFile);

  }

  void setMaxDoc(int max) {
    maxDoc = max;
  }

  int32_t getMaxDoc() {
    return maxDoc;
  }


  // Currently only called for a full block of positions.
  // TODO: move to .cpp unless we template this class... allows for removal of the associated include files
  void flushPositions() {
    if (posdeltas.empty()) {
      return;
    }
    compressed_output.resize(Postings::POSITIONS_BLOCK_SIZE * sizeof(int32_t) + 1024);
    uint32_t compressedSize = compressed_output.size(); // this gets changed to the actual size
    postings.posCodec.encodeBlock(reinterpret_cast<uint32_t *>(posdeltas.data()), posdeltas.size(), compressed_output.data(),
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

    assert(docs.back() < maxDoc); // sanity check to ensure we didn't go over provided maxDoc

    // NOTE: some codecs (like s4-fastpfor-d1) modify the input array to calculate deltas!
    // given that we (could) already have deltas, is there an easy way to bypass that part?
    // NOTE: SIMDCompressionAndIntersection puts 32 bit size at start!  Look at C version and see if it's easier to modify?
    // The simdcomp C library does have lower level interfaces that just handle a single 128 value block

    compressed_output.resize(Postings::DOCS_BLOCK_SIZE * sizeof(int32_t) + 1024);
    uint32_t compressedSize = compressed_output.size(); // this gets changed to the actual size
    postings.docCodec.encodeBlock(reinterpret_cast<uint32_t *>(docs.data()), docs.size(), compressed_output.data(),
                      compressedSize);
    docOutput.write(compressed_output.data(), compressedSize);

    //
    // now the term freqs
    //
    compressed_output.resize(Postings::TERMS_BLOCK_SIZE + 1024);
    compressedSize = compressed_output.size(); // this gets changed to the actual size
    postings.tfreqCodec.encodeBlock(reinterpret_cast<uint32_t *>(tfreqs.data()), tfreqs.size(), compressed_output.data(),
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

    termBlockOffsets.push_back( termOutput.size() - fieldInfo->termsLoc);
  }

  void flushTerms(bool endingField) {
    if (termList.empty()) {
      termBlockOffsets.pop_back();  // last block has no terms in it.
      return;
    }

    fieldInfo->numTerms += termList.size();

    // TODO: find common prefix (i.e. min_prefix_len) for all terms in block and strip it off (same as common prefix of first and last)
    // important for some things that share long prefixes, like URLs for example.
    // TODO: store min term size (or minimum suffix length) and then code the suffix lengths as additional to that? Would help indexing things like text uuids.
    // Store max term size to help optimize readers?

    TermRef reference = termList[0];
    auto [refdata, reflen] = reference.unpack();

    // Write the terms block header.
    termOutput.writeStr(refdata, reflen);
    termOutput.writeVlong(locOfDocsForTermBlock - fieldInfo->docsLoc);
    termOutput.writeVlong(locOfPositionsForTermBlock - fieldInfo->posLoc);

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
        // suffix length (otherwise it would be the same term)
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
      // a doc block from it's tail anyway.  We could save a little space (smaller doc skipping index) if we didn't need
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


  void startTerm(TermRef term) {
    termList.push_back(term);  // we don't really need the term name at this point (could add in endTerm), but it might be nice for debugging / exceptions?
    docsFlushed = 0;
    positionsHandled = 0;
    locOfPositionsForTerm = posOutput.size();
    locOfDocsForTerm = docOutput.size();
  }

  void endTerm(TermRef term) {
    unused(term);
    auto totalTermFreq = getTotalTermFreq();
    if (docs.size() > 0) {
      assert(docs.back() < maxDoc); // sanity check to ensure we didn't go over provided maxDoc
    }
    // TODO: handle case when all docs were deleted for term (and term should no longer appear)
    if (totalTermFreq == 1) {
      assert(getDocFileSize()==0 && docs.size()==1 && posdeltas.size() == 1);
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
      // TODO: make first delta an actual delta from the last block... not from 0.  Not too important though given that that this is only sub-optimal
      //   when the docfreq is larger than the doc block size.
      int lastdoc = 0;
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

  void startField(const std::string& fieldName) {
    fieldInfo = &fieldInfos.emplace_back();
    fieldInfo->fieldName = fieldName;
    fieldInfo->termsLoc = termOutput.size();
    fieldInfo->docsLoc = docOutput.size();
    fieldInfo->posLoc = posOutput.size();
    fieldInfo->numTerms = 0;
    fieldInfo->flags = 0x01;  // text field


    termBlockOffsets.resize(0);

    _startTermBlock(false);
  }


  void endFieldTerms(const std::string& fieldName) {
    // OPT: investigate inlining small fields in the terms index instead of pointing out to other files?  If we don't know how large the field will be,
    // we could always do it after-the-fact if the other outputs are rewindable (i.e. all in memory.)  If not, we could make it so by always starting
    // with new outputs for every field with first page in RAM.
    // OPT: For many fields, the field index and the terms index should perhaps have the same structure (prefix compressed blocks?)
    flushTerms(true);

    // write index into the blocks of the terms dict
    // TODO: use a more efficient encoding for this array
    //   - make offsets be from the start of this index array... 32 bit normally fine, but not always for huge field?
    //   - sequence will be monotonically increasing (or decreasing)... interpolate?
    // Indexing RAM OPT: for fields with huge number of terms, we could stream this to separate file.  That would also facilitate alignment if it's important.
    fieldInfo->termBlockIndexLoc = termOutput.size();
    assert((int)termBlockOffsets.size() == ((fieldInfo->numTerms-1) / Postings::TERMS_BLOCK_SIZE) + 1);
    termOutput.write(&(termBlockOffsets[0]), termBlockOffsets.size() * sizeof(termBlockOffsets[0]) );
  }

  void endField(const std::string& fieldName) {
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




private:
  void writeSegmentInfo() {
    assert(maxDoc >= 1);
    segOutput.writeVint(maxDoc);
    // Other info we should eventually write: version info, what other files are present, cfs info
  }

  void writeFieldIndex() {
    //
    // This format is position independent.  One just needs a pointer to the end of the final structure.
    //
    // Format:
    // List of field metadata, followed by an array of offsets for each field, followed by the number of fields.
    //

    std::vector<uint32_t> fieldOffs;  // location of each field in fieldFile (TODO: what is the max number of fields we will support?)
    fieldOffs.reserve(fieldInfos.size());

    auto fieldsStart = fieldOutput.size();  // where this index starts

    for (auto& finfo : fieldInfos) {
      auto fieldLoc = fieldOutput.size();
      fieldOffs.push_back(fieldLoc - fieldsStart);  // make the location relative so we can append this to a large file if necessary

      fieldOutput.writeStr(finfo.fieldName);

      if ((finfo.flags & 0x01) != 0) {
        fieldOutput.writeVint(0x01);

        fieldOutput.writeVlong(finfo.termBlockIndexLoc);
        fieldOutput.writeVlong(
                finfo.termsLoc);  // TODO: If we change termBlockOffsets to be relative to the start of that index, we can remove termsLoc
        fieldOutput.writeVlong(finfo.docsLoc);
        fieldOutput.writeVlong(finfo.posLoc);
        fieldOutput.writeVint(finfo.numTerms);
      }

      if ((finfo.flags & 0x02) != 0) {
        fieldOutput.writeVint(0x02);
        fieldOutput.writeVlong(finfo.docsWithValue);
        fieldOutput.writeVlong(finfo.docsWithValueEndLoc);
        fieldOutput.writeVlong(finfo.columnLoc);
      }
    }

    // Now write the start of each fieldInfo
    // TODO: align this on 4 byte boundary
    // Now make field offsets relative to the start of the locations array instead of the beginning of fields.
    // It's minor, but allows us to remove another pointer (to the start of the fields)
    auto locationsOff = fieldOutput.size() - fieldsStart;
    for (auto& loc : fieldOffs) {
      loc = locationsOff - loc;
    }

    fieldOutput.write(&(fieldOffs[0]), fieldOffs.size() * sizeof(fieldOffs[0]));
    // write the size of the array at the end so we can use it to find the start when reading
    fieldOutput.writeInt((int32_t)fieldOffs.size());
  }

  friend class IntColWriter;
};




class TextWriter {
public:




};




//
// Integer column writing
// TODO: nest these within postings writer? Or use a namespace?
//
class IntColWriter {
  MemPool& pool;
  PostingsWriter& postingsWriter;
  PostingsWriter::FieldInfo& fieldInfo;
  IntColStats* stats;
  OutputStream& colOutput;
  ScreamingBuilder docsWithVal;
  int64_t colStart;
  int64_t idEndLoc;
  int32_t nAdded = 0;            // number of values added. redundant with numDocsWithValue, for sanity check
public:

  // This field writer does not do any visible pool rollbacks, but does allocate from the pool.
  IntColWriter(MemPool& pool, PostingsWriter& postingsWriter, PostingsWriter::FieldInfo& fieldInfo) : pool(pool), postingsWriter(postingsWriter), fieldInfo(fieldInfo), colOutput(postingsWriter.colOutput),
                                                                docsWithVal(pool, colOutput) {
    // TODO: docsWithVal allocates 17K from pool that may not be used... should we try to delay this somehow? (an explicit init function?)
    // Perhaps the indirection associated with delaying the ScreamingBuilder construction would be optimized away since startDoc() would be
    // called in a tight loop.
    fieldInfo.flags = 0x02;  // int col
  }

  // refine these APIs as we get more use-cases (like segment merging)

  // The passed fieldStats should remain valid until after endField is called.
  // Should this just be folded into the constructor?
  void startFieldIntCol(const std::string& fieldName, IntColStats& fieldStats) {
    // column writing could be parallelized better by using multiple files and grabbing a free file at this point.
    stats = &fieldStats;
    colStart = colOutput.size();
  }


  void addDocsWithVal(roaring::Roaring& roaring) {
    // numDocsWithValue = roaring.cardinality();
    auto frozenSize = roaring.getFrozenSizeInBytes();
    auto bufSize = frozenSize + 31; // need space to align
    std::vector<char> buf(bufSize);  // TODO: replace with something that can write directly to our output streams
    void* buffer = buf.data();
    buffer = std::align(32, frozenSize, buffer, bufSize);
    roaring.writeFrozen((char*)buffer);
    idEndLoc = colOutput.size();
    // TODO: what alignment requirements do we have for reading?
    colOutput.write(buffer, frozenSize);
  }

  void startDocsWithValue() {
  }

  // target for DocStream.pushDocs...  currently only for recording what docs have a value.  Should all be done
  // at once (not interleaved with addInt*)
  void startDoc(int32_t docid) {
    docsWithVal.add(docid);
  }

  void endDocsWithValue() {
  }


  void addInt64(int64_t val) {
    nAdded++;
    // temporary worst-case implementation with no compression
    colOutput.writeLong(val);
  }

  void endField(const std::string& fieldName) {
    assert(nAdded == stats->numVals());
    bool allDocsHaveValue = nAdded == postingsWriter.maxDoc;

    // write any necessary index into encoded blocks here (assuming it's small enough to keep in memory)

    assert(allDocsHaveValue || docsWithVal.cardinality() == nAdded); // TODO: turn into actual exception
    // TODO: write pointers (or add pointers to list to later be serialized)

    if (!allDocsHaveValue) {
      auto bytes = docsWithVal.flush();
      idEndLoc = colOutput.size();
    } else {
      idEndLoc = 0; // just to avoid reading uninitialized values
    }

    // FUTURE:write index into value blocks here
    fieldInfo.docsWithValue = nAdded;
    fieldInfo.docsWithValueEndLoc = idEndLoc;
    fieldInfo.columnLoc = colStart;
  }
};





} // end namespace