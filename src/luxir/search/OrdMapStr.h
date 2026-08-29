#pragma once
#include <optional>

#include "OrdMap.h"
#include "luxir/reader/TermsEnum.h"
#include "luxir/util/MemPool.h"

namespace luxir {

class OrdMapStr {
public:
  struct TermStats {
    std::optional<int64_t> globalOrd;
    int64_t docFreq = 0;
  };

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

  // Resolve an exact term without requiring every requested key to have a
  // global ordinal. A full segment is sufficient when present; otherwise the
  // first segment containing the term supplies the stable global mapping.
  std::optional<int64_t> strToOrd(std::string_view term) {
    if (ordMap == nullptr) return std::nullopt;
    int firstFull = ordMap->firstFullSeg();
    if (firstFull >= 0) {
      TermsEnum* tenum = getTermsEnum(firstFull);
      return tenum->seek(term) ? std::optional<int64_t>(tenum->ord())
                               : std::nullopt;
    }
    for (int32_t segnum = 0; segnum < (int32_t)enums.size(); segnum++) {
      auto mapping = ordMap->getSegToGlobal(segnum);
      if (mapping.numOrds == 0) continue;
      TermsEnum* tenum = getTermsEnum(segnum);
      if (tenum->seek(term)) return mapping.globalOrd(tenum->ord());
    }
    return std::nullopt;
  }

  // Resolve a global ordinal and sum its exact segment docFreqs in one pass.
  // This is the point-stat counterpart to strToOrd: callers that need both
  // should not resolve the term and then walk the dictionaries again.
  TermStats termStats(std::string_view term) {
    TermStats result;
    if (ordMap == nullptr) return result;
    for (int32_t segnum = 0; segnum < (int32_t)enums.size(); segnum++) {
      auto mapping = ordMap->getSegToGlobal(segnum);
      if (mapping.numOrds == 0) continue;
      TermsEnum* tenum = getTermsEnum(segnum);
      if (!tenum->seek(term)) continue;
      int64_t globalOrd = mapping.globalOrd(tenum->ord());
      assert(!result.globalOrd.has_value()
             || *result.globalOrd == globalOrd);
      result.globalOrd = globalOrd;
      result.docFreq += tenum->docFreq();
    }
    return result;
  }
};
}
