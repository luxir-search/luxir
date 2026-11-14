// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <map>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "TestUtils.h"
#include "luxir/index/IntColWriter.h"
#include "luxir/index/NormsWriter.h"
#include "luxir/index/OrdColWriter.h"
#include "luxir/index/StoredFieldsWriter.h"
#include "luxir/reader/OwnershipShape.h"
#include "luxir/reader/SegmentCore.h"

namespace luxir::test {

struct RowSpaceFixture {
  static constexpr std::string_view CHILD = "items";

  struct Row {
    std::optional<int64_t> number;
    std::vector<std::string> words;
    std::optional<std::string> label;
    std::optional<std::string> stored;
  };

  static std::pair<seg_location, int64_t> mono(PostingsWriter& writer, std::span<const int64_t> values) {
    auto out = writer.getOutputStream();
    MonoWriter mono(writer.pool, *out);
    for (int64_t value : values) mono.addInt64(value);
    mono.finish();
    return {mono.blockLoc, mono.metaOff};
  }

  static OwnershipDescriptor shape(PostingsWriter& writer, std::span<const int64_t> ends,
                                   std::optional<std::span<const int64_t>> owners = {}) {
    auto [loc, metaOff] = mono(writer, ends);
    OwnershipDescriptor descriptor{loc, metaOff, {}, 0};
    if (owners) {
      auto reverse = mono(writer, *owners);
      descriptor.owners = reverse.first;
      descriptor.ownersMetaOff = reverse.second;
    }
    return descriptor;
  }

  static void presence(PostingsWriter& writer, PostingsWriter::IndexFieldInfo& field,
                       std::span<const int32_t> docs, int32_t maxDoc) {
    DocsWithValWriter presence(writer.pool, writer, field);
    if ((int32_t)docs.size() == maxDoc) presence.finishDense(maxDoc);
    else {
      for (int32_t doc : docs) presence.startDoc(doc);
      presence.finish();
    }
  }

  static void fields(PostingsWriter& writer, SpaceOrdinal space, std::span<const Row> rows,
                     std::span<const uint8_t> norms = {}) {
    if (rows.empty()) return;
    auto count = (int32_t)rows.size();
    std::vector<int32_t> docs;
    for (int32_t doc = 0; doc < count; doc++) if (rows[doc].number) docs.push_back(doc);
    if (!docs.empty()) {
      auto& field = writer.addField(space, "number");
      field.type = FieldType::INT;
      field.flags = FieldType::COLUMN_STORED;
      {
        auto out = writer.getOutputStream();
        IntColWriter column(*out);
        for (int32_t doc : docs) column.addInt64(*rows[doc].number);
        column.finish(field);
      }
      presence(writer, field, docs, count);
    }

    std::map<std::string, std::map<int32_t, int32_t>> terms;
    docs.clear();
    for (int32_t doc = 0; doc < count; doc++) {
      if (!rows[doc].words.empty()) docs.push_back(doc);
      for (const auto& word : rows[doc].words) terms[word][doc]++;
    }
    if (!terms.empty()) {
      auto& field = writer.addField(space, "body");
      field.type = FieldType::TEXT;
      field.flags = FieldType::INDEX_DOCS_FREQS;
      DocStream normDocs(writer.pool);
      Stream normBytes;
      NormsWriter::PreparedNorms prepared;
      if (!norms.empty()) {
        assert(norms.size() == docs.size());
        for (size_t i = 0; i < docs.size(); i++) {
          normDocs.addDoc(writer.pool, docs[i]);
          normBytes.writeByte(writer.pool, norms[i]);
        }
        prepared = NormsWriter::prepare(writer.pool, writer, field, normBytes, normDocs, (int32_t)docs.size());
      }
      TextWriter text(writer);
      text.startField(&field);
      if (!norms.empty()) text.setNorms(prepared.textView());
      for (const auto& [word, postings] : terms) {
        TermRef term(writer.pool, word.data(), word.size());
        text.startTerm(term);
        for (auto [doc, freq] : postings) text.addDoc(doc, freq);
        text.endTerm(term);
      }
      text.endField();
      if (norms.empty()) presence(writer, field, docs, count);
      else NormsWriter::writeValues(writer.pool, writer, field, prepared, normDocs);
    }

    terms.clear();
    for (int32_t doc = 0; doc < count; doc++) if (rows[doc].label) terms[*rows[doc].label][doc] = 1;
    if (!terms.empty()) {
      auto& field = writer.addField(space, "label");
      field.type = FieldType::STRING;
      field.flags = FieldType::INDEX_DOCS | FieldType::COLUMN_STORED;
      OrdCollector ords(writer.pool, count);
      {
        TextWriter text(writer);
        text.startField(&field);
        for (const auto& [word, postings] : terms) {
          TermRef term(writer.pool, word.data(), word.size());
          auto ord = text.startTerm(term);
          for (auto [doc, freq] : postings) {
            text.addDoc(doc, freq);
            ords.add(doc, (uint32_t)ord);
          }
          text.endTerm(term);
        }
        text.endField();
      }
      OrdColWriter column(writer.pool, writer, field, ords);
      column.finish();
    }

    StoredFieldsWriter stored(writer, space);
    for (int32_t doc = 0; doc < count; doc++) {
      if (rows[doc].stored) stored.addValue(doc, "payload", *rows[doc].stored);
    }
    stored.finish(count);
  }

  static std::vector<FileDescriptor> write(Directory& dir, uint64_t id,
                                          std::span<const Row> roots, std::span<const Row> children,
                                          std::span<const int64_t> ends,
                                          std::optional<std::span<const int64_t>> owners = {}) {
    PostingsWriter writer(dir, id, 1, (int32_t)roots.size());
    fields(writer, SpaceOrdinal::ROOT, roots);
    if (!children.empty()) {
      auto child = writer.addSpace(CHILD, (int32_t)children.size(), shape(writer, ends, owners));
      fields(writer, child, children);
    }
    writeUnversionedRuns(writer);
    std::vector<FileDescriptor> files;
    EXPECT_TRUE(writer.finish(nullptr, &files));
    return files;
  }
};

} // namespace luxir::test
