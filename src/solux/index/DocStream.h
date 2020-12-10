#pragma once

#include "Stream.h"


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


// Documents matching a term
SOLUX_PACKED_START
class DocStream {
public:
  Stream docs;
  int lastDoc;
  int docFreq;  // number of docs with this term

  DocStream(MemPool& pool, int docid) : lastDoc(docid) , docFreq(1) {
  }
  DocStream(const DocStream&) = delete;
  void operator=(const DocStream&) = delete;

  void addDoc(MemPool& pool, int docid) {
    int delta = docid - lastDoc;
    assert(delta >= 0);
    if (delta != 0) {
      assert (docid > lastDoc);
      docs.writeVInt(pool, delta);
      ++docFreq;
      lastDoc = docid;
    }
  }
}
SOLUX_PACKED_END;

SOLUX_PACKED_START
class DocFreqStream {

public:


  Stream docs;
  int lastDoc;
  int docFreq;
  int lastDocCode;
  int termFreq;

  explicit DocFreqStream(int docid) : lastDoc(docid) , docFreq(1), lastDocCode(docid<<1), termFreq(1) {
  }
  DocFreqStream(const DocFreqStream&) = delete;
  DocFreqStream(DocFreqStream&&) = delete;



  void addDoc(MemPool& pool, int docid) {
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
class DocFreqPosStream {
public:
  Stream docs;
  Stream positions;
  int lastDoc;
  int docFreq;
  int lastDocCode;
  int termFreq;

  int lastPos;

  // todo: support positions > 2B?  Not useful?  Perhaps support with a special marker in the stream (like a 0 length payload that means
  // read a vint and multiply that by 2B and add it to the delta

  DocFreqPosStream(MemPool& pool, int docid, int pos) : lastDoc(docid) , docFreq(1), lastDocCode(docid << 1), termFreq(1), lastPos(pos) {
    positions.writeVInt(pool, pos<<1);
  }
  DocFreqPosStream(const DocFreqPosStream&) = delete;
  DocFreqPosStream(DocFreqPosStream&&) = delete;

  void writePos(MemPool& pool, int pos) {
    int posCode = pos - lastPos;
    assert(posCode >= 0);
    // TODO: do we need to support duplicate positions for the same term for the same doc???  Would seem to make search code more complex.
    positions.writeVInt(pool, posCode << 1);
    lastPos = pos;
  }

  void addDoc(MemPool& pool, int docid, int pos) { // TODO: add payload
    int delta = docid - lastDoc;
    if (delta == 0) {
      // same document
      ++termFreq;

      writePos(pool, pos);
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

      lastPos = 0;
      writePos(pool, pos);
    }
  }
}
SOLUX_PACKED_END;


/*** prototype code for reading back postings
// TODO: make a PushPositionsIterator that you could template / inherit from / pass a lambda to.
// It will call nextDoc() and nextPos() until exhaustion.  May be faster since we keep context?  coroutine generator
// is another option, but may be slower and we might need to test for doc change instead of being told.

class DocFreqPosStreamReader : public PostingsEnum {
public:
  DocFreqPosStream& source_;
  int docid;
  int remaining_;
  uint8_t remainingInSlice_;
  uint8_t sliceSize_;

  // allow someone to pull postings, or should we push?
  // We want pull if we want to be able to search?

  DocFreqPosStreamReader(DocFreqPosStream& source) : source_(source) {
    docid = -1;
    remaining_ = source.termFreq;
    sliceSize_ = Stream::FIRST_LEVEL_SIZE;
    remainingInSlice_ = sliceSize_;
  }

  virtual int docID() override {
    return 0;
  }


  int readVInt() {
    return 0;

  }

  virtual int nextDoc() override {
    if (remaining_ <= 0) {
      docid = NO_MORE_DOCS;
    } else {
      // TODO: factor out a stream reader
      int delta = readVInt();
// nocommit

    }
return 0; // nocommit
  }

  virtual int advance(int target) override {
    return DocIterator::advance(target);
  }

  virtual int64_t cost() override {
    return source_.termFreq;
  }

};

***/