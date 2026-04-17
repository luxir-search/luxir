#pragma once

#include <string_view>
#include <optional>
#include "DocsReader.h"
#include "PostingsReader.h"
#include "IntColReader.h"
#include "solux/codec/Codec.h"
#include "solux/store/InputStream.h"
#include "solux/schema/FieldType.h"

namespace solux {

/// Reader for string/binary column data written by StrColHandler.
///
/// Layout:
///   - column bytes: concatenated raw value bytes.
///   - endOffsetReader (optional): per-value -> byte offset. Absent when all values are
///     the same size; in that case fixedSize holds the common value size.
///   - endRankReader (optional): per-doc -> per-value rank boundary. Present only for
///     multi-valued fields.  For single-valued, value rank == doc rank.
class StrColReader {
private:
  DocsReader docs;
  InputStream valuesIS;                       // Stream for concatenated string values
  const char* valuesData;                     // Pointer to concatenated string data
  std::optional<MonoReader> endRankReader;    // per-doc -> end value rank (multi-valued only)
  std::optional<MonoReader> endOffsetReader;  // per-value -> end byte offset (variable-size only)
  int32_t docsWithField = 0;
  int64_t nvals = 0;
  int32_t fixedSize = -1;                     // -1 for variable size, >= 0 for fixed size

public:
  /// The fieldInfo is only used in the constructor and can be discarded after.
  StrColReader(PostingsReader& postingsReader, const SegFieldInfo& fieldInfo) :
    docs(postingsReader, fieldInfo),
    valuesIS(postingsReader.getInputStreamSeek(fieldInfo.columnLoc)),
    valuesData(valuesIS.ptr()),
    docsWithField(fieldInfo.docsWithField),
    nvals(fieldInfo.numValues)
  {
    if (fieldInfo.flags & FieldType::MULTI_VALUED) {
      endRankReader.emplace(postingsReader, fieldInfo.monoLoc, fieldInfo.monoMetaOff, fieldInfo.docsWithField);
    }
    if (fieldInfo.mono2Loc.offset() == 0 && fieldInfo.mono2Loc.filenum() == 0) {
      fixedSize = (int32_t)fieldInfo.mono2MetaOff;
    } else {
      endOffsetReader.emplace(postingsReader, fieldInfo.mono2Loc, fieldInfo.mono2MetaOff, fieldInfo.numValues);
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
    return endRankReader.has_value();
  }

  bool isFixedSize() const {
    return !endOffsetReader.has_value();
  }

  /// For multi-valued fields, returns the endRankReader which maps per-doc-rank ->
  /// end value rank (cumulative count).  Use MonoReader::valuesAt(docRank) to get
  /// [startRank, endRank) for a doc.  Returns nullptr for single-valued fields.
  /// Binary-searching the returned reader gives the inverse mapping (valueRank -> docRank),
  /// needed e.g. for vector-index chunk -> doc resolution.
  /// Valid as long as this StrColReader is valid.
  MonoReader* getEndRankReader() {
    return endRankReader ? &(*endRankReader) : nullptr;
  }

  /// For variable-size fields, returns the endOffsetReader which maps per-value-rank ->
  /// end byte offset.  Returns nullptr for fixed-size fields; use fixedValueSize() instead.
  /// Valid as long as this StrColReader is valid.
  MonoReader* getEndOffsetReader() {
    return endOffsetReader ? &(*endOffsetReader) : nullptr;
  }

  /// Fixed value size for fixed-size fields; -1 for variable size.
  int32_t fixedValueSize() const {
    return fixedSize;
  }

  /// Get the value at a given global value rank (0..numValues()-1).
  std::string_view valueAt(int64_t valueRank) const {
    assert(valueRank >= 0 && valueRank < nvals);
    if (endOffsetReader) {
      auto [startOffset, endOffset] = endOffsetReader->valuesAt(valueRank);
      return std::string_view(valuesData + startOffset, endOffset - startOffset);
    }
    int64_t startOffset = valueRank * fixedSize;
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
    return endRankReader->valuesAt(docRank);
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
  /// class that also bulks the endRankReader would be a better fit.
  ///
  /// Valid as long as the parent StrColReader is valid.  Not thread-safe (one cache
  /// slot); callers that need concurrent access should construct one per thread.
  class DocValues {
    std::optional<MonoReader::BulkValues> offsets;  // absent for fixed-size
    const char* valuesData;
    int64_t nvals;
    int32_t fixedSize;
  public:
    DocValues(const StrColReader& reader)
      : valuesData(reader.valuesData), nvals(reader.nvals), fixedSize(reader.fixedSize) {
      if (reader.endOffsetReader) {
        offsets.emplace(*reader.endOffsetReader);
      }
    }

    int64_t numValues() const { return nvals; }

    std::string_view valueAt(int64_t valueRank) {
      assert(valueRank >= 0 && valueRank < nvals);
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
  static void getValues(MemPool& pool, PostingsReader& postingsReader, SegFieldInfo& segFieldInfo,
                        std::ranges::input_range auto&& sortedDocIds, auto&& callback) {
    StrColReader strColReader(postingsReader, segFieldInfo);
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
  /// DocValues so consecutive values share a block decode.  The endRankReader is still
  /// accessed sparsely (once per matched doc), appropriate for top-K style callers.
  static void getMultiValues(MemPool& pool, PostingsReader& postingsReader, SegFieldInfo& segFieldInfo,
                             std::ranges::input_range auto&& sortedDocIds, auto&& callback) {
    StrColReader strColReader(postingsReader, segFieldInfo);
    StrColReader::Iterator iter(strColReader);
    StrColReader::DocValues docValues(strColReader);

    int32_t foundid = -1;
    size_t idx = 0;
    for (int32_t docid : sortedDocIds) {
      if (foundid < docid) {
        foundid = iter.advance(docid);
      }
      if (foundid == docid) {
        auto [startRank, endRank] = iter.valueRange();
        int64_t numVals = endRank - startRank;
        for (int64_t v = 0; v < numVals; v++) {
          callback(idx, docid, docValues.valueAt(startRank + v), v, numVals);
        }
      } else if (foundid == StrColReader::Iterator::ENDDOC) {
        break;
      }
      idx++;
    }
  }

  /// Get string values starting with a field name.
  static void getValues(MemPool& pool, PostingsReader& postingsReader, std::string_view field,
                        std::ranges::input_range auto&& sortedDocIds, auto&& callback) {
    FieldReader fieldReader(pool, postingsReader);
    bool found = fieldReader.seek(field);
    if (!found) {
      return;
    }
    SegFieldInfo segFieldInfo;
    fieldReader.readFieldInfo(segFieldInfo);

    getValues(pool, postingsReader, segFieldInfo, sortedDocIds, callback);
  }
};

} // end namespace solux
