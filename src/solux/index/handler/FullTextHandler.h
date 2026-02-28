#pragma once

#include "solux/index/DocStream.h"
#include "solux/index/Inverter.h"
#include "solux/index/IntColWriter.h"


using namespace solux;
namespace solux::handler {

// Indexes text field with frequencies and positions.
class FullTextHandler : public Inverter::IndexHandler {
  friend Inverter;

  TermValHash<DocFreqPosStream> termsHash; // the set of terms contained in this field
  std::unique_ptr<TokenChain> tokenChain;

  IntColHandler fieldLengthCol; // to store the field length needed for scoring among other things.
public:
  FullTextHandler(Inverter& inverter, const std::string_view& fieldName, const std::shared_ptr<FieldType>& fieldType)
    : IndexHandler(PackedTerm(inverter.pool, fieldName), fieldType),
      termsHash(inverter.pool, 4),
      fieldLengthCol(inverter, this->fieldName, this->fieldType, *this) {

    // Future optimization: cache the analyzer for the type if it isn't field-specific
    // This can help with memory consumption when the same analyzer can be used for many fields.

    // downcast to TextFieldType to get the analyzer. Use of dynamic_cast is OK this is only done once per field.
    auto textFieldType = dynamic_cast<TextFieldType*>(fieldType.get());
    tokenChain = textFieldType->createAnalyzer(fieldName);
  }

  ~FullTextHandler() override = default;

  void index(Inverter& inverter, const proto::Val& val) override {
    // TODO: handle bytes
    std::string_view sv;
    if (val.has_s()) {
      sv = val.s();
    }
    else if (val.has_bin()) {
      sv = val.bin();
    }

    // TODO: handle arrays as well.  Hard to do in virtual methods where you can't use templates though.

    indexSingle(inverter, sv);
  }

  void index(Inverter& inverter, std::string_view val) override {
    indexSingle(inverter, val);
  }

  // TODO: handle multi-valued. Or is that a diff subclass?
  void indexSingle(Inverter& inverter, std::string_view val) {
    TokenChain& tc = *tokenChain;
    tc.head.setValue(val);

    int numTokens = 0;
    int pos = -1;
    Token& tok = tc.head.getToken();
    TokenStream& tail = *tc.tail;
    int docid = inverter.getDoc();
    for (;;) {
      bool hasNext = tail.incrementToken(numTokens == 0);
      if (!hasNext) break;
      ++numTokens;
      pos += tok.positionIncrement;
      auto term = std::string_view(tok.ptr, tok.end);
      // int tokLen = tok.end - tok.ptr;

      auto [entry, inserted] = termsHash.try_emplace(term, termsHash.getMemPool(), docid, pos);
      if (!inserted) {
        entry->val().addDoc(termsHash.getMemPool(), docid, pos);
      }
    }

    // We index the length even if all tokens were removed somehow, because we still want
    // to record that there was a doc for this field.
    fieldLengthCol.indexSingle(inverter, numTokens);
  }

  void flush(Inverter& inverter) override {
    flushPositions(inverter);
  }

  void flushPositions(Inverter& inverter) {
    auto sz = termsHash.size();
    if (sz == 0) {
      return;  // Drop the field if it has no terms.
    }

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
    boost::sort::spreadsort::string_sort(terms, terms + sz, TermRef::bracket(), TermRef::getsize(),
                                         TermRef::lessthan());
    // auto endTime = std::chrono::high_resolution_clock::now();
    // auto thisElapsed = std::chrono::duration_cast<std::chrono::nanoseconds>( endTime - startTime ).count();
    // std::cout << "terms=" << sz << " SORT time ns=" << thisElapsed << std::endl;

    // Either reduce the resource for these, or share across different fields (in the same thread)
    TextWriter textWriter(inverter.getPostingsWriter());
    PostingsWriter::IndexFieldInfo& fieldInfo = inverter.getPostingsWriter().addField(fieldName);
    fieldInfo.type = fieldType->type();
    fieldInfo.flags = fieldType->flags_ & ~FieldType::ABSTRACT;

    textWriter.startField(&fieldInfo);
    for (size_t tnum = 0; tnum < sz; tnum++) {
      auto term = terms[tnum];
      textWriter.startTerm(term);
      // push all the docs / positions for this term
      term.val().pushDocs(inverter.pool, textWriter);
      textWriter.endTerm(term);
    }
    textWriter.endField();
    termsHash.free();  // free up memory early.

    // now flush the field length column
    fieldLengthCol.flushIntCol(inverter, fieldInfo);
  }

};

}
