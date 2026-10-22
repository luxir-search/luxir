// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once
#include "PostingsReader.h"
#include "SegFieldInfo.h"
#include "luxir/schema/FieldType.h"

namespace luxir {
/// Not thread safe
class FieldReader {
  friend class DocsEnum;
  friend class TermsEnum;
  friend class IntColReader;

  InputStream fieldIS;
  int32_t nFields;
  int32_t currField = -1;
  const uint32_t* fieldOffsets;  // the array of field offsets, written by PostingsWriter::writeFieldIndex()
  int64_t fieldOffsetsLoc; // location in the file of the above array

  PackedTerm fieldname{nullptr};
  bool fieldInfoRead = false;      // has field metadata been read for this field?

  // TODO: field number?
public:
  explicit FieldReader(PostingsReader& postingsReader) {
    // Field metadata ends where the offsets table begins; reads are checked against that bound.
    auto file = postingsReader.getInputStream(0);
    fieldIS = InputStream(file.ptr(0), file.ptr(postingsReader.fieldOffsetsLoc));
    nFields = postingsReader.nFields;
    fieldOffsets = postingsReader.fieldOffsets;
    fieldOffsetsLoc = postingsReader.fieldOffsetsLoc;
  }

private:
  friend class PostingsReader;
  FieldReader(PostingsReader& reader, int32_t ordinal) : FieldReader(reader) {
    currField = ordinal - 1;
    if (!readNextField()) throw std::logic_error("Invalid field ordinal");
  }

public:
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
    fieldname = fieldIS.checkedPackedTerm();
    fieldInfoRead = false;
    return true;
  }

  // only valid after readNextField() returns true.
  // The SegFieldInfo produced is independent of FieldReader.
  void readFieldInfo(SegFieldInfo& fieldInfo) {
    assert(fieldIS.left() > 0); // this triggers if this fieldReader is unpositioned.
    assert(!fieldname.isNull());
    assert(!fieldInfoRead);  // we could back up and re-read based on currField
    if (!fieldInfoRead) {
      fieldInfoRead = true;
      fieldInfo.fieldname = fieldname;
      fieldInfo.type = static_cast<FieldType::Type>(fieldIS.checkedVint());
      fieldInfo.flags = fieldIS.checkedVint();
      fieldInfo.docsWithField = fieldIS.checkedVint();
      fieldInfo.rangeTableLoc = {0, 0};
      if (fieldInfo.flags & FieldType::INDEX_DOCS) {
        fieldInfo.termBlockIndexLoc = seg_location::checkedRead(fieldIS);
        fieldInfo.termsLoc = seg_location::checkedRead(fieldIS);
        fieldInfo.docsLoc = seg_location::checkedRead(fieldIS);
        fieldInfo.posLoc = seg_location::checkedRead(fieldIS);
        fieldInfo.nTerms = fieldIS.checkedVlong();
        fieldInfo.sumDocFreq = fieldInfo.nTerms + fieldIS.checkedVlong();
        fieldInfo.sumTotalTermFreq = fieldInfo.sumDocFreq + fieldIS.checkedVlong();
        fieldInfo.trieLoc = seg_location::checkedRead(fieldIS);
        fieldInfo.trieRootOff = fieldIS.checkedVlong();
        if ((fieldInfo.flags & FieldType::TERM_RANGES) != 0) {
          fieldInfo.rangeTableLoc = seg_location::checkedRead(fieldIS);
        }
      }

      fieldInfo.docsWithFieldEndLoc = seg_location::checkedRead(fieldIS);
      fieldInfo.columnLoc = seg_location::checkedRead(fieldIS);
      fieldInfo.columnMetaOff = fieldIS.checkedVlong();
      fieldInfo.numValues = fieldIS.checkedVlong();
      fieldInfo.ordFormat = fieldIS.checkedVint();
      fieldInfo.ordIndexing = fieldIS.checkedVint();
      fieldInfo.ordBits = fieldIS.checkedVint();
      fieldInfo.pointsLoc = seg_location::checkedRead(fieldIS);
      fieldInfo.pointsMetaOff = fieldIS.checkedVlong();
      fieldInfo.normsFormat = fieldIS.checkedVint();
      fieldInfo.normsLoc = seg_location::checkedRead(fieldIS);
      fieldInfo.normsLen = fieldIS.checkedVlong();

      fieldInfo.monoLoc = seg_location::checkedRead(fieldIS);
      fieldInfo.monoMetaOff = fieldIS.checkedVlong();
      fieldInfo.mono2Loc = seg_location::checkedRead(fieldIS);
      fieldInfo.mono2MetaOff = fieldIS.checkedVlong();
      fieldInfo.valDocLoc = seg_location::checkedRead(fieldIS);
      fieldInfo.valDocMetaOff = fieldIS.checkedVlong();
    }
  }

  PackedTerm name() const noexcept { return fieldname; }

private:

};

}
