// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <algorithm>
#include <span>

#include "luxir/codec/NumColumn.h"
#include "FieldParts.h"
#include "ReaderErrors.h"
#include "luxir/store/InputStream.h"
#include "luxir/store/OutputStream.h"

namespace luxir {
class SegmentCore;
class PostingsReader;

// Reads monotonic offsets, ranks, and ownership indexes from numeric blocks.
class MonoReader {
  MonoColumn values;

public:
  static constexpr uint32_t BLOCK_SIZE = MonoColumn::BLOCK_SIZE;
  static constexpr uint32_t BULK_SIZE = MonoColumn::BULK_SIZE;
  static constexpr int64_t ENDINDEX = MonoColumn::ENDINDEX;
  using BlockInfo = NumBlockInfo;

  MonoReader() = default;
  MonoReader(SegmentCore& core, field_parts::Slice slice, int64_t numValues,
             PackedTerm field = PackedTerm(nullptr), ReadClass readClass = ReadClass::DATA);

  static MonoReader openEndValueRanks(PostingsReader& reader, const DecodedFieldRecord& fieldInfo);

  // Checks the block used for an endpoint read before touching its payload.
  int64_t checkedValueAt(int64_t rank, SegmentReadContext context) const;
  void requireLast(int64_t expected, SegmentReadContext context, const char* reason) const;

  bool has_value() const noexcept { return values.has_value(); }
  explicit operator bool() const noexcept { return has_value(); }

  // Opens a standalone buffer for numeric codec tests.
  MonoReader(InputStream& columnIS, int64_t loc, int64_t metaOff, int64_t numValues) {
    const char* blocks = columnIS.ptr(loc);
    values = MonoColumn(blocks, blocks + metaOff, numValues);
  }

  [[nodiscard]] int64_t numValues() const {
    return values.numValues();
  }

  uint64_t sizeInBytes() const { return values.has_value() ? values.blockBytes() : 0; }

  [[nodiscard]] int64_t valueAt(int64_t index) const {
    return values.valueAt(index);
  }

  // Retrieve values[index-1], values[index]. If index is 0, the first value is
  // defined as zero.
  [[nodiscard]] std::pair<int64_t, int64_t> valuesAt(int64_t index) const {
    return values.valuesAt(index);
  }

  // Decode a contiguous run. Full frames go straight to the destination;
  // only partial edge frames need a temporary and a copy.
  void readValues(int64_t index, std::span<int64_t> out) const {
    assert(index >= 0 && index <= numValues());
    assert(out.size() <= (uint64_t)(numValues() - index));
    int64_t frame[MonoColumn::BULK_SIZE];
    while (!out.empty()) {
      uint32_t count;
      size_t copied;
      if (index % MonoColumn::BULK_SIZE == 0 && out.size() >= MonoColumn::BULK_SIZE) {
        values.decodeFrame(index, out.data(), count);
        copied = count;
      } else {
        int64_t start = values.decodeFrame(index, frame, count);
        size_t offset = (size_t)(index - start);
        copied = std::min(out.size(), (size_t)count - offset);
        std::copy_n(frame + offset, copied, out.data());
      }
      index += (int64_t)copied;
      out = out.subspan(copied);
    }
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
