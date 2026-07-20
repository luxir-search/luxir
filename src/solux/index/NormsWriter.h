#pragma once

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <span>

#include "DocStream.h"
#include "ColumnIndexing.h"
#include "PostingsWriter.h"
#include "Stream.h"

namespace solux {

class NormsWriter {
public:
  struct PreparedNorms {
    std::span<const uint8_t> ordinalNorms;
    screaming::BitSet docsBitset;
    int32_t docsWithField = 0;
    int32_t maxDoc = 0;
    bool hasBitset = false;
    bool flat = false;

    TextNormsView textView() const {
      return TextNormsView(ordinalNorms, hasBitset ? &docsBitset : nullptr);
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

  static void writePresenceBitset(MemPool& pool, PostingsWriter& postingsWriter,
                                  PostingsWriter::IndexFieldInfo& fieldInfo,
                                  DocStream& docsWithVal, int32_t docsWithField,
                                  PreparedNorms& prepared) {
    RAMFile tempFile("norm-docs");
    OutputStream tempOut(&tempFile);
    MemPool scratchPool;
    DocsWriter docsWriter(scratchPool, tempOut);
    docsWithVal.pushDocs(pool, docsWriter);
    int32_t added = docsWriter.finish();
    assert(added == docsWithField);
    tempOut.close();

    size_t bitsetLen = tempFile.size();
    assert(bitsetLen > 0);
    char* bitsetBytes = pool.alloc(bitsetLen);
    tempFile.copyTo(bitsetBytes);
    prepared.docsBitset.set(bitsetBytes + bitsetLen);
    prepared.hasBitset = true;

    auto outputPtr = postingsWriter.getOutputStream();
    OutputStream& out = *outputPtr;
    out.write(bitsetBytes, bitsetLen);
    fieldInfo.docsWithField = docsWithField;
    fieldInfo.docsWithFieldEndLoc = out.slocation();
  }

public:
  static PreparedNorms prepare(MemPool& pool, PostingsWriter& postingsWriter,
                               PostingsWriter::IndexFieldInfo& fieldInfo,
                               const Stream& normBytes, DocStream& docsWithVal,
                               int32_t docsWithField) {
    int32_t maxDoc = postingsWriter.getMaxDoc();
    assert(maxDoc >= docsWithField);
    assert(normBytes.size(pool) == docsWithField);

    PreparedNorms prepared;
    prepared.docsWithField = docsWithField;
    prepared.maxDoc = maxDoc;
    fieldInfo.numValues = docsWithField;
    if (docsWithField == 0) {
      fieldInfo.docsWithField = 0;
      fieldInfo.docsWithFieldEndLoc = {0, 0};
      fieldInfo.normsFormat = SegFieldInfo::NORMS_NONE;
      fieldInfo.normsLoc = {0, 0};
      fieldInfo.normsLen = 0;
      return prepared;
    }

    prepared.flat = useDocIdIndexing(docsWithField, maxDoc);
    prepared.ordinalNorms = materializeOrdinalNorms(pool, normBytes, docsWithField);

    if (docsWithField < maxDoc) {
      writePresenceBitset(pool, postingsWriter, fieldInfo, docsWithVal, docsWithField, prepared);
    } else {
      fieldInfo.docsWithField = docsWithField;
      fieldInfo.docsWithFieldEndLoc = {0, 0};
    }
    return prepared;
  }

  static void writeValues(MemPool& pool, PostingsWriter& postingsWriter,
                          PostingsWriter::IndexFieldInfo& fieldInfo,
                          const PreparedNorms& prepared, DocStream& docsWithVal) {
    int32_t maxDoc = prepared.maxDoc;
    int32_t docsWithField = prepared.docsWithField;
    assert(maxDoc == postingsWriter.getMaxDoc());
    assert(maxDoc >= docsWithField);

    if (docsWithField == 0) {
      return;
    }

    auto outPtr = postingsWriter.getOutputStream();
    OutputStream& out = *outPtr;
    size_t start = out.size();
    fieldInfo.normsLoc = seg_location(out.streamNumber, start);
    if (prepared.flat) {
      fieldInfo.normsFormat = SegFieldInfo::NORMS_FLAT;
      fieldInfo.normsLen = maxDoc;
      if (!prepared.hasBitset) {
        assert((int32_t)prepared.ordinalNorms.size() == maxDoc);
        out.write(prepared.ordinalNorms.data(), (size_t)maxDoc);
      } else {
        int32_t nextDoc = 0;
        int32_t ord = 0;
        docsWithVal.forEachDoc(pool, [&](int32_t docid) {
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
      fieldInfo.normsFormat = SegFieldInfo::NORMS_SPARSE;
      fieldInfo.normsLen = docsWithField;
      out.write(prepared.ordinalNorms.data(), (size_t)docsWithField);
    }
  }

  static void finish(MemPool& pool, PostingsWriter& postingsWriter,
                     PostingsWriter::IndexFieldInfo& fieldInfo,
                     const Stream& normBytes, DocStream& docsWithVal,
                     int32_t docsWithField) {
    PreparedNorms prepared = prepare(pool, postingsWriter, fieldInfo, normBytes, docsWithVal,
                                     docsWithField);
    writeValues(pool, postingsWriter, fieldInfo, prepared, docsWithVal);
  }
};

} // namespace solux
