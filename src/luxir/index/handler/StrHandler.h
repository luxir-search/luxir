// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "luxir/util/ApiError.h"
#include "StringValue.h"

#include "luxir/index/DocStream.h"
#include "luxir/index/Inverter.h"
#include "luxir/index/OrdCollector.h"
#include "luxir/index/OrdColWriter.h"
#include "luxir/schema/ValCoerce.h"
#include "luxir/util/TermValHash.h"


using namespace luxir;
namespace luxir::handler {

/// Single-valued string field (indexed and column-stored)
/// The column has ords stored for each doc.  Retrieving the corresponding term is done via TermsEnum.
class StrHandler final : public Inverter::IndexHandler {
  friend Inverter;
  StringValue stringValue;

  // TODO: for unique fields like "id", this could be TermValHash<int32_t>
  TermValHash<DocStream> termsHash; // the set of terms contained in this field

  // Postings store a field-rank (the doc's rank among docs-with-value) rather than a
  // docid, so the ord collector at flush is sized to docsWithField, not maxDoc.
  // docsWithField maps rank->docid, in doc order, and resolves the real docids back
  // for the inverted-index and the docs-with-field column.
  DocStream docsWithField;
  int32_t nDocsWithField = 0;   // next field-rank == count of docs with a value
  int32_t lastFieldDoc = -1;    // last doc that contributed a value to this field

public:
  StrHandler(Inverter& inverter, const std::string_view& fieldName, const std::shared_ptr<FieldType>& fieldType)
    : IndexHandler(PackedTerm(inverter.pool, fieldName), fieldType),
      stringValue(*fieldType), termsHash(inverter.pool, 4), docsWithField(inverter.pool) {
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
    if (auto a = std::get_if<luxir::api::ArrStr>(&val.kind)) {
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
    term = stringValue.normalize(term);
    PackedTerm::TermBuffer scratch;
    auto fitted = PackedTerm::fitTerm(fieldType->longTerms, term, scratch);
    if (!fitted) {
      throw DocumentError(fmt::format("Field '{}': string value is {} bytes after normalization; maximum is {}",
                                      std::string_view(fieldName), term.size(), PackedTerm::MAX_LEN));
    }
    term = *fitted;
    // Assign this doc a field-rank the first time it contributes any value, recording
    // rank->docid in docsWithField.  All of a doc's terms then share that rank.
    int32_t doc = inverter.getDoc();
    if (lastFieldDoc != doc) {
      lastFieldDoc = doc;
      // Built and read back through termsHash's pool, like the per-term streams, so
      // both sides name the same pool explicitly rather than relying on it happening
      // to equal inverter.pool.
      docsWithField.addDoc(termsHash.getMemPool(), doc);
      nDocsWithField++;
    }
    int32_t rank = nDocsWithField - 1;
    auto [entry, inserted] = termsHash.try_emplace(term, termsHash.getMemPool());
    unused(inserted);
    // don't record duplicates for the same doc (postings are keyed by rank).
    if (entry->val().getLastDoc() != rank) {
      entry->val().addDoc(termsHash.getMemPool(), rank);
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
  // append: observed-multi segments are only legal for declared-multi fields.
  void checkMultiValued(Inverter& inverter, size_t n) {
    unused(inverter);
    if (n > 1 && !fieldType->multiValued()) {
      throw DocumentError(fmt::format("Field '{}' is single-valued but received multiple values",
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
    fieldInfo.flags = fieldType->segmentFlags();

    // Postings are keyed by field-rank, so the ord collector is sized to docsWithField
    // (not maxDoc).  For a full field rank == docid, so we skip the rank->docid table
    // and index directly; otherwise materialize it once for both the inverted-index
    // scatter and the docs-with-field column.
    bool full = (nDocsWithField == nDocs);
    std::vector<int32_t> rankToDoc;
    if (!full) {
      rankToDoc.reserve((size_t)nDocsWithField);
      docsWithField.forEachDoc(termsHash.getMemPool(), [&](int d) { rankToDoc.push_back(d); });
    }

    OrdCollector ords(guard.pool(), nDocsWithField);

    textWriter.startField(&fieldInfo);
    for (int32_t tnum = 0; tnum < uniqueVals; tnum++) {
      auto term = terms[tnum];
      int64_t ord = textWriter.startTerm(term);
      assert(ord <= INT32_MAX);
      // push all the docs for this term to the TextWriter, as well as record the ordinal for each doc
      term.val().forEachDoc(termsHash.getMemPool(), [&](int rank) {
        int32_t docid = full ? rank : rankToDoc[rank];
        textWriter.addDoc(docid, 1);  // DOCS-only field: record the doc, no freq/position stored
        ords.add(rank, (uint32_t)ord);
      });
      textWriter.endTerm(term);
    }
    textWriter.endField();
    termsHash.free();

    OrdColWriter ordsWriter(guard.pool(), inverter.postingsWriter, fieldInfo, ords);
    ordsWriter.finish(rankToDoc);
  }

};

}
