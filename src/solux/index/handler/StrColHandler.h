#pragma once

#include "solux/index/DocStream.h"
#include "solux/index/Inverter.h"
#include "solux/index/OrdCollector.h"
#include "solux/index/OrdColWriter.h"
#include "solux/util/TermValHash.h"



namespace solux::handler {

/// String field handler for column stored only (values are stored verbatim per-doc, values not deduped)
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

  DocStream docsWithVal;      // set of docs that have this field
  IntStream valSizeStream;    // per-value sizes (one entry per value)
  IntStream valCountStream;   // per-doc value counts (one entry per doc with field; only used if multi-valued)
  // TODO: a RAMFile per field with the current buffer sizes (starting at 1K) is bad for many fields...
  // We should have initial small buffers pool allocated.
  RAMFile valuesFile;
  OutputStream valuesOut;
  int64_t numDocs = 0;
  int64_t numValuesTotal = 0;
  int32_t minSize = std::numeric_limits<int32_t>::max();
  int32_t maxSize = std::numeric_limits<int32_t>::min();

public:
  StrColHandler(Inverter& inverter, const std::string_view& fieldName, const std::shared_ptr<FieldType>& fieldType)
    : IndexHandler(PackedTerm(inverter.pool, fieldName), fieldType),
      docsWithVal(inverter.pool),
      valSizeStream(inverter.pool),
      valCountStream(inverter.pool),
      valuesFile(fieldName),
      valuesOut(&valuesFile)
  {
  }

  ~StrColHandler() override = default;

  void index(Inverter& inverter, const proto::Val& val) override {
    if (val.has_s()) {
      indexSingle(inverter, val.s());
    }
    else if (val.has_bin()) {
      // TODO: only OK if this is a binary type, otherwise we could accept non-unicode and then attempt
      // to return that as a string later and cause gRPC or someone else up the line to choke.
      indexSingle(inverter, val.bin());
    }
    else if (val.has_arr_s()) {
      auto& arr = val.arr_s().v();
      std::span<const std::string* const> values(arr.data(), arr.size());
      indexMulti(inverter, values);
    }
    else if (val.has_arr_bin()) {
      auto& arr = val.arr_bin().v();
      std::vector<std::string_view> views;
      views.reserve(arr.size());
      for (const auto& bin : arr) {
        views.push_back(bin);
      }
      std::span<std::string_view> values(views);
      indexMulti(inverter, values);
    }
    else {
      throw std::runtime_error("StrColHandler: expected string or binary value, got " + val.DebugString());
    }
  }

  void index(Inverter& inverter, std::string_view val) override {
    indexSingle(inverter, val);
  }

  void indexSingle(Inverter& inverter, std::string_view term) {
    numDocs++;
    docsWithVal.addDoc(inverter.pool, inverter.getDoc());
    addValue(inverter, term);
    if (fieldType->flags_ & FieldType::MULTI_VALUED) {
      valCountStream.addVal(inverter.pool, 1);
    }
  }

  void indexMulti(Inverter& inverter, std::span<const std::string* const> vals) {
    if (!(fieldType->flags_ & FieldType::MULTI_VALUED)) {
      throw std::runtime_error(fmt::format("Field '{}' is single-valued but received multiple values",
                                          std::string_view(fieldName)));
    }

    numDocs++;
    docsWithVal.addDoc(inverter.pool, inverter.getDoc());
    for (const auto* val : vals) {
      addValue(inverter, std::string_view(*val));
    }
    valCountStream.addVal(inverter.pool, (int64_t)vals.size());
  }

  void indexMulti(Inverter& inverter, std::span<std::string_view> vals) {
    if (!(fieldType->flags_ & FieldType::MULTI_VALUED)) {
      throw std::runtime_error(fmt::format("Field '{}' is single-valued but received multiple values",
                                          std::string_view(fieldName)));
    }

    numDocs++;
    docsWithVal.addDoc(inverter.pool, inverter.getDoc());
    for (const auto& val : vals) {
      addValue(inverter, val);
    }
    valCountStream.addVal(inverter.pool, (int64_t)vals.size());
  }

private:
  void addValue(Inverter& inverter, std::string_view val) {
    int32_t valSize = (int32_t)val.size();
    minSize = std::min(minSize, valSize);
    maxSize = std::max(maxSize, valSize);
    valuesOut.write(val.data(), valSize);
    valSizeStream.addVal(inverter.pool, valSize);
    numValuesTotal++;
  }

public:

  void flush(Inverter& inverter) override {
    if (numDocs == 0) {
      return; // drop the field.
    }
    auto& tmpPool = MemPool::threadLocal();

    PostingsWriter& postingsWriter = inverter.getPostingsWriter();
    PostingsWriter::IndexFieldInfo& fieldInfo = postingsWriter.addField(fieldName);
    fieldInfo.type = fieldType->type();
    fieldInfo.flags = fieldType->flags_ & ~FieldType::ABSTRACT;
    fieldInfo.numValues = numValuesTotal;

    auto maxDoc = inverter.getMaxDoc();
    bool full = numDocs == maxDoc;
    bool multiValued = (fieldInfo.flags & FieldType::MULTI_VALUED) != 0;
    // When there are no values (e.g., multi-valued field with only empty arrays),
    // minSize was never updated.  Treat that as fixed-size 0.
    bool fixedSize = (numValuesTotal == 0) || (minSize == maxSize);

    // Write the concatenated value bytes.
    {
      valuesOut.flush(true);
      auto valuesSize = valuesFile.size();

      OutputStreamPtr out = postingsWriter.getOutputStream();
      out->flush(true);
      fieldInfo.columnLoc = out->slocation();
      out->getFile()->destructiveAppend(valuesFile);
      out->updateFlushedSize(out->size() + valuesSize);
      // no metadata for the raw value bytes; columnMetaOff is the size of the column.
      fieldInfo.columnMetaOff = out->size() - fieldInfo.columnLoc.offset();
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
