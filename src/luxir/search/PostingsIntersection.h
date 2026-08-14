#pragma once

#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <memory>
#include <span>

#include "luxir/reader/DocsEnum.h"
#include "luxir/search/DocSet.h"

namespace luxir {

// Advancing once per array doc wins only when consecutive domain docs are
// expected to jump over whole postings blocks. Otherwise decoding each
// postings block once and intersecting its doc span is the cheaper merge.
inline bool shouldDrivePostingsFromArray(int32_t docFreq,
                                         int32_t domainCard) {
  return domainCard > 0
      && (int64_t) docFreq
          > (int64_t) domainCard * Postings::DOCS_BLOCK_SIZE;
}

// Materialize the intersection of one term's postings and a segment domain.
// The postings cursor is consumed. A null domain means all segment docs.
//
// Route by representation and relative cardinality:
//  * a very sparse array drives postings.advance(), skipping whole blocks;
//  * other arrays merge against decoded DocsEnum spans through DocSetProbe;
//  * bitset domains and dense unfiltered results use L1-sized bit windows,
//    preserving the postings codec's direct word/contiguous-block fill paths;
//  * small unfiltered results use spans, avoiding a bit-window round trip.
inline std::unique_ptr<DocSet> materializePostingsIntersection(
    DocsOnlyEnum& postings, int32_t docFreq, DocSet* domain, int32_t maxDoc) {
  assert(maxDoc >= 0);
  assert(docFreq >= 0 && docFreq <= maxDoc);
  assert(postings.docId() < 0);
  DocSetBuilder builder(maxDoc);

  int32_t cachedDomainCard = domain == nullptr ? maxDoc : domain->cachedCard();
  assert(cachedDomainCard == -1
         || (cachedDomainCard >= 0 && cachedDomainCard <= maxDoc));
  if (docFreq == 0 || cachedDomainCard == 0) {
    return builder.build();
  }

  if (domain != nullptr && domain->type == DocSet::ARRAY
      && shouldDrivePostingsFromArray(docFreq, domain->card())) {
    std::span<const int32_t> docs = ((ArrDocSet*) domain)->docs();
    const int32_t* p = docs.data();
    const int32_t* end = p + docs.size();
    while (p != end) {
      int32_t target = *p;
      if (postings.docId() < target
          && postings.advance(target) == DocsEnumMeta::END) {
        break;
      }
      int32_t landing = postings.docId();
      if (landing == target) {
        builder.add(target);
        p++;
      } else {
        assert(landing > target);
        p = screaming::gallopLowerBound(p + 1, end, landing);
      }
    }
    return builder.build();
  }

  bool bitWindowRoute = domain != nullptr
      ? domain->type == DocSet::BITSET
      : docFreq > builder.arrayLimit();
  if (bitWindowRoute) {
    static constexpr int32_t kWindowSize = DocsEnumMeta::L1_DOCS;
    static constexpr int32_t kWindowWords = kWindowSize / 64;
    static_assert((kWindowSize % 64) == 0);
    std::array<uint64_t, kWindowWords> windowWords{};
    const FixedBitSet* domainBits = domain == nullptr
        ? nullptr : &((BitDocSet*) domain)->bits();
    assert(domainBits == nullptr || domainBits->size() == maxDoc);

    int32_t firstDoc = postings.advance(0);
    if (firstDoc == DocsEnumMeta::END) {
      return builder.build();
    }
    int32_t windowStart = (firstDoc / kWindowSize) * kWindowSize;
    while (windowStart < maxDoc) {
      int32_t windowEnd = (int32_t) std::min<int64_t>(
          (int64_t) windowStart + kWindowSize, maxDoc);
      windowWords.fill(0);
      postings.intoBitSet(windowWords, windowStart, windowEnd);
      if (domainBits != nullptr) {
        intersectBitSetWindow(windowWords, windowStart, windowEnd, *domainBits);
      }
      int32_t card = 0;
      for (uint64_t word : windowWords) {
        card += (int32_t) std::popcount(word);
      }
      if (card != 0) {
        builder.addWindowWords(
            windowWords.data(), windowStart, windowEnd, card);
      }

      int32_t next = postings.docId();
      if (next == DocsEnumMeta::END) {
        break;
      }
      if (next < windowEnd) {
        next = postings.advance(windowEnd);
        if (next == DocsEnumMeta::END) {
          break;
        }
      }
      windowStart = (next / kWindowSize) * kWindowSize;
      assert(windowStart >= windowEnd);
    }
    return builder.build();
  }

  DocSetProbe probe(domain);
  for (;;) {
    std::span<const int32_t> docs = postings.peekDocBlock();
    if (docs.empty()) {
      break;
    }
    probe.intersect(docs,
        [&](int32_t doc, int32_t) LUXIR_INLINE { builder.add(doc); });
    postings.consumeDocBlock((int32_t) docs.size());
  }
  return builder.build();
}

}  // namespace luxir
