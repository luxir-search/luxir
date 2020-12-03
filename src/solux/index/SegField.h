#pragma once

#include "solux/analysis/Analyzer.h"
#include "ByteBlockPool.h"
#include "solux/FieldType.h"
#include "solux/BytesRefHash.h"
#include "solux/StrValHash.h"
#include "DocStream.h"

/***
class SegmentTerm {
public:

  // Since Streams aren't currently relocabable (they can point to themselves),
  ByteBlockPool::DeltaVintStream termDocs;
};
 ***/

// TODO: we don't have to worry too much about the current schema going away during our indexing session
// *but* we may need a way to invalidate / update??? Or maybe not...
// if we're auto-detecting a field, it can be from a new schema
// Can't *change* the type of a field in an indexing session? (would we need to? do we want to?)  Maybe it's enough to change for this indexer only
// (i.e. if we see we need to start adding payloads on a field or something?)


// Another approach would be to put the SegmentTerm immediately following the string bytes (aligned or unaligned) in memory...
// We could also put the string immediately following the struct (although it perhaps boils down to the same amount of memory waste?)
// OR: we could put the string and the struct right next to eachother... but align the start of the pair such that the struct ends up aligned.
//   That would mean the struct address would be easier to calculate (ptr+len, no alignment)
//   We could also point to the beginning of the struct (and hence the end of the string)
// Alternatively, one could store the term number (4 bytes if limiting to 4B terms)?  That requires a BBPool type indirect lookup though, or resizing (what we have now?)





// Could use composition here...
// Have something saying how to index, something saying how to store (docValues, etc), something saying how to index numerically
//
//
// A field represented in a single segment
/***
template <class StreamType>
class SegmentField {
public:
  FieldInfo& field_;
  StrValHash<StreamType> termsHash;

  // pointer to the global field info...
  // prob not necessary to store (it can be implicit and we could pass along FieldInfo everywhere)
  // but it's not that large per-field.

  // TODO: do we need the analyzer (or field_ should point to that?)

  // Reusable TokenStream, instantiated on demand (may be null)
  // The analyzer can be obtained from the FieldInfo
  TokenStream* tokenStream;

  int fieldNum;
  int docsWithField;
  // What about docValues fields?

  SegmentField(ByteBlockPool& pool, FieldInfo& field) :  field_(field), termsHash(pool, 4) {
  }
};
***/

// Term stats kept at all levels
struct FieldStats {
  int64_t uniqueTerms; // number of unique terms... only valid at segment level
  int64_t totalTermFreq;
  int64_t overlaps;
  int64_t docsWithField;
  int maxTermFreq;  // largest term freq of any term
};

// temporary stats for indexing a single field value
struct FieldValueStats {
  int pos;  // current term position

};



class SegFieldIndexed {
public:
  // Pointer to the global field type.
  const FieldType& field_;

  // Reusable TokenStream, instantiated on demand (may be null)
  // The analyzer can be obtained from the FieldType
  std::unique_ptr<TokenChain> tokenChain;  // TODO: allocate in a pool associated with the indexer?

  // TODO: these could also be calculated by the postings writer as long as they are not needed up front?
  int64_t sumTotalTermFreq = 0;  // sum of term freq across all terms for all documents
  int64_t sumDocFreq = 0;        // sum of doc freqs across all terms
  int docsWithField = 0;      // count of how many documents have this field

  explicit SegFieldIndexed(const FieldType& field) : field_(field) {}

  virtual void indexSingleValue(int docid, const char* ptr, int len) {}
  virtual void indexMultipleValues() {}
  virtual void indexTokenStream(int docid, char* mutableVal, int len) {}

  virtual ~SegFieldIndexed() {}
};

class SegFieldDocs : public SegFieldIndexed {
public:
  StrValHash<DocStream> termsHash;

  SegFieldDocs(const FieldType &field, ByteBlockPool& pool) : SegFieldIndexed(field) , termsHash(pool, 4) { }

  virtual void indexSingleValue(int docid, const char *ptr, int len) override {

//    DocStream& stream = termsHash.get(ptr, len);
//    stream.addDoc(termsHash.pool_, docid);
  }

  virtual void indexTokenStream(int docid, char* mutableVal, int len) override {  // todo: could make a templatized version
    tokenChain->head.setMutableValue(mutableVal, len);

    Token& tok = *tokenChain->token;
    bool first = true;
    for(;;) {
      bool hasNext = tokenChain->tail->incrementToken(first);
      first = false;
      if (!hasNext) break;

      int tokLen = tok.end - tok.ptr;  // todo: check overflow
      // DocStream& stream = termsHash.get(tok.ptr, tokLen);
      /***
      termsHash.update(tok.ptr, tokLen
              , [=](DocStream* p){ new (p) DocStream(docid); }
              , [=](DocStream& s){ s.addDoc(termsHash.pool_, docid); }
      );
       ***/

      auto [entry, inserted] = termsHash.try_emplace(tok.ptr, tokLen, termsHash.pool_, docid);
      if (!inserted) {
          entry.val().addDoc(termsHash.pool_, docid);
      }

    }
  }

};

/*
class SegFieldDocsFreq : SegFieldIndexed {
public:
  StrValHash<DocFreqStream> termsHash;
};
*/


class SegFieldDocsFreqPos : public SegFieldIndexed {
public:
  StrValHash<DocFreqPosStream> termsHash;

  SegFieldDocsFreqPos(const FieldType &field, ByteBlockPool& pool) : SegFieldIndexed(field) , termsHash(pool, 4) { }

  inline void indexSingleTerm(int docid, char* term, int len, int pos) {
    auto [entry, inserted] = termsHash.try_emplace(term, len, termsHash.pool_, docid, pos);
    if (!inserted) {
      entry.val().addDoc(termsHash.pool_, docid, pos);
    }
  }

  // TODO: an optimized variant that can add multiple positions at the same time (useful if we are indexing term vectors)


  // TODO: OPT: try templatizing by the token chain?
  virtual void indexTokenStream(int docid, char* val, int len) {
    tokenChain->head.setMutableValue(val, len);

    int numTokens = 0;
    int pos = 0;
    Token& tok = *tokenChain->token;
    bool first = true;
    for(;;) {
      bool hasNext = tokenChain->tail->incrementToken(first);
      if (!hasNext) break;
      first = false;
      ++numTokens;
      pos += tok.positionIncrement;
      int tokLen = tok.end - tok.ptr;  // todo: check overflow? (impossible if we start with "int" len?)


      indexSingleTerm(docid, tok.ptr, tokLen, pos);
      // TODO: benchmark these variants once we have a good benchmark suite


      /**
      termsHash.update(tok.ptr, tokLen
              , [=](DocFreqPosStream* p){ new (p) DocFreqPosStream(termsHash.pool_, docid, pos); }
              , [=](DocFreqPosStream& s){ s.addDoc(termsHash.pool_, docid, pos); }
      );
      **/


      /**
      termsHash.updateT(tok.ptr, tokLen
              , [=](DocFreqPosStream* p){ new (p) DocFreqPosStream(termsHash.pool_, docid, pos); }
              , [=](DocFreqPosStream& s){ s.addDoc(termsHash.pool_, docid, pos); }
      );
      **/


      /***
      StrValRef<DocFreqPosStream>& entry = termsHash.lookup(tok.ptr, tokLen);
      if (entry.isNull()) {
        entry = StrValRef<DocFreqPosStream>::alloc(termsHash.pool_, tok.ptr, tokLen);
        new (entry.valPtr()) DocFreqPosStream(termsHash.pool_, docid, pos);
      } else {
        entry.val().addDoc(termsHash.pool_, docid, pos);
      }
      ***/

      /*
      StrValRef<DocFreqPosStream> entry(0,0);  // try moving outside loop for better perf?
      bool inserted;
      [entry, inserted] = termsHash.lookupOrAdd(tok.ptr, tokLen);
      if (inserted) {
        new (entry.valPtr()) DocFreqPosStream(termsHash.pool_, docid, pos);
      } else {
        entry.val().addDoc(termsHash.pool_, docid, pos);
      }
     */


      /*
      auto [entry, inserted] = termsHash.lookupOrAdd(tok.ptr, tokLen);
      if (inserted) {
          new (entry.valPtr()) DocFreqPosStream(termsHash.pool_, docid, pos);
      } else {
          entry.val().addDoc(termsHash.pool_, docid, pos);
      }
       */



/*
      StrValRef<DocFreqPosStream>* entry;
      bool inserted = termsHash.lookupOrAdd(tok.ptr, tokLen, entry);
      if (inserted) {
        new (entry->valPtr()) DocFreqPosStream(termsHash.pool_, docid, pos);
      } else {
        entry->val().addDoc(termsHash.pool_, docid, pos);
      }
*/

    }

    // todo: calc norm?

    sumTotalTermFreq += numTokens;

  }


  // Attempt to remove virtual function call per token?
  // TODO: templatize the WhitespaceTokenizer::process
  // Results of indexing directly with this method:
  // clang=130MB/sec vs 122MB/sec  (i.e. 6.5% increase)
  // gcc: 159MB/sec vs 172MB/sec (i.e. this one is slower than the TokenChain version by 8% ???)
  void indexWhitespace(int docid, char* val, int len) {
      int numTokens = 0;
      int pos = 0;
      WhitespaceTokenizer::process(val, len,
              [&](const char* token, int tokLen) {
          numTokens++;
          pos++;  // need to have the tokenizer do this in case tokens are skipped or overlapped?
          auto [entry, inserted] = termsHash.try_emplace(token, tokLen, termsHash.pool_, docid, pos);
          if (!inserted) {
              entry.val().addDoc(termsHash.pool_, docid, pos);
          }
      }
      );

      sumTotalTermFreq += numTokens;
    }

};


