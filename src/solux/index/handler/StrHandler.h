#pragma once

#include "solux/index/DocStream.h"
#include "solux/index/Inverter.h"
#include "solux/index/OrdCollector.h"
#include "solux/index/OrdColWriter.h"
#include "solux/util/TermValHash.h"


using namespace solux;
namespace solux::handler {

/// Single-valued string field (indexed and column-stored)
/// The column has ords stored for each doc.  Retrieving the corresponding term is done via TermsEnum.
class StrHandler final : public Inverter::IndexHandler {
  friend Inverter;

  // TODO: for unique fields like "id", this could be TermValHash<int32_t>
  TermValHash<DocStream> termsHash; // the set of terms contained in this field
  size_t maxValues = 1; // maximum number of values seen for a single doc

public:
  StrHandler(Inverter& inverter, const std::string_view& fieldName, const std::shared_ptr<FieldType>& fieldType)
    : IndexHandler(PackedTerm(inverter.pool, fieldName), fieldType),
      termsHash(inverter.pool, 4) {
  }

  ~StrHandler() override = default;

  void index(Inverter& inverter, const IndexVal& val) override {
    std::string_view v;

    if (std::holds_alternative<std::string_view>(val.kind)) {
      v = std::get<std::string_view>(val.kind);
    }
    else if (std::holds_alternative<::hpp_proto::bytes_view>(val.kind)) {
      const auto& b = std::get<::hpp_proto::bytes_view>(val.kind);
      v = std::string_view((const char*)b.data(), b.size());
    }
    else if (std::holds_alternative<solux::api::ArrStr>(val.kind)) {
      const auto& arr = std::get<solux::api::ArrStr>(val.kind).v;
      index(inverter, std::span<const std::string_view>(arr.data(), arr.size()));
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
    // values live in inverter.pool; only the heap table is outside-pool.
    accountExtraRam(inverter, termsHash.memSize());
  }

  void resetExtraRam() override { lastExtraBytes_ = termsHash.memSize(); }

  void index(Inverter& inverter, std::span<const std::string_view> vals) override {
    for (auto val : vals) {
      indexSingle(inverter, val);
    }
    maxValues = std::max(maxValues, vals.size());
    // TODO: check if fieldType allows multiple values?
  }

  void flush(Inverter& inverter) override {
    int32_t uniqueVals = termsHash.size();
    if (uniqueVals == 0) {
      return; // drop the field.
    }

    auto nDocs = inverter.getMaxDoc();
    auto guard = MemPool::threadLocalPoolGuard();


    auto terms = termsHash.destructiveCompress();
    boost::sort::spreadsort::string_sort(terms, terms + uniqueVals, TermRef::bracket(), TermRef::getsize(),
                                         TermRef::lessthan());

    // TODO: TextWriter should be refactored (or templated) to handle strings without positions.
    TextWriter textWriter(inverter.getPostingsWriter());
    PostingsWriter::IndexFieldInfo& fieldInfo = inverter.getPostingsWriter().addField(fieldName);
    fieldInfo.type = fieldType->type();
    fieldInfo.flags = fieldType->flags_ & ~FieldType::ABSTRACT;

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
        textWriter.addDoc(docid, 1);  // DOCS-only field: record the doc, no freq/position stored

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