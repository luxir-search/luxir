#pragma once

#include "solux/index/DocStream.h"
#include "solux/index/Inverter.h"
#include "solux/index/IntColWriter.h"
#include "solux/util/DateTime.h"
#include "solux/util/NumericUtils.h"

#include <fmt/format.h>

#include <ranges>

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

  void index(Inverter& inverter, const proto::Val& val) override {
    // expected kinds first: array form, then a single value
    if (val.has_arr_i()) {
      auto& arr = val.arr_i().v();
      std::span<const int64_t> values(arr.data(), arr.size());
      index(inverter, values);
    }
    else if (val.has_i()) {
      index(inverter, val.i());
    }
  }

  void index(Inverter& inverter, int64_t int64) override {
    indexMulti(inverter, std::span<const int64_t>(&int64, 1));
  }

  void index(Inverter& inverter, std::span<const int64_t> vals) override {
    indexMulti(inverter, vals);
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


//
// FLOAT and DOUBLE columns reuse the int column machinery by storing
// Lucene/Solr-style sortable bits (see util/NumericUtils.h): doubles as the full
// 64-bit encoding, floats as the 32-bit encoding sign-extended to int64.
// Incoming values are coerced from any numeric proto kind (i, f, d and the
// array forms) to the field's own type before encoding, so clients don't
// have to match the wire type exactly.
//

class DoubleColHandler final : public IntColHandler {
public:
  using IntColHandler::IntColHandler;

  void index(Inverter& inverter, const proto::Val& val) override {
    if (val.has_d()) {
      indexSingle(inverter, doubleToSortableInt64(val.d()));
    } else if (val.has_f()) {
      indexSingle(inverter, doubleToSortableInt64((double)val.f()));
    } else if (val.has_i()) {
      indexSingle(inverter, doubleToSortableInt64((double)val.i()));
    }
  }

  void index(Inverter& inverter, int64_t int64) override {
    indexSingle(inverter, doubleToSortableInt64((double)int64));
  }
};

class FloatColHandler final : public IntColHandler {
public:
  using IntColHandler::IntColHandler;

  void index(Inverter& inverter, const proto::Val& val) override {
    if (val.has_f()) {
      indexSingle(inverter, (int64_t)floatToSortableInt32(val.f()));
    } else if (val.has_d()) {
      indexSingle(inverter, (int64_t)floatToSortableInt32((float)val.d()));
    } else if (val.has_i()) {
      indexSingle(inverter, (int64_t)floatToSortableInt32((float)val.i()));
    }
  }

  void index(Inverter& inverter, int64_t int64) override {
    indexSingle(inverter, (int64_t)floatToSortableInt32((float)int64));
  }
};

class MultiDoubleColHandler final : public MultiIntColHandler {
public:
  using MultiIntColHandler::MultiIntColHandler;

  void index(Inverter& inverter, const proto::Val& val) override {
    auto encode = [](double d) { return doubleToSortableInt64(d); };
    // expected kinds first (array form, then a single value), coercions after
    if (val.has_arr_d()) {
      indexMulti(inverter, val.arr_d().v() | std::views::transform(encode));
    } else if (val.has_d()) {
      indexMulti(inverter, std::views::single(encode(val.d())));
    } else if (val.has_arr_f()) {
      indexMulti(inverter, val.arr_f().v()
                 | std::views::transform([&](float f) { return encode((double)f); }));
    } else if (val.has_f()) {
      indexMulti(inverter, std::views::single(encode((double)val.f())));
    } else if (val.has_arr_i()) {
      indexMulti(inverter, val.arr_i().v()
                 | std::views::transform([&](int64_t i) { return encode((double)i); }));
    } else if (val.has_i()) {
      indexMulti(inverter, std::views::single(encode((double)val.i())));
    }
  }

  void index(Inverter& inverter, int64_t int64) override {
    indexMulti(inverter, std::views::single(doubleToSortableInt64((double)int64)));
  }

  void index(Inverter& inverter, std::span<const int64_t> vals) override {
    indexMulti(inverter, vals
               | std::views::transform([](int64_t i) { return doubleToSortableInt64((double)i); }));
  }
};

class MultiFloatColHandler final : public MultiIntColHandler {
public:
  using MultiIntColHandler::MultiIntColHandler;

  void index(Inverter& inverter, const proto::Val& val) override {
    auto encode = [](float f) { return (int64_t)floatToSortableInt32(f); };
    // expected kinds first (array form, then a single value), coercions after
    if (val.has_arr_f()) {
      indexMulti(inverter, val.arr_f().v() | std::views::transform(encode));
    } else if (val.has_f()) {
      indexMulti(inverter, std::views::single(encode(val.f())));
    } else if (val.has_arr_d()) {
      indexMulti(inverter, val.arr_d().v()
                 | std::views::transform([&](double d) { return encode((float)d); }));
    } else if (val.has_d()) {
      indexMulti(inverter, std::views::single(encode((float)val.d())));
    } else if (val.has_arr_i()) {
      indexMulti(inverter, val.arr_i().v()
                 | std::views::transform([&](int64_t i) { return encode((float)i); }));
    } else if (val.has_i()) {
      indexMulti(inverter, std::views::single(encode((float)val.i())));
    }
  }

  void index(Inverter& inverter, int64_t int64) override {
    indexMulti(inverter, std::views::single((int64_t)floatToSortableInt32((float)int64)));
  }

  void index(Inverter& inverter, std::span<const int64_t> vals) override {
    indexMulti(inverter, vals
               | std::views::transform([](int64_t i) { return (int64_t)floatToSortableInt32((float)i); }));
  }
};


//
// DATE columns store int64 milliseconds since the Unix epoch directly in the
// int column (no sortable-bits transform - signed millis already sorts in
// chronological order).  Incoming values are either an int (epoch millis,
// passthrough) or an ISO-8601 string parsed via parseDateToEpochMillis.  A
// string that does not parse throws, so the per-doc update path marks the doc
// failed (same contract as VectorHandler).
//

// Parse an ISO-8601 / epoch-millis date string or throw a per-doc failure.
inline int64_t parseDateOrThrow(std::string_view fieldName, std::string_view text) {
  if (auto ms = parseDateToEpochMillis(text)) return *ms;
  throw std::runtime_error(fmt::format(
      "DATE field '{}': cannot parse '{}' as a date (expected ISO-8601 or epoch millis)",
      fieldName, text));
}

class DateColHandler final : public IntColHandler {
public:
  using IntColHandler::IntColHandler;

  void index(Inverter& inverter, const proto::Val& val) override {
    if (val.has_i()) {
      indexSingle(inverter, val.i());
    } else if (val.has_s()) {
      indexSingle(inverter, parseDateOrThrow(std::string_view(fieldName), val.s()));
    }
  }

  void index(Inverter& inverter, int64_t int64) override {
    indexSingle(inverter, int64);
  }
};

class MultiDateColHandler final : public MultiIntColHandler {
public:
  using MultiIntColHandler::MultiIntColHandler;

  void index(Inverter& inverter, const proto::Val& val) override {
    auto parse = [&](std::string_view s) { return parseDateOrThrow(std::string_view(fieldName), s); };
    // expected kinds first (array form, then a single value)
    if (val.has_arr_i()) {
      index(inverter, std::span<const int64_t>(val.arr_i().v().data(), val.arr_i().v().size()));
    } else if (val.has_i()) {
      indexMulti(inverter, std::views::single(val.i()));
    } else if (val.has_arr_s()) {
      // Parse every element up front: indexMulti appends to the value stream
      // as it iterates, so a throw partway through a lazy transform would
      // leave already-parsed values orphaned (the column reconstructs
      // positionally, corrupting later docs).  Materialize first so a parse
      // failure throws before any stream mutation.
      auto& arr = val.arr_s().v();
      std::vector<int64_t> millis;
      millis.reserve(arr.size());
      for (const auto& s : arr) millis.push_back(parse(s));
      index(inverter, std::span<const int64_t>(millis.data(), millis.size()));
    } else if (val.has_s()) {
      indexMulti(inverter, std::views::single(parse(val.s())));
    }
  }

  void index(Inverter& inverter, int64_t int64) override {
    indexMulti(inverter, std::views::single(int64));
  }

  void index(Inverter& inverter, std::span<const int64_t> vals) override {
    indexMulti(inverter, vals);
  }
};


} // namespace solux::handler
