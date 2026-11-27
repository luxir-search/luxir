// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <algorithm>
#include <bit>
#include <cassert>
#include <cstdint>
#include <span>

#include "DocStream.h"
#include "ColumnIndexing.h"
#include "PostingsWriter.h"
#include "Stream.h"
#include "luxir/reader/NormsLayout.h"

namespace luxir {

class NormsWriter {
public:
  // Tests compare the CONSTANT and PACKED layouts against the raw FLAT and
  // SPARSE layouts, which this forces.
  static inline bool rawLayoutsForTests = false;

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

  // The packed encoding of a field's norm bytes, chosen from their distinct
  // values. bits == 0 means the bytes need more than MAX_PACKED_BITS.
  struct PackedPlan {
    uint32_t bits = 0;
    uint32_t tableLen = 0;  // 0: frame of reference from min
    uint8_t min = 0;
    uint8_t table[NormsLayout::MAX_PACKED_CODES] = {};
    uint8_t codeOf[256] = {};
  };

  // Picks the narrower of frame of reference over [min, max] and a dictionary
  // of the distinct bytes; on a tie, frame of reference (a 1-byte header).
  // withZero adds byte 0 to the code set, for docid-addressed codes whose
  // missing docs must decode to 0. Its code is then 0, which is what the
  // zero-filled slots of missing docs hold.
  static PackedPlan planPacked(std::span<const uint8_t> norms, bool withZero) {
    bool seen[256] = {};
    seen[0] = withZero;
    for (uint8_t b : norms) seen[b] = true;
    PackedPlan plan;
    uint32_t distinct = 0;
    uint32_t lo = 255, hi = 0;
    for (uint32_t b = 0; b < 256; b++) {
      if (!seen[b]) continue;
      distinct++;
      lo = std::min(lo, b);
      hi = std::max(hi, b);
    }
    uint32_t dictBits = NormsLayout::packedBits(distinct);
    uint32_t forBits = NormsLayout::packedBits(hi - lo + 1);
    if (dictBits == 0) {
      return plan;
    }
    plan.min = (uint8_t) lo;
    if (forBits != 0 && forBits <= dictBits) {
      plan.bits = forBits;
      for (uint32_t b = lo; b <= hi; b++) plan.codeOf[b] = (uint8_t) (b - lo);
    } else {
      plan.bits = dictBits;
      for (uint32_t b = lo; b <= hi; b++) {
        if (!seen[b]) continue;
        plan.codeOf[b] = (uint8_t) plan.tableLen;
        plan.table[plan.tableLen++] = (uint8_t) b;
      }
    }
    assert(!withZero || plan.codeOf[0] == 0);
    return plan;
  }

  static int64_t packedRegionBytes(const PackedPlan& plan, int64_t count) {
    int64_t header = NormsLayout::PACKED_HEADER_BYTES
        + (plan.tableLen == 0 ? 1 : (int64_t) plan.tableLen);
    return header + NormsLayout::packedBytes(count, plan.bits);
  }

  // Codes go to slots by docid (missing docs keep code 0, see planPacked) or
  // by rank.
  static void writePacked(MemPool& pool, OutputStream& out, const PackedPlan& plan,
                          const PreparedNorms& prepared, DocStream& docs,
                          bool byDoc) {
    int64_t count = byDoc ? prepared.maxDoc : prepared.presence.docsWithField;
    size_t len = (size_t) NormsLayout::packedBytes(count, plan.bits);
    uint8_t* codes = (uint8_t*) pool.alloc(len);
    std::fill(codes, codes + len, (uint8_t) 0);
    uint32_t perByteLog = 3 - (uint32_t) std::countr_zero(plan.bits);
    uint32_t idxMask = (1u << perByteLog) - 1;
    auto put = [&](int64_t slot, uint8_t norm) {
      uint32_t shift = ((uint32_t) slot & idxMask) * plan.bits;
      codes[(size_t) (slot >> perByteLog)] |= (uint8_t) (plan.codeOf[norm] << shift);
    };
    if (byDoc && prepared.presence.bitmap) {
      int32_t ord = 0;
      docs.forEachDoc(pool, [&](int32_t docid) {
        put(docid, prepared.ordinalNorms[(size_t) ord++]);
      });
      assert(ord == prepared.presence.docsWithField);
    } else {
      for (size_t i = 0; i < prepared.ordinalNorms.size(); i++) {
        put((int64_t) i, prepared.ordinalNorms[i]);
      }
    }
    uint8_t header[NormsLayout::PACKED_HEADER_BYTES] = {(uint8_t) plan.bits,
                         byDoc ? NormsLayout::BY_DOC : NormsLayout::BY_RANK,
                         (uint8_t) plan.tableLen};
    out.write((const char*) header, sizeof(header));
    if (plan.tableLen == 0) {
      out.write((char) plan.min);
    } else {
      out.write((const char*) plan.table, plan.tableLen);
    }
    out.write((const char*) codes, len);
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

    // Also guards a field written without norm bytes.
    if (docsWithField == 0 || prepared.ordinalNorms.empty()) return {};

    auto outPtr = postingsWriter.getOutputStream();
    OutputStream& out = *outPtr;
    std::span<const uint8_t> norms = prepared.ordinalNorms;
    // Docid-addressed layouts read a doc without the field as norm byte 0.
    bool zeroForMissing = prepared.flat && prepared.presence.bitmap;
    bool uniform = std::all_of(norms.begin(), norms.end(),
                               [&](uint8_t b) { return b == norms[0]; });
    if (uniform && !zeroForMissing && !rawLayoutsForTests) {
      // Every doc with the field has this byte. Without a bitmap every doc
      // has the field; with one (rank addressing) readers walk the bitmap.
      out.write((char) norms[0]);
      auto slice = postingsWriter.finishStream(std::move(outPtr));
      return field_parts::Norms{slice.location, field_parts::NormsMode::CONSTANT};
    }
    // Packed keeps the raw layout's addressing (docid when the field is
    // dense enough for FLAT, presence rank otherwise) and is used only when
    // it is smaller than the raw bytes. A uniform field with docid
    // addressing and gaps packs to 1 bit over {0, byte}.
    int64_t rawCount = prepared.flat ? maxDoc : docsWithField;
    PackedPlan plan = planPacked(norms, zeroForMissing);
    if (plan.bits != 0 && packedRegionBytes(plan, rawCount) < rawCount && !rawLayoutsForTests) {
      writePacked(pool, out, plan, prepared, docs, prepared.flat);
      auto slice = postingsWriter.finishStream(std::move(outPtr));
      return field_parts::Norms{slice.location, field_parts::NormsMode::PACKED, slice.bytes};
    }
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
    assert(slice.bytes == field_parts::Norms::bytes(mode, maxDoc, docsWithField, 0));
    return field_parts::Norms{slice.location, mode};
  }

};

} // namespace luxir
