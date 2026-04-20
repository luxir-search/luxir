#pragma once
#include "IndexWriter.h"
#include "OrdCollector.h"
#include "OrdColWriter.h"
#include "StoredFieldsWriter.h"
#include "solux/reader/DocsEnum.h"
#include "solux/reader/StoredFieldsReader.h"
#include "solux/reader/StrColReader.h"
#include "solux/search/IndexReader.h"

// This file is only included in IndexWriter.cpp

#define MERGER_DEBUG LOG_TRACE
// #define MERGER_DEBUG LOG_DEBUG

namespace solux {

class SegmentMerger {

  struct Segment {
    PostingsReader* postingsReader;
    FieldReader* fieldReader;
    int32_t numLive; // number of live documents in this segment
    int32_t base; // docId base for this segment in the merged segment, accounting for deletions
    int ord; // 0-based ordinal (index in original merge list)
    std::vector<int32_t> remap;  // 0 based (segment local) old to new id mapping, -1 means deleted

    bool hasDeletes() {
      return !remap.empty();
    }

    // returns a pair of (mappedDocId, isDeleted)
    std::pair<int32_t, bool> remapDocId(int32_t localId) const {
      bool isDeleted = false;
      int32_t mappedDoc;
      if (!remap.empty()) {
        mappedDoc = remap[localId];
        if (mappedDoc == -1) {
          isDeleted = true;
        }
      } else {
        mappedDoc = localId;
      }
      mappedDoc += base;
      return {mappedDoc, isDeleted};
    }


  };

  struct MergeFieldInfo {
    SegFieldInfo segFieldInfo;
    Segment* seg;  // points to the segment that produced this.
  };

  std::span<PostingsReader *> preaders;
  std::span<LiveDocs*> liveDocs; // parallel to preaders, nullptr if no deletes
  PostingsWriter& postingsWriter;

  std::vector<FieldReader> fieldReaders;  // todo - pool allocate (& use smart ptr on MergeSeg if destructors needed)
  std::vector<Segment> segs;
public:


  SegmentMerger(std::span<PostingsReader *> preaders, std::span<LiveDocs*> liveDocs, PostingsWriter& postingsWriter)
  : preaders(preaders), liveDocs(liveDocs), postingsWriter(postingsWriter)
  {
    assert(preaders.size() == liveDocs.size());
  }

  void merge() {
    auto guard = MemPool::threadLocalPoolGuard();
    auto& pool = guard.pool();

    segs.reserve(preaders.size());
    fieldReaders.reserve(preaders.size());  // This is important since we take pointers to these! Pool allocate later...

    int64_t totalLive = 0;  // used to set each segment base, will be equal to total live docs after the loop
    for (size_t i = 0; i < preaders.size(); i++) {
      auto preader = preaders[i];
      auto segLiveDocs = liveDocs[i];
      
      MERGER_DEBUG("SegmentMerger: segment {}, maxDoc={}, liveDocs={}",
               i, preader->maxDoc(), segLiveDocs ? segLiveDocs->numLive() : preader->maxDoc());
      
      fieldReaders.emplace_back(pool, *preader);
      FieldReader& fieldReader = fieldReaders.back();

      // position fieldReader on first field and add to segs if it's non-empty
      if (fieldReader.readNextField()) {
        // Calculate number of live documents
        int32_t numLive = segLiveDocs ? segLiveDocs->numLive() : preader->maxDoc();
        
        segs.emplace_back(preader, &fieldReader, numLive, totalLive, (int)segs.size());
        MERGER_DEBUG("SegmentMerger: added segment to merge, base={}, numLive={}", totalLive, numLive);
        totalLive += numLive; // Count only live documents for base offset
      } else {
        LOG_ERROR("Empty fieldReader!");
      }
    }
    // Set maxDoc to total number of live documents
    postingsWriter.setMaxDoc(totalLive);

    MERGER_DEBUG("SegmentMerger: total live docs to merge: {}", totalLive);

    // Now build the docId maps based on liveDocs, then release the liveDocs.
    for (size_t i = 0; i < segs.size(); i++) {
      auto& seg = segs[i];
      buildDocIdMapping(seg, liveDocs[i], seg.remap);
      // TODO: pass the actual liveDocs shared_ptr into the merger so we can release them.
    }

    auto fnameComp = [](const Segment& a, const Segment& b){ return b.fieldReader->name() < a.fieldReader->name(); };
    std::vector<Segment*> segPtrs(segs.size());
    // IndirectPQ<Segment, decltype(fnameComp)> fieldPQ(segs, segPtrs, false);
    IndirectPQ<Segment, decltype(fnameComp)> fieldPQ(segs, segPtrs);

    std::vector<MergeFieldInfo> mergeFieldInfos;
    mergeFieldInfos.reserve(segs.size());
    while (fieldPQ.size() > 0) {
      mergeFieldInfos.resize(0);
      auto currField = fieldPQ.top().fieldReader->name();

      // a redundant compare the first time through here, but simpler code.
      while (fieldPQ.size() > 0 && currField == fieldPQ.top().fieldReader->name()) {
        MergeFieldInfo& segField = mergeFieldInfos.emplace_back();
        fieldPQ.top().fieldReader->readFieldInfo(segField.segFieldInfo);
        segField.seg = &fieldPQ.top();  // point to which segment produced the segFieldInfo

        // increment to next field name and fix up heap
        if (!fieldPQ.top().fieldReader->readNextField()) {
          fieldPQ.removeTop();
        } else {
          fieldPQ.updateTop();
        }
      }

      // If merging would break any of the segment max constraints (i.e. number of unique terms in a field)
      // we could bail early.

      // TBB If this gets turned into a task, we would need to copy the mergeFieldInfos since
      // they will be reused.
      mergeField(mergeFieldInfos);
    }

    // Caller is responsible for calling postingsWriter.finish()
    // so it can collect the filenames written.
  }

private:
  // Helper to build mapping from old doc IDs to new doc IDs for a segment, accounting for deletes
  // Returns a vector where vector[oldDocId] = newDocId, or -1 if deleted.
  // This is still 0 based, so add the base in both cases.
  void buildDocIdMapping(const Segment& seg, LiveDocs* liveDocs, std::vector<int32_t>& target) {
    if (liveDocs == nullptr) {
      return; // no deletes, so nothing to do.
    }
    int32_t maxDoc = seg.postingsReader->maxDoc();
    assert(liveDocs->size() == maxDoc); // make sure this is the right liveDocs for the segment.
    target.resize(maxDoc);
    int32_t newDocId = 0;
    const FixedBitSet& bitset = liveDocs->bitset();
    
    for (int32_t oldDocId = 0; oldDocId < maxDoc; oldDocId++) {
      if (bitset.get(oldDocId)) {
        target[oldDocId] = newDocId++;
      } else {
        target[oldDocId] = -1; // Document is deleted
      }
    }

    assert(newDocId == seg.numLive); // Make sure we got the right number of live documents
  }

  // add docs and positions from the provided DocsEnum
  void addDocsPos(TextWriter& textWriter, DocsEnum& docsEnum, const Segment& seg) {
    for (;;) {
      int32_t docid = docsEnum.nextDoc();
      if (docid == INT_MAX) break;

      // predictable branch for deleted vs not
      int mappedDoc = seg.remap.empty() ? docid : seg.remap[docid];
      if (mappedDoc == -1) {
        continue; // Document is deleted
      }
      int32_t newDocid = seg.base + mappedDoc;

      textWriter.startDoc(newDocid);
      docsEnum.startPositions();
      int32_t lastPos = -1;
      for (;;) {
        auto pos = docsEnum.nextPosition();
        if (pos == INT_MAX) break;
        textWriter.addPositionDelta(pos - lastPos);
        lastPos = pos;
      }
      textWriter.endDoc(newDocid);
    }

  }

  // add docs and ordinals from the provided DocsEnum (for string column, record ord in docToOrd for each doc, to be written later)
  void addDocsOrds(TextWriter& textWriter, DocsEnum& docsEnum, const Segment& seg, OrdCollector& docToOrd, int32_t ord) {
    for(;;) {
      int32_t docid = docsEnum.nextDoc();
      if (docid == INT_MAX) break;

      // predictable branch for deleted vs not
      int mappedDoc = seg.remap.empty() ? docid : seg.remap[docid];
      if (mappedDoc == -1) {
        continue; // Document is deleted
      }
      int32_t newDocid = seg.base + mappedDoc;

      docToOrd.add(newDocid, ord);
      textWriter.startDoc(newDocid);
      docsEnum.startPositions();
      int32_t lastPos = -1;
      for(;;) {
        auto pos = docsEnum.nextPosition();
        if (pos == INT_MAX) break;
        textWriter.addPositionDelta(pos - lastPos);
        lastPos = pos;
      }
      textWriter.endDoc(newDocid);
    }
  }

  void mergeField(std::vector<MergeFieldInfo>& mergeFieldInfos) {
    int32_t nDocs = postingsWriter.getMaxDoc();
    auto poolGuard = MemPool::threadLocalPoolGuard();
    auto& pool = poolGuard.pool();

    // Stored-fields resources are segment-wide rather than normal per-field
    // columns.  Route each one (default or named column family) to a
    // dedicated merger.  Recognize them by the combination of type=BIN and
    // flags=STORED set by StoredFieldsWriter::finish().
    const auto& firstInfo = mergeFieldInfos[0].segFieldInfo;
    if (firstInfo.type == FieldType::BIN
        && (firstInfo.flags & FieldType::STORED) != 0) {
      mergeStoredFields(mergeFieldInfos);
      return;
    }

    // sort mergeFieldInfos so we can add docids low to high.
    // This is a simple O(n+m) sort where n=number of segments and m is number of segments with this specific field.
    // Simply slot the field into it's place and then compact.
    std::vector<MergeFieldInfo*> sortedFields(segs.size());
    for (auto& field : mergeFieldInfos) {
      sortedFields[field.seg->ord] = &field;
    }

    // compactFields are sorted with nulls removed.
    std::vector<MergeFieldInfo*> compactFields;
    compactFields.reserve(mergeFieldInfos.size());
    for (auto* field : sortedFields) {
      if (field) {
        compactFields.push_back(field);
      }
    }

    // TODO: check if fields are compatible!
    // TODO: gather other stats to help us build the field (like if it's a dense field!)
    int32_t allFlags = 0;
    FieldType::Type type = FieldType::Type::NONE;
    for (auto* field : compactFields) {
      if (type == FieldType::Type::NONE) {
        type = field->segFieldInfo.type;
      } else if (type != field->segFieldInfo.type) {
        LOG_ERROR("Field types don't match! {} {}", (int)type, (int)field->segFieldInfo.type);
        // now what?
      }
      allFlags |= field->segFieldInfo.flags;
    }

    // get/reserve a new fieldInfo from the postingsReader
    PostingsWriter::IndexFieldInfo& outputFieldInfo = postingsWriter.addField(compactFields[0]->segFieldInfo.fieldname);
    outputFieldInfo.type = type;
    outputFieldInfo.flags = allFlags;

    // if this is an indexed string column, we need to collect the ordinals for each doc
    bool isOrdCol = (type == FieldType::Type::STRING) && (allFlags & FieldType::INDEX_DOCS);
    std::optional<OrdCollector> ordCollector;
    // use a separate pool for the ordCollector since the ords will be built at the same time as the postings are read/written,
    // and we want to roll back much of that allocation, but preserve the ords.
    std::optional<MemPool> ordPool;
    if (isOrdCol) {
      ordPool.emplace();
      ordCollector.emplace(ordPool.value(), nDocs);
    }

    if (allFlags & FieldType::INDEX_DOCS) {
      // nocommit outputFieldInfo.flags |= 0x01;
      TextWriter textWriter(postingsWriter);
      textWriter.startField(&outputFieldInfo);

      // Collect TermsEnum for each segment.  Keep track of the index so we can visit in ascending order one at a time.
      struct TermsEnumIdx {
        TermsEnum tenum;
        size_t idx;
      };
      std::vector<TermsEnumIdx> tenums;
      tenums.reserve(compactFields.size());
      std::vector<TermsEnumIdx*> tenumPtrs;
      tenumPtrs.reserve(compactFields.size());

      for (size_t idx = 0; idx<compactFields.size(); idx++) {
        auto field = compactFields[idx];
        tenums.emplace_back(TermsEnumIdx{TermsEnum(pool, *field->seg->postingsReader, field->segFieldInfo), idx});
        // Position on the first term.  If none, don't add to the PQ
        if (tenums.back().tenum.nextTerm()) {
          tenumPtrs.push_back(&tenums.back());
        }
      }

      auto termCmp = [](const TermsEnumIdx& a, const TermsEnumIdx& b){
        int cmp = b.tenum.term() <=> a.tenum.term();
        return cmp < 0 || (cmp == 0 && b.idx < a.idx);  // tiebreak by index so we visit segments in order
      };
      IndirectPQ<TermsEnumIdx, decltype(termCmp)> termPQ(tenums, tenumPtrs, false);

      // iterate through the terms in sorted order
      while (termPQ.size() > 0) {
        TermsEnumIdx& first = termPQ.top();
        // Need to make a copy of the term since it will be invalidated after tenum.nextTerm()
        // is called.  It needs to exist until the end of textWriter (currently).  See comments on startTerm()
        // for ideas.
        // TODO: FIXME: revisit this! We should add the term to a pool we can roll back as soon as possible.
        PackedTerm term(pool, std::string_view(first.tenum.term()));
        auto termOrd = textWriter.startTerm(term);

        do {
          TermsEnumIdx& entry = termPQ.top();
          // need to create the docsEnum while the termsEnum is still positioned on the term.
          DocsEnum docsEnum(pool, *compactFields[entry.idx]->seg->postingsReader, entry.tenum);

          if (isOrdCol) {
            // this is a string column, so keep track of the ordinals for each doc
            addDocsOrds(textWriter, docsEnum, *compactFields[entry.idx]->seg, ordCollector.value(), termOrd);
          } else {
            // text field, so add docs with positions to the textWriter
            addDocsPos(textWriter, docsEnum, *compactFields[entry.idx]->seg);
          }

          // advance that entry to the next term, removing from pq if exhausted.
          if (entry.tenum.nextTerm()) {
            termPQ.updateTop();
          } else {
            termPQ.removeTop();
          }
          // continue while more enums are positioned on the same term
        } while (termPQ.size() > 0 && termPQ.top().tenum.term() == term);

        textWriter.endTerm(term);
      }

      textWriter.endField();
    }

    if (isOrdCol) {
      // auto guard = pool.rewindScopeGuard();
      OrdColWriter ordsWriter(pool, postingsWriter, outputFieldInfo, ordCollector.value());
      ordsWriter.finish();
      // nothing is done after this in this method, so we can let the normal destructors clean up.
      // ordCollector.reset();
      // ordPool.reset();

    } else if (type == FieldType::Type::STRING && !(allFlags & FieldType::INDEX_DOCS)) {
      // Non-indexed string column (column-only storage)
      mergeStrCol(sortedFields, postingsWriter, outputFieldInfo);
    } else {
      // int column that is not an ord column (assume all other field types have this (currently true)
      mergeIntCol2(sortedFields, postingsWriter, outputFieldInfo);
    }
  }


  // Merge one stored-fields resource (default or named column family).
  // Decompresses each source segment's live docs in merged-docID order and
  // re-emits them via a fresh StoredFieldsWriter for the same resource name.
  // Multi-valued fields are preserved as single addValues calls so the
  // on-disk grouping survives the merge.  Verbatim chunk-copy optimization
  // is a follow-up.
  void mergeStoredFields(std::vector<MergeFieldInfo>& mergeFieldInfos) {
    // Resource name comes from the (identical across segments) field name.
    std::string_view resourceName(mergeFieldInfos[0].segFieldInfo.fieldname);

    // Build a per-segment reader vector indexed by seg ord.  Null for segments
    // whose source had no stored-fields resource.
    std::vector<std::unique_ptr<StoredFieldsReader>> readers(segs.size());
    for (auto& mfi : mergeFieldInfos) {
      auto ord = mfi.seg->ord;
      readers[ord] = std::make_unique<StoredFieldsReader>(
          *mfi.seg->postingsReader, mfi.segFieldInfo);
    }

    // Config isn't threaded through the merger yet; writer uses defaults.
    // When per-family codec/chunk-size becomes meaningful, look up the
    // StoredFieldType in the current schema here.
    StoredFieldsWriter writer(postingsWriter, resourceName);

    for (auto& seg : segs) {
      auto* reader = readers[seg.ord].get();
      if (!reader) continue;  // segment had no stored-fields; writer pads automatically
      int32_t maxDocIn = seg.postingsReader->maxDoc();
      for (int32_t localId = 0; localId < maxDocIn; localId++) {
        auto [mappedDoc, isDeleted] = seg.remapDocId(localId);
        if (isDeleted) continue;
        reader->readDoc(localId,
            [&](std::string_view name, std::span<const std::string_view> values) {
          if (values.size() == 1) {
            writer.addValue(mappedDoc, name, values[0]);
          } else {
            writer.addValues(mappedDoc, name, values);
          }
        });
      }
    }

    writer.finish(postingsWriter.getMaxDoc());
  }

  void mergeIntCol(std::span<MergeFieldInfo*> sortedFields, PostingsWriter& postingsWriter,
                   PostingsWriter::IndexFieldInfo& outputFieldInfo) {
    auto poolGuard = MemPool::threadLocalPoolGuard();
    auto& pool = poolGuard.pool();

    auto outputPtr = postingsWriter.getOutputStream();
    IntColWriter intColWriter(*outputPtr);

    // currently all values must be written before all docs - TODO FIXME - is this still true??
    for (auto* field : sortedFields) {
      auto baseId = (int32_t)field->seg->base;
      unused(baseId);

      IntColReader reader(*field->seg->postingsReader, field->segFieldInfo);
      IntColReader::BulkValues values(reader);


      // int32_t highest = field->seg->postingsReader->numDocs();
      for (;;) {
        auto index = values.next();
        if (index == IntColReader::ENDINDEX) {
          break;
        }
        int64_t val = values.value();
        intColWriter.addInt64(val);
      }
    }

    bool full = false; // TODO: calculate if this column is dense!

    if (!full) {
      DocsWithValWriter docsWriter(pool, postingsWriter, outputFieldInfo);
      for (auto* field : sortedFields) {
        auto baseId = (int32_t)field->seg->base;

        IntColReader reader(*field->seg->postingsReader, field->segFieldInfo);
        IntColReader::Iterator colIter(reader);

        [[maybe_unused]] int32_t highest = field->seg->postingsReader->maxDoc();
        for (;;) {
          int32_t localId = colIter.next();
          if (localId == IntColReader::ENDDOC) {
            break;
          }
          assert(localId < highest);
          docsWriter.startDoc(baseId + localId);
        }
      }
      docsWriter.finish();
    }


    intColWriter.finish(outputFieldInfo);
    if (outputFieldInfo.flags & FieldType::MULTI_VALUED) {
      auto guard = pool.rewindScopeGuard();
      OutputStreamPtr out = postingsWriter.getOutputStream();
      MonoWriter endRankWriter(pool, *out);
      int64_t endRankBase = 0;

      for (auto* field : sortedFields) {
        assert(field->segFieldInfo.flags & FieldType::MULTI_VALUED);
        // open IntColReader for each segment
        IntColReader reader( *field->seg->postingsReader, field->segFieldInfo);
        MonoReader* endRankReader = reader.getEndRankReader();
        assert(endRankReader != nullptr);
        int64_t endRank;
        for (int i = 0; i < endRankReader->numValues(); i++) {
          endRank = endRankBase + endRankReader->valueAt(i);
          endRankWriter.addInt64(endRank);
        }
        endRankBase = endRank;
      }
      endRankWriter.finish();
      outputFieldInfo.monoLoc = endRankWriter.blockLoc;
      outputFieldInfo.monoMetaOff = endRankWriter.metaOff;
    }
  }


  // Merges non-indexed string/binary columns written by StrColHandler.
  // Rebuilds the concatenated byte stream, the per-value endOffsetReader (if values are
  // variable-size), and the per-doc endRankReader (if the field is multi-valued).
  void mergeStrCol(std::span<MergeFieldInfo*> sortedFields, PostingsWriter& postingsWriter,
                   PostingsWriter::IndexFieldInfo& outputFieldInfo) {
    assert(sortedFields.size() == segs.size());

    auto poolGuard = MemPool::threadLocalPoolGuard();
    auto& pool = poolGuard.pool();

    assert(outputFieldInfo.type == FieldType::Type::STRING);
    assert(!(outputFieldInfo.flags & FieldType::INDEX_DOCS));

    bool multiValued = (outputFieldInfo.flags & FieldType::MULTI_VALUED) != 0;

    // Output stream for the concatenated value bytes.
    OutputStreamPtr valuesOut = postingsWriter.getOutputStream();
    outputFieldInfo.columnLoc = valuesOut->slocation();

    u_ptr<DocsWithValWriter> docsWriter = nullptr;

    // endOffsetWriter is created lazily the first time we see a value whose size differs
    // from the first value's size.  Until then we track the uniform size and can skip
    // writing the mono column.
    u_ptr<MonoWriter> endOffsetWriter = nullptr;
    OutputStreamPtr endOffsetOut;

    // endRankWriter is created up-front when the field is multi-valued.
    u_ptr<MonoWriter> endRankWriter = nullptr;
    OutputStreamPtr endRankOut;
    if (multiValued) {
      endRankOut = postingsWriter.getOutputStream();
      endRankWriter = pool.make_unique_align<MonoWriter>(8, pool, *endRankOut);
    }

    int32_t docsWithField = 0;
    int64_t totalValues = 0;
    int32_t minSize = std::numeric_limits<int32_t>::max();
    int32_t maxSize = std::numeric_limits<int32_t>::min();
    int64_t cumulativeBytes = 0;
    bool isDense = true;

    // Emits one value into the merged output: appends its bytes, updates size tracking,
    // and writes the corresponding endOffset entry (lazily starting the mono column if
    // a size mismatch appears).
    auto emitValue = [&](std::string_view value) {
      int32_t valueSize = (int32_t)value.size();
      valuesOut->write(value.data(), valueSize);
      cumulativeBytes += valueSize;
      int32_t prevMin = minSize;
      minSize = std::min(minSize, valueSize);
      maxSize = std::max(maxSize, valueSize);

      if (endOffsetWriter) {
        endOffsetWriter->addInt64(cumulativeBytes);
      } else if (totalValues > 0 && valueSize != prevMin) {
        // First size mismatch: start writing the mono column and backfill prior values.
        endOffsetOut = postingsWriter.getOutputStream();
        endOffsetWriter = pool.make_unique_align<MonoWriter>(8, pool, *endOffsetOut);
        for (int64_t i = 1; i <= totalValues; i++) {
          endOffsetWriter->addInt64((int64_t)prevMin * i);
        }
        endOffsetWriter->addInt64(cumulativeBytes);
      }
      totalValues++;
    };

    // Process segments in order - since doc remapping is monotonic,
    // we can simply iterate through each segment sequentially.
    for (size_t segnum = 0; segnum < sortedFields.size(); segnum++) {
      auto* field = sortedFields[segnum];
      auto& seg = segs[segnum];

      if (field == nullptr) {
        // Field didn't exist for this segment.  If segment has live docs, the merged
        // field becomes sparse: create docsWriter and backfill.
        if (seg.numLive > 0) {
          isDense = false;
          if (!docsWriter) {
            docsWriter = pool.make_unique_align<DocsWithValWriter>(8, pool, postingsWriter, outputFieldInfo);
            for (int32_t i = 0; i < docsWithField; i++) {
              docsWriter->startDoc(i);
            }
          }
        }
        continue;
      }

      assert(field->seg == &seg);

      StrColReader reader(*field->seg->postingsReader, field->segFieldInfo);
      StrColReader::Iterator iter(reader);

      if (reader.docsReader().hasBitset() && !docsWriter) {
        isDense = false;
        docsWriter = pool.make_unique_align<DocsWithValWriter>(8, pool, postingsWriter, outputFieldInfo);
        for (int32_t i = 0; i < docsWithField; i++) {
          docsWriter->startDoc(i);
        }
      }

      for (int32_t localId = iter.advance(0); localId != StrColReader::Iterator::ENDDOC; localId = iter.next()) {
        auto [mappedDoc, isDeleted] = seg.remapDocId(localId);
        if (isDeleted) {
          continue;
        }

        assert(!isDense || mappedDoc == docsWithField);

        if (multiValued) {
          auto [startRank, endRank] = iter.valueRange();
          for (int64_t r = startRank; r < endRank; r++) {
            emitValue(reader.valueAt(r));
          }
          endRankWriter->addInt64(totalValues);
        } else {
          emitValue(iter.value());
        }

        if (docsWriter) {
          docsWriter->startDoc(mappedDoc);
        }

        docsWithField++;
      }
    }

    if (docsWithField == 0) {
      outputFieldInfo.docsWithField = 0;
      outputFieldInfo.columnMetaOff = 0;
      outputFieldInfo.numValues = 0;
      return;
    }

    valuesOut->flush(true);
    outputFieldInfo.columnMetaOff = valuesOut->size() - outputFieldInfo.columnLoc.offset();
    outputFieldInfo.numValues = totalValues;

    // endOffsetReader (mono2)
    if (endOffsetWriter) {
      endOffsetWriter->finish();
      outputFieldInfo.mono2Loc = endOffsetWriter->blockLoc;
      outputFieldInfo.mono2MetaOff = endOffsetWriter->metaOff;
    } else {
      // All values are the same size - store size in mono2MetaOff, leave mono2Loc zero.
      outputFieldInfo.mono2Loc = {0, 0};
      outputFieldInfo.mono2MetaOff = (totalValues == 0) ? 0 : minSize;
    }

    // endRankReader (mono)
    if (endRankWriter) {
      endRankWriter->finish();
      outputFieldInfo.monoLoc = endRankWriter->blockLoc;
      outputFieldInfo.monoMetaOff = endRankWriter->metaOff;
    }

    outputFieldInfo.docsWithField = docsWithField;

    if (docsWriter) {
      assert(docsWriter->numAdded() == docsWithField);
      if (docsWriter->numAdded() == postingsWriter.getMaxDoc()) {
        docsWriter->finishDense(postingsWriter.getMaxDoc());
      } else {
        docsWriter->finish();
      }
    } else {
      outputFieldInfo.docsWithFieldEndLoc = {0, 0};
    }
  }

  void mergeIntCol2(std::span<MergeFieldInfo*> sortedFields, PostingsWriter& postingsWriter,
                   PostingsWriter::IndexFieldInfo& outputFieldInfo) {
    assert(sortedFields.size() == segs.size());  // expect non-compacted fields to make the code a little simpler.

    auto poolGuard = MemPool::threadLocalPoolGuard();
    auto& pool = poolGuard.pool();

    auto outputPtr = postingsWriter.getOutputStream();
    auto intColWriter = pool.make_unique_align<IntColWriter>(8, *outputPtr);

    u_ptr<DocsWithValWriter> docsWriter = nullptr;  // docs with the field, created on demand if needed

    u_ptr<MonoWriter> endRankWriter = nullptr; // null means single valued, otherwise multi-valued
    OutputStreamPtr monoOut;                   // output stream for the monoWriter.

    if (outputFieldInfo.flags & FieldType::MULTI_VALUED) {
      monoOut = postingsWriter.getOutputStream();
      endRankWriter = pool.make_unique_align<MonoWriter>(8, pool, *monoOut);
    }

    int64_t endRankBase = 0;  // used to calculate the endRank for each segment, if multivalued.
    int32_t docsWithField = 0;

    for (size_t segnum = 0; segnum < sortedFields.size(); segnum++) {
      auto* field = sortedFields[segnum];
      auto& seg = segs[segnum];
      auto base = seg.base;

      if (field == nullptr) {
        // If the field didn't exist for this segment, then output can't be dense.
        // If no docsWriter exists yet (because all full) we need to create one and fill it in up until base.
        if (docsWriter == nullptr) {
          docsWriter = pool.make_unique_align<DocsWithValWriter>(8, pool, postingsWriter, outputFieldInfo);
          for (int32_t i = 0; i < base; i++) {
            docsWriter->startDoc(i); // write all docs up to base as they were dense before this segment.
          }
        }
        continue; // no values to write, so skip to next segment.
      }

      assert(field->seg == &seg);

      IntColReader& reader = *pool.make_align<IntColReader>(8, *field->seg->postingsReader, field->segFieldInfo);
      MonoReader* endRankReader = reader.getEndRankReader();
      IntColReader::BulkValues& values = *pool.make_align<IntColReader::BulkValues>(8, reader);
      screaming::BitSet::Iterator* docsIter = nullptr; // null means all docs have values.
      if (reader.docsReader().hasBitset()) {
        docsIter = pool.make_align<screaming::BitSet::Iterator>(8, reader.docsReader().bitset());
        // not all docs have values, so we need to create a docsWriter and fill it in up until base.
        if (docsWriter == nullptr) {
          docsWriter = pool.make_unique_align<DocsWithValWriter>(8, pool, postingsWriter, outputFieldInfo);
          for (int32_t i = 0; i < base; i++) {
            docsWriter->startDoc(i); // write all docs up to base as they were dense before this segment.
          }
        }
      }

      // Strategy:
      //   - read nextDocWithValue
      //   - if multiValued, read next endRank and calculate number of values (use previous endRank)
      //   - read that many values and write that many values (1 if single valued)
      //   - write mapped docWithValue, write mapped endRank, write values.
      //   - if the doc is deleted, still do reads, but skip the writes.
      // variable naming: In suffix is for reading, Out suffix is for writing.

      int32_t localId = -1;
      int32_t maxDocIn = seg.postingsReader->maxDoc();

      // we don't need to check liveDocs since we have the doc mapping.
      // auto* liveBits = liveDocs[seg.ord] ? &liveDocs[seg.ord]->bitset() : nullptr;

      int64_t lastEndRankIn = 0;
      int32_t docRankIn = -1;  // rank of the doc we are on, faster than figuring out from docsIter.
      for (;;) {
        if (docsIter) {
          localId = docsIter->next();
        } else {
          localId++;
        }
        docRankIn++;
        if (localId >= maxDocIn) {
          break;
        }

        auto [mappedDoc, isDeleted] = seg.remapDocId(localId);

        if (isDeleted) {
          //
          // nothing needs to be adjusted for single-valued fields.
          //   - docRankIn will still be incremented and be correct for reading
          //   - mappedDoc already contains the adjustments for the docsWithValues (docsWriter)
          // for multi-valued fields, we need to read how many values there were for this deleted doc.
          //   - end rank values written need to be adjusted down by the number of values.
          //     (adjust endRankBase by the number of values skipped)
          //   - lastEndRankIn needs to be maintained correctly (that's how we tell how many values a doc has)
          if (endRankReader) {
            int64_t endRank = endRankReader->valueAt(docRankIn);
            auto nVals = endRank - lastEndRankIn;
            endRankBase -= nVals; // adjust the base down by the number of values skipped.
            lastEndRankIn = endRank;
          }
          continue;
        }


        if (docsWriter) {
          docsWriter->startDoc(mappedDoc);
        }

        docsWithField++;
        if (endRankReader) {
          //  multi-valued
          // TODO: OPT: use a bulk iterator for the ranks.
          int64_t endRank = endRankReader->valueAt(docRankIn);

          // write the new endRank
          endRankWriter->addInt64(endRankBase + endRank);

          // transfer the values
          for (int64_t inRank = lastEndRankIn; inRank < endRank; inRank++) {
            auto val = values.valueAt(inRank);
            intColWriter->addInt64(val);
          }

          // TODO: if doc was deleted, then adjust endRankBase down by nValues.
          lastEndRankIn = endRank;
        } else {
          // single-valued
          auto val = values.valueAt(docRankIn);
          intColWriter->addInt64(val);
        }
      }

      // endRankBase was already adjusted down for deletes, do just add the last endRankIn to
      // get the new base.
      endRankBase += lastEndRankIn;
    } // for-each-seg

    intColWriter->finish(outputFieldInfo);
    if (docsWriter) {
      assert(docsWriter->numAdded() == docsWithField);
      if (docsWriter->numAdded() == postingsWriter.getMaxDoc()) {
        // all docs were added.  We may have written stuff into the index,
        // but we can forget it (it will be dropped on the next merge).
        // TODO: we could look into rewinding any part of the OutputStream that was unflushed as well.
        docsWriter->finishDense(postingsWriter.getMaxDoc());
      } else {
        // some docs were not added, so we need to finish it.
        docsWriter->finish();
      }
    } else {
      outputFieldInfo.docsWithField = postingsWriter.getMaxDoc();
      outputFieldInfo.docsWithFieldEndLoc = {0, 0};
    }
    if (endRankWriter) {
      endRankWriter->finish();
      outputFieldInfo.monoLoc = endRankWriter->blockLoc;
      outputFieldInfo.monoMetaOff = endRankWriter->metaOff;
    }
  }

};


}

