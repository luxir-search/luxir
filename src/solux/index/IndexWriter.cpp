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

  MemPool& origPool;
  std::span<PostingsReader *> preaders;
  PostingsWriter& postingsWriter;

  std::vector<FieldReader> fieldReaders;  // todo - pool allocate (& use smart ptr on MergeSeg if destructors needed)
  std::vector<Segment> segs;

public:


  SegmentMerger(MemPool& pool, std::span<PostingsReader *> preaders, PostingsWriter& postingsWriter)
  : origPool(pool), preaders(preaders), postingsWriter(postingsWriter) {

  }

  void merge() {
    segs.reserve(preaders.size());
    fieldReaders.reserve(preaders.size());  // This is important since we take pointers to these! Pool allocate later...

    int64_t base = 0;
    for (auto preader : preaders) {
      fieldReaders.emplace_back(origPool, *preader);
      FieldReader& fieldReader = fieldReaders.back();

      // position fieldReader on first field and add to segs if it's non-empty
      if (fieldReader.readNextField()) {
        segs.emplace_back(preader, &fieldReader, base, (int)segs.size());
        base += preader->maxDoc();
      } else {
        LOG_ERROR("Empty fieldReader!");
      }
    }
    // after the loop, base will be equal to nDocs
    postingsWriter.setMaxDoc(base);

    auto fnameComp = [](const Segment& a, const Segment& b){ return b.fieldReader->name() < a.fieldReader->name(); };
    std::vector<Segment*> segPtrs(segs.size());
    IndirectPQ<Segment, decltype(fnameComp)> fieldPQ(segs, segPtrs, false);

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

  void mergeField(std::vector<MergeFieldInfo>& mergeFieldInfos) {
    MemPool& writerPool = origPool;  // needs to be different when we move to multi-threaded
    MemPool::ScopeGuard guard(writerPool);

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
    for (auto* field : sortedFields) {
      allFlags |= field->segFieldInfo.flags;
    }

    // get/reserve a new fieldInfo from the postingsReader
    PostingsWriter::IndexFieldInfo& outputFieldInfo = postingsWriter.fieldInfos.emplace_back();
    outputFieldInfo.fieldname = sortedFields[0]->segFieldInfo.fieldname;  // we should ensure out postingsWriter outlives the lifetime of the postings readers!
    outputFieldInfo.flags = 0;


    MemPool readerPool; // TODO: can this be the same as writerPool?
    if (allFlags & 0x02) { // int column
      outputFieldInfo.flags |= 0x02;
      IntColWriter intColWriter(writerPool, postingsWriter, outputFieldInfo);
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

        IntColReader reader(readerPool, *field->seg->postingsReader, field->segFieldInfo);
        IntColReader::Iterator colIter(reader);


        // int32_t highest = field->seg->postingsReader->numDocs();
        for(;;) {
          int32_t localId = colIter.next();
          if (localId == IntColReader::END) {
            break;
          }
          int64_t val = colIter.value();
          intColWriter.addInt64(val);
        }
      }

      intColWriter.startDocsWithValue();
      for (auto* field : sortedFields) {
        auto baseId = (int32_t) field->seg->base;

        IntColReader reader(readerPool, *field->seg->postingsReader, field->segFieldInfo);
        IntColReader::Iterator colIter(reader);

        // int32_t highest = field->seg->postingsReader->numDocs();
        for(;;) {
          int32_t localId = colIter.next();
          if (localId == IntColReader::END) {
            break;
          }
          intColWriter.startDoc(baseId + localId);
        }
      }
      intColWriter.endDocsWithValue();


      intColWriter.endField();
    } else {
      LOG_ERROR("Unknown Type!");
    }

  }


};




void IndexWriter::mergeSegments(MemPool &pool, std::span<PostingsReader *> preaders, PostingsWriter &postingsWriter) {
  SegmentMerger merger(pool, preaders, postingsWriter);
  merger.merge();

}




} // end namespace solux