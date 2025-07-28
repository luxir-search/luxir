#pragma once
#include "OrdMap.h"
#include "solux/reader/TermsEnum.h"
#include "solux/util/MemPool.h"

namespace solux {

class OrdMapStr {
public:
  MemPool& pool;
  OrdMap& ordMap;
  CoreIndex& index;
  std::string_view fieldName;
  std::span<TermsEnum*> enums;
  IntColReader::DenseValues* deltas = nullptr;

  OrdMapStr(MemPool& pool, OrdMap& ordMap, CoreIndex& index, std::string_view fieldName) : pool(pool), ordMap(ordMap), index(index), fieldName(fieldName) {
    enums = pool.make_span<TermsEnum*>(index.segments().size());
    if (ordMap.getGlobDeltas()) {
      deltas = pool.make<IntColReader::DenseValues>(pool, *ordMap.getGlobDeltas());
    }
  }

  TermsEnum* getTermsEnum(int32_t segnum) {
    assert(segnum >= 0 && segnum < (int32_t)enums.size());
    auto* tenum = enums[segnum];
    if (!tenum) {
      PostingsReader& postingsReader = index.segments()[segnum].postingsReader();
      FieldReader fieldReader(pool, postingsReader);
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

  std::string_view ordToStr(int64_t ord) {
    auto* firstSegs = ordMap.getFirstSegs();
    auto segOrd = ord;
    auto* tenum = nullptr;

    if (firstSegs) {
      auto firstSeg = firstSegs->get(ord);
      tenum = getTermsEnum(firstSeg);
      auto delta = deltas->valueAt(ord);
      segOrd -= delta;  // adjust ord to segment ord
    } else {
      // we can just use the terms enum for the first segment that has all ords
      auto firstFullSeg = ordMap.firstFullSeg();
      assert(firstFullSeg != -1);
      tenum = getTermsEnum(firstFullSeg);
    }

    tenum->seekOrd(segOrd);
    return tenum->term();
  }
};
}
