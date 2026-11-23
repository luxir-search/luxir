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
/// Binary values use the same concatenated byte stream. Variable-width
/// values have an end-offset index, and multi-valued fields have an end-rank index.
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
  DocStream docs;      // set of docs that have this field
  IntStream valSizeStream;    // per-value sizes (one entry per value)
  IntStream valueCounts;   // per-doc value counts (one entry per doc with field; only used if multi-valued)
  // TODO: a RAMFile per field with the current buffer sizes (starting at 1K) is bad for many fields...
  // We should have initial small buffers pool allocated.
  RAMFile valuesFile;
  OutputStream bufferedValuesOut;
  ValueStorage valueStorage;
  OutputStreamPtr streamedValuesOut;
  field_parts::Slice streamedValues;
  int64_t docsWithField = 0;
  int64_t numValues = 0;
  int32_t minSize = std::numeric_limits<int32_t>::max();
  int32_t maxSize = std::numeric_limits<int32_t>::min();

public:
  StrColHandler(Inverter& inverter, const std::string_view& fieldName, const std::shared_ptr<FieldType>& fieldType, SpaceOrdinal space)
    : StrColHandler(inverter, fieldName, fieldType, space, ValueStorage::BUFFERED) {
  }

protected:
  StrColHandler(Inverter& inverter, const std::string_view& fieldName,
                const std::shared_ptr<FieldType>& fieldType, SpaceOrdinal space, ValueStorage valueStorage)
    : IndexHandler(PackedTerm(inverter.pool, fieldName), fieldType, space),
      stringValue(*fieldType),
      docs(inverter.pool),
      valSizeStream(inverter.pool),
      valueCounts(inverter.pool),
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
    docsWithField++;
    docs.addDoc(inverter.pool, inverter.getDoc());
    addValue(inverter, term);
    if (fieldType->flags_ & FieldType::MULTI_VALUED) {
      valueCounts.addVal(inverter.pool, 1);
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

    docsWithField++;
    docs.addDoc(inverter.pool, inverter.getDoc());
    for (const auto& val : vals) {
      addValue(inverter, val);
    }
    valueCounts.addVal(inverter.pool, (int64_t)vals.size());
    if (valueStorage == ValueStorage::BUFFERED) {
      accountExtraRam(inverter, valuesFile.size() + scratchBytes);
    }
  }

protected:
  /// Whether to persist a monotonic map from value ranks to owner docs, the
  /// reverse of end value ranks. Only written for multi-valued columns whose
  /// consumers need vector-rank -> doc resolution; VectorHandler overrides this to
  /// true so multi-valued kNN hits can be grouped back to their document.  For
  /// single-valued fields valueRank == docRank, so no map is ever written.
  virtual bool writesOwners() const { return false; }

  /// Lets a subclass record type-specific column metadata (the vector
  /// encoding) on the descriptor written at flush.
  virtual void describeColumn(field_parts::BytesColumn& column) const {
    unused(column);
  }

private:
  void addValue(Inverter& inverter, std::string_view val) {
    int32_t valSize = (int32_t)val.size();
    minSize = std::min(minSize, valSize);
    maxSize = std::max(maxSize, valSize);
    valueOutput(inverter).write(val.data(), valSize);
    valSizeStream.addVal(inverter.pool, valSize);
    numValues++;
  }

  OutputStream& valueOutput(Inverter& inverter) {
    if (valueStorage == ValueStorage::BUFFERED) {
      return bufferedValuesOut;
    }
    if (!streamedValuesOut) {
      streamedValuesOut = inverter.getPostingsWriter().getOutputStream();
    }
    return *streamedValuesOut;
  }

public:

  void finishIndexing(Inverter& inverter) override {
    if (streamedValuesOut) {
      streamedValues = inverter.getPostingsWriter().finishStream(std::move(streamedValuesOut));
    }
  }

  void flush(Inverter& inverter) override {
    if (docsWithField == 0) {
      return; // drop the field.
    }
    auto& postingsWriter = inverter.getPostingsWriter();
    auto& fieldInfo = postingsWriter.addField(space, fieldName);
    fieldInfo.header.type = fieldType->type();
    fieldInfo.header.flags = fieldType->segmentFlags();
    fieldInfo.header.typeParameters = fieldType->segmentTypeParameters();
    auto column = writeColumn(inverter);
    describeColumn(column.column);
    fieldInfo.setColumn(std::move(column));
  }

  [[nodiscard]] WrittenColumn<field_parts::BytesColumn> writeColumn(Inverter& inverter) {
    auto& postingsWriter = inverter.getPostingsWriter();
    auto& tmpPool = MemPool::threadLocal();
    bool multiValued = (fieldType->flags_ & FieldType::MULTI_VALUED) != 0;
    // When there are no values (e.g., multi-valued field with only empty arrays),
    // minSize was never updated.  Treat that as fixed-size 0.
    bool fixedSize = (numValues == 0) || (minSize == maxSize);

    auto values = writeValues(postingsWriter);
    field_parts::Slice endOffsets;

    // A variable-width column needs an offset for each value.
    if (!fixedSize) {
      auto guard = tmpPool.rewindScopeGuard();
      OutputStreamPtr out = postingsWriter.getOutputStream();
      MonoWriter endOffsetWriter(tmpPool, *out);
      int64_t endOffset = 0;
      valSizeStream.visitValues(inverter.pool, [&endOffset, &endOffsetWriter](auto val) {
        endOffset += val;
        endOffsetWriter.addInt64(endOffset);
      });
      endOffsets = endOffsetWriter.finish(postingsWriter, std::move(out));
    }

    // Single-valued fields use the row rank as the value rank.
    field_parts::Slice endValueRanks;
    if (multiValued) {
      auto guard = tmpPool.rewindScopeGuard();
      OutputStreamPtr out = postingsWriter.getOutputStream();
      MonoWriter endValueRankWriter(tmpPool, *out);
      int64_t endValueRank = 0;
      valueCounts.visitValues(inverter.pool, [&endValueRank, &endValueRankWriter](auto val) {
        endValueRank += val;
        endValueRankWriter.addInt64(endValueRank);
      });
      endValueRanks = endValueRankWriter.finish(postingsWriter, std::move(out));
    }

    // Write the per-value-rank -> docId map (owners) when a multi-valued consumer
    // needs the reverse lookup (vectors).  We emit each doc's id once per value it
    // owns, in doc order, so the array is monotonic non-decreasing.  Single-valued
    // fields never need it (valueRank == docRank) and string columns opt out.
    field_parts::Slice owners;
    if (multiValued && numValues > 0 && writesOwners()) {
      auto guard = tmpPool.rewindScopeGuard();
      // doc ids of docs with the field, in increasing order (one per valueCounts entry).
      std::vector<int32_t> docids;
      docids.reserve((size_t)docsWithField);
      docs.forEachDoc(inverter.pool, [&docids](int d) { docids.push_back(d); });

      OutputStreamPtr out = postingsWriter.getOutputStream();
      MonoWriter ownerWriter(tmpPool, *out);
      size_t docIdx = 0;
      valueCounts.visitValues(inverter.pool, [&](int32_t count) {
        int32_t docid = docids[docIdx++];
        for (int32_t v = 0; v < count; v++) ownerWriter.addInt64(docid);
      });
      assert(docIdx == docids.size());
      owners = ownerWriter.finish(postingsWriter, std::move(out));
    }

    auto presence = PresenceWriter::write(tmpPool, postingsWriter, space, docsWithField,
        [&](auto&& accept) { docs.forEachDoc(inverter.pool, accept); });
    field_parts::BytesColumn bytes{values, endOffsets};
    return {bytes, numValues, presence, endValueRanks, owners, {}};
  }

private:
  field_parts::Slice writeValues(PostingsWriter& postingsWriter) {
    if (valueStorage == ValueStorage::BUFFERED) {
      bufferedValuesOut.flush(true);
      auto out = postingsWriter.getOutputStream();
      out->appendFile(valuesFile);
      return postingsWriter.finishStream(std::move(out));
    }
    // Streamed values were finalized in finishIndexing().
    assert(!streamedValuesOut);
    assert(numValues > 0);
    assert(!streamedValues.location.isNull());
    return streamedValues;
  }

};

}
