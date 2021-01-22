#pragma once

#include <unordered_map>
#include <unordered_set>
#include "solux/util/MemPool.h"
#include "solux/FieldType.h"
#include "solux/util/TermHash.h"
#include "solux/util/TermValHash.h"
#include "DocStream.h"

namespace solux {


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





// todo - a way to use multiple pools to expand our mem usage beyond 2G?
// or get rid of that 2G limitation (get rid of block addressing)
class Inverter {
public:

  // Should this have pointers back to the Table object (Table or TableData)
  // Should FieldInfo lookups been done already?


  // TODO: should this the ultimate owner of the pool?
  // If things are highly sharded, we could share a pool_ and have multiple
  // uninverters per thread (highly multi-tennanted)


  // This means we should provide a way to specify a pool to use and then
  // have a higher level construct that represents an indexing thread.
  // This way, multiple shards could be handled by one thread.  They would all need to
  // flush at the same time to release the pool though.
  // Either that, or completely finish indexing (i.e. flush a segment) for each tenant
  // every time you get a batch of docs.  Then we could wind back the pool for each different tenant batch.
  MemPool pool_;

  // TODO: if our hashes are faster, use a TermValHash to do the mapping from string to
  // field.  Need to handle variable size though... (packing string first, then value would
  // eliminate the need to know the size.)  But destructors are off the list if we use BBP.
  // Still, we should switch to a monotonic allocator for this since we will never need to
  // release individually.

  std::unordered_map<std::string, SegFieldIndexed *> segFields_;


  int currDoc_ = -1;  // the current document being indexed


  // TODO: normal map, or TermHash for field names?

  // when does FieldType get looked up?
  // ability to reuse FieldType


  // For "id", if we have DocValues, do we even want to create postings?  In essence, we already have all the info
  // (just sort the DocValues to create the lookup later)
  // - could use the PackedTerm returned and just have an array of those?
  // - store anything with the ID as a payload (like "_version_"?)
  // How does lucene store DocValues at first?
  // Would the smaller size of doing it like lucene make up for the extra load (need to load buffer start)...should be cached?

  // if we are both indexing and storing docvalues, then use the string pointer that is returned?

  // single hash lookup to go from field name to SegmentFieldInfo or whatever?
  // TODO: where is autodetection done?
  // TODO: avoid hash lookup for "id" field?

  // pass function that fills in token, or pass function that actually indexes?


  void index(Document &doc);

  void indexField(FieldValue &fv);



  /***


  SegmentField& getSegmentField(const std::string& fname) {
    auto segField = segFields_[fname];
    if (segField == nullptr) {
      // TODO: use a different pool for less waste?  Or a vector if we want to use fieldnums efficiently?
      auto bbAddr = pool_.allocateTypeAligned(segField);
      segField = new(segField) SegmentField(pool_, fi);
      segFields_[fname] = segField;
    }
  }


  void indexToken(SegmentField& segField, Token token) {
    auto& entry = segField.terms.lookupOrAdd(token.ptr, token.numBytes());
    if (entry.second.ptr_ == nullptr) {
      new (&entry.second)TermsHash::value_type();
      entry.second.writeFirstInt(pool_, currDoc_);
    } else {
      entry.second.writeAnotherInt(pool_, currDoc_);
    }
  }

  SegmentTerm& indexTokenDoc(FieldInfo& fi, SegmentField& segField, char* ptr, char* end) {

    SegmentTerm& segTerm = segField.terms.lookupOrAdd(ptr, (int)(end-ptr)).second;  // todo: handle overflow of token length
    // &segTerm can be null at this point... should we create the value_type in lookupOrAdd instead?

    if (segTerm.ptr_ == nullptr) {
      new (&segTerm)TermsHash::value_type();
      segTerm.writeFirstInt(pool_, currDoc_);
    } else {
      segTerm.writeAnotherInt(pool_, currDoc_);
    }
    return segTerm;
  }

   ***/

};

} // end namespace

