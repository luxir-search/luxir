#pragma once

#include "solux/index/DocStream.h"
#include "solux/index/Inverter.h"
#include "solux/index/IntColWriter.h"

using namespace solux;
namespace solux::handler {

//
// Info for one single valued column
//
class IntColHandler : public Inverter::IndexHandler {
  friend class Inverter;
  LongStream longStream;
  DocStream docsWithVal;
  int32_t numVals = 0;

public:
  IntColHandler(Inverter& inverter, const std::string_view& fieldName, const std::shared_ptr<FieldType>& fieldType)
    : IndexHandler(PackedTerm(inverter.pool, fieldName), fieldType), longStream(inverter.pool),
      docsWithVal(inverter.pool) {
  }

  IntColHandler(Inverter& inverter, PackedTerm fieldName, const std::shared_ptr<FieldType>& fieldType,
                IndexHandler& parent)
    : IndexHandler(fieldName, fieldType), longStream(inverter.pool), docsWithVal(inverter.pool) {
    unused(parent);
  }

  ~IntColHandler() override = default;

  void index(Inverter& inverter, const proto::Val& val) override {
    if (val.has_i()) {
      int64_t ival = val.i();
      indexSingle(inverter, ival);
    }
  }

  void index(Inverter& inverter, int64_t int64) override {
    indexSingle(inverter, int64);
  }

  void indexSingle(Inverter& inverter, int64_t val) {
    longStream.addVal(inverter.pool, val);
    docsWithVal.addDoc(inverter.pool, inverter.getDoc());
    numVals++;
  }

  void flush(Inverter& inverter) override {
    flushIntCol(inverter);
  }

  // TODO: make static and pass everything needed so it's composable
  void flushIntCol(Inverter& inverter) {
    if (numVals == 0) {
      return;  // drop the field.
    }
    PostingsWriter& postingsWriter = inverter.getPostingsWriter();

    // TODO: move this to postingsWriter method
    PostingsWriter::IndexFieldInfo& fieldInfo = postingsWriter.addField(fieldName);
    fieldInfo.type = fieldType->type();
    fieldInfo.flags = fieldType->flags_;
    flushIntCol(inverter, fieldInfo);
  }

  // This is the version called directly from text field for norms
  void flushIntCol(Inverter& inverter, PostingsWriter::IndexFieldInfo& fieldInfo) {
    auto& pool = MemPool::threadLocal();
    PostingsWriter& postingsWriter = inverter.getPostingsWriter();
    auto full = numVals >= postingsWriter.getMaxDoc();

    // push values
    {
      auto guard = pool.rewindScopeGuard();
      IntColWriter writer(pool, postingsWriter, fieldInfo);
      // TODO: not having the docids here makes it impossible to do a dense field encoding!  Of course that's
      // not really possible if we're writing the column directly and incrementally since we don't know
      // min, max, numbits, gcd, etc.  But we *could* know that stuff when merging segments!
      // For direct incremental columns, and for merging, we could have a IntColWriter method that accepts (docid,val)
      // For that, we could have versions of push that take a number of values to push so we can have the best
      // of both worlds.
      // We could also be told it's a required field, in which case we would always chose a dense encoding unless
      // all bits are somehow needed.  In that case, we would decode blocks of docs so we could feed doc/val
      // pairs to the inverter.  A co-routine generator might be perfect for this (one that fills blocks, not
      // individual values)
      longStream.pushValues(inverter.pool, writer);
      writer.finish();
    }

    // push docs
    {
      auto guard = pool.rewindScopeGuard();
      // TODO: when things go parallel, we don't want to reserve an OutputStream if this is dense.
      DocsWithValWriter docsWriter(pool, postingsWriter, fieldInfo);
      if (!full) {
        docsWithVal.pushDocs(inverter.pool, docsWriter);
        docsWriter.finish();
      }
      else {
        docsWriter.finishDense(numVals);
      }
    }

  }
};


class MultiIntColHandler : public Inverter::IndexHandler {
  friend class Inverter;
  LongStream longStream;
  IntStream lengthStream;
  DocStream docsWithVal;
  int64_t numVals = 0;
  int32_t numDocs = 0;

public:
  MultiIntColHandler(Inverter& inverter, const std::string_view& fieldName,
                     const std::shared_ptr<FieldType>& fieldType)
    : IndexHandler(PackedTerm(inverter.pool, fieldName), fieldType), longStream(inverter.pool),
      lengthStream(inverter.pool), docsWithVal(inverter.pool) {
  }

  ~MultiIntColHandler() override = default;

  void index(Inverter& inverter, const proto::Val& val) override {
    if (val.has_i()) {
      index(inverter, val.i());
    }
    else if (val.has_arr_i()) {
      auto& arr = val.arr_i().v();
      std::span<const int64_t> values(arr.data(), arr.size());
      index(inverter, values);
    }
  }

  void index(Inverter& inverter, int64_t int64) override {
    indexMulti(inverter, {&int64, 1});
  }

  void index(Inverter& inverter, std::span<const int64_t> vals) override {
    indexMulti(inverter, vals);
  }

  void indexMulti(Inverter& inverter, std::span<const int64_t> values) {
    for (auto& val : values) {
      longStream.addVal(inverter.pool, val);
      numVals++;
    }
    docsWithVal.addDoc(inverter.pool, inverter.getDoc());
    lengthStream.addVal(inverter.pool, values.size());
    numDocs++;
  }

  void flush(Inverter& inverter) override {
    flushIntCol(inverter);
  }

  // TODO: make static and pass everything needed so it's composable
  void flushIntCol(Inverter& inverter) {
    if (numVals == 0) {
      return;  // drop the field.
    }
    PostingsWriter& postingsWriter = inverter.getPostingsWriter();

    // TODO: move this to postingsWriter method
    PostingsWriter::IndexFieldInfo& fieldInfo = postingsWriter.addField(fieldName);
    fieldInfo.type = fieldType->type();
    fieldInfo.flags = fieldType->flags_;
    flushIntCol(inverter, fieldInfo);
  }

  // This is the version called directly from text field for norms
  void flushIntCol(Inverter& inverter, PostingsWriter::IndexFieldInfo& fieldInfo) {
    auto& pool = MemPool::threadLocal();
    PostingsWriter& postingsWriter = inverter.getPostingsWriter();
    auto full = numDocs >= postingsWriter.getMaxDoc();

    // push values
    {
      auto guard = pool.rewindScopeGuard();
      IntColWriter writer(pool, postingsWriter, fieldInfo);
      longStream.pushValues(inverter.pool, writer);
      writer.finish();
    }

    // push docs
    {
      auto guard = pool.rewindScopeGuard();
      // TODO: when things go parallel, we don't want to reserve an OutputStream if this is dense.
      DocsWithValWriter docsWriter(pool, postingsWriter, fieldInfo);
      if (!full) {
        docsWithVal.pushDocs(inverter.pool, docsWriter);
        docsWriter.finish();
      }
      else {
        docsWriter.finishDense(numDocs);
      }
    }

    // push lengths
    {
      auto guard = pool.rewindScopeGuard();
      OutputStreamPtr out = postingsWriter.getOutputStream();
      MonoWriter endRankWriter(pool, *out);
      int64_t endRank = 0;

      lengthStream.visitValues(pool, [&endRank, &endRankWriter](auto val) {
        endRank += val;
        endRankWriter.addInt64(endRank);
      });

      endRankWriter.finish();
      fieldInfo.monoLoc = endRankWriter.blockLoc;
      fieldInfo.monoMetaOff = endRankWriter.metaOff;
    }

  }
};


} // namespace solux::handler