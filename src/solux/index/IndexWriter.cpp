#include "IndexWriter.h"
#include "solux/util/heap.h"

namespace solux {

  // When merging, we need to:
  //   - know the "base" of each segment... just because reader7 is the only one with a field, everything
  //     needs to line up correctly by docid.

// we could do this with just functions, but having a class allows us more freedom (like avoiding forward references, etc)
class SegmentMerger {

  struct Segment {
    PostingsReader* postingsReader;
    FieldReader* fieldReader;
    int64_t base; // docId base for this segment in the merged segment, currently not taking into account deletions
    // Actually, this could be int32_t here for merging since we can't have a single seg larger than that.
    // But maybe we could reuse this class elsewhere?
    int ord; // 0-based ordinal
  };

  struct MergeFieldInfo {
    SegFieldInfo segFieldInfo;
    Segment* seg;  // points to the segment that produced this.
  };

  std::span<PostingsReader *> preaders;
  PostingsWriter& postingsWriter;

  std::vector<FieldReader> fieldReaders;  // todo - pool allocate (& use smart ptr on MergeSeg if destructors needed)
  std::vector<Segment> segs;

public:


  SegmentMerger(std::span<PostingsReader *> preaders, PostingsWriter& postingsWriter)
  : preaders(preaders), postingsWriter(postingsWriter)
  {
  }

  void merge() {
    auto guard = MemPool::threadLocalPoolGuard();
    auto& pool = guard.pool();

    segs.reserve(preaders.size());
    fieldReaders.reserve(preaders.size());  // This is important since we take pointers to these! Pool allocate later...

    int64_t base = 0;
    for (auto preader : preaders) {
      fieldReaders.emplace_back(pool, *preader);
      FieldReader& fieldReader = fieldReaders.back();

      // position fieldReader on first field and add to segs if it's non-empty
      if (fieldReader.readNextField()) {
        segs.emplace_back(preader, &fieldReader, base, (int)segs.size());
        base += preader->numDocs();
      } else {
        LOG_ERROR("Empty fieldReader!");
      }
    }
    // after the loop, base will be equal to nDocs
    postingsWriter.setMaxDoc(base);

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

    // TODO: where should postingsWriter be finished?
    postingsWriter.finish();
  }

private:

  // add docs and positions from the provided DocsEnum
  void addDocsPos(TextWriter& textWriter, DocsEnum& docsEnum, int32_t base) {
    for(;;) {
      int32_t docid = docsEnum.nextDoc();
      if (docid == INT_MAX) break;
      int32_t newDocid = base + docid;
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

  // add docs and ordinals from the provided DocsEnum (for string column, record ord in docToOrd for each doc, to be written later)
  void addDocsOrds(TextWriter& textWriter, DocsEnum& docsEnum, int32_t base, OrdCollector& docToOrd, int32_t ord) {
    for(;;) {
      int32_t docid = docsEnum.nextDoc();
      if (docid == INT_MAX) break;
      int32_t newDocid = base + docid;
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

    // sort mergeFieldInfos so we can add docids low to high.
    // This is a simple O(n+m) sort where n=number of segments and m is number of segments with this specific field.
    // Simply slot the field into it's place and then compact.
    std::vector<MergeFieldInfo*> sortedFields(segs.size());
    for (auto& field : mergeFieldInfos) {
      sortedFields[field.seg->ord] = &field;
    }

    // compact list
    auto iter = sortedFields.begin();
    for (auto* field : sortedFields) {
      if (field != nullptr) {
        *iter++ = field;
      }
    }
    assert(size_t(iter - sortedFields.begin()) == mergeFieldInfos.size());
    sortedFields.resize(mergeFieldInfos.size());

    // TODO: check if fields are compatible!
    // TODO: gather other stats to help us build the field (like if it's a dense field!)
    int32_t allFlags = 0;
    FieldType::Type type = FieldType::Type::NONE;
    for (auto* field : sortedFields) {
      if (type == FieldType::Type::NONE) {
        type = field->segFieldInfo.type;
      } else if (type != field->segFieldInfo.type) {
        LOG_ERROR("Field types don't match! {} {}", (int)type, (int)field->segFieldInfo.type);
        // now what?
      }
      allFlags |= field->segFieldInfo.flags;
    }

    // get/reserve a new fieldInfo from the postingsReader
    PostingsWriter::IndexFieldInfo& outputFieldInfo = postingsWriter.addField(sortedFields[0]->segFieldInfo.fieldname);
    outputFieldInfo.type = type;
    outputFieldInfo.flags = allFlags;

    // if this is a string column, we need to collect the ordinals for each doc
    bool isOrdCol = (type == FieldType::Type::STRING);
    std::optional<OrdCollector> ordCollector;
    // use a separate pool for the ordCollector since the ords will be built at the same time as the postings are read/written,
    // and we want to roll back much of that allocation, but preserve the ords.
    std::optional<MemPool> ordPool;
    if (isOrdCol) {
      ordPool.emplace();
      ordCollector.emplace(ordPool.value(), nDocs);
    }

    MemPool readerPool; // TODO: can this be the same as writerPool?

    if (allFlags & FieldType::INDEX_DOCS) {
      auto readerPoolGuard = pool.rewindScopeGuard();
      // nocommit outputFieldInfo.flags |= 0x01;
      TextWriter textWriter(postingsWriter);
      textWriter.startField(&outputFieldInfo);

      // Collect TermsEnum for each segment.  Keep track of the index so we can visit in ascending order one at a time.
      struct TermsEnumIdx {
        TermsEnum tenum;
        size_t idx;
      };
      std::vector<TermsEnumIdx> tenums;  // TODO: a term enum may get pretty large... may not want contiguous if many segs?      std::vector<TermsEnum> tenums;
      tenums.reserve(sortedFields.size());
      std::vector<TermsEnumIdx*> tenumPtrs;
      tenumPtrs.reserve(sortedFields.size());

      for (size_t idx = 0; idx<sortedFields.size(); idx++) {
        auto field = sortedFields[idx];
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
        PackedTerm term(pool, std::string_view(first.tenum.term()));
        auto termOrd = textWriter.startTerm(term);

        do {
          TermsEnumIdx& entry = termPQ.top();
          // need to create the docsEnum while the termsEnum is still positioned on the term.
          DocsEnum docsEnum(pool, *sortedFields[entry.idx]->seg->postingsReader, entry.tenum);

          if (isOrdCol) {
            // this is a string column, so keep track of the ordinals for each doc
            addDocsOrds(textWriter, docsEnum, sortedFields[entry.idx]->seg->base, ordCollector.value(), termOrd);
          } else {
            // text field, so add docs with positions to the textWriter
            addDocsPos(textWriter, docsEnum, sortedFields[entry.idx]->seg->base);
          }

          addDocsPos(textWriter, docsEnum, sortedFields[entry.idx]->seg->base);

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

    } else { // int column that is not an ord column (assume all other field types have this (currently true)
      // nocommit outputFieldInfo.flags |= 0x02;
      IntColWriter intColWriter(pool, postingsWriter, outputFieldInfo);
      intColWriter.startField();


      // TODO: it will be most efficient to write all docids and then write all values rather than try to correlate
      // them.  EXCEPT in the case of a dense encoding that encodes them together (missingValue, etc)
      // Expose the raw values in that case???


      // if we need to write docs and values together, could have either docs or values push to us
      // and then in the acceptor, use an iterate over the other.  Pushing both should be
      // fastest though. TODO: implement screaming bitset pusher, column value pusher

      // TODO: write a batch of docids here after we gain the ability to do them incrementally!


      // currently all values must be written before all docs
      for (auto* field : sortedFields) {
        auto baseId = (int32_t) field->seg->base;
        unused(baseId);

        IntColReader reader(readerPool, *field->seg->postingsReader, field->segFieldInfo);
        IntColReader::BulkValues values(reader);


        // int32_t highest = field->seg->postingsReader->numDocs();
        for(;;) {
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
        for (auto *field: sortedFields) {
          auto baseId = (int32_t) field->seg->base;

          IntColReader reader(readerPool, *field->seg->postingsReader, field->segFieldInfo);
          IntColReader::Iterator colIter(reader);

          [[maybe_unused]] int32_t highest = field->seg->postingsReader->numDocs();
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


      intColWriter.finish();
      if (outputFieldInfo.flags & FieldType::MULTI_VALUED) {
        auto guard = pool.rewindScopeGuard();
        OutputStreamPtr out = postingsWriter.getOutputStream();
        MonoWriter endRankWriter(pool, *out);
        int64_t endRankBase = 0;

        for (auto* field : sortedFields) {
          assert(field->segFieldInfo.flags & FieldType::MULTI_VALUED);
          // open IntColReader for each segment
          IntColReader reader(readerPool, *field->seg->postingsReader, field->segFieldInfo);
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
  }


};




void IndexWriter::mergeSegments(MemPool &pool, std::span<PostingsReader *> preaders, PostingsWriter &postingsWriter) {
  unused(pool);
  SegmentMerger merger(preaders, postingsWriter);
  merger.merge();

}




} // end namespace solux