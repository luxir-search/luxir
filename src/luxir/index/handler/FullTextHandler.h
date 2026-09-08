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
    for (std::string_view val : vals) {
      if (!first) {
        pos += POSITION_INCREMENT_GAP;
      }
      first = false;
      tc.head.setValue(val);
      tc.reset();
      for (;;) {
        bool hasNext = tail.incrementToken();
        if (!hasNext) break;
        ++numTokens;
        pos += tok.positionIncrement;
        // The token bytes are transient (the chain may reuse the buffer on the
        // next pull); try_emplace copies them into the MemPool below.
        // Oversized tokens index truncated; query-time term building truncates
        // identically (QueryBuilder::copyTerm), so exact match still works.
        std::string_view term = PackedTerm::truncate(tok.text);

        auto [entry, inserted] = termsHash.try_emplace(term, termsHash.getMemPool(), docid, pos);
        if (!inserted) {
          entry->val().addDoc(termsHash.getMemPool(), docid, pos);
        }
      }
    }

    // We index the length even if all tokens were removed somehow, because we still want
    // to record that there was a doc for this field.
    // Store the SmallFloat-encoded norm byte, not the raw token count: the scorers index
    // invNorm[(uint8_t)encodedNorm], so the column must already hold the encoded byte
    // (identity for lengths 0..40, quantized above). Storing raw numTokens silently
    // mis-scores docs over 40 tokens and wraps mod 256 above 255.
    uint8_t encodedNorm = SmallFloat::intToByte4(numTokens);
    normBytes.writeByte(inverter.pool, encodedNorm);
    normDocsWithField.addDoc(inverter.pool, docid);
    numDocsWithField++;

    // termsHash values (posting streams) live in inverter.pool; only its heap table
    // is outside-pool. Account the table growth (usually a no-op: it rehashes rarely).
    accountExtraRam(inverter, termsHash.memSize());
  }

  void flush(Inverter& inverter) override {
    flushPositions(inverter);
  }

  void flushPositions(Inverter& inverter) {
    auto sz = termsHash.size();
    if (numDocsWithField == 0) return;

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
    if (sz > 0) {
      boost::sort::spreadsort::string_sort(terms, terms + sz, TermRef::bracket(), TermRef::getsize(),
                                           TermRef::lessthan());
    }
    // auto endTime = std::chrono::high_resolution_clock::now();
    // auto thisElapsed = std::chrono::duration_cast<std::chrono::nanoseconds>( endTime - startTime ).count();
    // std::cout << "terms=" << sz << " SORT time ns=" << thisElapsed << std::endl;

    PostingsWriter::IndexFieldInfo& fieldInfo = inverter.getPostingsWriter().addField(fieldName);
    fieldInfo.type = fieldType->type();
    fieldInfo.flags = fieldType->flags_ & ~FieldType::ABSTRACT;

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
