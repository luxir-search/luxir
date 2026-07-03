#pragma once

#include "solux/index/DocStream.h"
#include "solux/index/Inverter.h"
#include "solux/index/OrdCollector.h"
#include "solux/index/OrdColWriter.h"
#include "solux/schema/ValCoerce.h"
#include "solux/util/TermValHash.h"


using namespace solux;
namespace solux::handler {

/// Single-valued string field (indexed and column-stored)
/// The column has ords stored for each doc.  Retrieving the corresponding term is done via TermsEnum.
class StrHandler final : public Inverter::IndexHandler {
  friend Inverter;

  // TODO: for unique fields like "id", this could be TermValHash<int32_t>
  TermValHash<DocStream> termsHash; // the set of terms contained in this field

public:
  StrHandler(Inverter& inverter, const std::string_view& fieldName, const std::shared_ptr<FieldType>& fieldType)
    : IndexHandler(PackedTerm(inverter.pool, fieldName), fieldType),
      termsHash(inverter.pool, 4) {
  }

  ~StrHandler() override = default;

  void index(Inverter& inverter, const IndexVal& val) override {
    // expected kinds first, coercions last
    if (auto s = std::get_if<std::string_view>(&val.kind)) {
      indexSingle(inverter, *s);
      return;
    }
    if (auto b = std::get_if<::hpp_proto::bytes_view>(&val.kind)) {
      indexSingle(inverter, std::string_view((const char*)b->data(), b->size()));
      return;
    }
    if (auto a = std::get_if<solux::api::ArrStr>(&val.kind)) {
      index(inverter, std::span<const std::string_view>(a->v.data(), a->v.size()));
      return;
    }
    if (coerce::isNull(val)) return;
    // Numeric arms (and arrays of them) index their canonical rendering.
    // buf is reused per element: indexSingle copies the term into the hash
    // before the next render.
    char buf[coerce::TEXT_BUF_SIZE];
    if (coerce::isArray(val)) {
      checkMultiValued(inverter, [&] {
        size_t n = 0;
        coerce::forEachElement(val, [&](const IndexVal&) { n++; });
        return n;
      }());
      coerce::forEachElement(val, [&](const IndexVal& elem) {
        indexSingle(inverter, fieldType->coerceTerm(elem, std::string_view(fieldName), buf));
      });
    } else {
      indexSingle(inverter, fieldType->coerceTerm(val, std::string_view(fieldName), buf));
    }
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

  void index(Inverter& inverter, std::span<const std::string_view> vals) override {
    checkMultiValued(inverter, vals.size());
    for (auto val : vals) {
      indexSingle(inverter, val);
    }
  }

private:
  // A single-valued STRING field must reject multi-value input before any
  // append: the ord column would silently become multi-valued while readers
  // shape results from the schema (and assert on the mismatch).
  void checkMultiValued(Inverter& inverter, size_t n) {
    unused(inverter);
    if (n > 1 && !fieldType->multiValued()) {
      throw std::runtime_error(fmt::format("Field '{}' is single-valued but received multiple values",
                                           std::string_view(fieldName)));
    }
  }

public:

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