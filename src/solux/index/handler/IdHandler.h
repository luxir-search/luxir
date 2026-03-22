#pragma once

#include <boost/sort/spreadsort/string_sort.hpp>
#include "solux/index/Inverter.h"
#include "solux/index/OrdCollector.h"
#include "solux/index/OrdColWriter.h"
#include "solux/util/TermValHash.h"

namespace solux::handler {

/// Entry stored in the TermValHash for the id field.
/// Each unique id maps to exactly one document and its version.
/// Future optimizations:
///   store variable sized entries... string, vint(docId), vint(version_delta_from_base)
SOLUX_PACKED_START
struct IdEntry {
  int32_t docId;
  uint64_t version;

  IdEntry(int32_t docId, uint64_t version) : docId(docId), version(version) {}

  friend std::ostream& operator<<(std::ostream& out, const IdEntry& e) {
    return out << "(doc=" << e.docId << " ver=" << e.version << ")";
  }
} SOLUX_PACKED_END;


/// Specialized handler for the unique "id" field.
/// Uses TermValHash<IdEntry> instead of TermValHash<DocStream> for much more compact storage.
/// Each id is unique (one doc per term), so we store just {docId, version} per entry.
/// At flush time, the sorted terms produce the delete list for free.
///
/// When inverter.overwrite is true, indexing an id automatically:
///   - queues a delete for previous versions of this id in other segments
///   - indexes currVersion into the _version_ int column
class IdHandler final : public Inverter::IndexHandler {
  friend Inverter;

  TermValHash<IdEntry> termsHash;
  Inverter::IndexHandler* versionHandler = nullptr; // lazily resolved on first overwrite

public:
  IdHandler(Inverter& inverter, const std::string_view& fieldName, const std::shared_ptr<FieldType>& fieldType)
    : IndexHandler(PackedTerm(inverter.pool, fieldName), fieldType),
      termsHash(inverter.pool, 4) {
  }

  ~IdHandler() override = default;

  void index(Inverter& inverter, const proto::Val& val) override {
    if (val.has_s()) {
      indexId(inverter, val.s());
    } else if (val.has_bin()) {
      indexId(inverter, val.bin());
    }
    // TODO: Missing id value is silently ignored — same as other string fields.
    // Validation should happen at a higher level?
  }

  void index(Inverter& inverter, std::string_view val) override {
    indexId(inverter, val);
  }

private:
  void indexId(Inverter& inverter, std::string_view id) {
    if (inverter.overwrite) {
      inverter.deleteId(id, inverter.currVersion);
      getVersionHandler(inverter).index(inverter, (int64_t)inverter.currVersion);
    }

    auto [entry, inserted] = termsHash.try_emplace(id, inverter.getDoc(), inverter.currVersion);
    if (!inserted) {
      // Same id indexed again. update to latest doc and version.
      // We *could* mark the old one as deleted here. Overwrite will handle deleting the old one, and
      // we just allow duplicates if overwrite==false.
      entry->val().docId = inverter.getDoc();
      entry->val().version = inverter.currVersion;
    }
  }

  Inverter::IndexHandler& getVersionHandler(Inverter& inverter) {
    if (versionHandler == nullptr) {
      versionHandler = &inverter.getIndexHandler("_version_");
    }
    return *versionHandler;
  }

public:
  void flush(Inverter& inverter) override {
    int32_t uniqueVals = (int32_t)termsHash.size();
    if (uniqueVals == 0) {
      return;
    }

    PostingsWriter& postingsWriter = inverter.getPostingsWriter();

    auto terms = termsHash.destructiveCompress();
    boost::sort::spreadsort::string_sort(terms, terms + uniqueVals, TermRef::bracket(), TermRef::getsize(),
                                         TermRef::lessthan());

    auto nDocs = inverter.getMaxDoc();
    auto guard = MemPool::threadLocalPoolGuard();

    // Write using TextWriter — same format as StrHandler for now.
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

    termsHash.free();

    OrdColWriter ordsWriter(guard.pool(), inverter.postingsWriter, fieldInfo, ords);
    ordsWriter.finish();
  }

};

} // namespace solux::handler
