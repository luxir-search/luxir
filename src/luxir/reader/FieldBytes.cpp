// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include "FieldBytes.h"

#include <algorithm>
#include "FieldReader.h"
#include "PostingsReader.h"
#include "luxir/util/screaming.h"

namespace luxir {

namespace {

constexpr int32_t NO_FIELD = -1;

// A recorded structure start (length 0: it runs to the next boundary) or an
// exact extent. A start with NO_FIELD only ends the structure before it.
struct Mark {
  uint64_t offset;
  uint64_t length;
  int32_t field;
  FieldBytes::Kind kind;
};

} // namespace

const char* FieldBytes::typeName(FieldType::Type type) noexcept {
  static constexpr const char* names[] = {
    "none", "string", "text", "bin", "int", "float", "double", "id", "vector", "date", "geo_point"
  };
  return (size_t)type < std::size(names) ? names[type] : "unknown";
}

SegmentFieldBytes segmentFieldBytes(PostingsReader& reader) {
  using Kind = FieldBytes::Kind;
  SegmentFieldBytes out;
  std::vector<std::vector<Mark>> marks(reader.numFiles());
  auto add = [&](seg_location location, uint64_t length, int32_t field, Kind kind) {
    if (location.isNull() || !reader.hasFile(location.filenum())) return;
    marks[location.filenum()].push_back({location.offset(), length, field, kind});
  };
  auto start = [&](seg_location location, int32_t field, Kind kind) {
    add(location, 0, field, kind);
  };
  auto exact = [&](seg_location location, uint64_t length, int32_t field, Kind kind) {
    if (length > 0) add(location, length, field, kind);
  };

  FieldReader fields(reader);
  while (fields.readNextField()) {
    SegFieldInfo info{};
    fields.readFieldInfo(info);
    int32_t id = (int32_t)out.fields.size();
    auto& entry = out.fields.emplace_back();
    entry.field = std::string((std::string_view)info.fieldname);
    entry.type = info.type;
    entry.docs = (uint64_t)info.docsWithField;
    entry.values = (uint64_t)info.numValues;

    bool positions = FieldType::hasPositions(info.flags);
    if (info.flags & FieldType::INDEX_DOCS) {
      entry.terms = (uint64_t)info.nTerms;
      entry.postings = (uint64_t)info.sumDocFreq;
      if (positions) entry.positions = (uint64_t)info.sumTotalTermFreq;
      start(info.termsLoc, id, Kind::TERMS);
      start(info.termBlockIndexLoc, id, Kind::TERM_INDEX);
      start(info.trieLoc, id, Kind::TERM_INDEX);
      start(info.docsLoc, id, Kind::POSTINGS);
      if (positions) start(info.posLoc, id, Kind::POSITIONS);
      if (!info.rangeTableLoc.isNull()) {
        start(info.rangeTableLoc, id, Kind::TERM_INDEX);
        // A partitioned merge writes each later range's docs and positions to
        // streams no other location points into; the rows size them.
        InputStream table = reader.getInputStreamSeek(info.rangeTableLoc);
        const auto* header = reinterpret_cast<const TermRangeTableHeader*>(table.ptr());
        const auto* rows = reinterpret_cast<const TermRangeRow*>(header + 1);
        for (uint32_t i = 0; i < header->nRanges; i++) {
          exact(rows[i].docsBase, rows[i].docsBytes, id, Kind::POSTINGS);
          if (positions) exact(rows[i].posBase, rows[i].posBytes, id, Kind::POSITIONS);
        }
      }
    }

    bool stored = info.type == FieldType::BIN && (info.flags & FieldType::STORED) != 0;
    start(info.columnLoc, id, stored ? Kind::STORED
        : info.type == FieldType::VECTOR ? Kind::VECTORS : Kind::COLUMN);
    start(info.monoLoc, id, stored ? Kind::STORED : Kind::COLUMN_INDEX);
    start(info.mono2Loc, id, stored ? Kind::STORED : Kind::COLUMN_INDEX);
    start(info.valDocLoc, id, stored ? Kind::STORED : Kind::COLUMN_INDEX);
    if (info.pointsMetaOff != 0) start(info.pointsLoc, id, Kind::POINTS);
    if (info.normsFormat != SegFieldInfo::NORMS_NONE) {
      exact(info.normsLoc, (uint64_t)info.normsLen, id, Kind::NORMS);
    }
    // The bitset is read back from its end, which also gives its start.
    if (!info.docsWithFieldEndLoc.isNull() && info.docsWithField != reader.maxDoc()) {
      InputStream is = reader.getInputStream(info.docsWithFieldEndLoc.filenum());
      screaming::BitSet bits(is.ptr((int64_t)info.docsWithFieldEndLoc.offset()));
      uint64_t begin = (uint64_t)(bits.start - is.ptr(0));
      exact(seg_location(info.docsWithFieldEndLoc.filenum(), begin),
            info.docsWithFieldEndLoc.offset() - begin, id, Kind::DOCS_WITH_FIELD);
    }
  }
  // File 0 ends with the field directory and segment info.
  start(seg_location(0, (uint64_t)reader.fieldDirectoryOffset()), NO_FIELD, Kind::TERMS);

  for (uint32_t fileNum = 0; fileNum < marks.size(); fileNum++) {
    if (!reader.hasFile(fileNum)) continue;
    auto& fileMarks = marks[fileNum];
    // At one offset a start goes first, so an exact extent there wins.
    std::sort(fileMarks.begin(), fileMarks.end(), [](const Mark& a, const Mark& b) {
      return a.offset != b.offset ? a.offset < b.offset : a.length < b.length;
    });
    uint64_t size = (uint64_t)reader.getInputStream(fileNum).size();
    uint64_t cursor = 0;
    const Mark* owner = nullptr;  // the open structure the bytes at cursor belong to
    auto charge = [&](const Mark* mark, uint64_t end) {
      end = std::min(end, size);
      if (end <= cursor) return;
      if (mark != nullptr && mark->field != NO_FIELD) {
        out.fields[(size_t)mark->field].bytes[mark->kind] += end - cursor;
      } else {
        out.unattributed += end - cursor;
      }
      cursor = end;
    };
    for (const Mark& mark : fileMarks) {
      charge(owner, mark.offset);
      if (mark.length == 0) {
        owner = &mark;
      } else {
        charge(&mark, mark.offset + mark.length);
        owner = nullptr;
      }
    }
    charge(owner, size);
  }
  return out;
}

} // namespace luxir
