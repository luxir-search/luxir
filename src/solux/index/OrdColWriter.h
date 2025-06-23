#pragma once

#include "PostingsWriter.h"
#include "IntColWriter.h"
#include "OrdCollector.h"

namespace solux {

class OrdColWriter {
  MemPool& pool;
  PostingsWriter& postingsWriter;
  PostingsWriter::IndexFieldInfo& fieldInfo;
  OrdCollector& ords;
  IntColWriter colWriter;
  u_ptr<MonoWriter> endRankWriter;
  OutputStreamPtr endRankOutput;
public:
  OrdColWriter(MemPool& pool, PostingsWriter& postingsWriter, PostingsWriter::IndexFieldInfo& fieldInfo, OrdCollector& ords)
  : pool(pool), postingsWriter(postingsWriter), fieldInfo(fieldInfo), ords(ords),
    colWriter(pool, postingsWriter, fieldInfo)
  {
    if (ords.multiValued()) {
      // If this is a multivalued field, then we also need to write to another column that
      // indicates the end of the values for this doc.
      endRankOutput = postingsWriter.getOutputStream();
      endRankWriter = pool.make_unique<MonoWriter>(pool, *endRankOutput);
    }
  }

  void finish() {
    int32_t nDocs = postingsWriter.getMaxDoc();
    fieldInfo.docsWithField = ords.docsWithValue();

    // Write the ordinals to the postings file.
    // This is pretty much repeated code from Inverter::StringIndexHandler - TODO: refactor to own Writer.
    {
      auto guard = pool.rewindScopeGuard();
      IntColWriter ordCol(pool, postingsWriter, fieldInfo);

      int64_t nValues = 0;
      for (int docid = 0; docid < nDocs; docid++) {
        auto prev = nValues;
        ords.pushValues(docid, [&](int32_t ord) {
          ordCol.addInt64(ord);
          nValues++;
        });

        if (prev != nValues && endRankWriter) {
          endRankWriter->addInt64(nValues);
        }
        prev = nValues;
      }

      ordCol.finish();
      if (endRankWriter) {
        endRankWriter->finish();
        fieldInfo.monoLoc = endRankWriter->blockLoc;
        fieldInfo.monoMetaOff = endRankWriter->metaOff;
        endRankOutput.reset();
        endRankWriter.reset();
      }
    }

    // TODO: we should do this at the same time now, not as a separate pass.
    // If we want them adjacent in the file, we could write to a buffer (docsWithValue will never be that large).
    bool full = (ords.docsWithValue() == nDocs);

    // currently "full" is determined via fieldInfo.docsWithField, so there is nothing else to write if full.
    if (!full) {
      auto guard = pool.rewindScopeGuard();
      DocsWithValWriter docsWriter(pool, postingsWriter, fieldInfo);
      for (int docid = 0; docid < nDocs; docid++) {
        if (ords.hasValues(docid)) {
          docsWriter.startDoc(docid);
        }
      }
      docsWriter.finish();
    }

  }

};





} // end namespace