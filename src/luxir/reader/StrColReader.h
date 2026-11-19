// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "luxir/reader/SegmentCore.h"
#include <string_view>
#include <optional>
#include "DocsReader.h"
#include "PostingsReader.h"
#include "IntColReader.h"
#include "luxir/codec/Codec.h"
#include "luxir/store/InputStream.h"
#include "luxir/store/OutputStream.h"  // ByteRange
#include "luxir/schema/FieldType.h"

namespace luxir {

/// Reader for string/binary column data written by StrColHandler.
///
/// Layout:
///   - column bytes: concatenated raw value bytes.
///   - endOffsetReader (optional): per-value -> byte offset. Absent when all values are
///     the same size; in that case fixedSize holds the common value size.
///   - endValueRankReader (optional): per-doc -> per-value rank boundary. Present only for
///     multi-valued fields.  For single-valued, value rank == doc rank.
///
/// Values::DEFERRED keeps a lazy (remote) values file unmaterialized: values are
/// then read by range (valueAt fetches what covers one value, valuesWindow() a
/// run of fixed-size values) and valuesData is null.  Resident files, and lazy
/// ones some reader already materialized, keep the pointer path either way.
/// The docs bitset and the mono readers often share the values' file (the
/// writer reuses its largest free stream), so DEFERRED reads them by range too.
class StrColReader {
public:
  enum class Values { WHOLE, DEFERRED };

private:
  DocsReader docs;
  InputStream valuesIS;                       // Stream for concatenated string values
  const char* valuesData;                     // Pointer to concatenated string data; null when deferred
  InputFile* valuesInput;                     // File holding valuesData (for read-ahead)
  size_t valuesFileOffset;                    // Offset of valuesData in valuesInput
  std::optional<MonoReader> endValueRankReader;    // per-doc -> end value rank (multi-valued only)
  std::optional<MonoReader> endOffsetReader;  // per-value -> end byte offset (variable-size only)
  std::optional<MonoReader> valDocReader;     // per-value rank -> owning docId (multi-valued reverse map; vectors only)
  bool arrayMode = false;
  int32_t docsWithField = 0;
  int64_t nvals = 0;
  int32_t fixedSize = -1;                     // -1 for variable size, >= 0 for fixed size

  static InputStream openValues(PostingsReader& postingsReader, const SegFieldInfo& fieldInfo, Values values) {
    seg_location loc = fieldInfo.columnLoc;
    if (values == Values::DEFERRED && postingsReader.core().getFile(loc.filenum())->lazy()) {
      auto resident = postingsReader.core().residentStream(loc.filenum());
      if (!resident) return InputStream();
      resident->seek((int64_t)loc.offset());
      return *resident;
    }
    return postingsReader.core().getInputStreamSeek(loc, (std::string_view)fieldInfo.fieldname);
  }

  std::string_view deferredValue(int64_t startOffset, int64_t size) const {
    if (size == 0) return {};
    InputStream range = valuesInput->rangeStream(valuesFileOffset + (uint64_t)startOffset, (uint64_t)size);
    return std::string_view(range.ptr(), (size_t)size);
  }

public:
  /// The fieldInfo is only used in the constructor and can be discarded after.
  StrColReader(PostingsReader& postingsReader, const SegFieldInfo& fieldInfo, Values values = Values::WHOLE) :
    docs(postingsReader, fieldInfo, values == Values::DEFERRED),
    valuesIS(openValues(postingsReader, fieldInfo, values)),
    valuesData(valuesIS.ptr()),
    valuesInput(postingsReader.core().getFile(fieldInfo.columnLoc.filenum())),
    valuesFileOffset((size_t)fieldInfo.columnLoc.offset()),
    arrayMode((fieldInfo.flags & FieldType::MULTI_ARRAY) != 0),
    docsWithField(fieldInfo.docsWithField),
    nvals(fieldInfo.numValues)
  {
    bool deferred = values == Values::DEFERRED;
    if (fieldInfo.flags & FieldType::MULTI_VALUED) {
      endValueRankReader.emplace(postingsReader, fieldInfo.monoLoc, fieldInfo.monoMetaOff, fieldInfo.docsWithField, deferred);
    }
    if (fieldInfo.mono2Loc.offset() == 0 && fieldInfo.mono2Loc.filenum() == 0) {
      fixedSize = (int32_t)fieldInfo.mono2MetaOff;
    } else {
      endOffsetReader.emplace(postingsReader, fieldInfo.mono2Loc, fieldInfo.mono2MetaOff, fieldInfo.numValues, deferred);
    }
    // valDoc map is optional (only multi-valued vector columns write it).
    if (!(fieldInfo.valDocLoc.offset() == 0 && fieldInfo.valDocLoc.filenum() == 0)) {
      valDocReader.emplace(postingsReader, fieldInfo.valDocLoc, fieldInfo.valDocMetaOff, fieldInfo.numValues, deferred);
    }
  }

  DocsReader& docsReader() {
    return docs;
  }

  int32_t docsWithValue() const {
    return docsWithField;
  }

  /// Total number of values across all docs (equals docsWithValue() for single-valued).
  int64_t numValues() const {
    return nvals;
  }

  bool isMultiValued() const {
    return endValueRankReader.has_value();
  }

  bool multiSet() const { return isMultiValued() && !arrayMode; }

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

  /// For multi-valued fields, returns the endValueRankReader which maps per-doc-rank ->
  /// end value rank (cumulative count).  Use MonoReader::valuesAt(docRank) to get
  /// [startValueRank, endValueRank) for a doc.  Returns nullptr for single-valued fields.
  /// Binary-searching the returned reader gives the inverse mapping (valueRank -> docRank),
  /// needed e.g. for vector-index chunk -> doc resolution.
  /// Valid as long as this StrColReader is valid.
  MonoReader* getEndValueRankReader() {
    return endValueRankReader ? &(*endValueRankReader) : nullptr;
  }

  /// For variable-size fields, returns the endOffsetReader which maps per-value-rank ->
  /// end byte offset.  Returns nullptr for fixed-size fields; use fixedValueSize() instead.
  /// Valid as long as this StrColReader is valid.
  MonoReader* getEndOffsetReader() {
    return endOffsetReader ? &(*endOffsetReader) : nullptr;
  }

  /// Reverse map per-value rank -> owning segment-local docId.  Present only for
  /// multi-valued columns that opted in at write time (vector fields).  nullptr for
  /// single-valued fields (where valueRank == docRank) and non-vector columns.
  /// Valid as long as this StrColReader is valid.
  MonoReader* getValDocReader() {
    return valDocReader ? &(*valDocReader) : nullptr;
  }

  /// The file holding the value bytes and their offset in it, for read-ahead.
  InputFile* valuesFile() const { return valuesInput; }
  size_t valuesOffset() const { return valuesFileOffset; }

  /// True when the values are read by range from a lazy file (Values::DEFERRED).
  bool valuesDeferred() const { return valuesData == nullptr; }

  /// File range of fixed-size value ranks [begin, end).
  ByteRange valuesRange(int64_t begin, int64_t end) const {
    assert(isFixedSize() && 0 <= begin && begin <= end && end <= nvals);
    return {valuesFileOffset + (uint64_t)begin * (uint64_t)fixedSize, (uint64_t)(end - begin) * (uint64_t)fixedSize};
  }

  /// Bytes of fixed-size value ranks [begin, end), valid while the file lives.
  /// A slice of the mapping on the pointer path; a deferred column fetches
  /// just this range.
  InputStream valuesWindow(int64_t begin, int64_t end) const {
    ByteRange range = valuesRange(begin, end);
    if (valuesData) {
      const char* start = valuesData + (int64_t)(range.offset - valuesFileOffset);
      return InputStream(start, start + range.length);
    }
    return valuesInput->rangeStream(range.offset, range.length);
  }

  /// Fixed value size for fixed-size fields; -1 for variable size.
  int32_t fixedValueSize() const {
    return fixedSize;
  }

  /// Get the value at a given global value rank (0..numValues()-1).
  // Keep fixed-size column access in scan loops independent of the TU inline budget.
  /// A deferred column fetches what covers the value; prefer valuesWindow()
  /// (or a prefetchRanges() of valuesRange()) for runs of values.
  LUXIR_INLINE std::string_view valueAt(int64_t valueRank) const {
    assert(valueRank >= 0 && valueRank < nvals);
    if (endOffsetReader) {
      auto [startOffset, endOffset] = endOffsetReader->valuesAt(valueRank);
      if (!valuesData) [[unlikely]] return deferredValue(startOffset, endOffset - startOffset);
      return std::string_view(valuesData + startOffset, endOffset - startOffset);
    }
    int64_t startOffset = valueRank * fixedSize;
    if (!valuesData) [[unlikely]] return deferredValue(startOffset, fixedSize);
    return std::string_view(valuesData + startOffset, fixedSize);
  }

  /// For single-valued fields, get the value for the doc at a given doc rank.
  std::string_view singleValueAt(int32_t docRank) const {
    assert(!isMultiValued());
    return valueAt(docRank);
  }

  /// For multi-valued fields, get the [startValueRank, endValueRank) range for a doc.
  std::pair<int64_t, int64_t> getStartEndValueRank(int32_t docRank) const {
    assert(isMultiValued());
    return endValueRankReader->valuesAt(docRank);
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
  /// class that also bulks the endValueRankReader would be a better fit.
  ///
  /// Valid as long as the parent StrColReader is valid.  Not thread-safe (one cache
  /// slot); callers that need concurrent access should construct one per thread.
  class DocValues {
    std::optional<MonoReader::BulkValues> offsets;  // absent for fixed-size
    const StrColReader& reader;
    const char* valuesData;
    int64_t nvals;
    int32_t fixedSize;
  public:
    DocValues(const StrColReader& reader)
      : reader(reader), valuesData(reader.valuesData), nvals(reader.nvals), fixedSize(reader.fixedSize) {
      if (reader.endOffsetReader) {
        offsets.emplace(*reader.endOffsetReader);
      }
    }

    int64_t numValues() const { return nvals; }

    std::string_view valueAt(int64_t valueRank) {
      assert(valueRank >= 0 && valueRank < nvals);
      if (!offsets) {
        int64_t startOffset = valueRank * fixedSize;
        if (!valuesData) return reader.deferredValue(startOffset, fixedSize);
        return std::string_view(valuesData + startOffset, fixedSize);
      }
      // MonoReader::BulkValues::valueAt caches a 128-value sub-block.  valueRank-1 and
      // valueRank usually land in the same cache window, so this is one block decode
      // (on miss) plus two array reads.
      int64_t startOffset = valueRank > 0 ? offsets->valueAt(valueRank - 1) : 0;
      int64_t endOffset = offsets->valueAt(valueRank);
      if (!valuesData) return reader.deferredValue(startOffset, endOffset - startOffset);
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
      docsIter(reader.docs.bitset()),
      maxRank(reader.docsWithValue()),
      dense(!reader.docs.hasBitset())
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
  static void getValues(MemPool& pool, PostingsReader& postingsReader, const SegFieldInfo& segFieldInfo,
                        std::ranges::input_range auto&& sortedDocIds, auto&& callback) {
    // Sparse (top-K) access: a lazy file is read per value, not materialized.
    StrColReader strColReader(postingsReader, segFieldInfo, Values::DEFERRED);
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
  /// Calls callback(size_t input_index, int32_t docid, std::string_view value, int64_t valIdx, int64_t numVals)
  /// for each value of each docid that has values.  Within-doc value access goes through
  /// DocValues so consecutive values share a block decode.  The endValueRankReader is still
  /// accessed sparsely (once per matched doc), appropriate for top-K style callers.
  static void getMultiValues(MemPool& pool, PostingsReader& postingsReader, const SegFieldInfo& segFieldInfo,
                             std::ranges::input_range auto&& sortedDocIds, auto&& callback) {
    StrColReader strColReader(postingsReader, segFieldInfo, Values::DEFERRED);
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
        int64_t numVals = endValueRank - startValueRank;
        for (int64_t v = 0; v < numVals; v++) {
          callback(idx, docid, docValues.valueAt(startValueRank + v), v, numVals);
        }
      } else if (foundid == StrColReader::Iterator::ENDDOC) {
        break;
      }
      idx++;
    }
  }


};

} // end namespace luxir
