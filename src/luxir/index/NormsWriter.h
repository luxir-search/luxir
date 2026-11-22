// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <span>

#include "DocStream.h"
#include "ColumnIndexing.h"
#include "PostingsWriter.h"
#include "Stream.h"

namespace luxir {

class NormsWriter {
public:
  struct PreparedNorms {
    std::span<const uint8_t> ordinalNorms;
    screaming::BitSet docsBitset;
    WrittenPresence presence;
    int32_t maxDoc = 0;
    bool flat = false;

    TextNormsView textView() const {
      return TextNormsView(ordinalNorms, presence.bitmap ? &docsBitset : nullptr);
    }
  };

private:
  static void writeTrailingZeroes(OutputStream& out, int64_t len) {
    static const char zeroes[4096] = {};
    while (len > 0) {
      int64_t chunk = len < (int64_t)sizeof(zeroes) ? len : (int64_t)sizeof(zeroes);
      out.write(zeroes, (size_t)chunk);
      len -= chunk;
    }
  }

  static std::span<const uint8_t> materializeOrdinalNorms(MemPool& pool,
                                                          const Stream& normBytes,
                                                          int32_t docsWithField) {
    uint8_t* bytes = (uint8_t*) pool.alloc((size_t)docsWithField);
    StreamReader reader(normBytes, pool);
    for (int32_t i = 0; i < docsWithField; i++) {
      assert(!reader.eof());
      bytes[i] = (uint8_t) reader.readByte();
    }
    assert(reader.eof());
    return {bytes, (size_t)docsWithField};
  }

  static WrittenPresence writePresenceBitset(MemPool& pool, PostingsWriter& postingsWriter,
                                  DocStream& docs, int32_t docsWithField,
                                  PreparedNorms& prepared) {
    RAMFile tempFile("norm-docs");
    OutputStream tempOut(&tempFile);
    MemPool scratchPool;
    DocsWriter docsWriter(scratchPool, tempOut);
    docs.pushDocs(pool, docsWriter);
    [[maybe_unused]] int32_t added = docsWriter.finish();
    assert(added == docsWithField);
    tempOut.close();

    size_t bitsetLen = tempFile.size();
    assert(bitsetLen > 0);
    // The set's start must be 8-aligned wherever it is materialized (see
    // screaming::BitSet::BLOCK_ALIGN): both in this request-lifetime copy and
    // at the spliced stream offset the readers will later mmap.
    char* bitsetBytes = pool.alloc(bitsetLen, screaming::BitSet::BLOCK_ALIGN);
    tempFile.copyTo(bitsetBytes);
    prepared.docsBitset.set(bitsetBytes + bitsetLen);

    auto outputPtr = postingsWriter.getOutputStream();
    OutputStream& out = *outputPtr;
    static constexpr char zeros[screaming::BitSet::BLOCK_ALIGN] = {};
    size_t pad = (screaming::BitSet::BLOCK_ALIGN
                  - (out.size() & (screaming::BitSet::BLOCK_ALIGN - 1)))
                 & (screaming::BitSet::BLOCK_ALIGN - 1);
    if (pad > 0) {
      out.write(zeros, pad);
    }
    auto start = outputPtr.pos();
    out.write(bitsetBytes, bitsetLen);
    auto slice = postingsWriter.finishStream(std::move(outputPtr));
    return {docsWithField, slice.subSlice(start, bitsetLen)};
  }

public:
  static PreparedNorms prepare(MemPool& pool, PostingsWriter& postingsWriter,
                               SpaceOrdinal space,
                               const Stream& normBytes, DocStream& docs,
                               int32_t docsWithField) {
    int32_t maxDoc = postingsWriter.getMaxDoc(space);
    assert(maxDoc >= docsWithField);
    assert(normBytes.size(pool) == docsWithField);

    PreparedNorms prepared;
    prepared.presence = {docsWithField, {}};
    prepared.maxDoc = maxDoc;
    if (docsWithField == 0) return prepared;

    prepared.flat = useDocIdIndexing(docsWithField, maxDoc);
    prepared.ordinalNorms = materializeOrdinalNorms(pool, normBytes, docsWithField);

    if (docsWithField < maxDoc) {
      prepared.presence = writePresenceBitset(pool, postingsWriter, docs, docsWithField, prepared);
    }
    return prepared;
  }

  [[nodiscard]] static std::optional<field_parts::Norms> writeValues(
      MemPool& pool, PostingsWriter& postingsWriter, const PreparedNorms& prepared, DocStream& docs) {
    int32_t maxDoc = prepared.maxDoc;
    int32_t docsWithField = prepared.presence.docsWithField;
    assert(maxDoc >= docsWithField);

    if (docsWithField == 0) return {};

    auto outPtr = postingsWriter.getOutputStream();
    OutputStream& out = *outPtr;
    if (prepared.flat) {
      if (!prepared.presence.bitmap) {
        assert((int32_t)prepared.ordinalNorms.size() == maxDoc);
        out.write(prepared.ordinalNorms.data(), (size_t)maxDoc);
      } else {
        int32_t nextDoc = 0;
        int32_t ord = 0;
        docs.forEachDoc(pool, [&](int32_t docid) {
          assert(docid >= nextDoc && docid < maxDoc);
          writeTrailingZeroes(out, (int64_t)docid - (int64_t)nextDoc);
          assert(ord < docsWithField);
          out.write((char)prepared.ordinalNorms[(size_t)ord++]);
          nextDoc = docid + 1;
        });
        assert(ord == docsWithField);
        writeTrailingZeroes(out, (int64_t)maxDoc - (int64_t)nextDoc);
      }
    } else {
      out.write(prepared.ordinalNorms.data(), (size_t)docsWithField);
    }
    auto mode = prepared.flat ? field_parts::NormsMode::FLAT : field_parts::NormsMode::SPARSE;
    auto slice = postingsWriter.finishStream(std::move(outPtr));
    assert(slice.bytes == field_parts::Norms::bytes(mode, maxDoc, docsWithField));
    return field_parts::Norms{slice.location, mode};
  }

};

} // namespace luxir
