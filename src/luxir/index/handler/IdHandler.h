// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <boost/sort/spreadsort/string_sort.hpp>
#include "luxir/index/Inverter.h"
#include "luxir/index/OrdCollector.h"
#include "luxir/index/OrdColWriter.h"
#include "luxir/schema/ValCoerce.h"
#include "luxir/util/TermValHash.h"

namespace luxir::handler {

/// Specialized handler for the unique "id" field.
/// Uses TermValHash<IdEntry> instead of TermValHash<DocStream> for much more compact storage.
/// Each id is unique (one doc per term), so we store just {docId, version} per entry.
/// At flush time, the sorted terms produce the delete list for free.
///
/// When inverter.overwrite is true, indexing an id automatically:
///   - queues a delete for previous versions of this id in other segments
///   - indexes currVersion into the _version_ int column
///
/// Owns its own MemPool (idPool) so the id string bytes can be transferred
/// to SortedDeletes after flush, outliving the inverter.
/// The overwrite hash and optional delete-by-id hash both allocate from idPool.
class IdHandler final : public Inverter::IndexHandler {
  friend Inverter;

  std::unique_ptr<MemPool> idPool;
  TermValHash<IdEntry> termsHash;            // overwrites: real docs with docId >= 0
  std::unique_ptr<TermValHash<IdEntry>> deleteHash;  // explicit delete-by-id: docId = -1
  bool hadOverwrites_ = false;  // true if any indexId() call had overwrite=true

  // Undo log so a failed document (or a failed all_or_none request) can roll back
  // its termsHash / deleteHash mutations.  A failed update must not delete the
  // previous version of the doc, nor displace an earlier in-inverter entry for
  // the same id.  TermValRef copies stay valid across rehash: the blobs live in
  // idPool and rehash only moves the refs.  Scope of an undo is a single update
  // message; IndexWriter::releaseInverter clears the log.
  struct UndoEntry {
    TermValRef<IdEntry> entry;
    IdEntry oldVal;
    bool inserted;
  };
  std::vector<UndoEntry> undoLog_;

  Inverter::InputHandler* versionHandler = nullptr; // lazily resolved on first overwrite

public:
  IdHandler(Inverter& inverter, const std::string_view& fieldName, const std::shared_ptr<FieldType>& fieldType)
    : IndexHandler(PackedTerm(inverter.pool, fieldName), fieldType),
      idPool(std::make_unique<MemPool>()),
      termsHash(*idPool, 4) {
  }

  ~IdHandler() override = default;

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

  /// Record an explicit delete-by-id (not an overwrite).
  void addDelete(std::string_view id, uint64_t version) {
    id = PackedTerm::truncate(id);  // must match indexId's truncation
    if (deleteHash == nullptr) {
      deleteHash = std::make_unique<TermValHash<IdEntry>>(*idPool, 4);
    }
    auto [entry, inserted] = deleteHash->try_emplace(id, -1, version);
    if (!inserted) {
      undoLog_.push_back({*entry, entry->val(), false});
      // Same id deleted again - keep the highest version.
      if (version > entry->val().version) {
        entry->val().version = version;
      }
    } else {
      undoLog_.push_back({*entry, IdEntry(-1, 0), true});
    }
  }

  size_t undoSize() const {
    return undoLog_.size();
  }

  /// Roll back all termsHash / deleteHash mutations made after the given mark
  /// (a previous undoSize() value).  Callers must separately mark the docs of
  /// the rolled-back scope as deleted; see Inverter::rollbackTo.
  void rollbackTo(size_t mark) {
    while (undoLog_.size() > mark) {
      UndoEntry& u = undoLog_.back();
      if (u.inserted) {
        // TermValHash has no erase; neutralize instead.  version==0 entries are
        // inert (applyDeletes skips them, merges filter them) and the doc this
        // entry points at is marked deleted by the caller.
        u.entry.val().version = 0;
      } else {
        u.entry.val() = u.oldVal;
      }
      undoLog_.pop_back();
    }
  }

  void clearUndoLog() {
    undoLog_.clear();
  }

private:
  void indexId(Inverter& inverter, std::string_view id) {
    // Oversized ids index truncated; overwrite, delete-by-id, and query lookups
    // all truncate the same way, so they keep agreeing. Two ids sharing their
    // first 255 bytes collide into one doc - accepted for degenerate ids.
    id = PackedTerm::truncate(id);
    // version=0 marks non-overwrite entries so they can be excluded from the delete list.
    // Update versions start at 1, so 0 is a safe sentinel.
    uint64_t version = 0;
    if (inverter.overwrite) {
      hadOverwrites_ = true;
      version = inverter.currVersion;
      getVersionHandler(inverter).index(inverter, (int64_t)version);
    }

    auto [entry, inserted] = termsHash.try_emplace(id, inverter.getDoc(), version);
    if (!inserted) {
      undoLog_.push_back({*entry, entry->val(), false});
      if (version > 0 && entry->val().docId != inverter.getDoc()) {
        // Overwrite of an id already indexed in this inverter: the previous doc is
        // superseded and must be marked deleted here.  applyDeletes can never reach
        // it - the id postings written at flush only list the latest doc per id.
        inverter.deleteDoc(entry->val().docId);
      }
      // Same id indexed again. update to latest doc and version.
      entry->val().docId = inverter.getDoc();
      // Keep the overwrite version if we had one, otherwise update
      if (version > 0 || entry->val().version == 0) {
        entry->val().version = version;
      }
    } else {
      undoLog_.push_back({*entry, IdEntry(0, 0), true});
    }
    // All id state (strings + IdEntry values + both hash tables) lives in the private
    // idPool + heap tables, none in inverter.pool - account the whole lot here.
    accountExtraRam(inverter, idExtraBytes());
  }

  // Non-pool RAM: the private idPool (id strings + IdEntry values, plus the values
  // portion of both hashes) and the two heap hash tables.
  size_t idExtraBytes() const {
    return idPool->size() + termsHash.memSize() + (deleteHash ? deleteHash->memSize() : 0);
  }

  Inverter::InputHandler& getVersionHandler(Inverter& inverter) {
    if (versionHandler == nullptr) {
      versionHandler = &inverter.getIndexHandler("_version_");
    }
    return *versionHandler;
  }

  /// Sort a TermValHash, detach its table, and compute version range.
  /// Returns {detached table pointer, count, minVersion, maxVersion}.
  struct SortResult {
    SortedDeletes::Entry* entries;
    int32_t count;
    uint64_t minVersion;
    uint64_t maxVersion;
  };

  static SortResult sortAndDetach(TermValHash<IdEntry>& hash) {
    int32_t count = (int32_t)hash.size();
    auto* terms = hash.destructiveCompress();
    boost::sort::spreadsort::string_sort(terms, terms + count, TermRef::bracket(), TermRef::getsize(),
                                         TermRef::lessthan());

    uint64_t minV = terms[0].val().version;
    uint64_t maxV = minV;
    for (int32_t i = 1; i < count; i++) {
      uint64_t v = terms[i].val().version;
      if (v < minV) minV = v;
      if (v > maxV) maxV = v;
    }

    auto* detached = hash.detachTable();
    return {detached, count, minV, maxV};
  }

public:
  void flush(Inverter& inverter) override {
    int32_t uniqueVals = (int32_t)termsHash.size();
    bool hasDeletes = deleteHash != nullptr && deleteHash->size() > 0;

    if (uniqueVals == 0 && !hasDeletes) {
      return;
    }

    // Write segment postings/ords from overwrite hash only.
    if (uniqueVals > 0) {
      PostingsWriter& postingsWriter = inverter.getPostingsWriter();

      auto terms = termsHash.destructiveCompress();
      boost::sort::spreadsort::string_sort(terms, terms + uniqueVals, TermRef::bracket(), TermRef::getsize(),
                                           TermRef::lessthan());

      auto nDocs = inverter.getMaxDoc();
      auto guard = MemPool::threadLocalPoolGuard();

      // Write using TextWriter - same format as StrHandler for now.
      // TODO: switch to a point-lookup-optimized format (FST or hash-based).
      TextWriter textWriter(postingsWriter);
      PostingsWriter::IndexFieldInfo& fieldInfo = postingsWriter.addField(fieldName);
      // Write as STRING type since we use the same segment format for now
      fieldInfo.type = FieldType::STRING;
      fieldInfo.flags = fieldType->segmentFlags();

      OrdCollector ords(guard.pool(), nDocs);

      textWriter.startField(&fieldInfo);
      for (int32_t tnum = 0; tnum < uniqueVals; tnum++) {
        auto term = terms[tnum];
        int64_t ord = textWriter.startTerm(term);
        assert(ord <= INT32_MAX);
        // id is unique: exactly one doc per term, docFreq=1, always pulsed
        textWriter.addDoc(term.val().docId, 1);  // DOCS-only field: record the doc, no freq/position stored
        ords.add(term.val().docId, (uint32_t)ord);
        textWriter.endTerm(term);
      }
      textWriter.endField();

      OrdColWriter ordsWriter(guard.pool(), inverter.postingsWriter, fieldInfo, ords);
      ordsWriter.finish();
    }

    // Produce SortedDeletes from overwrite hash and/or delete hash.
    bool hasOverwrites = uniqueVals > 0 && hadOverwrites_;

    if (!hasOverwrites && !hasDeletes) {
      // termsHash was used for segment writing but overwrite=false, no delete list needed.
      termsHash.free();
      return;
    }

    auto sortedDeletes = std::make_unique<SortedDeletes>(std::move(idPool));

    if (hasOverwrites) {
      // termsHash is already sorted from the segment writing above, just detach.
      // Compute version range.
      auto* terms = termsHash.detachTable();
      uint64_t minV = terms[0].val().version;
      uint64_t maxV = minV;
      for (int32_t i = 1; i < uniqueVals; i++) {
        uint64_t v = terms[i].val().version;
        if (v < minV) minV = v;
        if (v > maxV) maxV = v;
      }
      sortedDeletes->addList(terms, uniqueVals, minV, maxV);
    } else {
      termsHash.free();
    }

    if (hasDeletes) {
      auto result = sortAndDetach(*deleteHash);
      sortedDeletes->addList(result.entries, result.count, result.minVersion, result.maxVersion);
    }

    inverter.sortedDeletes = std::move(sortedDeletes);
  }

};

} // namespace luxir::handler
