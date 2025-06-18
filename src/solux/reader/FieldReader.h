#pragma once
#include "PostingsReader.h"
#include "solux/schema/FieldType.h"

namespace solux {
struct SegFieldInfo {
  PackedTerm fieldname;
  FieldType::Type type;  // really only need a byte here
  int32_t flags;  // from FieldType
  seg_location termBlockIndexLoc;  // location of index into the terms blocks
  seg_location termsLoc;
  seg_location docsLoc;
  seg_location posLoc;
  int32_t nTerms;
  int32_t docsWithField;

  // I don't know if things like nTerms, sumDocFreq, sumTotalTermFreq will stay in fieldInfo
  // or perhaps be moved into the terms section of the postings (i.e. pushed down so one needs
  // a TermsEnum to read them).  To be safe, we should only access through TermsEnum for now.
  // Only reason to keep at this level would be if they are sometimes needed even without a TermsEnum.
  // They could be written right before the termBlock offsets that termBlockIndexLoc points to.
  // One advantage of keeping this stuff here is it makes the TermBlockOffsets fixed size.
  int64_t sumDocFreq;
  int64_t sumTotalTermFreq;

  // column
  seg_location docsWithFieldEndLoc;
  seg_location columnLoc;    // location of the start of the column
  int64_t columnMetaOff;     // offset from the start of the column to the metadata
  int64_t numValues;         // numValues in the column. for singleValued fields, docsWithField == numValues

  seg_location monoLoc;   // location of the monotonic column
  int64_t monoMetaOff;    // offset from the start of the mono column to the metadata
};





/// Not thread safe
class FieldReader {
  friend class DocsEnum;
  friend class TermsEnum;
  friend class IntColReader;

  InputStream fieldIS;
  int32_t nFields;
  int32_t currField = -1;
  uint32_t* fieldOffsets;  // the array of field offsets, written by PostingsWriter::writeFieldIndex()
  int64_t fieldOffsetsLoc; // location in the file of the above array

  PackedTerm fieldname{nullptr};
  bool fieldInfoRead = false;      // has field metadata been read for this field?

  // TODO: field number?
public:
  FieldReader(MemPool& pool, PostingsReader& postingsReader) {
    unused(pool);
    fieldIS = postingsReader.getInputStream(0);
    fieldIS.seek(postingsReader.segInfoOffset - sizeof(int32_t));
    fieldOffsetsLoc = fieldIS.offset();
    auto fieldLocEnd = fieldIS.ptr();
    nFields = fieldIS.readInt();
    fieldOffsets = ((uint32_t*)(fieldLocEnd)) - nFields;
    fieldOffsetsLoc -= nFields * sizeof(uint32_t);
  }

  FieldReader(MemPool& pool, PostingsReader& postingsReader, const InputStream& is) : fieldIS(is) {
    unused(pool, postingsReader);
  };

  int32_t numFields() {
    return nFields;
  }

  // TODO: we could make a readFieldInfo(std::string_view fieldName) that is thread safe (doesn't modify the FieldReader)
  // Although it might just be simpler to make a copy?
  // IndexReader could return an array of const FieldReaders

  [[nodiscard]] bool seek(const std::string_view fieldName) {
    auto comparator = [&](const int32_t fieldOff, const std::string_view key) {
      auto fieldNameFound = fieldIS.readPackedTerm(fieldOffsetsLoc - fieldOff);
      return fieldNameFound < key;
    };
    auto endPtr = fieldOffsets + nFields;
    uint32_t* fieldOffPtr = std::lower_bound(fieldOffsets, endPtr, fieldName, comparator);
    if (fieldOffPtr != endPtr) {
      // This may be slightly repeated work (additional read term and compare), but it may not be worth it to eliminate.
      auto fieldNameFound = fieldIS.readPackedTerm(fieldOffsetsLoc - *fieldOffPtr);

      if (fieldNameFound == fieldName) {
        currField = fieldOffPtr - fieldOffsets - 1;  // back up to previous field since we will increment in readNextField
        return readNextField();
      }
    }
    return false;
  }


  // TODO: should this read into a different structure?  Only if we want to iterate over all fields but not read them?
  // one possible use case: a wildcard in field names (fast iteration would be a bonus)
  [[nodiscard]] bool readNextField() {
    if (currField+1 >= nFields) {
      return false;
    }
    ++currField;
    fieldIS.seek(fieldOffsetsLoc - fieldOffsets[currField]);

    //
    // See PostingsWriter.endField() for the format written.
    //
    fieldname = fieldIS.readPackedTerm();
    fieldInfoRead = false;
    return true;
  }

  // only valid after readNextField() returns true or seek() returns true.
  // The SegFieldInfo produced is independent of FieldReader.
  void readFieldInfo(SegFieldInfo& fieldInfo) {
    assert(fieldIS.left() > 0); // this triggers if this fieldReader is unpositioned.
    assert(!fieldname.isNull());
    assert(!fieldInfoRead);  // we could back up and re-read based on currField
    if (!fieldInfoRead) {
      fieldInfoRead = true;
      fieldInfo.fieldname = fieldname;
      fieldInfo.type = static_cast<FieldType::Type>(fieldIS.readVint());
      fieldInfo.flags = fieldIS.readVint();
      fieldInfo.docsWithField = fieldIS.readVint();
      if (fieldInfo.flags & FieldType::INDEX_DOCS) {
        fieldInfo.termBlockIndexLoc = fieldIS.readVal<seg_location>();
        fieldInfo.termsLoc = fieldIS.readVal<seg_location>();
        fieldInfo.docsLoc = fieldIS.readVal<seg_location>();
        fieldInfo.posLoc = fieldIS.readVal<seg_location>();
        fieldInfo.nTerms = fieldIS.readVint();
        fieldInfo.sumDocFreq = fieldInfo.nTerms + fieldIS.readVlong();
        fieldInfo.sumTotalTermFreq = fieldInfo.sumDocFreq + fieldIS.readVlong();
      }

      fieldInfo.docsWithFieldEndLoc = fieldIS.readVal<seg_location>();
      fieldInfo.columnLoc = fieldIS.readVal<seg_location>();
      fieldInfo.columnMetaOff = fieldIS.readVlong();
      fieldInfo.numValues = fieldIS.readVlong();

      fieldInfo.monoLoc = fieldIS.readVal<seg_location>();
      fieldInfo.monoMetaOff = fieldIS.readVlong();
    }
  }

  PackedTerm name() const noexcept { return fieldname; }

private:

};

}
