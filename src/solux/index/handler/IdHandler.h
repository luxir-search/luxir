#pragma once

#include <boost/sort/spreadsort/string_sort.hpp>
#include "solux/index/Inverter.h"
#include "solux/index/OrdCollector.h"
#include "solux/index/OrdColWriter.h"
#include "solux/util/TermValHash.h"

namespace solux::handler {

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

  Inverter::IndexHandler* versionHandler = nullptr; // lazily resolved on first overwrite

public:
  IdHandler(Inverter& inverter, const std::string_view& fieldName, const std::shared_ptr<FieldType>& fieldType)
    : IndexHandler(PackedTerm(inverter.pool, fieldName), fieldType),
      idPool(std::make_unique<MemPool>()),
      termsHash(*idPool, 4) {
  }

  ~IdHandler() override = default;

  void index(Inverter& inverter, const proto::Val& val) override {
    if (val.has_s()) {
      indexId(inverter, val.s());
    } else if (val.has_bin()) {
      indexId(inverter, val.bin());
    }
    // TODO: Missing id value is silently ignored - same as other string fields.
    // Validation should happen at a higher level?
  }

  void index(Inverter& inverter, std::string_view val) override {
    indexId(inverter, val);
  }

  /// Record an explicit delete-by-id (not an overwrite).
  void addDelete(std::string_view id, uint64_t version) {
    if (deleteHash == nullptr) {
      deleteHash = std::make_unique<TermValHash<IdEntry>>(*idPool, 4);
    }
    auto [entry, inserted] = deleteHash->try_emplace(id, -1, version);
    if (!inserted) {
      // Same id deleted again - keep the highest version.
      if (version > entry->val().version) {
        entry->val().version = version;
      }
    }
  }

private:
  void indexId(Inverter& inverter, std::string_view id) {
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
      // Same id indexed again. update to latest doc and version.
      entry->val().docId = inverter.getDoc();
      // Keep the overwrite version if we had one, otherwise update
      if (version > 0 || entry->val().version == 0) {
        entry->val().version = version;
      }
    }
  }

  Inverter::IndexHandler& getVersionHandler(Inverter& inverter) {
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

    // --- Write segment postings/ords from overwrite hash only ---
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
      fieldInfo.flags = fieldType->flags_ & ~FieldType::ABSTRACT;

      OrdCollector ords(guard.pool(), nDocs);

      textWriter.startField(&fieldInfo);
      for (int32_t tnum = 0; tnum < uniqueVals; tnum++) {
        auto term = terms[tnum];
        textWriter.startTerm(term);
        // id is unique: exactly one doc per term, docFreq=1, always pulsed
        textWriter.startDoc(term.val().docId);
        textWriter.addPositionDelta(1); // dummy position for TextWriter compatibility
        textWriter.endDoc(term.val().docId);
        ords.add(term.val().docId, tnum + 1); // +1 because 0 means "missing"
        textWriter.endTerm(term);
      }
      textWriter.endField();

      OrdColWriter ordsWriter(guard.pool(), inverter.postingsWriter, fieldInfo, ords);
      ordsWriter.finish();
    }

    // --- Produce SortedDeletes from overwrite hash and/or delete hash ---
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

} // namespace solux::handler
