// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "luxir/util/ApiError.h"

#include "luxir/index/DocStream.h"
#include "luxir/index/Inverter.h"
#include "luxir/index/NormsWriter.h"
#include "luxir/schema/ValCoerce.h"
#include "luxir/search/Similarity.h"


using namespace luxir;
namespace luxir::handler {

// Indexes text field with frequencies and positions.
class FullTextHandler : public Inverter::IndexHandler {
  friend Inverter;

  TermValHash<DocFreqPosStream> termsHash; // the set of terms contained in this field
  std::unique_ptr<TokenChain> tokenChain;
  Stream normBytes;
  DocStream normDocsWithField;
  int32_t numDocsWithField = 0;
public:
  FullTextHandler(Inverter& inverter, const std::string_view& fieldName, const std::shared_ptr<FieldType>& fieldType)
    : IndexHandler(PackedTerm(inverter.pool, fieldName), fieldType),
      termsHash(inverter.pool, 4),
      normDocsWithField(inverter.pool) {

    // Future optimization: cache the analyzer for the type if it isn't field-specific
    // This can help with memory consumption when the same analyzer can be used for many fields.

    // downcast to TextFieldType to get the analyzer. Use of dynamic_cast is OK this is only done once per field.
    auto textFieldType = dynamic_cast<TextFieldType*>(fieldType.get());
    tokenChain = textFieldType->createAnalyzer(fieldName);
  }

  ~FullTextHandler() override = default;

  // Positions jump by this between values of a multi-valued text field. The
  // gap discourages ordinary phrase matches but does not isolate values:
  // slop >= 100 can cross the boundary (adjacent raw positions differ by 101).
  static constexpr int POSITION_INCREMENT_GAP = 100;

  void index(Inverter& inverter, const IndexVal& val) override {
    // expected kinds first, coercions last
    if (auto s = std::get_if<std::string_view>(&val.kind)) {
      indexValues(inverter, std::span<const std::string_view>(s, 1));
      return;
    }
    if (auto b = std::get_if<::hpp_proto::bytes_view>(&val.kind)) {
      std::string_view sv((const char*)b->data(), b->size());
      indexValues(inverter, std::span<const std::string_view>(&sv, 1));
      return;
    }
    if (auto a = std::get_if<luxir::api::ArrStr>(&val.kind)) {
      indexValues(inverter, std::span<const std::string_view>(a->v.data(), a->v.size()));
      return;
    }
    if (coerce::isNull(val)) return;
    if (coerce::isArray(val)) {
      // mixed / numeric arrays: coerce every element up front (buf is
      // per-element transient, so materialize) and index as multi-valued text
      char buf[coerce::TEXT_BUF_SIZE];
      std::vector<std::string> storage;
      coerce::forEachElement(val, [&](const IndexVal& elem) {
        storage.emplace_back(fieldType->coerceTerm(elem, std::string_view(fieldName), buf));
      });
      std::vector<std::string_view> views(storage.begin(), storage.end());
      indexValues(inverter, std::span<const std::string_view>(views.data(), views.size()));
      return;
    }
    // numeric scalars analyze their canonical rendering ({"n_w": 42} indexes "42")
    char buf[coerce::TEXT_BUF_SIZE];
    std::string_view sv = fieldType->coerceTerm(val, std::string_view(fieldName), buf);
    indexValues(inverter, std::span<const std::string_view>(&sv, 1));
  }

  void index(Inverter& inverter, std::string_view val) override {
    indexValues(inverter, std::span<const std::string_view>(&val, 1));
  }

  void index(Inverter& inverter, std::span<const std::string_view> vals) override {
    indexValues(inverter, vals);
  }

  // Analyze one doc's values for this field: positions continue across values
  // (with POSITION_INCREMENT_GAP between them) and ONE norm entry records the
  // total token count.  Multiple values used to silently index an empty
  // string (the values were only visible via the stored-fields copy, never
  // searchable).
  void indexValues(Inverter& inverter, std::span<const std::string_view> vals) {
    if (vals.size() > 1 && !fieldType->multiValued()) {
      throw DocumentError(fmt::format("Field '{}' is single-valued but received multiple values",
                                           std::string_view(fieldName)));
    }
    if (vals.empty()) return;

    TokenChain& tc = *tokenChain;
    Token& tok = tc.head.getToken();
    TokenStream& tail = *tc.tail;
    int docid = inverter.getDoc();

    int numTokens = 0;
    int pos = -1;
    bool first = true;
    PackedTerm::TermBuffer scratch;
    for (std::string_view val : vals) {
      if (!first) {
        pos += POSITION_INCREMENT_GAP;
      }
      first = false;
      tc.setValue(val);
      tc.reset();
      for (;;) {
        bool hasNext = tail.incrementToken();
        if (!hasNext) break;
        // The token bytes are transient (the chain may reuse the buffer on the
        // next pull); try_emplace copies them into the MemPool below.
        auto term = PackedTerm::fitTerm(fieldType->longTerms, tok.text, scratch);
        if (!term) {
          // Rollback marks the doc deleted but retains earlier postings. They
          // still need a norm at flush, and their table memory must be charged.
          finishValues(inverter, numTokens);
          throw DocumentError(fmt::format("Field '{}': text token is {} bytes after analysis; maximum is {}",
                                          std::string_view(fieldName), tok.text.size(), PackedTerm::MAX_LEN));
        }
        ++numTokens;
        pos += tok.positionIncrement;
        auto [entry, inserted] = termsHash.try_emplace(*term, termsHash.getMemPool(), docid, pos);
        if (!inserted) {
          entry->val().addDoc(termsHash.getMemPool(), docid, pos);
        }
      }
    }

    finishValues(inverter, numTokens);
  }

private:
  void finishValues(Inverter& inverter, int numTokens) {
    // We index the length even if all tokens were removed somehow, because we still want
    // to record that there was a doc for this field.
    // Store the SmallFloat-encoded norm byte, not the raw token count: the scorers index
    // invNorm[(uint8_t)encodedNorm], so the column must already hold the encoded byte
    // (identity for lengths 0..40, quantized above). Storing raw numTokens silently
    // mis-scores docs over 40 tokens and wraps mod 256 above 255.
    uint8_t encodedNorm = SmallFloat::intToByte4(numTokens);
    normBytes.writeByte(inverter.pool, encodedNorm);
    normDocsWithField.addDoc(inverter.pool, inverter.getDoc());
    numDocsWithField++;

    // termsHash values (posting streams) live in inverter.pool; only its heap table
    // is outside-pool. Account the table growth (usually a no-op: it rehashes rarely).
    accountExtraRam(inverter, termsHash.memSize());
  }

public:
  void flush(Inverter& inverter) override {
    flushPositions(inverter);
  }

  void flushPositions(Inverter& inverter) {
    auto sz = termsHash.size();
    if (numDocsWithField == 0) return;

    // Sorting terms per field could run in parallel across fields.
    auto terms = sortForFlush(inverter, termsHash);

    PostingsWriter::IndexFieldInfo& fieldInfo = inverter.getPostingsWriter().addField(fieldName);
    fieldInfo.type = fieldType->type();
    fieldInfo.flags = fieldType->segmentFlags();

    auto preparedNorms = NormsWriter::prepare(inverter.pool, inverter.getPostingsWriter(),
                                              fieldInfo, normBytes, normDocsWithField,
                                              numDocsWithField);
    {
      // Either reduce the resource for these, or share across different fields (in the same thread)
      TextWriter textWriter(inverter.getPostingsWriter());
      textWriter.startField(&fieldInfo);
      textWriter.setNorms(preparedNorms.textView());
      for (size_t tnum = 0; tnum < sz; tnum++) {
        auto term = terms[tnum];
        textWriter.startTerm(term);
        // push all the docs / positions for this term
        term.val().pushDocs(inverter.pool, textWriter);
        textWriter.endTerm(term);
      }
      textWriter.endField();
    }
    termsHash.free();  // free up memory early.

    NormsWriter::writeValues(inverter.pool, inverter.getPostingsWriter(), fieldInfo,
                             preparedNorms, normDocsWithField);
  }

};

}
