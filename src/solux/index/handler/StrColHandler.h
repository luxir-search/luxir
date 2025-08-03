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

  // size_t maxValues = 1; // maximum number of values seen for a single doc

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
    }
    else if (val.has_bin()) {
      // TODO: only OK if this is a binary type, otherwise we could accept non-unicode and then attempt
      // to return that as a string later and cause gRPC or someone else up the line to choke.
      v = val.bin();
    }
    else {
      throw std::runtime_error("StrColHandler: expected string or binary value, got " + val.DebugString());
    }
    // TODO: handle arrays of binary as well

    indexSingle(inverter, v);
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
    lengthStream.addVal(inverter.pool, valSize);
    valuesOut.write(term.data(), valSize);
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