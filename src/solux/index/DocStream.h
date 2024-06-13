#pragma once

#include "Stream.h"
#include "roaring.hh"

namespace solux {

// if for whatever reason we needed to access the string from the given DocStream,
// we could add the string directly following the value
// (only for string types that have the length info in the data itself? perhaps just standardize on that?)


// FUTURE OPTIMIZATION: Use a separate MemPool that is 64 byte aligned (cache line)
// and use the extra space at the end as buffer space.  Instead of using it as the head of the byte stream,
// we can avoid a cache miss by always using it, and copying out the bytes when full.
// Current sizes: Stream=18  DocStream=26  DocFreqStream=34 DocFreqPosStream=56
// So for DocFreqPosStream we would want to round up to 128 bytes.
// DOWNSIDE: indexing unique ids would take up quite a bit more room?  Maybe not too much though since the
// term itself is after the DocStream and also in the cache line.
// Maybe just special case unique terms! All we need to store is a single lastDoc with the term!
//
// Also, if we have a local buffer (not in the linked list), then we don't need the starting small
// buffers to save memory any more!  We could just always use 64 byte chunks in the linked list, and the
// overhead wouldn't be horrible... 4/64==6%.  And if all chunks in the MemPool are 64 byte aligned,
// then so could offsets, and we could address 4GB*64 in one pool with 4 bytes.  We could also investigate
// wasting a little more memory and using a full pointer (8 bytes) to point to the next chunk and potentially
// avoid a miss on the vector of pointers in the MemPool?  Presumably that vector should be hot though
// and a miss should be unlikely?
//


// DocValues that are indexed: can we just record <id><value> pairs in a stream and then index after the fact?
// OR, use the indexed version to drive the docValue writing (esp if it's ord based)
// Hints on whether the values would be unique or not would be very helpful.  It could be a "uniqueness" value on the field?
// With high uniqueness, we would still have to do the inversion work (find all docs with a value), but
// the intermediate storage would be much less and should be more efficient.
// An adaptive solution might be interesting, but complicated.  Could index 1000 terms, and then if the number of
// docs per term is low enough, switch to just recording value+id pairs.  Going from term->docvalues is sort of like
// UnInvertedField in Solr.
// IDEA: implement an iterative RLE for indexed values (no positions / term-freqs)... if delta==1, then just increment a counter.
// otherwise.  possible encoding: leading bit 0: just normal 7 bit delta, leading bits 10: normal vint, leading bits 11: vint followed by count
// count could be just a single byte to save space as well.


/// List of documents (for docs-in-a-term, docs-with-value, etc)
SOLUX_PACKED_START
class DocStream {
public:
  Stream stream;
  int lastDoc;
  int runStart;   // start of the current run
  int prevRunEnd; // end of the previous run


  DocStream(MemPool &pool) : lastDoc(-1), runStart(0), prevRunEnd(-1) {
    unused(pool);
  }

  DocStream(const DocStream &) = delete;

  void operator=(const DocStream &) = delete;

  int32_t getLastDoc() const {
    return lastDoc;
  }

  void addDoc(MemPool &pool, int docid) {
    int delta = docid - lastDoc;
    assert(delta > 0);
    if (delta != 1) {
      // end of a run
      int runSize = lastDoc - runStart + 1;
      int gap = runStart - prevRunEnd;
      // TODO: this will be very inefficient if we add every other doc. Add support for bitmaps?
      // add support for delta coding?
      // This will add a zero-size run at the start that we could avoid but is it worth the extra check in here?
      // We could use low bit of gap to signal runSize==1 if it's common (shift actual gap left by 1)
      // Then the worst case would be 110110110  (gap of 1, runsize of 2, so 2 bytes for every 3 docs)
      // Or we could use 5 bits for start of gap vint and 3 bits for start of run vint.
      stream.writeVInt(pool, gap);
      stream.writeVInt(pool, runSize);
      runStart = docid;
      prevRunEnd = lastDoc;
    }

    lastDoc = docid;
  }

  /// Calls sink.startDoc(int docid) only for each doc
  template <class PostingsConsumer>
  void pushDocs(MemPool& pool, PostingsConsumer& sink) {
    int runPtr = -1;

    StreamReader dstream(stream, pool);
    while (!dstream.eof()) {
      int gap = dstream.readVint();
      int runSize = dstream.readVint();
      runPtr += gap;
      for (int i = 0; i<runSize; i++) {
        sink.startDoc(runPtr + i);
      }
      runPtr += runSize - 1;  // next gap is from the end of this run
    }

    // handle last run
    for (int docid = runStart; docid <=lastDoc; docid++) {
      sink.startDoc(docid);
    }
  }

  /// Calls f(int docid) for each doc.
  /// The pool passed here must be the same pool used to build the stream.
  template <class F>
  void forEachDoc(MemPool& pool, F f) {
    int runPtr = -1;

    StreamReader dstream(stream, pool);
    while (!dstream.eof()) {
      int gap = dstream.readVint();
      int runSize = dstream.readVint();
      runPtr += gap;
      for (int i = 0; i<runSize; i++) {
        f(runPtr + i);
      }
      runPtr += runSize - 1;  // next gap is from the end of this run
    }

    // handle last run
    for (int docid = runStart; docid <=lastDoc; docid++) {
      f(docid);
    }
  }


  // TODO: add a more specific PostingsConsumer that can communicate runs to a compressed bitset builder.


} SOLUX_PACKED_END;

SOLUX_PACKED_START
class DocFreqStream {

public:


  Stream docs;
  int lastDoc;
  int docFreq;
  int lastDocCode;
  int termFreq;

  explicit DocFreqStream(int docid) : lastDoc(docid), docFreq(1), lastDocCode(docid << 1), termFreq(1) {
  }

  DocFreqStream(const DocFreqStream &) = delete;

  DocFreqStream(DocFreqStream &&) = delete;


  void addDoc(MemPool &pool, int docid) {
    int delta = docid - lastDoc;
    if (delta == 0) {
      // same document
      ++termFreq;
    } else {
      // new document... first write doc+freq for prev document
      int code = termFreq != 1 ? lastDocCode : (lastDocCode | 0x01);
      docs.writeVInt(pool, code);
      if (termFreq != 1) {
        docs.writeVInt(pool, termFreq);
      }

      // now handle stuff for new doc
      termFreq = 1;
      lastDocCode = delta << 1;
      lastDoc = docid;
      ++docFreq;
    }
  }
}
  SOLUX_PACKED_END;

// TODO: consider indexing payloads as separate type? (separate index options)
// That would save 1 bit per position.
// OPTIMIZATION TODO: somehow consolidate the docs and positions streams!
//   - if we don't consolidate we can't use a single local buffer (see optimization ideas at the top)
//   - if we use a single stream, it does mean that when reading back, we won't know how many positions
//     there are without iterating over them.  The postings writer would have to be written with that in mind.
//   - consolidating the streams would also shrink this by 18 bytes and allow that much more space in the cache line.
//   - we also don't necessarily need to store termFreq (writing it last does no good)
//   - likewise, can we get rid of tracking docFreq as well?  Does the postings writer need it ahead of time for some reason?
//     For now, try to write the postings writer without that info.
//   - How does hyperthreading (which can switch to a diff thread on a cache miss) play into this? My guess is we would
//     want to avoid the contention.

SOLUX_PACKED_START
class alignas(1) DocFreqPosStream {
  Stream docs;
  Stream positions;
  int lastDoc;      // we only write lastDoc when we receive a new doc
  int lastDocDelta;
  int docFreq;      // we don't strictly need to track this unless we need to know for some reason.  we can read to end of doc stream.
  int termFreq;
  int lastPos;      // just for calculating deltas... we always write positions to the stream as we get them.


  void writePos(MemPool &pool, int pos) {
    int posCode = pos - lastPos;
    assert(posCode >= 0);
    // TODO: do we need to support duplicate positions for the same term for the same doc???  Would seem to make search code more complex.
    positions.writeVInt(pool, posCode);
    lastPos = pos;
  }

public:

  // todo: support positions > 2B?  Not useful?  Perhaps support with a special marker in the stream (like a 0 length payload that means
  // read a vint and multiply that by 2B and add it to the delta

  DocFreqPosStream(MemPool &pool, int docid, int pos) : lastDoc(docid), lastDocDelta(docid), docFreq(1),
                                                        termFreq(1), lastPos(pos) {
    lastPos = -1;
    writePos(pool, pos);
  }

  DocFreqPosStream(const DocFreqPosStream &) = delete;
  DocFreqPosStream(DocFreqPosStream &&) = delete;

  void addDoc(MemPool &pool, int docid, int pos) {
    int delta = docid - lastDoc;
    if (delta == 0) {
      // same document
      ++termFreq;
      writePos(pool, pos);
    } else {
      // new document... first write doc+freq for prev document
      int code = termFreq == 1 ? (lastDocDelta<<1)|0x01 : lastDocDelta<<1;
      docs.writeVInt(pool, code);
      if (termFreq != 1) {
        docs.writeVInt(pool, termFreq);
      }

      // now handle stuff for new doc
      ++docFreq;
      termFreq = 1;
      lastDoc = docid;
      lastDocDelta = delta;
      lastPos = -1;
      writePos(pool, pos);
    }
  }

  template <class PostingsConsumer>
  void pushDocs(MemPool& pool, PostingsConsumer& sink) {
    StreamReader dstream(docs, pool);
    StreamReader pstream(positions, pool);
    int docid = 0;  // pass in base or have external user add if needed?
    int handledIds = 0;
    bool readLastId = false;
    do {
      int tf;
      if (!dstream.eof()) {
        uint32_t docCode = (uint32_t) dstream.readVint();
        int docDelta = docCode >> 1;
        docid += docDelta;
        tf = (int)docCode & 0x01;
        if (tf != 1) {
          tf = dstream.readVint();
        }
      } else {
        docid = lastDoc;
        tf = termFreq;
        readLastId = true;
      }
      handledIds++;  // just sanity check

      sink.startDoc(docid);
      for (int i=0; i<tf; i++) {
        int posCode = pstream.readVint();
        sink.addPositionDelta(posCode);
      }
      sink.endDoc(docid);

    } while (!readLastId);

    assert(pstream.eof());
    assert(handledIds == docFreq);
  }

  friend std::ostream &operator<<(std::ostream &out, const DocFreqPosStream &obj) {
    return out << "DocFreqPosStream(lastDoc=" << obj.lastDoc
               << ",lastPos=" << obj.lastPos
               << ')';
  }
}
SOLUX_PACKED_END;



// list of integers
SOLUX_PACKED_START
class IntStream {
public:
  Stream storage;
  int32_t lastVal;

  IntStream(MemPool &pool) : lastVal(0) {
    unused(pool);
  }

  IntStream(MemPool &pool, int32_t val) : lastVal(val) {
    storage.writeVInt(pool, val);
  }

  IntStream(const IntStream &) = delete;
  void operator=(const IntStream &) = delete;

  void addVal(MemPool &pool, int32_t val) {
    // XOR with previous value will remove common high bits (including high bits of successive negative values)
    // TODO: should we calculate other statistics at this point (min, max, gcd?)
    int32_t code = lastVal ^ val;
    storage.writeVInt(pool, code);
    lastVal = val;
  }

  /// Calls sink.addInt32(int32_t val)
  template <class PostingsConsumer>
  void pushValues(MemPool& pool, PostingsConsumer& sink) {
    StreamReader vstream(storage, pool);
    int32_t prev = 0;
    while (!vstream.eof()) {
      int32_t code = vstream.readVint();
      int32_t val = prev ^ code;
      prev = val;
      sink.addInt32(val);
    }
  }


} SOLUX_PACKED_END;


SOLUX_PACKED_START
class IntDeltaStream {
public:
  Stream storage;
  int32_t lastVal;

  IntDeltaStream(MemPool &pool) : lastVal(0) {
    unused(pool);
  }

  IntDeltaStream(MemPool &pool, int32_t val) : lastVal(val) {
    storage.writeVInt(pool, val);
  }

  IntDeltaStream(const IntStream &) = delete;
  void operator=(const IntStream &) = delete;

  void addVal(MemPool &pool, int32_t val) {
    assert(val >= lastVal);
    int32_t code = val - lastVal;
    storage.writeVInt(pool, code);
    lastVal = val;
  }

  /// Calls sink(int32_t val)
  template <class PostingsConsumer>
  void pushValues(MemPool& pool, PostingsConsumer&& sink) {
    StreamReader vstream(storage, pool);
    int32_t prev = 0;
    while (!vstream.eof()) {
      int32_t code = vstream.readVint();
      int32_t val = prev + code;
      prev = val;
      sink(val);
    }
  }
} SOLUX_PACKED_END;

// list of integers
SOLUX_PACKED_START
class LongStream {
public:
  Stream storage;
  int64_t lastVal;

  LongStream(MemPool &pool) : lastVal(0) {
    unused(pool);
  }

  LongStream(MemPool &pool, int64_t val) : lastVal(val) {
    storage.writeVInt(pool, val);
  }

  LongStream(const LongStream &) = delete;
  void operator=(const LongStream &) = delete;

  void addVal(MemPool &pool, int64_t val) {
    // XOR with previous value will remove common high bits (including high bits of successive negative values)
    // TODO: should we calculate other statistics at this point (min, max, gcd?)  Seems like yes because we would
    // have this info when merging segments, but we won't for the initial segment (because it will be built
    // incrementally when reading from this stream, not buffered completely in memory.)
    int64_t code = lastVal ^ val;
    storage.writeVLong(pool, code);
    lastVal = val;
  }

  /// Calls sink.addInt64(int64_t val)
  template <class PostingsConsumer>
  void pushValues(MemPool& pool, PostingsConsumer& sink) {
    StreamReader vstream(storage, pool);
    int64_t prev = 0;
    while (!vstream.eof()) {
      int64_t code = vstream.readVlong();
      int64_t val = prev ^ code;
      prev = val;
      sink.addInt64(val);
    }
  }



} SOLUX_PACKED_END;



} // end namespace
