#pragma once

#include <string_view>
#include <optional>
#include "DocsReader.h"
#include "PostingsReader.h"
#include "IntColReader.h"
#include "solux/codec/Codec.h"

namespace solux {

/// Reader for string column data written by StrColHandler.
/// Strings are stored concatenated in columnLoc, with cumulative lengths in monoLoc.
class StrColReader {
private:
  DocsReader docs;
  InputStream valuesIS;      // Stream for concatenated string values
  const char* valuesData;    // Pointer to concatenated string data
  std::optional<MonoReader> lengthReader;   // Reader for cumulative lengths (empty for fixed-size)
  int32_t docsWithField = 0;
  int32_t fixedSize = -1;    // -1 for variable size, >= 0 for fixed size

public:
  /// The fieldInfo is only used in the constructor and can be discarded after.
  StrColReader(PostingsReader& postingsReader, const SegFieldInfo& fieldInfo) :
    docs(postingsReader, fieldInfo),
    valuesIS(postingsReader.getInputStreamSeek(fieldInfo.columnLoc)),
    valuesData(valuesIS.ptr()),
    docsWithField(fieldInfo.docsWithField)
  {
    if (fieldInfo.monoLoc.offset() == 0 && fieldInfo.monoLoc.filenum() == 0) {
      // Fixed-size mode: monoLoc is null/zero, size is in monoMetaOff
      fixedSize = (int32_t)fieldInfo.monoMetaOff;
    } else {
      // Variable-size mode: create MonoReader
      lengthReader.emplace(postingsReader, fieldInfo.monoLoc, fieldInfo.monoMetaOff, fieldInfo.docsWithField);
    }
  }

  DocsReader& docsReader() {
    return docs;
  }

  // get underlying endRankReader, null if all values have the same size.
  MonoReader* getEndRankReader() {
    return lengthReader ? &(*lengthReader) : nullptr;
  }

  int32_t docsWithValue() const {
    return docsWithField;
  }

  /// Get the string value for a document by its rank (index in docs with value).
  /// Returns empty string_view if rank is out of bounds.
  std::string_view valueAt(int32_t rank) const {
    assert(rank >= 0 && rank < docsWithField);
    if (fixedSize >= 0) {
      // Fixed-size mode: direct offset calculation
      int64_t startOffset = (int64_t)rank * fixedSize;
      return std::string_view(valuesData + startOffset, fixedSize);
    } else {
      // Variable-size mode: use MonoReader
      auto [startOffset, endOffset] = lengthReader->valuesAt(rank);
      return std::string_view(valuesData + startOffset, endOffset - startOffset);
    }
  }

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

    /// Get the string value for the current document.
    std::string_view value() const {
      return reader.valueAt(docRank);
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
      if (docRank + 1 >= maxRank) {
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

  /// Get string values for a sorted range of docids.
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
        auto val = iter.value();
        callback(idx, docid, val);
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