// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "luxir/index/Inverter.h"
#include "luxir/index/OrdCollector.h"
#include "luxir/index/OrdColWriter.h"
#include "luxir/schema/ValCoerce.h"
#include "luxir/util/TermValHash.h"

namespace luxir::handler {

/// One local document and a delete watermark per id. Birth versions come from
/// the inverter's request runs. The private pool and sorted hash table transfer
/// to SortedDeletes at flush, outliving the inverter.
class IdHandler final : public Inverter::IndexHandler {
  friend Inverter;

  std::unique_ptr<MemPool> idPool;
  TermValHash<IdEntry> ids;
  size_t nonzeroWatermarks = 0;
  VersionRunBuilder::Lookup birthLookup;

  // References survive rehash because the values live in idPool. An insertion
  // starts neutral, so restoring its old value also undoes its occupancy.
  struct UndoEntry {
    TermValRef<IdEntry> entry;
    IdEntry oldVal;
  };
  std::vector<UndoEntry> undoLog_;

public:
  IdHandler(Inverter& inverter, const std::string_view& fieldName, const std::shared_ptr<FieldType>& fieldType)
    : IndexHandler(PackedTerm(inverter.pool, fieldName), fieldType),
      idPool(std::make_unique<MemPool>(&indexingArena())), ids(*idPool, 4) {}

  void index(Inverter& inverter, const IndexVal& val) override {
    if (auto s = std::get_if<std::string_view>(&val.kind)) {
      indexId(inverter, *s);
      return;
    }
    if (auto b = std::get_if<::hpp_proto::bytes_view>(&val.kind)) {
      indexId(inverter, std::string_view((const char*)b->data(), b->size()));
      return;
    }
    // TODO: Missing/null id value is silently ignored - same as other fields.
    // Validation should happen at a higher level?
    if (coerce::isNull(val)) return;
    // Numeric ids index their canonical rendering, so {"id": 123} and
    // {"id": "123"} are the same document (a numeric JSON id used to index
    // NOTHING, leaving the doc without an id and thus not overwritable).
    char buf[coerce::TEXT_BUF_SIZE];
    indexId(inverter, fieldType->coerceTerm(val, std::string_view(fieldName), buf));
  }

  void index(Inverter& inverter, std::string_view val) override {
    indexId(inverter, val);
  }

  /// An explicit delete also resolves any older local occupant immediately.
  void addDelete(Inverter& inverter, std::string_view id, uint64_t version) {
    PackedTerm::TermBuffer scratch;
    auto& entry = entryFor(fitId(id, scratch));
    if (raiseWatermark(entry, version)) resolveOccupant(inverter, entry);
    accountExtraRam(inverter, idExtraBytes());
  }

  size_t undoSize() const { return undoLog_.size(); }

  void rollbackTo(Inverter& inverter, size_t mark) {
    while (undoLog_.size() > mark) {
      auto& undo = undoLog_.back();
      nonzeroWatermarks -= undo.entry.val().deleteWatermark != 0;
      nonzeroWatermarks += undo.oldVal.deleteWatermark != 0;
      undo.entry.val() = undo.oldVal;
      undoLog_.pop_back();
    }
    accountExtraRam(inverter, idExtraBytes());
  }

  void clearUndoLog() { undoLog_.clear(); }

  size_t retainedDeleteBytes() const {
    return producesPacket() ? sizeof(SortedDeletes) + idPool->capacity() + ids.tableBytes() : 0;
  }

private:
  bool producesPacket() const { return nonzeroWatermarks > 0; }

  std::string_view fitId(std::string_view id, PackedTerm::TermBuffer& scratch) const {
    auto term = PackedTerm::fitTerm(fieldType->longTerms, id, scratch);
    if (!term) {
      throw DocumentError(fmt::format("Field '{}': id value is {} bytes; maximum is {}",
                                      std::string_view(fieldName), id.size(), PackedTerm::MAX_LEN));
    }
    return *term;
  }

  IdEntry& entryFor(std::string_view id) {
    auto entry = ids.try_emplace(id, -1, 0).first;
    undoLog_.push_back({*entry, entry->val()});
    return entry->val();
  }

  bool raiseWatermark(IdEntry& entry, uint64_t version) {
    // An occupant already satisfies an unchanged watermark.
    if (version <= entry.deleteWatermark) return false;
    if (entry.deleteWatermark == 0) nonzeroWatermarks++;
    entry.deleteWatermark = version;
    return true;
  }

  uint64_t birth(const Inverter& inverter, int32_t docId) {
    return inverter.versionRuns.birth((uint32_t)docId, birthLookup);
  }

  void resolveOccupant(Inverter& inverter, IdEntry& entry) {
    if (entry.docId >= 0 && birth(inverter, entry.docId) < entry.deleteWatermark) {
      inverter.deleteDoc(entry.docId);
      entry.docId = -1;
    }
  }

  void indexId(Inverter& inverter, std::string_view id) {
    PackedTerm::TermBuffer scratch;
    auto& entry = entryFor(fitId(id, scratch));
    if (inverter.overwrite && raiseWatermark(entry, inverter.currVersion)) resolveOccupant(inverter, entry);

    int32_t docId = inverter.getDoc();
    if (inverter.currVersion < entry.deleteWatermark) {
      inverter.deleteDoc(docId);
    } else if (entry.docId != docId) {
      if (entry.docId >= 0 && birth(inverter, entry.docId) > inverter.currVersion) {
        inverter.deleteDoc(docId);
      } else {
        if (entry.docId >= 0) inverter.deleteDoc(entry.docId);
        entry.docId = docId;
      }
    }
    accountExtraRam(inverter, idExtraBytes());
  }

  size_t idExtraBytes() const {
    return idPool->capacity() + ids.memSize() + undoLog_.capacity() * sizeof(UndoEntry)
        + (producesPacket() ? sizeof(SortedDeletes) : 0);
  }

public:
  void flush(Inverter& inverter) override {
    int32_t count = (int32_t)ids.size();
    if (count == 0) return;
    auto* terms = sortForFlush(inverter, ids);
    bool occupied = std::any_of(terms, terms + count, [](const auto& term) { return term.val().docId >= 0; });

    if (occupied) {
      PostingsWriter& postingsWriter = inverter.getPostingsWriter();
      auto guard = MemPool::threadLocalPoolGuard();
      TextWriter textWriter(postingsWriter);
      PostingsWriter::IndexFieldInfo& fieldInfo = postingsWriter.addField(fieldName);
      fieldInfo.type = fieldType->persistedType();
      fieldInfo.flags = fieldType->segmentFlags();
      fieldInfo.typeFlags = fieldType->segmentTypeFlags();
      OrdCollector ords(guard.pool(), inverter.getMaxDoc());
      inverter.noteFlushWorkspace(ords.slotBytes());

      textWriter.startField(&fieldInfo);
      // Lets applyDeletes skip this segment, or single ids, without a terms seek.
      // count bounds the terms written; vacated ids are skipped below and
      // never enter the filter or the min/max range.
      textWriter.buildIdFilter(count);
      for (int32_t i = 0; i < count; i++) {
        auto term = terms[i];
        if (term.val().docId < 0) continue;
        int64_t ord = textWriter.startTerm(term);
        assert(ord <= INT32_MAX);
        textWriter.addDoc(term.val().docId, 1);
        ords.add(term.val().docId, (uint32_t)ord);
        textWriter.endTerm(term);
      }
      textWriter.endField();
      OrdColWriter ordsWriter(guard.pool(), inverter.postingsWriter, fieldInfo, ords);
      ordsWriter.finish();
    }

    if (!producesPacket()) {
      ids.free();
      return;
    }

    int32_t facts = 0;
    uint64_t minVersion = UINT64_MAX;
    uint64_t maxVersion = 0;
    for (int32_t i = 0; i < count; i++) {
      uint64_t watermark = terms[i].val().deleteWatermark;
      if (watermark == 0) continue;
      terms[facts++] = terms[i];
      minVersion = std::min(minVersion, watermark);
      maxVersion = std::max(maxVersion, watermark);
    }
    assert((size_t)facts == nonzeroWatermarks);
    auto packet = std::make_unique<SortedDeletes>(std::move(idPool));
    packet->addList(ids.detachTable(), facts, minVersion, maxVersion);
    inverter.sortedDeletes = std::move(packet);
  }
};

} // namespace luxir::handler
