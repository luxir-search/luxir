// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include "CollapseMembers.h"
#include "luxir/reader/StrColReader.h"
#include "luxir/search/EmitDocs.h"
#include "luxir/search/SortKindRegistry.h"
#include "luxir/value/ValueExpr.h"
#include "luxir/util/NumericUtils.h"

namespace luxir {
CollapseMembers::CollapseMembers(const CollapsePlan& plan,
    std::span<const CollapseKey> keys, RequestMemTracker& tracker)
    : plan(plan), keys(keys), tracker(tracker), resource(tracker, "page groups", "collapse member state") {}

void renderSortRanks(std::span<const api::ArrVal> ranks, const SortPlan& sort,
                     uint64_t flags, std::pmr::memory_resource& mr) {
  if (RequestFlags::dateMode(flags) == RequestFlags::DateMode::NATIVE) return;
  for (size_t i = 0; i < sort.clauses.size(); i++) {
    const auto& clause = sort.clauses[i];
    ValueNature nature = ValueNature::NUMBER;
    if (clause.getKind() == SortClause::EXPR) nature = clause.getValueProgram().root().nature;
    else if (clause.getKind() == SortClause::COLUMN) {
      auto& type = const_cast<FieldType&>(clause.getSortField().getFieldType());
      if (type.type() == FieldType::DATE) {
        nature = dateValueNature(DateFieldType::unitOf(type));
      }
    }
    withDateOutput(nature, flags, mr, [&] {
      if (clause.getKind() == SortClause::PLUGIN) return std::string(clause.getSortKindName());
      return clause.getKind() == SortClause::EXPR
          ? clause.getValueProgram().describeDateSources(clause.getValueProgram().rootNode)
          : clause.getSortField().getFieldName();
    }, [&](auto output) {
      if constexpr (!std::is_same_v<decltype(output), NativeDateOutput>) {
        for (const auto& rank : ranks) {
          if (!rank.v.empty()) renderDateValue(const_cast<api::Val&>(rank.v[i]), output);
        }
      }
    });
  }
}

void emitCollapseRank(luxir::api::ArrVal& out, const SortPlan& sort,
    IndexReader& reader, segdoc doc, float score, std::pmr::memory_resource& mr) {
  using namespace luxir::api;
  size_t size = std::max<size_t>(1, sort.clauses.size());
  auto* values = build::allocArray(out.v, size, mr);
  MemPool pool;
  auto& segment = reader.segments()[(size_t)doc.segment()];
  if (sort.clauses.empty()) { values[0].kind = score; return; }
  for (size_t i = 0; i < sort.clauses.size(); i++) {
    const auto& clause = sort.clauses[i];
    auto& value = values[i];
    value.kind = google::protobuf::NullValue::NULL_VALUE;
    switch (clause.getKind()) {
      case SortClause::SCORE: value.kind = score; break;
      case SortClause::DOC: value.kind = segment.base + doc.docId(); break;
      case SortClause::EXPR: {
        const auto& program = clause.getValueProgram();
        auto* expression = program.bind(pool, segment, program.resolveFields(pool, reader));
        const auto result = expression->evalPoint(doc.docId(), score);
        if (result.valid) {
          if (clause.getValueProgram().root().type == ValueType::DOUBLE) value.kind = result.doubleValue;
          else value.kind = result.intValue;
        }
        break;
      }
      case SortClause::PLUGIN:
        clause.getSortKind().value(reader, segment, doc.docId(), value, mr);
        break;
      case SortClause::COLUMN: {
        const auto& field = clause.getSortField();
        auto* found = reader.field(field.getFieldName()).at(segment);
        if (!found) break;
        const SegFieldInfo& info = *found;
        auto type = const_cast<FieldType&>(field.getFieldType()).type();
        if (FieldType::sortsByIntColumn(type)) {
          IntColReader column(segment.postingsReader(), info);
          IntColReader::Iterator iter(column);
          if (iter.advance(doc.docId()) != doc.docId()) break;
          int64_t raw = column.multiValued()
              ? column.selectedValue(iter.rank(), field.isReversed(), iter.values())
              : iter.value();
          if (type == FieldType::FLOAT) value.kind = sortableInt32ToFloat((int32_t)raw);
          else if (type == FieldType::DOUBLE) value.kind = sortableInt64ToDouble(raw);
          else value.kind = raw;
        } else if (const_cast<FieldType&>(field.getFieldType()).isSet(FieldType::INDEX_DOCS)) {
          OrdColReader column(segment.postingsReader(), info);
          int32_t ord = selectedSortOrd(column, doc.docId(), field.isReversed());
          if (ord == 0) break;
          TermsEnum terms(pool, segment.postingsReader(), info); terms.seekOrd((int64_t)ord - 1);
          value.kind = build::arenaStr(mr, (std::string_view)terms.term());
        } else {
          StrColReader column(segment.postingsReader(), info);
          StrColReader::DocIterator iter(column);
          if (iter.advance(doc.docId()) != doc.docId()) break;
          if (column.isMultiValued()) {
            StrColReader::DocValues values(column);
            value.kind = build::arenaStr(mr, column.selectedValue(iter.rank(), field.isReversed(), values));
          } else {
            value.kind = build::arenaStr(mr, iter.value());
          }
        }
        break;
      }
    }
  }
}
DocSortValues sortValuesUnder(const SortPlan& sort, IndexReader& reader, uint64_t flags) {
  return [sort = &sort, reader = &reader, flags](luxir::api::ArrVal& out, segdoc doc, float score,
                                                 std::pmr::memory_resource& mr) {
    emitCollapseRank(out, *sort, *reader, doc, score, mr);
    renderSortRanks(std::span<const luxir::api::ArrVal>(&out, 1), *sort, flags, mr);
  };
}
} // namespace luxir
