#pragma once

#include <span>
#include "PostingsWriter.h"
#include "IntColWriter.h"
#include "OrdCollector.h"

namespace solux {

class OrdColWriter {
  MemPool& pool;
  PostingsWriter& postingsWriter;
  PostingsWriter::IndexFieldInfo& fieldInfo;
  OrdCollector& ords;
  u_ptr<MonoWriter> endValueRankWriter;
  OutputStreamPtr endValueRankOutput;
public:
  OrdColWriter(MemPool& pool, PostingsWriter& postingsWriter, PostingsWriter::IndexFieldInfo& fieldInfo, OrdCollector& ords)
  : pool(pool), postingsWriter(postingsWriter), fieldInfo(fieldInfo), ords(ords)
  {
    if (ords.multiValued()) {
      // If this is a multivalued field, then we also need to write to another column that
      // indicates the end of the values for this doc.
      endValueRankOutput = postingsWriter.getOutputStream();
      endValueRankWriter = pool.make_unique<MonoWriter>(pool, *endValueRankOutput);
    }
  }

  // rankToDoc maps a rank-indexed collector's slot (a field-rank) back to its docid.
  // Empty means the collector is indexed directly by docid (slot == docid).
  void finish(std::span<const int32_t> rankToDoc = {}) {
    int32_t nDocs = postingsWriter.getMaxDoc();
    int32_t nSlots = ords.size();
    // Either docid-indexed (no map) or one docid per slot.
    assert(rankToDoc.empty() || (int32_t)rankToDoc.size() == nSlots);
    fieldInfo.docsWithField = ords.docsWithValue();

    // Write the ordinals to the postings file.
    // This is pretty much repeated code from Inverter::StringIndexHandler - TODO: refactor to own Writer.
    // Slots are walked in index order, which is doc order for both docid- and rank-indexed
    // collectors, so the ord column comes out doc-ordered either way.
    {
      auto outputPtr = postingsWriter.getOutputStream();
      IntColWriter ordCol(*outputPtr);

      int64_t nValues = 0;
      for (int slot = 0; slot < nSlots; slot++) {
        auto prev = nValues;
        ords.pushValues(slot, [&](int32_t ord) {
          ordCol.addInt64(ord);
          nValues++;
        });

        if (prev != nValues && endValueRankWriter) {
          endValueRankWriter->addInt64(nValues);
        }
      }

      ordCol.finish(fieldInfo);

      if (endValueRankWriter) {
        endValueRankWriter->finish();
        fieldInfo.monoLoc = endValueRankWriter->blockLoc;
        fieldInfo.monoMetaOff = endValueRankWriter->metaOff;
        endValueRankOutput.reset();
        endValueRankWriter.reset();
      }
    }

    // TODO: we should do this at the same time now, not as a separate pass.
    // If we want them adjacent in the file, we could write to a buffer (docsWithValue will never be that large).
    bool full = (ords.docsWithValue() == nDocs);

    // currently "full" is determined via fieldInfo.docsWithField, so there is nothing else to write if full.
    if (!full) {
      auto guard = pool.rewindScopeGuard();
      DocsWithValWriter docsWriter(pool, postingsWriter, fieldInfo);
      for (int slot = 0; slot < nSlots; slot++) {
        if (ords.hasValues(slot)) {
          docsWriter.startDoc(rankToDoc.empty() ? slot : rankToDoc[slot]);
        }
      }
      docsWriter.finish();
    }

  }

};





} // end namespace