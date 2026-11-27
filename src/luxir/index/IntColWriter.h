// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once
#include <numeric>

#include "PostingsWriter.h"
#include "luxir/codec/LinearPack.h"
#include "luxir/codec/NumColumnFormat.h"
#include "luxir/reader/IntColReader.h"
#include "luxir/codec/NumColumnWriter.h"

namespace luxir {

//
// Integer column writing
// TODO: currently all values must be written before all docs!  Decouple this so we can write columns incrementally?
// Wait until we implement deleted docs (and hence we need to know the docids as we write the values) before thinking
// about how to refactor this.
// To increase locality and decrease seeks, we could ensure that docsWithField always immediately follow the values.
// We could use an OutputStream that writes to RAM or even a MemPool implementation (or OutputStream writing to MemPool)
//


// The block kinds a field's value column may take: all of them, with ALP
// for FLOAT/DOUBLE. Internal columns keep the default (FOR only).
inline NumColumnFormat::BlockCodecs fieldColumnCodecs(FieldType::Type type) {
  NumColumnFormat::BlockCodecs codecs;
  codecs.enabled = true;
  codecs.domain = type == FieldType::FLOAT ? NumColumnFormat::ValueDomain::FLOAT
      : type == FieldType::DOUBLE ? NumColumnFormat::ValueDomain::DOUBLE
      : NumColumnFormat::ValueDomain::INT;
  return codecs;
}

// The value sketch a field's value column keeps (NumColumnWriter): one for
// the columns field-sort planning reads, `sortable`, persisted unless the
// column is BOOLEAN or small.
inline NumColumnFormat::SketchMode fieldColumnSketch(FieldType::Type type, bool sortable) {
  if (!sortable) return NumColumnFormat::SketchMode::NONE;
  return type == FieldType::BOOLEAN ? NumColumnFormat::SketchMode::FROM_ZONES
                                    : NumColumnFormat::SketchMode::PERSIST;
}

class IntColWriter {
public:
  static constexpr uint32_t BLOCK_SIZE = NumColumnFormat::BLOCK_SIZE;
  using ColData = NumColumnWriter::FinishData;

private:
  NumColumnWriter writer;

public:
  static bool writesValueSketch(FieldType::Type type, bool hasEndValueRanks) {
    return FieldType::isNumericColumn(type) && !hasEndValueRanks;
  }

  // `sketch`: whether the column persists a value sketch, and `codecs`
  // which block kinds it may take (NumColumnWriter).
  explicit IntColWriter(OutputStream& target,
                        NumColumnFormat::SketchMode sketch = NumColumnFormat::SketchMode::NONE,
                        NumColumnFormat::BlockCodecs codecs = {})
      : writer(target, true, true, sketch, codecs) {}

  void addInt64(int64_t val) {
    writer.addInt64(val);
  }

  [[nodiscard]] WrittenNumberColumn finish(PostingsWriter& postings, OutputStreamPtr&& out) {
    return placeColumn(postings, std::move(out), finish());
  }

  [[nodiscard]] static WrittenNumberColumn placeColumn(PostingsWriter& postings, OutputStreamPtr&& out,
                                                         const ColData& data) {
    auto slice = postings.finishStream(std::move(out));
    return {field_parts::NumberColumn{
                slice.location.addOffset(data.columnLoc), data.blocksBytes, data.indexBytes},
            data.numValues,
            data.bounds};
  }

  ColData finish() {
    return writer.finish(true);
  }

};


class MonoWriter {
  NumColumnWriter writer;

public:
  MonoWriter(MemPool& pool, OutputStream& out)
      : writer(out, false, false) {
    unused(pool);
  }

  void addInt64(int64_t val) {
    writer.addInt64(val);
  }

  [[nodiscard]] field_parts::Slice finish(PostingsWriter& postings, OutputStreamPtr&& out) {
    return placeIndex(postings, std::move(out), finish());
  }

  [[nodiscard]] static field_parts::Slice placeIndex(PostingsWriter& postings, OutputStreamPtr&& out,
                                                             const NumColumnWriter::FinishData& data) {
    auto slice = postings.finishStream(std::move(out));
    return slice.subSlice(data.columnLoc, data.columnMetaOff + data.indexBytes);
  }

  NumColumnWriter::FinishData finish() {
    return writer.finish(false);
  }

};

} // end namespace
