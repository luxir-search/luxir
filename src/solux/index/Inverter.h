#pragma once

#include <algorithm>
#include <boost/sort/spreadsort/string_sort.hpp>
#include "gtl/phmap.hpp"
#include "solux/util/MemPool.h"
#include "solux/FieldType.h"
#include "solux/util/TermValHash.h"
#include "DocStream.h"
#include "PostingsWriter.h"

// so IndexHandler can consume protobuf types
#include "protos/solux_types.pb.h"



namespace solux {

using std::iter_swap; // for boost string_sort




class Inverter {
private:
  int currDoc = -1;  // the current document being indexed
  std::vector<int> deleted; // use a docstream for this?

  // TODO: this is temporary... we should get FieldTypes and TokenChains from the schema somehow
  // and TokenChains should not be shared across different threads.
  gtl::flat_hash_map<std::string, std::pair<std::unique_ptr<FieldType>, std::unique_ptr<TokenChain>>> typeInfo;

public:
  MemPool pool;

  // Having a handle to the postings writer means that we can start flushing whenever we want or
  // even directly write certain dense columns without uninverting first.  It could be directly contained, or
  // we could have a reference to it.
  PostingsWriter postingsWriter;

  // TODO: create a class for indexing parameters / config?
  Inverter(solux::Directory& dir, std::string_view segid) : postingsWriter(dir, segid) {
  }

  PostingsWriter& getPostingsWriter() { return postingsWriter; }


  class IndexHandler {
    friend Inverter;

    PackedTerm fieldName;
    FieldType& fieldType;

    IndexHandler(IndexHandler& other) = delete; // let's not move any of these
  public:
    IndexHandler(PackedTerm fieldName, FieldType &fieldType)
    : fieldName(fieldName), fieldType(fieldType) {
    }
    virtual ~IndexHandler() = default;

    auto operator<=>(const IndexHandler& other) const {
      return this->fieldName <=> other.fieldName;
    }

    auto operator<=>(const std::string_view& sv) const {
      return this->fieldName <=> sv;
    }

    auto operator<=>(const PackedTerm& fname) const {
      return this->fieldName <=> fname;
    }

    auto operator==(const std::string_view& sv) const {
      return this->fieldName == sv;
    }

    auto operator==(const PackedTerm& fname) const {
      return this->fieldName == fname;
    }

    virtual void index(Inverter& inverter, char* mutableVal, int len) {
      unused(inverter, mutableVal, len);
    }
    virtual void index(Inverter& inverter, int64_t val) {
      unused(inverter, val);
    }

    virtual void index(Inverter& inverter, const proto::Val& val) {
      if (val.has_s()) {
        auto &sval = val.s();
        index(inverter, const_cast<char *>(sval.data()), sval.size());  // TODO: get rid of the const-cast
      }
    }

    virtual void flush(Inverter &inverter) = 0;

    friend std::ostream& operator<<(std::ostream &out, const IndexHandler &sf) {
      return out << "{IndexHandler field:" << sf.fieldName << "}";
    }
  };


  //
  // Info for one single valued column
  //
  class IntColHandler : public IndexHandler {
    friend class Inverter;
    LongStream longStream;
    DocStream docsWithVal;
    int32_t numVals = 0;
  public:
    IntColHandler(Inverter &inverter, const std::string_view &fieldName, FieldType& fieldType)
            : IndexHandler(PackedTerm(inverter.pool,fieldName), fieldType), longStream(inverter.pool), docsWithVal(inverter.pool) {
    }

    IntColHandler(Inverter &inverter, PackedTerm fieldName, FieldType& fieldType, IndexHandler& parent)
            : IndexHandler(fieldName, fieldType), longStream(inverter.pool), docsWithVal(inverter.pool) {
      unused(parent);
    }

    ~IntColHandler() override = default;

    void index(Inverter &inverter, const proto::Val &val) override {
      if (val.has_i()) {
        int64_t ival = val.i();
        indexSingle(inverter, ival);
      }
    }

    void index(Inverter &inverter, int64_t int64) override {
      indexSingle(inverter, int64);
    }

    void indexSingle(Inverter &inverter, int64_t val) {
      longStream.addVal(inverter.pool, val);
      docsWithVal.addDoc(inverter.pool, inverter.currDoc);
      numVals++;
    }

    void flush(Inverter &inverter) override {
      flushIntCol(inverter);
    }

    // TODO: make static and pass everything needed so it's composable
    void flushIntCol(Inverter& inverter) {
      PostingsWriter& postingsWriter = inverter.getPostingsWriter();

      // TODO: move this to postingsWriter method
      PostingsWriter::IndexFieldInfo& fieldInfo = postingsWriter.fieldInfos.emplace_back();
      flushIntCol(inverter, fieldInfo);
    }

    // This is the version called directly from text field for norms
    void flushIntCol(Inverter& inverter, PostingsWriter::IndexFieldInfo& fieldInfo) {
      PostingsWriter& postingsWriter = inverter.getPostingsWriter();
      fieldInfo.fieldname = fieldName;
      auto full = numVals >= postingsWriter.getMaxDoc();

      // push values
      {
        auto guard = postingsWriter.pool.rewindScopeGuard();
        IntColWriter writer(postingsWriter.pool, postingsWriter, fieldInfo);
        writer.startField();
        // TODO: not having the docids here makes it impossible to do a dense field encoding!  Of course that's
        // not really possible if we're writing the column directly and incrementally since we don't know
        // min, max, numbits, gcd, etc.  But we *could* know that stuff when merging segments!
        // For direct incremental columns, and for merging, we could have a IntColWriter method that accepts (docid,val)
        // For that, we could have versions of push that take a number of values to push so we can have the best
        // of both worlds.
        // We could also be told it's a required field, in which case we would always chose a dense encoding unless
        // all bits are somehow needed.  In that case, we would decode blocks of docs so we could feed doc/val
        // pairs to the inverter.  A co-routine generator might be perfect for this (one that fills blocks, not
        // individual values)
        longStream.pushValues(inverter.pool, writer);
        writer.finish();
      }

      // push docs
      {
        auto guard = postingsWriter.pool.rewindScopeGuard();
        // TODO: when things go parallel, we don't want to reserve an OutputStream if this is dense.
        DocsWithValWriter docsWriter(inverter.pool, postingsWriter, fieldInfo);
        if (!full) {
          docsWithVal.pushDocs(inverter.pool, docsWriter);
          docsWriter.finish();
        } else {
          docsWriter.finishDense(numVals);
        }
      }

    }
  };


  // indexes text with positions
  class PosIndexHandler : public IndexHandler {
    friend Inverter;

    TermValHash<DocFreqPosStream> termsHash;  // the set of terms contained in this field
    TokenChain *tokenChain;  // TODO: change to ref?  is this nullable?

    IntColHandler fieldLengthCol;  // to store the field length needed for scoring among other things.
  public:

    PosIndexHandler(Inverter &inverter, const std::string_view &fieldName, FieldType& fieldType, TokenChain *tokenChain)
            : IndexHandler(PackedTerm(inverter.pool,fieldName), fieldType),
            termsHash(inverter.pool, 4),
            tokenChain(tokenChain),
            fieldLengthCol(inverter, this->fieldName, this->fieldType, *this)
            {
    }
    ~PosIndexHandler() override = default;

    void index(Inverter& inverter, const proto::Val& val) override {
      // TODO: handle bytes
      char* mutableValue = nullptr;
      int len = 0;
      if (val.has_s()) {
        auto &sval = val.s();
        mutableValue = const_cast<char *>(sval.data());  // TODO: get rid of the const-cast
        len = sval.size();
      } else if (val.has_bin()) {
        // TODO: handle binary
      }

      // TODO: handle arrays as well.  Hard to do in virtual methods where you can't use templates though.

      indexSingle(inverter, mutableValue, len);
    }

    void index(Inverter &inverter, char *mutableVal, int len) override {
      indexSingle(inverter, mutableVal, len);
    }

    // TODO: handle multi-valued. Or is that a diff subclass?
    void indexSingle(Inverter& inverter, char* mutableVal, int len) {
      TokenChain& tc = *tokenChain;
      tc.head.setMutableValue(mutableVal, len);

      int numTokens = 0;
      int pos = -1;
      Token &tok = tc.head.getToken();
      TokenStream& tail = *tc.tail;
      int docid = inverter.currDoc;
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

      // We index the length even if all tokens were removed somehow, because we still want
      // to record that there was a doc for this field.
      fieldLengthCol.indexSingle(inverter, numTokens);
    }

    void flush(Inverter &inverter) override {
      flushPositions(inverter);
    }

    void flushPositions(Inverter &inverter) {
      auto sz = termsHash.size();
      // gathering and sorting terms for each field could be done in parallel, but it probably doesn't
      // represent much time.  Fields that can result in their own file should be able to be parallelized easily!
      auto terms = termsHash.destructiveCompress();

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

      // Either reduce the resource for these, or share across different fields (in the same thread)
      TextWriter textWriter(inverter.getPostingsWriter());
      PostingsWriter::IndexFieldInfo& fieldInfo = inverter.getPostingsWriter().fieldInfos.emplace_back();
      fieldInfo.fieldname = fieldName;

      textWriter.startField(&fieldInfo);
      for (size_t tnum=0; tnum<sz; tnum++) {
        auto term = terms[tnum];
        textWriter.startTerm(term);
        // push all the docs / positions for this term
        term.val().pushDocs(inverter.pool, textWriter);
        textWriter.endTerm(term);
      }
      textWriter.endField();
      termsHash.free();

      // now flush the field length column
      fieldLengthCol.flushIntCol(inverter, fieldInfo);
    }

  };


  // single-valued string field (indexed and stored)
  class StringIndexHandler : public IndexHandler {
    friend Inverter;

    // TODO: for unique fields like "id", this could be TermValHash<int32_t>
    TermValHash<DocStream> termsHash;  // the set of terms contained in this field

  public:
    StringIndexHandler(Inverter &inverter, const std::string_view &fieldName, FieldType& fieldType)
            : IndexHandler(PackedTerm(inverter.pool,fieldName), fieldType),
              termsHash(inverter.pool, 4) {
    }
    ~StringIndexHandler() override = default;

    void index(Inverter& inverter, const proto::Val& val) override {
      std::string_view v;

      if (val.has_s()) {
        v = val.s();
      } else if (val.has_bin()) {
        v = val.bin();
      }
      // TODO: handle arrays as well.

      indexSingle(inverter, v);
    }

    void indexSingle(Inverter& inverter, std::string_view term) {
      // TODO: error check if term is too long to index.
      auto[entry, inserted] = termsHash.try_emplace(term, termsHash.getMemPool());
      unused(inserted);
      entry->val().addDoc(termsHash.getMemPool(), inverter.currDoc);
    }

    void flush(Inverter& inverter) override {
      auto sz = termsHash.size();
      auto terms = termsHash.destructiveCompress();
      boost::sort::spreadsort::string_sort(terms, terms+sz, TermRef::bracket(), TermRef::getsize(), TermRef::lessthan());

      TextWriter textWriter(inverter.getPostingsWriter());
      PostingsWriter::IndexFieldInfo& fieldInfo = inverter.getPostingsWriter().fieldInfos.emplace_back();
      fieldInfo.fieldname = fieldName;

      // For ordinals, we already know the number of unique terms, so we can use an optimal number of bits right off the bat
      // for dense fields.  Then we could simply memcpy the ordinals into the postings file.
      std::vector<int> docToOrd(inverter.currDoc+1, 0); // This is very inefficient temporary implementation.
      // ord vec must me 0 initialized since that is value that means "missing".

      textWriter.startField(&fieldInfo);
      for (size_t tnum=0; tnum<sz; tnum++) {
        auto term = terms[tnum];
        textWriter.startTerm(term);
        // push all the docs for this term to the TextWriter, as well as record the ordinal for each doc
        term.val().forEachDoc(inverter.pool, [&](int docid) {
          textWriter.startDoc(docid);
          textWriter.endDoc(docid);
          docToOrd[docid] = tnum + 1;  // +1 because 0 means "missing"
        });
        textWriter.endTerm(term);
      }
      textWriter.endField();
      termsHash.free();

      // TODO: need to write the ordinals to the postings file.
    }

  };


  // OPTIMIZATION: since we only do additions and not deletions, a monotonic allocator that had destructor
  // support would be good here.  Or we could add to our MemPool and manually destruct later.
  // This could also be a Set with a little more work since the fieldname is already in the value.
  // We don't want the values to move since clients can cache and reuse when indexing.
  gtl::flat_hash_map<std::string, std::unique_ptr<IndexHandler>> indexHandlers;


  // The returned reference will be valid for the duration of indexing this block.
  // TODO: a version that gets a set at a time, so the inverter can pick the best columns to write directly?
  // What about adjusting number of files on postings writer? We should be able to make that dynamic up until a max.
  IndexHandler& getIndexHandler(const std::string_view name) {
    auto iter = indexHandlers.find(name);
    if (iter != indexHandlers.end()) {
      return *iter->second;
    }

    // How do we incorporate schema changes? Or do we?
    // If we get a whole new schema, we would still need to hold onto all old ones if anything points back to them
    // without a shared pointer?
    // What if we turned FieldType into a value type
    // and used std::variant / std::visit?
    // Perhaps used named analysis chains... this would also facilitate specifying different ones at query time

    // TODO: Need to look up the correct field type in schema.  For now just inline it.

    std::string_view suffix = name.substr(name.find_last_of('_'));
    if (suffix.empty() && name == "id") {
      suffix = "_s";
    }

    // TEMPORARY: based on the suffix, try to find both the cached FieldType and associated TokenChain
    auto typeIter = typeInfo.find(suffix);

    if (typeIter == typeInfo.end()) {
      auto ft = std::make_unique<FieldType>();
      ft->name_ = suffix;
      std::unique_ptr<TokenChain> tc;

      if (suffix == "_i") {
        ft->flags_ = 0;
      } else if (suffix == "_s") {
        ft->flags_ = FieldType::INDEX_DOCS;
      } else {
        ft->flags_ = FieldType::INDEX_DOCS_AND_FREQS_AND_POSITIONS | FieldType::NUM_TOKENS_APPROX;
        auto wsTok = std::make_unique<WhitespaceTokenizer>();
        auto &headRef = *wsTok;
        if (suffix == "_w") {
          tc = make_unique<TokenChain>(headRef, std::move(wsTok));  // ws only
        } else if (suffix == "_wl") {
          auto lowerFilt = std::make_unique<LowercaseFilter>(std::move(wsTok));
          tc = make_unique<TokenChain>(headRef, std::move(lowerFilt));
        }
      }
      auto [it2, inserted] = typeInfo.try_emplace(suffix, std::move(ft), std::move(tc));
      typeIter = it2;
    }

    FieldType& fieldType = *typeIter->second.first;
    TokenChain* tokenChain = typeIter->second.second.get();

    // Create the correct IndexHandler based on the suffix.  This should probably be moved to FieldType::createIndexHandler()?
    std::unique_ptr<IndexHandler> fieldHandler;
    if (suffix == "_i") {
      fieldHandler = std::make_unique<IntColHandler>(*this, name, fieldType);
    } else if (suffix == "_s") {
      fieldHandler = std::make_unique<StringIndexHandler>(*this, name, fieldType);
    } else {
      fieldHandler = std::make_unique<PosIndexHandler>(*this, name, fieldType, tokenChain);
    }

    auto [newIter, inserted] = indexHandlers.try_emplace(name, std::move(fieldHandler));
    return *(newIter->second);
  }

  void setDoc(int32_t docid) {
    assert (docid >= currDoc);
    currDoc = docid;
  }

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

  void index(Document &doc);

  size_t memSize() {
    // TODO: take into account more than just the pool
    return pool.size();
  }

  /// finishes indexing this segment (also calls finish on the underlying postings writer)
  void flush() {
    getPostingsWriter().setMaxDoc(getMaxDoc());  // TODO: this won't always be accurate currently?

    // We could either sort fields first, or after they have been indexed.  Merging segments will presumably
    // go in sorted field order, so lets do the same thing here and sort first.
    std::vector<IndexHandler*> fields;
    fields.reserve(indexHandlers.size());
    for (auto& entry : indexHandlers) {
      fields.push_back(entry.second.get());
    }

    // For normal usecases, spreadsort will fall back to pdqsort (less than 1000 fields, but we want to
    // take care of the outliers as well (esp when it doesn't hurt the average case)
    boost::sort::spreadsort::string_sort(fields.begin(), fields.end(),
                                         [](const IndexHandler* x, size_t offset) {return x->fieldName[offset];},
                                         [](const IndexHandler* x) {return x->fieldName.size();},
                                         [](const IndexHandler* x, const IndexHandler* y) {return *x < *y;});


    for (auto fieldHandler : fields) {
      fieldHandler->flush(*this);
    }

    getPostingsWriter().finish();
  }


};

} // end namespace

