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


class IntColWriter {
public:
  static constexpr uint32_t BLOCK_SIZE = NumColumnFormat::BLOCK_SIZE;
  using ColData = NumColumnWriter::FinishData;

private:
  NumColumnWriter writer;

public:
  // `sketch`: whether the column persists a value sketch
  // (NumColumnWriter).
  explicit IntColWriter(OutputStream& target, bool sketch = false)
      : writer(target, true, true, sketch) {}

  void addInt64(int64_t val) {
    writer.addInt64(val);
  }

  ColData finish() {
    return writer.finish(true, true);
  }

  // Fills in fieldInfo with numeric column info.
  // returns number of values written
  int64_t finish(PostingsWriter::IndexFieldInfo& fieldInfo) {
    auto data = finish();

    fieldInfo.columnLoc = writer.columnLocation();
    fieldInfo.columnMetaOff = data.columnMetaOff;
    fieldInfo.numValues = data.numValues;  // TODO: do this here?
    // Numeric segment summary bounds come straight from the column's own.
    if (fieldInfo.summary != nullptr && data.numValues > 0 && FieldType::sortsByIntColumn(fieldInfo.type)) {
      fieldInfo.summary->addNumericBounds(writer.min(), writer.max());
    }
    return data.numValues;
  }
};


class MonoWriter {
private:
  OutputStream& out;
  NumColumnWriter writer;

public:
  /// "output" column metadata that is filled in / valid after finish() is called.
  /// Both are currently needed for MonoReader.
  seg_location blockLoc;
  int64_t metaOff;

  MonoWriter(MemPool& pool, OutputStream& out)
      : out(out), writer(out, false, false) {
    unused(pool);
  }

  OutputStream& getOutputStream() {
    return out;
  }

  void addInt64(int64_t val) {
    writer.addInt64(val);
  }

  // returns number of values written and sets metadata to be read.
  size_t finish() {
    auto data = writer.finish(false, false);
    blockLoc = writer.columnLocation();
    metaOff = data.columnMetaOff;
    return (size_t)data.numValues;
  }

};

} // end namespace
