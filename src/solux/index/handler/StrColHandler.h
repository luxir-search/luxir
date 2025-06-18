#pragma once

#include "solux/index/DocStream.h"
#include "solux/index/Inverter.h"
#include "solux/index/OrdCollector.h"
#include "solux/index/OrdColWriter.h"
#include "solux/util/TermValHash.h"


using namespace solux;
namespace solux::handler {

// single-valued string field (indexed and stored)
class StrColHandler : public Inverter::IndexHandler {
  friend Inverter;

  // TODO: for unique fields like "id", this could be TermValHash<int32_t>
  TermValHash<DocStream> termsHash; // the set of terms contained in this field
  size_t maxValues = 1; // maximum number of values seen for a single doc

public:
  StrColHandler(Inverter& inverter, const std::string_view& fieldName, const std::shared_ptr<FieldType>& fieldType)
    : IndexHandler(PackedTerm(inverter.pool, fieldName), fieldType),
      termsHash(inverter.pool, 4) {
  }

  ~StrColHandler() override = default;

  void index(Inverter& inverter, const proto::Val& val) override {
    std::string_view v;

    if (val.has_s()) {
      v = val.s();
    }
    else if (val.has_bin()) {
      v = val.bin();
    }
    else if (val.has_arr_s()) {
      auto& arr = val.arr_s().v();
      std::span<const std::string* const> values(arr.data(), arr.size());
      index(inverter, values);
      return;
    }
    // TODO: handle arrays of binary as well

    indexSingle(inverter, v);
  }

  void index(Inverter& inverter, std::string_view val) override {
    indexSingle(inverter, val);
  }

  void indexSingle(Inverter& inverter, std::string_view term) {
    // TODO: error check if term is too long to index.
    auto [entry, inserted] = termsHash.try_emplace(term, termsHash.getMemPool());
    unused(inserted);
    // don't record duplicates for the same doc.
    if (entry->val().getLastDoc() != inverter.getDoc()) {
      entry->val().addDoc(termsHash.getMemPool(), inverter.getDoc());
    }
  }

  void index(Inverter& inverter, std::span<const std::string* const> vals) override {
    for (auto val : vals) {
      indexSingle(inverter, *val);
    }
    maxValues = std::max(maxValues, vals.size());
    // TODO: check if fieldType allows multiple values?
  }

  void index(Inverter& inverter, std::span<std::string_view> vals) override {
    for (auto val : vals) {
      indexSingle(inverter, val);
    }
    maxValues = std::max(maxValues, vals.size());
    // TODO: check if fieldType allows multiple values?
  }

  void flush(Inverter& inverter) override {
    auto nDocs = inverter.getMaxDoc();
    auto guard = MemPool::threadLocalPoolGuard();

    int32_t uniqueVals = termsHash.size();

    auto terms = termsHash.destructiveCompress();
    boost::sort::spreadsort::string_sort(terms, terms + uniqueVals, TermRef::bracket(), TermRef::getsize(),
                                         TermRef::lessthan());

    // TODO: TextWriter should be refactored (or templated) to handle strings without positions.
    TextWriter textWriter(inverter.getPostingsWriter());
    PostingsWriter::IndexFieldInfo& fieldInfo = inverter.getPostingsWriter().addField(fieldName);
    fieldInfo.type = fieldType->type();
    fieldInfo.flags = fieldType->flags_;

    // For ordinals, we already know the number of unique terms, so we can use an optimal number of bits right off the bat
    // for dense fields.  Then we could simply memcpy the ordinals into the postings file.
    // std::vector<int> docToOrd(inverter.currDoc+1, 0); // This is very inefficient temporary implementation.
    // ord vec must me 0 initialized since that is value that means "missing".
    OrdCollector ords(guard.pool(), nDocs);

    textWriter.startField(&fieldInfo);
    for (int32_t tnum = 0; tnum < uniqueVals; tnum++) {
      auto term = terms[tnum];
      textWriter.startTerm(term);
      // push all the docs for this term to the TextWriter, as well as record the ordinal for each doc
      term.val().forEachDoc(inverter.pool, [&](int docid) {
        // LOG_INFO("WRITE docid={}, tnum={}", docid, tnum);
        textWriter.startDoc(docid);
        textWriter.addPositionDelta(1);
        // add a dummy position for now since we are using TextWriter, which expects them.
        textWriter.endDoc(docid);

        // single-valued version.
        // docToOrd[docid] = tnum + 1;  // +1 because 0 means "missing"
        ords.add(docid, tnum + 1);
      });
      textWriter.endTerm(term);
    }
    textWriter.endField();
    termsHash.free();

    OrdColWriter ordsWriter(guard.pool(), inverter.postingsWriter, fieldInfo, ords);
    ordsWriter.finish();
  }

};

}