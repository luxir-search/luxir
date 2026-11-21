// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "luxir/reader/SegmentCore.h"
#include <string_view>
#include <optional>
#include "DocsReader.h"
#include "ReaderErrors.h"
#include "PostingsReader.h"
#include "IntColReader.h"
#include "luxir/codec/Codec.h"
#include "luxir/store/InputStream.h"
#include "luxir/schema/FieldType.h"

namespace luxir {

/// Reader for string/binary column data written by StrColHandler.
///
/// Layout:
///   - column bytes: concatenated raw value bytes.
/// End byte offsets delimit variable-size strings, and end value ranks delimit
/// multi-valued docs. A single-valued doc holds the value at its doc rank.
class StrColReader {
private:
  DocsReader docs;
  const char* valuesData;                     // Pointer to concatenated string data
  InputFile* valuesInput;                     // File holding valuesData (for read-ahead)
  size_t valuesFileOffset;                    // Offset of valuesData in valuesInput
  bool deferred = false;                      // valuesData is null: the values are acquired by range
  MonoReader endValueRanks;    // per-doc -> end value rank (multi-valued only)
  MonoReader endOffsetReader;  // per-value -> end byte offset (variable-size only)
  MonoReader owners;           // per-value rank -> owning docId (multi-valued reverse map; vectors only)
  bool arrayMode = false;
  int64_t values = 0;  // Number of values in the column.
  int32_t fixedSize = -1;                     // -1 for variable size, >= 0 for fixed size

public:
  /// deferValues: do not acquire the value bytes (valuesData stays null; callers
  /// acquire ranges of valuesFile() themselves). It applies to a fixed-size column
  /// on a file whose holds are not free; on a resident file the values are
  /// mapped as usual and deferred() is false.
  StrColReader(PostingsReader& reader, const DecodedFieldRecord& fieldInfo, bool deferValues = false)
      : docs(reader, fieldInfo),
        arrayMode((fieldInfo.flags & FieldType::MULTI_ARRAY) != 0),
        values(fieldInfo.numValues) {
    const auto* bytes = fieldInfo.bytesColumn();
    if (!bytes) missingColumn(fieldInfo, "bytes");
    valuesInput = reader.core().getFile(bytes->values.location.filenum());
    valuesFileOffset = (size_t)bytes->values.location.offset();
    deferred = deferValues && !bytes->endOffsets && !valuesInput->holdsFree();
    std::span<const char> data;
    if (deferred) {
      valuesData = nullptr;
    } else {
      data = reader.core().slice(bytes->values);
      valuesData = data.data();
    }
    size_t valuesBytes = (size_t)bytes->values.bytes;

    endValueRanks = MonoReader::openEndValueRanks(reader, fieldInfo);
    if (bytes->endOffsets) {
      auto context = segmentReadContext(reader.core(), fieldInfo, bytes->endOffsets.location.filenum());
      endOffsetReader = MonoReader(reader.core(), bytes->endOffsets, values, context.field);
      endOffsetReader.requireLast((int64_t)valuesBytes, context, "last end offset differs from the bytes column length");
    } else {
      fixedSize = fieldInfo.bytesStride();
    }
    owners = MonoReader(reader.core(), fieldInfo.owners(), values, fieldInfo.name);
  }

  const DocsReader& docsReader() const noexcept {
    return docs;
  }

  int32_t docsWithField() const {
    return docs.docsWithField();
  }

  /// Total number of values across all docs (equals docsWithField() for single-valued).
  int64_t numValues() const {
    return values;
  }

  bool multiValued() const {
    return endValueRanks.has_value();
  }

  bool multiSet() const { return multiValued() && !arrayMode; }

  // Multi-valued string sort selection. A SET reads its edge; an ARRAY scans
  // stored bytes. Callers keep single-valued reads on their existing path.
  std::string_view selectedValue(int32_t docRank, bool maximum, auto& values) const {
    auto [start, end] = getStartEndValueRank(docRank);
    assert(start < end);
    if (multiSet()) return values.valueAt(maximum ? end - 1 : start);
    auto selected = values.valueAt(start);
    for (int64_t rank = start + 1; rank < end; rank++) {
      auto value = values.valueAt(rank);
      if (maximum ? value > selected : value < selected) selected = value;
    }
    return selected;
  }

  bool isFixedSize() const {
    return !endOffsetReader.has_value();
  }

  /// For multi-valued fields, returns the endValueRanks which maps per-doc-rank ->
  /// end value rank (cumulative count).  Use MonoReader::valuesAt(docRank) to get
  /// [startValueRank, endValueRank) for a doc.  Returns nullptr for single-valued fields.
  /// Binary-searching the returned reader gives the inverse mapping (valueRank -> docRank),
  /// needed e.g. for vector-index chunk -> doc resolution.
  /// Valid as long as this StrColReader is valid.
  MonoReader* getEndValueRankReader() {
    return endValueRanks ? &endValueRanks : nullptr;
  }

  /// For variable-size fields, returns the endOffsetReader which maps per-value-rank ->
  /// end byte offset.  Returns nullptr for fixed-size fields; use fixedValueSize() instead.
  /// Valid as long as this StrColReader is valid.
  MonoReader* getEndOffsetReader() {
    return endOffsetReader ? &endOffsetReader : nullptr;
  }

  /// Reverse map per-value rank -> owning segment-local docId.  Present only for
  /// multi-valued columns that opted in at write time (vector fields).  nullptr for
  /// single-valued fields (where valueRank == docRank) and non-vector columns.
  /// Valid as long as this StrColReader is valid.
  MonoReader* ownerIndex() {
    return owners ? &owners : nullptr;
  }

  /// True when the value bytes were not acquired (see the constructor).
  bool deferredValues() const { return deferred; }

  /// The file holding the value bytes and their offset in it, for read-ahead.
  InputFile* valuesFile() const { return valuesInput; }
  size_t valuesOffset() const { return valuesFileOffset; }

  /// Fixed value size for fixed-size fields; -1 for variable size.
  int32_t fixedValueSize() const {
    return fixedSize;
  }

  /// Get the value at a given global value rank (0..numValues()-1).
  // Keep fixed-size column access in scan loops independent of the TU inline budget.
  LUXIR_INLINE std::string_view valueAt(int64_t valueRank) const {
    assert(valueRank >= 0 && valueRank < values);
    assert(valuesData != nullptr || values == 0);
    if (endOffsetReader) {
      auto [startOffset, endOffset] = endOffsetReader.valuesAt(valueRank);
      return std::string_view(valuesData + startOffset, endOffset - startOffset);
    }
    int64_t startOffset = valueRank * fixedSize;
    return std::string_view(valuesData + startOffset, fixedSize);
  }

  /// For single-valued fields, get the value for the doc at a given doc rank.
  std::string_view singleValueAt(int32_t docRank) const {
    assert(!multiValued());
    return valueAt(docRank);
  }

  /// For multi-valued fields, get the [startValueRank, endValueRank) range for a doc.
  std::pair<int64_t, int64_t> getStartEndValueRank(int32_t docRank) const {
    assert(multiValued());
    return endValueRanks.valuesAt(docRank);
  }

  /// Per-doc value accessor for sparse doc access patterns (e.g. top-K hits).
  /// For each matched doc the caller typically reads all values in the doc's rank
  /// range - those reads cluster in the endOffsetReader, so caching a sub-block of
  /// offsets avoids repeated block decodes.  Internally wraps MonoReader::BulkValues
  /// over the endOffsetReader (variable-size); for fixed-size columns it degrades to
  /// pointer arithmetic.
  ///
  /// Use this over the non-caching StrColReader::valueAt() for multi-value fields
  /// where you retrieve all values for a document.
  ///
  /// For high-density access (iterating every doc in the segment) a future BulkValues
  /// class that also bulks the endValueRanks would be a better fit.
  ///
  /// Valid as long as the parent StrColReader is valid.  Not thread-safe (one cache
  /// slot); callers that need concurrent access should construct one per thread.
  class DocValues {
    std::optional<MonoReader::BulkValues> offsets;  // absent for fixed-size
    const char* valuesData;
    int64_t values;  // Number of values in the column.
    int32_t fixedSize;
  public:
    DocValues(const StrColReader& reader)
      : valuesData(reader.valuesData), values(reader.values), fixedSize(reader.fixedSize) {
      if (reader.endOffsetReader) {
        offsets.emplace(reader.endOffsetReader);
      }
    }

    int64_t numValues() const { return values; }

    std::string_view valueAt(int64_t valueRank) {
      assert(valueRank >= 0 && valueRank < values);
      if (!offsets) {
        int64_t startOffset = valueRank * fixedSize;
        return std::string_view(valuesData + startOffset, fixedSize);
      }
      // MonoReader::BulkValues::valueAt caches a 128-value sub-block.  valueRank-1 and
      // valueRank usually land in the same cache window, so this is one block decode
      // (on miss) plus two array reads.
      int64_t startOffset = valueRank > 0 ? offsets->valueAt(valueRank - 1) : 0;
      int64_t endOffset = offsets->valueAt(valueRank);
      return std::string_view(valuesData + startOffset, endOffset - startOffset);
    }
  };

  /// Iterator over documents with string values.
  class DocIterator {
  private:
    const StrColReader& reader;
    screaming::BitSet::Iterator docsIter;
    int32_t docRank = -1;
    int32_t doc = -1;
    int32_t maxRank;
    bool dense;

  public:
    static constexpr int32_t ENDDOC = std::numeric_limits<int32_t>::max();

    DocIterator(const StrColReader& reader) :
      reader(reader),
      docsIter(reader.docs.bitmapIterator()),
      maxRank(reader.docsWithField()),
      dense(reader.docs.allRows())
    {
    }

    int32_t docId() const {
      return doc;
    }

    int32_t rank() const {
      return docRank;
    }

    /// For single-valued fields, the value for the current doc.
    /// For multi-valued fields, use values() to iterate the doc's values.
    std::string_view value() const {
      return reader.singleValueAt(docRank);
    }

    /// For multi-valued fields, the [startValueRank, endValueRank) range for the current doc.
    std::pair<int64_t, int64_t> valueRange() const {
      return reader.getStartEndValueRank(docRank);
    }

    /// Advance to target docid or the next docid >= target.
    int32_t advance(int32_t target) {
      if (dense) {
        if (target >= maxRank) {
          doc = ENDDOC;
        } else {
          doc = docRank = target;
        }
      } else {
        doc = docsIter.advance(target);
        docRank = docsIter.rank();
      }
      return doc;
    }

    /// Move to the next document with a value.
    int32_t next() {
      if (docRank >= maxRank - 1) {
        doc = ENDDOC;
        return doc;
      }
      docRank++;

      if (dense) {
        doc++;
      } else {
        doc = docsIter.next();
      }

      return doc;
    }
  };

  using Iterator = DocIterator;

  /// Get string values for a sorted range of docids (single-valued fields).
  /// Calls callback(size_t input_index, int32_t docid, std::string_view value) for each docid that has a value.
  static void getValues(MemPool& pool, PostingsReader& postingsReader,
                        const DecodedFieldRecord& fieldInfo, std::ranges::input_range auto&& sortedDocIds, auto&& callback) {
    StrColReader strColReader(postingsReader, fieldInfo);
    StrColReader::Iterator iter(strColReader);

    int32_t foundid = -1;
    size_t idx = 0;
    for (int32_t docid : sortedDocIds) {
      if (foundid < docid) {
        foundid = iter.advance(docid);
      }
      if (foundid == docid) {
        callback(idx, docid, iter.value());
      } else if (foundid == StrColReader::Iterator::ENDDOC) {
        break;
      }
      idx++;
    }
  }

  /// Get multi-valued string values for a sorted range of docids.
  /// Calls callback(size_t input_index, int32_t docid, std::string_view value, int64_t valIdx, int64_t numValues)
  /// for each value of each docid that has values.  Within-doc value access goes through
  /// DocValues so consecutive values share a block decode.  The endValueRanks is still
  /// accessed sparsely (once per matched doc), appropriate for top-K style callers.
  static void getMultiValues(MemPool& pool, PostingsReader& postingsReader,
                        const DecodedFieldRecord& fieldInfo, std::ranges::input_range auto&& sortedDocIds, auto&& callback) {
    StrColReader strColReader(postingsReader, fieldInfo);
    StrColReader::Iterator iter(strColReader);
    StrColReader::DocValues docValues(strColReader);

    int32_t foundid = -1;
    size_t idx = 0;
    for (int32_t docid : sortedDocIds) {
      if (foundid < docid) {
        foundid = iter.advance(docid);
      }
      if (foundid == docid) {
        auto [startValueRank, endValueRank] = iter.valueRange();
        int64_t numValues = endValueRank - startValueRank;
        for (int64_t v = 0; v < numValues; v++) {
          callback(idx, docid, docValues.valueAt(startValueRank + v), v, numValues);
        }
      } else if (foundid == StrColReader::Iterator::ENDDOC) {
        break;
      }
      idx++;
    }
  }


};

} // end namespace luxir
