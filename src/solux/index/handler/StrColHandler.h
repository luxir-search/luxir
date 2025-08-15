#pragma once

#include "solux/index/DocStream.h"
#include "solux/index/Inverter.h"
#include "solux/index/OrdCollector.h"
#include "solux/index/OrdColWriter.h"
#include "solux/util/TermValHash.h"



namespace solux::handler {

/// String field handler for column stored only (values are stored verbatim per-doc, values not deduped)
/// If the string is indexed and stored, see StrHandler.
/// Values are all stored catenated, with cumulative lengths stored in a monotonic column.
/// This class also handles binary values.
class StrColHandler final : public Inverter::IndexHandler {
  friend Inverter;

  DocStream docsWithVal; // the set of docs that have this field
  IntStream lengthStream;
  // TODO: a RAMFile per field with the current buffer sizes (starting at 1K) is bad for many fields...
  // We should have initial small buffers pool allocated.
  // OPT: keep track all the RAMFiles used by the Inverter that are eligible to be directly written
  // and then start directly writing one or more of them if they get big enough.
  RAMFile valuesFile;
  OutputStream valuesOut;
  int64_t numDocs = 0;
  int32_t minSize = std::numeric_limits<int32_t>::max();
  int32_t maxSize = std::numeric_limits<int32_t>::min();
  size_t maxValues = 1; // maximum number of values seen for a single doc
  int32_t minBlockSize = std::numeric_limits<int32_t>::max(); // For multi-valued: min total block size
  int32_t maxBlockSize = std::numeric_limits<int32_t>::min(); // For multi-valued: max total block size

public:
  StrColHandler(Inverter& inverter, const std::string_view& fieldName, const std::shared_ptr<FieldType>& fieldType)
    : IndexHandler(PackedTerm(inverter.pool, fieldName), fieldType),
      docsWithVal(inverter.pool),
      lengthStream(inverter.pool),
      valuesFile(fieldName),
      valuesOut(&valuesFile)
  {
  }

  ~StrColHandler() override = default;

  void index(Inverter& inverter, const proto::Val& val) override {
    std::string_view v;

    if (val.has_s()) {
      v = val.s();
      indexSingle(inverter, v);
    }
    else if (val.has_bin()) {
      // TODO: only OK if this is a binary type, otherwise we could accept non-unicode and then attempt
      // to return that as a string later and cause gRPC or someone else up the line to choke.
      v = val.bin();
      indexSingle(inverter, v);
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
    int32_t valSize = (int32_t)term.size();
    minSize = std::min(minSize, valSize);
    maxSize = std::max(maxSize, valSize);
    
    // If this is a multi-valued field type, write inline metadata even for single values
    if (fieldType->flags_ & FieldType::MULTI_VALUED) {
      maxValues = std::max(maxValues, size_t(1));
      int64_t startPos = valuesOut.size();
      valuesOut.writeVint(1);  // number of values
      // For single value, we don't need the length - it's implicit from the total size
      valuesOut.write(term.data(), valSize);
      int64_t totalSize = valuesOut.size() - startPos;
      lengthStream.addVal(inverter.pool, totalSize);  // Track total size including metadata
      // Track min/max block sizes for fixed-size optimization
      minBlockSize = std::min(minBlockSize, (int32_t)totalSize);
      maxBlockSize = std::max(maxBlockSize, (int32_t)totalSize);
    } else {
      // Single-valued field - write value directly
      lengthStream.addVal(inverter.pool, valSize);
      valuesOut.write(term.data(), valSize);
    }
  }

  void indexMulti(Inverter& inverter, std::span<const std::string* const> vals) {
    // Check if field is single-valued - if so, throw exception
    if (!(fieldType->flags_ & FieldType::MULTI_VALUED)) {
      throw std::runtime_error(fmt::format("Field '{}' is single-valued but received multiple values", 
                                          std::string_view(fieldName)));
    }
    
    // Empty arrays still need to be recorded for the document
    numDocs++;
    docsWithVal.addDoc(inverter.pool, inverter.getDoc());
    maxValues = std::max(maxValues, vals.size());
    
    int64_t startPos = valuesOut.size();
    // Write inline metadata: number of values as vint (0 for empty)
    valuesOut.writeVint(vals.size());
    
    // Write each value with its length, except the last one
    // The last value's length can be calculated from the total size
    for (size_t i = 0; i < vals.size(); ++i) {
      const auto* val = vals[i];
      int32_t valSize = (int32_t)val->size();
      minSize = std::min(minSize, valSize);
      maxSize = std::max(maxSize, valSize);
      
      // Only write length for all but the last value
      if (i < vals.size() - 1) {
        valuesOut.writeVint(valSize);
      }
      valuesOut.write(val->data(), valSize);
    }
    
    int64_t totalSize = valuesOut.size() - startPos;
    lengthStream.addVal(inverter.pool, totalSize);  // Track total size including metadata
    // Track min/max block sizes for fixed-size optimization
    minBlockSize = std::min(minBlockSize, (int32_t)totalSize);
    maxBlockSize = std::max(maxBlockSize, (int32_t)totalSize);
  }
  
  void indexMulti(Inverter& inverter, std::span<std::string_view> vals) {
    // Check if field is single-valued - if so, throw exception
    if (!(fieldType->flags_ & FieldType::MULTI_VALUED)) {
      throw std::runtime_error(fmt::format("Field '{}' is single-valued but received multiple values", 
                                          std::string_view(fieldName)));
    }
    
    // Empty arrays still need to be recorded for the document
    numDocs++;
    docsWithVal.addDoc(inverter.pool, inverter.getDoc());
    maxValues = std::max(maxValues, vals.size());
    
    int64_t startPos = valuesOut.size();
    // Write inline metadata: number of values as vint (0 for empty)
    valuesOut.writeVint(vals.size());
    
    // Write each value with its length, except the last one
    // The last value's length can be calculated from the total size
    for (size_t i = 0; i < vals.size(); ++i) {
      const auto& val = vals[i];
      int32_t valSize = (int32_t)val.size();
      minSize = std::min(minSize, valSize);
      maxSize = std::max(maxSize, valSize);
      
      // Only write length for all but the last value
      if (i < vals.size() - 1) {
        valuesOut.writeVint(valSize);
      }
      valuesOut.write(val.data(), valSize);
    }
    
    int64_t totalSize = valuesOut.size() - startPos;
    lengthStream.addVal(inverter.pool, totalSize);  // Track total size including metadata
    // Track min/max block sizes for fixed-size optimization
    minBlockSize = std::min(minBlockSize, (int32_t)totalSize);
    maxBlockSize = std::max(maxBlockSize, (int32_t)totalSize);
  }

  void flush(Inverter& inverter) override {
    if (numDocs == 0) {
      return; // drop the field.
    }
    auto& tmpPool = MemPool::threadLocal();

    PostingsWriter& postingsWriter = inverter.getPostingsWriter();
    PostingsWriter::IndexFieldInfo& fieldInfo = postingsWriter.addField(fieldName);
    fieldInfo.type = fieldType->type();
    fieldInfo.flags = fieldType->flags_;
    
    // The multi-valued flag is determined by the field type, not by the data we saw
    // If we saw multiple values on a single-valued field, we would have already thrown an exception

    auto maxDoc = inverter.getMaxDoc();
    auto guard = MemPool::threadLocalPoolGuard();
    bool full = numDocs == maxDoc;

    // Write the values
    {
      valuesOut.flush(true);  // Flush but keep the stream usable
      auto valuesSize = valuesFile.size();
      
      OutputStreamPtr out = postingsWriter.getOutputStream();
      // TODO: depending on the type, we may want to align here.
      out->flush(true);
      fieldInfo.columnLoc = out->slocation();
      out->getFile()->destructiveAppend(valuesFile);
      // Update the OutputStream's size tracking after destructiveAppend
      out->updateFlushedSize(out->size() + valuesSize);
      // right now there is no metadata for these catenated values, so columnMetaOff is just the size of the column.
      fieldInfo.columnMetaOff = out->size() - fieldInfo.columnLoc.offset();
    }


    // write lengths
    if (fieldInfo.flags & FieldType::MULTI_VALUED) {
      // Multi-valued fields - check if all inline metadata blocks are the same size
      if (minBlockSize == maxBlockSize) {
        // All blocks have the same size - skip writing mono column
        // monoLoc remains 0 (default), indicating fixed-size mode
        fieldInfo.monoMetaOff = minBlockSize;  // Store the fixed block size
      } else {
        // Variable-size blocks - write mono column to track cumulative sizes
        auto guard = tmpPool.rewindScopeGuard();
        OutputStreamPtr out = postingsWriter.getOutputStream();
        MonoWriter endRankWriter(tmpPool, *out);
        int64_t endRank = 0;

        lengthStream.visitValues(inverter.pool, [&endRank, &endRankWriter](auto val) {
          endRank += val;
          endRankWriter.addInt64(endRank);
        });

        endRankWriter.finish();
        fieldInfo.monoLoc = endRankWriter.blockLoc;
        fieldInfo.monoMetaOff = endRankWriter.metaOff;
      }
    } else {
      // Single-valued fields
      if (minSize == maxSize) {
        // All strings have the same size - skip writing mono column
        // monoLoc remains 0 (default), indicating fixed-size mode
        fieldInfo.monoMetaOff = minSize;  // Store the fixed element size
      } else {
        // Variable size strings - write mono column as before
        auto guard = tmpPool.rewindScopeGuard();
        OutputStreamPtr out = postingsWriter.getOutputStream();
        MonoWriter endRankWriter(tmpPool, *out);
        int64_t endRank = 0;

        lengthStream.visitValues(inverter.pool, [&endRank, &endRankWriter](auto val) {
          endRank += val;
          endRankWriter.addInt64(endRank);
        });

        endRankWriter.finish();
        fieldInfo.monoLoc = endRankWriter.blockLoc;
        fieldInfo.monoMetaOff = endRankWriter.metaOff;
      }
    }


    // write docs-with-value
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