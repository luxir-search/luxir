// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include "luxir/search/PostingsIntersection.h"

namespace luxir {

std::unique_ptr<DocSet> materializeIdentityPostingsIntersection(DocsOnlyEnum& postings, int32_t docFreq,
                                                                DocSet* domain, int32_t maxDoc) {
  if (!domain || domain->type != DocSet::BITSET || !shouldDrivePostingsFromArray(docFreq, domain->card())) {
    return materializePostingsIntersection(postings, docFreq, domain, maxDoc);
  }
  DocSetBuilder builder(maxDoc);
  const auto& bits = ((BitDocSet*)domain)->bits();
  int32_t doc = bits.nextSetBit(0);
  while (doc < maxDoc) {
    // advance() is strict: a previous landing may already sit on this doc.
    int32_t landing = postings.docId() < doc ? postings.advance(doc) : postings.docId();
    if (landing == DocsEnumMeta::END) break;
    if (landing == doc) builder.add(doc);
    int32_t next = std::max(doc + 1, landing);
    if (next >= maxDoc) break;
    doc = bits.nextSetBit(next);
  }
  return builder.build();
}

}  // namespace luxir
