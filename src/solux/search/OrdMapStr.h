#pragma once
#include "OrdMap.h"
#include "solux/reader/TermsEnum.h"
#include "solux/util/MemPool.h"

namespace solux {

class OrdMapStr {
public:
  MemPool& pool;
  OrdMap* ordMap;
  IndexReader& index;
  std::string_view fieldName;
  std::span<TermsEnum*> enums;
  IntColReader::SparseValues* deltas = nullptr;
  IntColReader::SparseValues* firstSegs = nullptr;

  OrdMapStr(MemPool& pool, OrdMap* ordMap, IndexReader& index, std::string_view fieldName) : pool(pool), ordMap(ordMap), index(index), fieldName(fieldName) {
    // Lazy per-segment cache: null means not yet opened.
    enums = pool.make_span_zeroed<TermsEnum*>(index.segments().size());
    if (ordMap && ordMap->getGlobDeltas()) {
      deltas = pool.make<IntColReader::SparseValues>(*ordMap->getGlobDeltas());
      firstSegs = pool.make<IntColReader::SparseValues>(*ordMap->getFirstSegs());
    }
  }

  TermsEnum* getTermsEnum(int32_t segnum) {
    assert(segnum >= 0 && segnum < (int32_t)enums.size());
    auto* tenum = enums[segnum];
    if (!tenum) {
      PostingsReader& postingsReader = index.segments()[segnum].postingsReader();
      FieldReader fieldReader(postingsReader);
      SegFieldInfo* segFieldInfo = pool.make<SegFieldInfo>();
      bool found = fieldReader.seek(fieldName);
      assert(found);
      fieldReader.readFieldInfo(*segFieldInfo);
      // Create a new TermsEnum in the pool for this segment
      tenum = pool.make<TermsEnum>(pool, postingsReader, *segFieldInfo);
      enums[segnum] = tenum;
    }
    return tenum;
  }

  //the return string view is only valid until the next call
  std::string_view ordToStr(int64_t ord) {
    auto segOrd = ord;
    TermsEnum* tenum = nullptr;

    if (firstSegs) {
      auto firstSeg = firstSegs->valueAt(ord);
      tenum = getTermsEnum(firstSeg);
      auto delta = deltas->valueAt(ord);
      segOrd -= delta;  // adjust ord to segment ord
    } else {
      // we can just use the terms enum for the first segment that has all ords
      auto firstFullSeg = ordMap->firstFullSeg();
      assert(firstFullSeg != -1);
      tenum = getTermsEnum(firstFullSeg);
    }

    tenum->seekOrd(segOrd);
    return (std::string_view) tenum->term();
  }
};
}
