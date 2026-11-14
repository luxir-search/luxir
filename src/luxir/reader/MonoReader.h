// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "luxir/codec/NumColumn.h"
#include "luxir/store/InputStream.h"
#include "luxir/store/OutputStream.h"

namespace luxir {
class PostingsReader;

// Monotonic semantic facade over the shared numeric-column physical layer.
class MonoReader {
  MonoColumn values;

public:
  static constexpr uint32_t BLOCK_SIZE = MonoColumn::BLOCK_SIZE;
  static constexpr int64_t ENDINDEX = MonoColumn::ENDINDEX;
  using BlockInfo = NumBlockInfo;

  MonoReader(PostingsReader& postingsReader, seg_location loc, int64_t metaOff,
             int64_t nValues);

  MonoReader(InputStream& columnIS, int64_t loc, int64_t metaOff,
             int64_t nValues) {
    const char* blocks = columnIS.ptr(loc);
    values = MonoColumn(blocks, blocks + metaOff, nValues);
  }

  [[nodiscard]] int64_t numValues() const {
    return values.numValues();
  }

  [[nodiscard]] int64_t valueAt(int64_t index) const {
    return values.valueAt(index);
  }

  // Retrieve values[index-1], values[index]. If index is 0, the first value is
  // defined as zero.
  [[nodiscard]] std::pair<int64_t, int64_t> valuesAt(int64_t index) const {
    return values.valuesAt(index);
  }

  class BulkValues {
    MonoColumn::Bulk values;

  public:
    explicit BulkValues(const MonoReader& column) : values(column.values) {}

    int64_t index() const {
      return values.index();
    }

    int64_t value() const {
      return values.value();
    }

    int64_t next() {
      return values.next();
    }

    int64_t advance(int64_t target) {
      return values.advance(target);
    }

    int64_t valueAt(int64_t index) {
      return values.valueAt(index);
    }
  };
};


} // namespace luxir
