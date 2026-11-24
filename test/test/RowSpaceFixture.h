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

  static field_parts::Slice mono(PostingsWriter& writer, std::span<const int64_t> values) {
    auto out = writer.getOutputStream();
    MonoWriter mono(writer.pool, *out);
    for (int64_t value : values) mono.addInt64(value);
    return mono.finish(writer, std::move(out));
  }

  static OwnershipDescriptor shape(PostingsWriter& writer, std::span<const int64_t> ends,
                                   std::optional<std::span<const int64_t>> owners = {}) {
    OwnershipDescriptor descriptor{mono(writer, ends), {}};
    if (owners) descriptor.owners = mono(writer, *owners);
    return descriptor;
  }

  static void fields(PostingsWriter& writer, SpaceOrdinal space, std::string_view path, std::span<const Row> rows,
                     std::span<const uint8_t> norms = {}) {
    if (rows.empty()) return;
    auto count = (int32_t)rows.size();
    std::vector<int32_t> docs;
    for (int32_t doc = 0; doc < count; doc++) if (rows[doc].number) docs.push_back(doc);
    if (!docs.empty()) {
      auto& field = writer.addField(space, Schema::qualify(path, "number"));
      field.header.type = FieldType::INT;
      field.header.flags = FieldType::COLUMN_STORED;
      {
        auto out = writer.getOutputStream();
        IntColWriter column(*out);
        for (int32_t doc : docs) column.addInt64(*rows[doc].number);
        auto written = column.finish(writer, std::move(out));
        auto presence = PresenceWriter::write(writer.pool, writer, space, (int32_t)docs.size(),
            [&](auto&& accept) { for (auto doc : docs) accept(doc); });
        field.setColumn(WrittenColumn{written.column, written.numValues, presence, {}, {},
                         !written.bounds.empty() ? field_parts::Bounds(written.bounds) : field_parts::Bounds()});
      }
    }

    std::map<std::string, std::map<int32_t, int32_t>> terms;
    docs.clear();
    for (int32_t doc = 0; doc < count; doc++) {
      if (!rows[doc].words.empty()) docs.push_back(doc);
      for (const auto& word : rows[doc].words) terms[word][doc]++;
    }
    if (!terms.empty()) {
      auto& field = writer.addField(space, Schema::qualify(path, "body"));
      field.header.type = FieldType::TEXT;
      field.header.flags = FieldType::INDEX_DOCS_FREQS | (norms.empty() ? FieldType::OMIT_NORMS : 0);
      DocStream normDocs(writer.pool);
      Stream normBytes;
      NormsWriter::PreparedNorms prepared;
      if (!norms.empty()) {
        assert(norms.size() == docs.size());
        for (size_t i = 0; i < docs.size(); i++) {
          normDocs.addDoc(writer.pool, docs[i]);
          normBytes.writeByte(writer.pool, norms[i]);
        }
        prepared = NormsWriter::prepare(writer.pool, writer, space, normBytes, normDocs, (int32_t)docs.size());
      }
      TextWriter text(writer);
      text.startField(space, field);
      if (!norms.empty()) text.setNorms(prepared.textView());
      for (const auto& [word, postings] : terms) {
        TermRef term(writer.pool, word.data(), word.size());
        text.startTerm(term);
        for (auto [doc, freq] : postings) text.addDoc(doc, freq);
        text.endTerm(term);
      }
      field.setPostings(text.endField());
      if (norms.empty()) writeFieldPresence(writer, field, docs);
      else {
        field.setPresence(prepared.presence);
        if (auto part = NormsWriter::writeValues(writer.pool, writer, prepared, normDocs)) field.setNorms(*part);
      }
    }

    terms.clear();
    for (int32_t doc = 0; doc < count; doc++) if (rows[doc].label) terms[*rows[doc].label][doc] = 1;
    if (!terms.empty()) {
      auto& field = writer.addField(space, Schema::qualify(path, "label"));
      field.header.type = FieldType::STRING;
      field.header.flags = FieldType::INDEX_DOCS | FieldType::COLUMN_STORED;
      OrdCollector ords(writer.pool, count);
      {
        TextWriter text(writer);
        text.startField(space, field);
        for (const auto& [word, postings] : terms) {
          TermRef term(writer.pool, word.data(), word.size());
          auto ord = text.startTerm(term);
          for (auto [doc, freq] : postings) {
            text.addDoc(doc, freq);
            ords.add(doc, (uint32_t)ord);
          }
          text.endTerm(term);
        }
        field.setPostings(text.endField());
      }
      OrdColWriter columnWriter(writer.pool, writer, space, field.header.flags, ords);
      auto column = columnWriter.finish(field.header.indexedTotals.numTerms);
      field.setColumn(std::move(column));
    }

    StoredFieldsWriter stored(writer);
    for (int32_t doc = 0; doc < count; doc++) {
      if (rows[doc].stored) stored.addValue(doc, Schema::qualify(path, "payload"), *rows[doc].stored);
    }
    if (auto part = stored.finish(count)) {
      writer.addStoredField(space, Postings::STORED_DEFAULT_RESOURCE, count, *part);
    }
  }

  static std::vector<FileDescriptor> write(Directory& dir, uint64_t id,
                                          std::span<const Row> roots, std::span<const Row> children,
                                          std::span<const int64_t> ends,
                                          std::optional<std::span<const int64_t>> owners = {}) {
    PostingsWriter writer(dir, id, 1, (int32_t)roots.size());
    fields(writer, SpaceOrdinal::ROOT, "", roots);
    if (!children.empty()) {
      auto child = writer.addSpace(CHILD);
      writer.finishSpace(child, (int32_t)children.size(), shape(writer, ends, owners));
      fields(writer, child, CHILD, children);
    }
    writeUnversionedRuns(writer);
    std::vector<FileDescriptor> files;
    EXPECT_TRUE(writer.finish(nullptr, &files));
    return files;
  }
};

} // namespace luxir::test
