// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "luxir/util/ApiError.h"
#include "StringValue.h"

#include "luxir/index/DocStream.h"
#include "luxir/index/Inverter.h"
#include "luxir/index/OrdCollector.h"
#include "luxir/index/ArrayOrdValues.h"
#include "luxir/index/OrdColWriter.h"
#include "luxir/schema/ValCoerce.h"
#include "luxir/util/TermValHash.h"


using namespace luxir;
namespace luxir::handler {

/// String field (indexed and column-stored)
/// The column has ords stored for each doc.  Retrieving the corresponding term is done via TermsEnum.
template <bool Array = false>
class StrHandler final : public Inverter::IndexHandler {
  friend Inverter;
  StringValue<true> stringValue;

  // TODO: for unique fields like "id", this could be TermValHash<int32_t>
  LUXIR_UNALIGNED_START
  struct ArrayTerm : DocStream {
    uint32_t id = 0;
    explicit ArrayTerm(MemPool& pool) : DocStream(pool) {}
  } LUXIR_UNALIGNED_END;
  struct NoArrayValues { explicit NoArrayValues(MemPool&) {} };
  [[no_unique_address]] std::conditional_t<Array, ArrayOrdValues, NoArrayValues> arrayValues;
  TermValHash<std::conditional_t<Array, ArrayTerm, DocStream>> termsHash; // the set of terms contained in this field

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
      stringValue(*fieldType), arrayValues(inverter.pool), termsHash(inverter.pool, 4), docsWithField(inverter.pool) {
  }

  ~StrHandler() override = default;

  void index(Inverter& inverter, const IndexVal& val) override {
    if (auto s = std::get_if<std::string_view>(&val.kind)) {
      index(inverter, *s);
      return;
    }
    if (coerce::isNull(val)) return;
    if (coerce::isArray(val)) {
      auto account = scope_guard([&] { accountExtraRam(inverter, termsHash.memSize() + stringValue.retainedBytes()); });
      auto values = stringValue.prepareMany(val, std::string_view(fieldName));
      checkMultiValued(inverter, values.size());
      for (auto term : values) appendPrepared(inverter, term);
    } else indexSingle(inverter, val);
  }

  void index(Inverter& inverter, std::string_view val) override {
    indexSingle(inverter, val);
  }

private:
  template <typename T>
  void indexSingle(Inverter& inverter, const T& val) {
    if (!stringValue.hasNormalizer()) {
      appendPrepared(inverter, stringValue.prepare(val, std::string_view(fieldName)));
      accountExtraRam(inverter, termsHash.memSize() + stringValue.retainedBytes());
      return;
    }
    auto account = scope_guard([&] { accountExtraRam(inverter, termsHash.memSize() + stringValue.retainedBytes()); });
    appendPrepared(inverter, stringValue.prepare(val, std::string_view(fieldName)));
  }

  void appendPrepared(Inverter& inverter, std::string_view term) {
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
      if constexpr (Array) arrayValues.startDoc(inverter.pool);
    }
    int32_t rank = nDocsWithField - 1;
    auto [entry, inserted] = termsHash.try_emplace(term, termsHash.getMemPool());
    if constexpr (Array) {
      if (inserted) entry->val().id = (uint32_t)termsHash.size();
      arrayValues.add(inverter.pool, entry->val().id);
    } else {
      unused(inserted);
    }
    // don't record duplicates for the same doc (postings are keyed by rank).
    if (entry->val().getLastDoc() != rank) {
      entry->val().addDoc(termsHash.getMemPool(), rank);
    }
  }

public:
  void index(Inverter& inverter, std::span<const std::string_view> vals) override {
    auto account = scope_guard([&] { accountExtraRam(inverter, termsHash.memSize() + stringValue.retainedBytes()); });
    checkMultiValued(inverter, vals.size());
    auto prepared = stringValue.prepareMany(vals, std::string_view(fieldName));
    for (auto term : prepared) appendPrepared(inverter, term);
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


    auto terms = sortForFlush(inverter, termsHash);

    // TODO: TextWriter should be refactored (or templated) to handle strings without positions.
    TextWriter textWriter(inverter.getPostingsWriter());
    PostingsWriter::IndexFieldInfo& fieldInfo = inverter.getPostingsWriter().addField(fieldName);
    fieldInfo.type = fieldType->type();
    fieldInfo.flags = fieldType->segmentFlags();
    fieldInfo.typeFlags = fieldType->segmentTypeFlags();

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

    std::conditional_t<Array, std::vector<uint32_t>, std::monostate> termToOrd;
    if constexpr (Array) {
      arrayValues.finish(inverter.pool);
      termToOrd.resize((size_t)uniqueVals + 1);
    }
    auto ords = [&]() {
      if constexpr (Array) return ArrayOrdValues::Mapped(arrayValues, inverter.pool, termToOrd);
      else return OrdCollector(guard.pool(), nDocsWithField);
    }();
    size_t poolStart = guard.pool().size();

    textWriter.startField(&fieldInfo);
    for (int32_t tnum = 0; tnum < uniqueVals; tnum++) {
      auto term = terms[tnum];
      int64_t ord = textWriter.startTerm(term);
      assert(ord <= INT32_MAX);
      if constexpr (Array) termToOrd[term.val().id] = (uint32_t)ord;
      // push all the docs for this term to the TextWriter, as well as record the ordinal for each doc
      term.val().forEachDoc(termsHash.getMemPool(), [&](int rank) {
        int32_t docid = full ? rank : rankToDoc[rank];
        textWriter.addDoc(docid, 1);  // DOCS-only field: record the doc, no freq/position stored
        if constexpr (!Array) ords.add(rank, (uint32_t)ord);
      });
      textWriter.endTerm(term);
    }
    textWriter.endField();
    termsHash.free();
    // Ord workspace at its peak; the pool's growth counts the spilled
    // multi-valued ords to within a block.
    size_t collectorBytes;
    if constexpr (Array) collectorBytes = termToOrd.capacity() * sizeof(uint32_t);
    else collectorBytes = ords.slotBytes();
    inverter.noteFlushWorkspace(collectorBytes + rankToDoc.capacity() * sizeof(int32_t)
                                + (guard.pool().size() - poolStart));

    BasicOrdColWriter<decltype(ords)> ordsWriter(guard.pool(), inverter.postingsWriter, fieldInfo, ords);
    ordsWriter.finish(rankToDoc);
  }

};

}
