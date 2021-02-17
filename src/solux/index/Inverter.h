#pragma once

#include <algorithm>
#include <boost/sort/spreadsort/string_sort.hpp>
#include <parallel_hashmap/phmap.h>
#include "solux/util/MemPool.h"
#include "solux/FieldType.h"
#include "solux/util/TermValHash.h"
#include "DocStream.h"
#include "PostingsWriter.h"



namespace solux {

using std::iter_swap; // for boost string_sort


/***
Minimum needed to index a single term position:
   TermValHash (one per field)
     used to lookup DocStream for the term
   MemPool
     where memory comes from
   Given: docid, position

Inverter has the mapping of field to TermValHash (currently encapsulated in SegFieldIndexed)


Minimum needed to index a field value:
   Cached token stream / analyzer to tokenize the field value.
   We want this per-type (not per field) to handle the tons-of-fields case.

   What about multi-valued fields and gaps?
   What about begin/end sentence, para, value? best left to analyzer I guess?
   What about has-value?
   What about indexing value and storing value?

   Ever a connection between indexed and stored values??? (what about docvalue ords and the index?)


*/





class Inverter {
private:


  std::vector<int> deleted; // use a docstream for this?

  // TODO: this is temporary... we should get FieldTypes and TokenChains from the schema somehow
  // and TokenChains should not be shared across different threads.
  phmap::flat_hash_map<std::string, std::pair<std::unique_ptr<FieldType>, std::unique_ptr<TokenChain>>> typeInfo;
public:
  // This means we should provide a way to specify a pool to use and then
  // have a higher level construct that represents an indexing thread.
  // This way, multiple shards could be handled by one thread.  They would all need to
  // flush at the same time to release the pool though.
  // Either that, or completely finish indexing (i.e. flush a segment) for each tenant
  // every time you get a batch of docs.  Then we could wind back the pool for each different tenant batch.
  MemPool pool;


  // Holds info for a single inverted field with positions for a single segment for this inverter.
  class SegFieldPos {
    friend class Inverter;

    TermValHash<DocFreqPosStream> terms;  // the set of terms contained in this field
    std::string fieldName;
    FieldType *fieldType;
    TokenChain *tokenChain;
  public:
    SegFieldPos(Inverter &inverter, const std::string_view &fieldName, FieldType *fieldType, TokenChain *tokenChain)
            : terms(inverter.pool, 4), fieldName(fieldName), fieldType(fieldType), tokenChain(tokenChain) {
    }

    ~SegFieldPos() = default;

    void index(Inverter &inverter, char *mutableVal, int len) {
      inverter.index(*this, mutableVal, len);
    }

    auto operator<=>(const SegFieldPos& other) const {
      return this->fieldName <=> other.fieldName;
    }

    friend std::ostream& operator<<(std::ostream &out, const SegFieldPos &sf) {
      return out << "{field:" << sf.fieldName << " terms:" << sf.terms << "}";
    }
  };

  // OPTIMIZATION: since we only do additions and not deletions, a monotonic allocator that had destructor
  // support would be good here.
  // This could also be a Set with a little more work since the fieldname is already in the value.
  phmap::flat_hash_map<std::string, SegFieldPos> segFields;

  SegFieldPos& getSegField(const std::string_view& name) {
    auto iter = segFields.find(name);
    if (iter != segFields.end()) {
      return iter->second;
    }

    // How do we incorporate schema changes? Or do we?
    // If we get a whole new schema, we would still need to hold onto all old ones if anything points back to them
    // without a shared pointer?
    // What if we turned FieldType into a value type
    // and used std::variant / std::visit?
    // Perhaps used named analysis chains... this would also facilitate specifying different ones at query time

    // TODO: Need to look up the correct field type in schema.  For now just inline it.

    std::string_view suffix = name.substr(name.find_last_of('_'));
    auto typeIter = typeInfo.find(suffix);

    if (typeIter == typeInfo.end()) {
      auto ft = std::make_unique<FieldType>();
      ft->name_ = suffix;
      ft->flags_ = FieldType::INDEX_DOCS_AND_FREQS_AND_POSITIONS | FieldType::NUM_TOKENS_APPROX;


      auto wsTok = std::make_unique<WhitespaceTokenizer>();
      auto& headRef = *wsTok;
      std::unique_ptr<TokenChain> tc;
      if (suffix == "_w") {
        tc = make_unique<TokenChain>(headRef, std::move(wsTok));  // ws only
      } else if (suffix =="_wl") {
        auto lowerFilt = std::make_unique<LowercaseFilter>(std::move(wsTok));
        tc = make_unique<TokenChain>(headRef, std::move(lowerFilt));
      }
      auto [it2, inserted] = typeInfo.try_emplace(suffix, std::move(ft), std::move(tc));
      typeIter = it2;
    }

    FieldType* fieldType = &*typeIter->second.first;
    TokenChain* tokenChain = &*typeIter->second.second;

    auto [newIter, inserted] = segFields.try_emplace(name, *this, name, fieldType, tokenChain);
    return newIter->second;
  }




  int currDoc = -1;  // the current document being indexed

  void startDoc() {
    currDoc++;
  }

  void finishDoc() {
  }

  int32_t getMaxDoc() {
    return currDoc + 1;
  }

  // mark the current doc as deleted if something went wrong.
  void deleteDoc(int docid) {
    deleted.push_back(docid);
  }

  void index(SegFieldPos& segField, char* mutableVal, int len) {
    TokenChain& tc = *segField.tokenChain;
    tc.head.setMutableValue(mutableVal, len);

    int numTokens = 0;
    int pos = 0;
    auto& termsHash = segField.terms;
    Token &tok = tc.head.getToken();
    TokenStream& tail = *tc.tail;
    int docid = currDoc;
    for (;;) {
      bool hasNext = tail.incrementToken(numTokens==0);
      if (!hasNext) break;
      ++numTokens;
      pos += tok.positionIncrement;
      auto term = std::string_view(tok.ptr, tok.end);
      // int tokLen = tok.end - tok.ptr;

      auto[entry, inserted] = termsHash.try_emplace(term, termsHash.getMemPool(), docid, pos);
      if (!inserted) {
        entry->val().addDoc(termsHash.getMemPool(), docid, pos);
      }
    }

    // segField.sumTotalTermFreq += numTokens;
    if (numTokens > 0) {
      // todo: index field length (for normalization / scoring) if we're indexing norms
      // todo: add to "document has field" if we're not indexing norms, or if we need
      // an index into sparse norms.  Same thing for any field with sparse docvalues...
      // Can these "document has field" sets be deduped?  Keep a hash of all others written
      // so far and if the hash compares, then compare the sets.
    }
  }


  void index(Document &doc);

  size_t memSize() {
    // TODO: take into account more than just the pool
    return pool.size();
  }

  void writePostings(PostingsWriter& postingsWriter) {
    // first gather and sort the fields
    std::vector<SegFieldPos*> fields;
    fields.reserve(segFields.size());
    for (auto& entry : segFields) {
      fields.push_back(&entry.second);
    }

    // For normal usecases, spreadsort will fall back to pdqsort (less than 1000 fields, but we want to
    // take care of the outliers as well (esp when it doesn't hurt the average case)
    boost::sort::spreadsort::string_sort(fields.begin(), fields.end(),
                                         [](const SegFieldPos* x, size_t offset) {return x->fieldName[offset];},
                                         [](const SegFieldPos* x) {return x->fieldName.size();},
                                         [](const SegFieldPos* x, const SegFieldPos* y) {return *x < *y;});


    for (auto field : fields) {
      // std::cout << "Writing field " << *field << std::endl;
      writePostings(postingsWriter, *field);
    }
  }

  void writePostings(PostingsWriter& postingsWriter, SegFieldPos& field) {
    auto sz = field.terms.size();
    // gathering and sorting terms for each field could be done in parallel, but it probably doesn't
    // represent much time.  Fields that can result in their own file should be able to be parallelized easily!
    auto terms = field.terms.destructiveCompress();

    // Sorting this with std::sort took ~7.3ms out of a total of ~22ms for inversion and 38ms for inversion+postings_writing!
    // There were only 41991 unique terms... how can sort be so slow? Cache misses?
    // For cache misses, we could try using a string type with a short-string optimization, or try packing all the strings
    // for a field together (would need to know what fields have many terms though.)
    // If strings are short on average, try an inline memcmp!
    // pdqsort == 6.8ms
    // spread_sort::string_sort == 3.9ms

    // std::chrono::high_resolution_clock::time_point startTime = std::chrono::high_resolution_clock::now();
    // std::sort(terms, terms+sz);
    // boost::sort::pdqsort(terms, terms+sz);
    boost::sort::spreadsort::string_sort(terms, terms+sz, TermRef::bracket(), TermRef::getsize(), TermRef::lessthan());
    // auto endTime = std::chrono::high_resolution_clock::now();
    // auto thisElapsed = std::chrono::duration_cast<std::chrono::nanoseconds>( endTime - startTime ).count();
    // std::cout << "terms=" << sz << " SORT time ns=" << thisElapsed << std::endl;

    postingsWriter.startField(field.fieldName);
    for (size_t tnum=0; tnum<sz; tnum++) {
      auto term = terms[tnum];
      postingsWriter.startTerm(term);
      // push all the docs / positions for this term
      term.val().pushDocs(pool, postingsWriter);
      postingsWriter.endTerm(term);
    }
    postingsWriter.endField(field.fieldName);
    field.terms.free();
  }


};

} // end namespace

