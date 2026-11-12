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



namespace luxir::handler {

/// Column-only strings: SET stores sorted unique normalized bytes; ARRAY keeps order and repeats.
/// If the string is indexed and stored, see StrHandler.
/// This class also handles binary values.
///
/// Layout:
///   - column bytes: concatenated raw value bytes (no inline metadata).
///   - endOffsetReader (mono2Loc): per-value -> byte offset. Absent when all values are the same
///     size; in that case mono2MetaOff holds the fixed value size.
///   - endValueRankReader (monoLoc): per-doc -> per-value rank boundary. Present only for multi-valued.
class StrColHandler : public Inverter::IndexHandler {
  friend Inverter;
  StringValue<false> stringValue;
  std::vector<std::string_view> docValues;
  size_t scratchBytes = 0;

protected:
  enum class ValueStorage {
    BUFFERED,
    STREAMED,
  };

private:
  DocStream docsWithVal;      // set of docs that have this field
  IntStream valSizeStream;    // per-value sizes (one entry per value)
  IntStream valCountStream;   // per-doc value counts (one entry per doc with field; only used if multi-valued)
  // TODO: a RAMFile per field with the current buffer sizes (starting at 1K) is bad for many fields...
  // We should have initial small buffers pool allocated.
  RAMFile valuesFile;
  OutputStream bufferedValuesOut;
  ValueStorage valueStorage;
  OutputStreamPtr streamedValuesOut;
  seg_location streamedValuesLoc;
  int64_t streamedValuesLength = 0;
  int64_t numDocs = 0;
  int64_t numValuesTotal = 0;
  int32_t minSize = std::numeric_limits<int32_t>::max();
  int32_t maxSize = std::numeric_limits<int32_t>::min();

public:
  StrColHandler(Inverter& inverter, const std::string_view& fieldName, const std::shared_ptr<FieldType>& fieldType)
    : StrColHandler(inverter, fieldName, fieldType, ValueStorage::BUFFERED) {
  }

protected:
  StrColHandler(Inverter& inverter, const std::string_view& fieldName,
                const std::shared_ptr<FieldType>& fieldType, ValueStorage valueStorage)
    : IndexHandler(PackedTerm(inverter.pool, fieldName), fieldType),
      stringValue(*fieldType),
      docsWithVal(inverter.pool),
      valSizeStream(inverter.pool),
      valCountStream(inverter.pool),
      valuesFile(fieldName, {}, inverter.pool.upstream_resource()),
      bufferedValuesOut(&valuesFile),
      valueStorage(valueStorage)
  {
  }

public:
  ~StrColHandler() override = default;

  void index(Inverter& inverter, const IndexVal& val) override {
    if (auto s = std::get_if<std::string_view>(&val.kind)) {
      index(inverter, *s);
      return;
    }
    if (coerce::isNull(val)) return;
    if (coerce::isArray(val)) {
      auto account = scope_guard([&] { accountScratch(inverter); });
      appendPrepared(inverter, stringValue.prepareMany(val, std::string_view(fieldName)));
    } else indexSingle(inverter, val);
  }

  void index(Inverter& inverter, std::string_view val) override {
    indexSingle(inverter, val);
  }

  void index(Inverter& inverter, std::span<const std::string_view> vals) override {
    auto account = scope_guard([&] { accountScratch(inverter); });
    appendPrepared(inverter, stringValue.prepareMany(vals, std::string_view(fieldName)));
  }

private:
  template <typename T>
  void indexSingle(Inverter& inverter, const T& val) {
    if (!stringValue.hasNormalizer()) {
      appendSingle(inverter, stringValue.prepare(val, std::string_view(fieldName)));
      return;
    }
    auto account = scope_guard([&] { accountScratch(inverter); });
    appendSingle(inverter, stringValue.prepare(val, std::string_view(fieldName)));
  }

  void appendPrepared(Inverter& inverter, std::span<const std::string_view> vals) {
    if (vals.empty()) return;
    if (!fieldType->multiValued()) {
      if (vals.size() > 1) throw DocumentError(fmt::format(
          "Field '{}' is single-valued but received multiple values", std::string_view(fieldName)));
      appendSingle(inverter, vals[0]);
    } else if (fieldType->multiSet()) {
      docValues.assign(vals.begin(), vals.end());
      std::ranges::sort(docValues);
      docValues.erase(std::unique(docValues.begin(), docValues.end()), docValues.end());
      appendMulti(inverter, docValues);
    } else appendMulti(inverter, vals);
  }

  void accountScratch(Inverter& inverter) {
    scratchBytes = docValues.capacity() * sizeof(std::string_view)
        + stringValue.retainedBytes();
    accountExtraRam(inverter, valuesFile.size() + scratchBytes);
  }

protected:
  // Raw column appends are also used by VectorHandler after vector validation.
  void appendSingle(Inverter& inverter, std::string_view term) {
    numDocs++;
    docsWithVal.addDoc(inverter.pool, inverter.getDoc());
    addValue(inverter, term);
    if (fieldType->flags_ & FieldType::MULTI_VALUED) {
      valCountStream.addVal(inverter.pool, 1);
    }
    // Buffered column bytes accumulate outside inverter.pool. Streamed columns
    // leave valuesFile empty; their OutputStream storage is owned by Directory.
    if (valueStorage == ValueStorage::BUFFERED) {
      accountExtraRam(inverter, valuesFile.size() + scratchBytes);
    }
  }

  void appendMulti(Inverter& inverter, std::span<const std::string_view> vals) {
    if (!(fieldType->flags_ & FieldType::MULTI_VALUED)) {
      throw DocumentError(fmt::format("Field '{}' is single-valued but received multiple values",
                                          std::string_view(fieldName)));
    }
    if (vals.empty()) return;

    numDocs++;
    docsWithVal.addDoc(inverter.pool, inverter.getDoc());
    for (const auto& val : vals) {
      addValue(inverter, val);
    }
    valCountStream.addVal(inverter.pool, (int64_t)vals.size());
    if (valueStorage == ValueStorage::BUFFERED) {
      accountExtraRam(inverter, valuesFile.size() + scratchBytes);
    }
  }

protected:
  /// Whether to persist a per-value-rank -> owning-docId monotonic column (the
  /// reverse of endValueRankReader).  Only written for multi-valued columns whose
  /// consumers need vector-rank -> doc resolution; VectorHandler overrides this to
  /// true so multi-valued kNN hits can be grouped back to their document.  For
  /// single-valued fields valueRank == docRank, so no map is ever written.
  virtual bool writesValueDocMap() const { return false; }

private:
  void addValue(Inverter& inverter, std::string_view val) {
    int32_t valSize = (int32_t)val.size();
    minSize = std::min(minSize, valSize);
    maxSize = std::max(maxSize, valSize);
    valueOutput(inverter).write(val.data(), valSize);
    valSizeStream.addVal(inverter.pool, valSize);
    numValuesTotal++;
  }

  OutputStream& valueOutput(Inverter& inverter) {
    if (valueStorage == ValueStorage::BUFFERED) {
      return bufferedValuesOut;
    }
    if (streamedValuesOut == nullptr) {
      streamedValuesOut = inverter.getPostingsWriter().getOutputStream();
      streamedValuesLoc = streamedValuesOut->slocation();
    }
    return *streamedValuesOut;
  }

public:

  void finishIndexing(Inverter& inverter) override {
    unused(inverter);
    if (streamedValuesOut != nullptr) {
      streamedValuesLength = (int64_t)streamedValuesOut->size()
          - (int64_t)streamedValuesLoc.offset();
      streamedValuesOut.reset();
    }
  }

  void flush(Inverter& inverter) override {
    if (numDocs == 0) {
      return; // drop the field.
    }
    auto& tmpPool = MemPool::threadLocal();

    PostingsWriter& postingsWriter = inverter.getPostingsWriter();
    PostingsWriter::IndexFieldInfo& fieldInfo = postingsWriter.addField(fieldName);
    fieldInfo.type = fieldType->type();
    fieldInfo.flags = fieldType->segmentFlags();
    fieldInfo.typeFlags = fieldType->segmentTypeFlags();
    fieldInfo.numValues = numValuesTotal;

    auto maxDoc = inverter.getMaxDoc();
    bool full = numDocs == maxDoc;
    bool multiValued = (fieldInfo.flags & FieldType::MULTI_VALUED) != 0;
    // When there are no values (e.g., multi-valued field with only empty arrays),
    // minSize was never updated.  Treat that as fixed-size 0.
    bool fixedSize = (numValuesTotal == 0) || (minSize == maxSize);

    // Buffered string/binary columns are copied to a segment output now.
    // Streamed columns already wrote their bytes during indexing and released
    // the output in finishIndexing(), before any field flush began.
    if (valueStorage == ValueStorage::BUFFERED) {
      bufferedValuesOut.flush(true);
      OutputStreamPtr out = postingsWriter.getOutputStream();
      fieldInfo.columnLoc = out->slocation();
      out->appendFile(valuesFile);
      // no metadata for the raw value bytes; columnMetaOff is the size of the column.
      fieldInfo.columnMetaOff = out->size() - fieldInfo.columnLoc.offset();
    } else {
      assert(streamedValuesOut == nullptr);
      assert(numValuesTotal > 0);
      assert(!streamedValuesLoc.isNull());
      fieldInfo.columnLoc = streamedValuesLoc;
      fieldInfo.columnMetaOff = streamedValuesLength;
    }

    // Write endOffsetReader (mono2) if variable-size; otherwise record the fixed value size.
    if (fixedSize) {
      fieldInfo.mono2MetaOff = (numValuesTotal == 0) ? 0 : minSize;
    } else {
      auto guard = tmpPool.rewindScopeGuard();
      OutputStreamPtr out = postingsWriter.getOutputStream();
      MonoWriter endOffsetWriter(tmpPool, *out);
      int64_t endOffset = 0;
      valSizeStream.visitValues(inverter.pool, [&endOffset, &endOffsetWriter](auto val) {
        endOffset += val;
        endOffsetWriter.addInt64(endOffset);
      });
      endOffsetWriter.finish();
      fieldInfo.mono2Loc = endOffsetWriter.blockLoc;
      fieldInfo.mono2MetaOff = endOffsetWriter.metaOff;
    }

    // Write endValueRankReader (mono) if multi-valued.  Single-valued fields don't need one
    // because value rank == doc rank.
    if (multiValued) {
      auto guard = tmpPool.rewindScopeGuard();
      OutputStreamPtr out = postingsWriter.getOutputStream();
      MonoWriter endValueRankWriter(tmpPool, *out);
      int64_t endValueRank = 0;
      valCountStream.visitValues(inverter.pool, [&endValueRank, &endValueRankWriter](auto val) {
        endValueRank += val;
        endValueRankWriter.addInt64(endValueRank);
      });
      endValueRankWriter.finish();
      fieldInfo.monoLoc = endValueRankWriter.blockLoc;
      fieldInfo.monoMetaOff = endValueRankWriter.metaOff;
    }

    // Write the per-value-rank -> docId map (valDoc) when a multi-valued consumer
    // needs the reverse lookup (vectors).  We emit each doc's id once per value it
    // owns, in doc order, so the array is monotonic non-decreasing.  Single-valued
    // fields never need it (valueRank == docRank) and string columns opt out.
    if (multiValued && numValuesTotal > 0 && writesValueDocMap()) {
      auto guard = tmpPool.rewindScopeGuard();
      // doc ids of docs-with-value, in increasing order (one per valCountStream entry).
      std::vector<int32_t> docids;
      docids.reserve((size_t)numDocs);
      docsWithVal.forEachDoc(inverter.pool, [&docids](int d) { docids.push_back(d); });

      OutputStreamPtr out = postingsWriter.getOutputStream();
      MonoWriter valDocWriter(tmpPool, *out);
      size_t docIdx = 0;
      valCountStream.visitValues(inverter.pool, [&](int32_t count) {
        int32_t docid = docids[docIdx++];
        for (int32_t v = 0; v < count; v++) valDocWriter.addInt64(docid);
      });
      assert(docIdx == docids.size());
      valDocWriter.finish();
      fieldInfo.valDocLoc = valDocWriter.blockLoc;
      fieldInfo.valDocMetaOff = valDocWriter.metaOff;
    }

    // Write docs-with-value.
    {
      auto guard = tmpPool.rewindScopeGuard();
      // TODO: when things go parallel, we don't want to reserve an OutputStream if this is dense.
      DocsWithValWriter docsWriter(tmpPool, postingsWriter, fieldInfo);
      if (!full) {
        docsWithVal.pushDocs(inverter.pool, docsWriter);
        docsWriter.finish();
      }
      else {
        docsWriter.finishDense(maxDoc);
      }
    }
  }

};

}
