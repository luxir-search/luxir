#pragma once

#include "solux/index/DocStream.h"
#include "solux/index/Inverter.h"
#include "solux/index/IntColWriter.h"
#include "solux/schema/ValCoerce.h"

#include <fmt/format.h>

#include <ranges>

using namespace solux;
namespace solux::handler {

//
// Info for one single valued column.
//
// IntColHandler and MultiIntColHandler serve the whole int-column family
// (INT, FLOAT, DOUBLE, DATE): the value a column stores is whatever
// FieldType::coerceColInt64 returns (raw ints, sortable bits, epoch millis),
// so the handlers themselves are encoding-blind.  Coercion failures throw,
// which the per-doc update path recovers by marking the doc failed.
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

  void index(Inverter& inverter, const IndexVal& val) override {
    // explicit null means "no value", same as an absent field
    if (coerce::isNull(val)) return;
    indexSingle(inverter, fieldType->coerceColInt64(val, std::string_view(fieldName)));
  }

  void index(Inverter& inverter, int64_t int64) override {
    indexSingle(inverter, fieldType->coerceColInt64(coerce::scalarVal(int64),
                                                    std::string_view(fieldName)));
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
    fieldInfo.flags = fieldType->flags_ & ~FieldType::ABSTRACT;
    flushIntCol(inverter, fieldInfo);
  }

  // This is the version called directly from text field for norms
  void flushIntCol(Inverter& inverter, PostingsWriter::IndexFieldInfo& fieldInfo) {
    auto& tmpPool = MemPool::threadLocal();
    PostingsWriter& postingsWriter = inverter.getPostingsWriter();
    auto full = numVals >= postingsWriter.getMaxDoc();

    // push values
    {
      auto outputPtr = postingsWriter.getOutputStream();
      IntColWriter writer(*outputPtr);
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
      writer.finish(fieldInfo);
    }

    // push docs
    {
      auto guard = tmpPool.rewindScopeGuard();
      // TODO: when things go parallel, we don't want to reserve an OutputStream if this is dense.
      DocsWithValWriter docsWriter(tmpPool, postingsWriter, fieldInfo);
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

  void index(Inverter& inverter, const IndexVal& val) override {
    // expected kinds first: array form, then a single value
    if (std::holds_alternative<solux::api::ArrInt>(val.kind)) {
      auto& arr = std::get<solux::api::ArrInt>(val.kind).v;
      index(inverter, std::span<const int64_t>(arr.data(), arr.size()));
      return;
    }
    if (std::holds_alternative<int64_t>(val.kind)) {
      index(inverter, std::get<int64_t>(val.kind));
      return;
    }
    if (coerce::isNull(val)) return;
    // Coercions last, element-wise through the field type.  Materialize the
    // full array before indexMulti: a throw partway through a lazy transform
    // would leave already-appended values without their doc/length entries
    // (the column reconstructs positionally, corrupting later docs); a throw
    // before any stream mutation just fails the doc.
    std::vector<int64_t> encoded;
    bool wasArray = coerce::forEachElement(val, [&](const IndexVal& elem) {
      encoded.push_back(fieldType->coerceColInt64(elem, std::string_view(fieldName)));
    });
    if (!wasArray) {
      encoded.push_back(fieldType->coerceColInt64(val, std::string_view(fieldName)));
    }
    indexMulti(inverter, std::span<const int64_t>(encoded.data(), encoded.size()));
  }

  void index(Inverter& inverter, int64_t int64) override {
    indexMulti(inverter, std::views::single(
        fieldType->coerceColInt64(coerce::scalarVal(int64), std::string_view(fieldName))));
  }

  void index(Inverter& inverter, std::span<const int64_t> vals) override {
    // int64 -> any int-column type never throws, so the lazy transform is safe
    // under the validate-before-mutate contract.
    indexMulti(inverter, vals | std::views::transform([this](int64_t v) {
      return fieldType->coerceColInt64(coerce::scalarVal(v), std::string_view(fieldName));
    }));
  }

  void indexMulti(Inverter& inverter, std::ranges::input_range auto&& values) {
    int64_t n = 0;
    for (auto val : values) {
      longStream.addVal(inverter.pool, val);
      n++;
    }
    numVals += n;
    docsWithVal.addDoc(inverter.pool, inverter.getDoc());
    lengthStream.addVal(inverter.pool, n);
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
    fieldInfo.flags = fieldType->flags_ & ~FieldType::ABSTRACT;
    flushIntCol(inverter, fieldInfo);
  }

  // This is the version called directly from text field for norms
  void flushIntCol(Inverter& inverter, PostingsWriter::IndexFieldInfo& fieldInfo) {
    auto& pool = MemPool::threadLocal();
    PostingsWriter& postingsWriter = inverter.getPostingsWriter();
    auto full = numDocs >= postingsWriter.getMaxDoc();

    // push values
    {
      auto outputPtr = postingsWriter.getOutputStream();
      IntColWriter writer(*outputPtr);
      longStream.pushValues(inverter.pool, writer);
      writer.finish(fieldInfo);
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
      MonoWriter endValueRankWriter(pool, *out);
      int64_t endValueRank = 0;

      lengthStream.visitValues(inverter.pool, [&endValueRank, &endValueRankWriter](auto val) {
        endValueRank += val;
        endValueRankWriter.addInt64(endValueRank);
      });

      endValueRankWriter.finish();
      fieldInfo.monoLoc = endValueRankWriter.blockLoc;
      fieldInfo.monoMetaOff = endValueRankWriter.metaOff;
    }

  }
};


} // namespace solux::handler
