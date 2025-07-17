#pragma once
#include "OrdMap.h"
#include "solux/index/IntColWriter.h"
#include "solux/reader/TermsEnum.h"
#include "solux/util/heap.h"

namespace solux {

//
// This is private, only used by OrdMap::build().  This header is only included in one .cpp file.
//
// Format (designed for index-time on-disk as well as on-demand in RAM):
// [seg1-deltas][seg2-deltas]...[segN-deltas]
// [firstSegs][globDeltas]
// [numGlobalOrds (vLong)]
// [numSegs (vLong)]
// [seg1-meta][seg2-meta]...[segN-meta]
// [firstSegs-meta][globDeltas-meta]
// [meta-size (int32_t)]  // 4 byte size of metadata, starting at [numSegs], not including this size field.
//
// seg1-deltas contains deltas to map segment ords into global ord space.
// Each seg encoded as separate integer column using monotonic compression (MonoWriter).
//
// seg1-meta contains MonoWriter metadata (offset, metaOffset, nValues)
//   nValues (vLong) = number of values in this column.  if nValues == 0 or numGlobalOrds, no other metadata is present.
//   offset (vLong) = offset from start of OrdMap (not absolute like the location is in SegFieldInfo)
//   metaOffset (vLong) = offset from the start of *this* column's start.
//
// There is a single firstSegs column that contains the first segment number that the globa ord appeared in.
// There is a single globDeltas column that contains the delta that was used to map from that segment ord to the global ord.
// Neither of these global columns are monotonic, so they are encoded with IntColWriter.
//   firstSegs-meta and globalDeltas-meta are similar to the per-segment meta.
//
// The metadata that the reader needs is simply the start and end locations of the entire structure.
//

class OrdMapBuilder {
  std::string_view field;
  CoreIndex& reader;

  // An alternate encoding could just catenate all of the deltas together in one numeric column (non-monotonic)
  // that would have less overhead for small segments.
  struct MonoDeltas {
    RAMFile file;
    OutputStream out;
    MonoWriter writer;
    explicit MonoDeltas(MemPool& pool) : file("tmp"), out(&file), writer(pool, out) {}
  };

  // Collect TermsEnum for each segment.  Keep track of the index so we can visit in ascending order one at a time.
  struct TermsEnumIdx {
    size_t idx;
    TermsEnum tenum;
    MonoDeltas deltas;

    TermsEnumIdx(size_t idx, MemPool& pool, PostingsReader& postingsReader, SegFieldInfo& finfo) : idx(idx), tenum(pool, postingsReader, finfo), deltas(pool) {}

    void addDelta(int64_t delta) {
      assert(delta >= 0);
      deltas.writer.addInt64(delta);
    }
  };

public:
  OrdMapBuilder(std::string_view field, CoreIndex& reader) : field(field), reader(reader) {}

  // Build fills these in currently.  In the future, the output may be written to disk.
  std::unique_ptr<char[]> data;
  int64_t start = 0;
  int64_t size = 0;

  void build() {
    auto poolGuard = MemPool::threadLocalPoolGuard();
    auto& pool = poolGuard.pool();

    const auto& segs = reader.segments();
    auto nsegs = segs.size();

    // the vector isn't in the pool, but the TermsEnumIdx instances will be.
    // This will "own" the TermsEnumIdx instances and call destructors.
    std::vector<u_ptr<TermsEnumIdx>> allTermsEnums;
    allTermsEnums.reserve(nsegs);

    int segsWithValue = 0;
    for (auto i=0u; i<nsegs; i++) {
      auto& seg = segs[i];
      // Allocate FieldReader in pool so it has same lifetime as SegFieldInfo
      auto* fieldReader = pool.make<FieldReader>(pool, seg.postingsReader());
      if (!fieldReader->seek(field)) {
        // Field not found in this segment
        allTermsEnums.emplace_back(nullptr);
        continue;
      }
      auto* fieldInfo = pool.make<SegFieldInfo>();  // Allocate in pool to avoid stack-use-after-scope
      fieldReader->readFieldInfo(*fieldInfo);
      //       TermsEnumIdx(size_t idx, MemPool& pool, PostingsReader& postingsReader, SegFieldInfo& finfo) : idx(idx), tenum(pool, postingsReader, finfo), deltas(pool) {}


      allTermsEnums.emplace_back(pool.make_unique<TermsEnumIdx>(i, pool, seg.postingsReader(), *fieldInfo));
      // position the TermsEnum on the first term
      if (!allTermsEnums.back()->tenum.nextTerm()) {
        // defensive coding, every field should have at least one term.
        allTermsEnums.back().reset();
        continue;
      }
      segsWithValue++;
    }

    if (segsWithValue == 0) {
      return;
    }
    
    // Fast path: if only one segment has values, no OrdMap is needed (identity mapping)
    if (segsWithValue == 1) {
      return;
    }

    // TODO: fast path when all segments are full or empty? (i.e. fast path check for full)

    // Fill in a compressed version (no nulls) of the TermsEnumIdx pointers for the priority queue.
    auto tenums = pool.make_span<TermsEnumIdx*>(segsWithValue);
    size_t tenumIdx = 0;
    for (auto& tenum : allTermsEnums) {
      if (tenum) {
        tenums[tenumIdx++] = tenum.get();
      }
    }

    auto termCmp = [](const TermsEnumIdx& a, const TermsEnumIdx& b) -> bool {
      // For a min-heap (smallest term first), we need: return true when a > b
      // This makes std::make_heap put the smallest element at the top
      auto cmp = a.tenum.term() <=> b.tenum.term();
      if (cmp != 0) {
        return cmp > 0; // return true when a > b (for min-heap)
      }

      // tiebreak by number of terms in the segment (largest first) so that "firstSegment" will normally
      // be the same segment for good locality during lookup.
      auto cmp2 = a.tenum.numTerms() - b.tenum.numTerms();
      if (cmp2 != 0) {
        return cmp2 < 0; // For min-heap: return true when a should come before b in heap order
      }

      // tiebreak by index last to prefer first segments over later segments.
      return a.idx > b.idx; // For min-heap: return true when a should come before b in heap order
    };
    IndirectPQ<TermsEnumIdx, decltype(termCmp)> termPQ(tenums);

    char packedTermData[PackedTerm::MAX_BYTES];
    PackedTerm packedTerm(packedTermData,0);  // the current term while iterating over all terms

    RAMFile firstSegsFile("firstSegs");
    OutputStream firstSegsOut(&firstSegsFile);
    IntColWriter firstSegs(firstSegsOut);

    RAMFile globDeltasFile("globDeltas");
    OutputStream globDeltasOut(&globDeltasFile);
    IntColWriter globDeltas(globDeltasOut);

    // iterate through the terms in sorted order
    int64_t globalOrd = -1;
    while (termPQ.size() > 0) {
      globalOrd++;
      TermsEnumIdx& first = termPQ.top();
      
      // There is a tradeoff here.  We need to find other TermsEnumIdx that are positioned on the same term.
      // We could: 1) make a copy of the term and then advance it with updateTop()
      //           2) remove to from the PQ, then after processing all matching segments, advance and reinsert it.
      // Option 1's expense is making a copy of the term, option 2's expense is an extra heap operation.

      // Option 1: make a copy of the term since it will be invalidated after tenum.nextTerm()
      first.tenum.term().copyTo(packedTerm);

      // calculate the mapping back from global ord to the segment ord space for term lookup
      auto firstSeg = first.idx;
      auto firstDelta = globalOrd - first.tenum.ord();
      firstSegs.addInt64(firstSeg);
      globDeltas.addInt64(firstDelta);

      do {
        TermsEnumIdx& entry = termPQ.top();
        auto delta = globalOrd - entry.tenum.ord();
        entry.addDelta(delta);

        // advance this entry to the next term, removing from pq if exhausted.
        if (entry.tenum.nextTerm()) {
          termPQ.updateTop();
        } else {
          termPQ.removeTop();
        }
        // continue while more enums are positioned on the same term
        // FUTURE OPT: we could special case when we are down to a single segment.  All deltas
        //             will be the same from then on.
      } while (termPQ.size() > 0 && termPQ.top().tenum.term() == packedTerm);
    }

    auto firstSegsInfo = firstSegs.finish();
    firstSegsOut.flush(true);
    auto globDeltasInfo = globDeltas.finish();
    globDeltasOut.flush(true);

    RAMFile metaOutFile("metaOut");
    OutputStream metaOut(&metaOutFile);
    metaOut.writeVlong(globalOrd + 1);  // numGlobalOrds
    metaOut.writeVlong(allTermsEnums.size());  // numSegs

    bool needGlobalDeltas = true;
    int64_t ordMapStart = 0;  // currently just one ord map per file/buffer, and no header.
    RAMFile* outFile = nullptr;
    uint64_t cumulativeSize = 0;

    // find the first non-empty and non-full segment and use its RAMFile as the output.
    for (auto& tenum : allTermsEnums) {
      bool writeDeltas = tenum && tenum->tenum.numTerms() < (globalOrd + 1);
      auto nTerms = tenum ? tenum->tenum.numTerms() : 0;
      if (outFile == nullptr && writeDeltas) {
        outFile = &tenum->deltas.file;
      }
      if (writeDeltas) {
        auto numValues = tenum->deltas.writer.finish();
        auto thisSize = tenum->deltas.out.size();
        tenum->deltas.out.flush(true);
        assert(thisSize == tenum->deltas.file.size());
        auto [filenum, loc] = tenum->deltas.writer.blockLoc.decode();
        auto metaOff = tenum->deltas.writer.metaOff;
        auto adjustedLoc = cumulativeSize + loc;

        if (outFile == nullptr) {
          outFile = &tenum->deltas.file;
        } else {
          outFile->destructiveAppend(tenum->deltas.file);
        }
        cumulativeSize += thisSize;
        assert(cumulativeSize == outFile->size());

        metaOut.writeVlong(numValues);
        metaOut.writeVlong(adjustedLoc);
        metaOut.writeVlong(metaOff);
      } else {
        // if empty or full, just write the number of terms.
        metaOut.writeVlong(nTerms);
      }
    }

    if (outFile == nullptr) {
      // we don't need to write any deltas for the segments, and that means we don't need
      // the firstSegs/globDeltas columns either.
      outFile = &metaOutFile;
      needGlobalDeltas = false;
    }

    if (needGlobalDeltas) {
      // redundant numValues for the global columns, but it makes reading simpler.
      assert(firstSegsInfo.numValues == globDeltasInfo.numValues);

      metaOut.writeVlong(firstSegsInfo.numValues);
      metaOut.writeVlong(firstSegsInfo.columnLoc + cumulativeSize);
      metaOut.writeVlong(firstSegsInfo.columnMetaOff);
      cumulativeSize += firstSegsFile.size();
      outFile->destructiveAppend(firstSegsFile);
      assert(outFile->size() == cumulativeSize);


      metaOut.writeVlong(globDeltasInfo.numValues);
      metaOut.writeVlong(globDeltasInfo.columnLoc + cumulativeSize);
      metaOut.writeVlong(globDeltasInfo.columnMetaOff);
      cumulativeSize += globDeltasFile.size();
      outFile->destructiveAppend(globDeltasFile);  // globDeltasFile is what we were appending metadata to.
      assert(outFile->size() == cumulativeSize);
    }

    // finally write the size of the metadata, then we can flush and add to outFile.
    auto metaSize = metaOut.size();
    cumulativeSize += metaSize;
    metaOut.writeInt((int32_t)metaSize);
    metaOut.flush(true);
    cumulativeSize += sizeof(int32_t);
    if (outFile != &metaOutFile) {
      outFile->destructiveAppend(metaOutFile);
    }

    assert(outFile->size() == cumulativeSize);

    size = outFile->size();
    start = ordMapStart;
    data.reset(new char[size]);
    outFile->copyTo(data.get());
  }
};


// In the future, we prob want to be able to accept a span of postings readers as well
// so that IndexWriter could pre-create a OrdMap for a field without constructing an IndexReader.
// Or we could just make IndexReader easier to construct w/o taking a Directory, etc.
std::shared_ptr<OrdMap> OrdMap::build(std::string_view field, CoreIndex& reader) {
  OrdMapBuilder builder(field, reader);
  builder.build();
  if (!builder.data) {
    return {};
  }
  return std::make_shared<OrdMap>(std::move(builder.data), builder.start, builder.size);
}


}
