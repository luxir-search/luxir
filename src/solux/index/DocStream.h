#pragma once

#include <stdexcept>

#include "Stream.h"

namespace solux {

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

  /// Calls sink.startDoc(int docid) only for each doc.
  /// pool is the source pool that was used to build the stream.
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
    if (posCode <= 0) {
      throw std::runtime_error("Term positions must be strictly increasing within a document");
    }
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
      if (pos == lastPos) {
        return;  // canonicalize duplicate (term, doc, position) occurrences
      }
      if (pos < lastPos) {
        throw std::runtime_error("Term positions must be strictly increasing within a document");
      }
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

  class Reader {
    StreamReader stream;
    int32_t previous = 0;

  public:
    Reader(const IntStream& source, const MemPool& pool) : stream(source.storage, pool) {
    }

    bool eof() const {
      return stream.eof();
    }

    int32_t next() {
      assert(!eof());
      int32_t value = previous ^ stream.readVint();
      previous = value;
      return value;
    }
  };

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
    Reader reader(*this, pool);
    while (!reader.eof()) {
      sink.addInt32(reader.next());
    }
  }

  // lambda / callable version
  void visitValues(MemPool& pool, auto&& sink) {
    Reader reader(*this, pool);
    while (!reader.eof()) {
      sink(reader.next());
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
    visitValues(pool, [&](int64_t value) { sink.addInt64(value); });
  }

  void visitValues(MemPool& pool, auto&& sink) {
    StreamReader vstream(storage, pool);
    int64_t prev = 0;
    while (!vstream.eof()) {
      int64_t code = vstream.readVlong();
      int64_t val = prev ^ code;
      prev = val;
      sink(val);
    }
  }



} SOLUX_PACKED_END;



} // end namespace
