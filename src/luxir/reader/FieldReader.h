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
    fieldIS = postingsReader.getInputStream(0);
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
    fieldname = fieldIS.readPackedTerm();
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
      fieldInfo.type = static_cast<FieldType::Type>(fieldIS.readVint());
      fieldInfo.flags = fieldIS.readVint();
      fieldInfo.docsWithField = fieldIS.readVint();
      fieldInfo.rangeTableLoc = {0, 0};
      if (fieldInfo.flags & FieldType::INDEX_DOCS) {
        fieldInfo.termBlockIndexLoc = fieldIS.readVal<seg_location>();
        fieldInfo.termsLoc = fieldIS.readVal<seg_location>();
        fieldInfo.docsLoc = fieldIS.readVal<seg_location>();
        fieldInfo.posLoc = fieldIS.readVal<seg_location>();
        fieldInfo.nTerms = fieldIS.readVlong();
        fieldInfo.sumDocFreq = fieldInfo.nTerms + fieldIS.readVlong();
        fieldInfo.sumTotalTermFreq = fieldInfo.sumDocFreq + fieldIS.readVlong();
        fieldInfo.trieLoc = fieldIS.readVal<seg_location>();
        fieldInfo.trieRootOff = fieldIS.readVlong();
        if ((fieldInfo.flags & FieldType::TERM_RANGES) != 0) {
          fieldInfo.rangeTableLoc = fieldIS.readVal<seg_location>();
        }
      }

      fieldInfo.docsWithFieldEndLoc = fieldIS.readVal<seg_location>();
      fieldInfo.columnLoc = fieldIS.readVal<seg_location>();
      fieldInfo.columnMetaOff = fieldIS.readVlong();
      fieldInfo.numValues = fieldIS.readVlong();
      fieldInfo.ordFormat = fieldIS.readVint();
      fieldInfo.ordIndexing = fieldIS.readVint();
      fieldInfo.ordBits = fieldIS.readVint();
      fieldInfo.pointsLoc = fieldIS.readVal<seg_location>();
      fieldInfo.pointsMetaOff = fieldIS.readVlong();
      fieldInfo.normsFormat = fieldIS.readVint();
      fieldInfo.normsLoc = fieldIS.readVal<seg_location>();
      fieldInfo.normsLen = fieldIS.readVlong();

      fieldInfo.monoLoc = fieldIS.readVal<seg_location>();
      fieldInfo.monoMetaOff = fieldIS.readVlong();
      fieldInfo.mono2Loc = fieldIS.readVal<seg_location>();
      fieldInfo.mono2MetaOff = fieldIS.readVlong();
      fieldInfo.valDocLoc = fieldIS.readVal<seg_location>();
      fieldInfo.valDocMetaOff = fieldIS.readVlong();
      fieldInfo.vectorEncoding = 0;
      fieldInfo.vectorNorms = false;
      if (fieldInfo.type == FieldType::VECTOR) {
        fieldInfo.vectorEncoding = fieldIS.readVint();
        fieldInfo.vectorNorms = fieldIS.readVint() != 0;
      }
    }
  }

  PackedTerm name() const noexcept { return fieldname; }

private:

};

}
