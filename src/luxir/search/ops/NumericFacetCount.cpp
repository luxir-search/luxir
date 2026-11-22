// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include "FacetOp.h"
#include "FieldFacetPlanning.h"

namespace luxir {
namespace {

// Count a segment from its term dictionary (index: match). Terms are the
// order-preserving encoding of the column values, so each decodes to its
// bucket key, and a term's postings hold each document with that value
// once, as the distinct column walk counts it. add(value, count) receives
// only finished positive counts: the accumulator is shared across
// segments.
//   all docs:   a term's count is its docFreq.
//   complement: docFreq minus the term's documents outside the domain,
//               read from the column. Deleted documents are in both.
//   term:       each term's postings intersected with the domain.
void LUXIR_NOINLINE countFromTerms(
    IntFacetReq& facet, IntFacetReq::MergeableIntFacet& data, int32_t segnum, DomainView& view,
    const DecodedFieldRecord& info, FieldFacetCountKind strategy, size_t denseSlots,
    ExecutionProfilePieceState* profile, auto&& add) {
  auto& postingsReader = facet.reader.segments()[(size_t)segnum].postingsReader();
  auto poolGuard = MemPool::threadLocalPoolGuard();
  auto& pool = poolGuard.pool();
  TermsEnum terms(pool, postingsReader, info);
  auto emitCounts = [&](auto&& countOf) {
    while (terms.nextTerm()) {
      int64_t value = decodeNumericTerm(std::string_view(terms.term()));
      int32_t count = countOf(value, terms.docFreq());
      assert(count >= 0);
      if (count > 0) add(value, (int64_t)count);
    }
  };
  auto detail = [&](std::string_view parentCount, std::string_view how) {
    if (profile == nullptr) return;
    profile->details.emplace_back(parentCount);
    profile->details.emplace_back(how);
  };

  if (view.compCard == 0) {
    detail("parent-count=column-complement", "all-docs domain, docFreq-only dictionary walk");
    emitCounts([](int64_t, int32_t docFreq) { return docFreq; });
    if (facet.missing) data.missing_num += (int64_t)view.maxDoc - info.docsWithField;
    return;
  }

  if (strategy == FieldFacetCountKind::TERM_DRIVEN) {
    detail("parent-count=term-driven", "per-term smallest-side postings intersection");
    view.materializeBits(pool);
    emitCounts([&](int64_t, int32_t docFreq) {
      DocsOnlyEnum postings(terms);
      return countTermInDomain(view, postings, docFreq);
    });
    if (facet.missing) {
      DocsReader docsReader(postingsReader, info);
      data.missing_num += countMissingInDomain(view, docsReader);
    }
    return;
  }

  assert(strategy == FieldFacetCountKind::COLUMN_COMPLEMENT);
  BitDocSet complement(complementBits(view, pool), view.compCard);
  int64_t compMissing = 0;
  FacetCountResource resource(facet.req.memoryTracker, facet.facetName, "numeric facet complement staging");
  if (denseSlots) {
    detail("parent-count=column-complement", "inverted domain view, dense value staging");
    std::pmr::vector<int32_t> staged(denseSlots, &resource);
    int64_t base = info.scalarBounds()->min;
    facet.facetSegIntDistinct(&complement, segnum, compMissing,
        [&](int32_t, int64_t value) LUXIR_INLINE { staged[(size_t)(value - base)]++; });
    emitCounts([&](int64_t value, int32_t docFreq) {
      int32_t outside = staged[(size_t)(value - base)];
      assert(outside <= docFreq);
      return docFreq - outside;
    });
  } else {
    detail("parent-count=column-complement", "inverted domain view, hash value staging");
    using Alloc = std::pmr::polymorphic_allocator<std::pair<const int64_t, int32_t>>;
    boost::unordered_flat_map<int64_t, int32_t, boost::hash<int64_t>, std::equal_to<int64_t>, Alloc>
        staged{&resource};
    facet.facetSegIntDistinct(&complement, segnum, compMissing,
        [&](int32_t, int64_t value) LUXIR_INLINE { staged[value]++; });
    emitCounts([&](int64_t value, int32_t docFreq) {
      auto it = staged.find(value);
      return it == staged.end() ? docFreq : docFreq - it->second;
    });
  }
  if (facet.missing) {
    int64_t inComplement = view.compCard - compMissing;
    data.missing_num += view.card - (info.docsWithField - inComplement);
  }
}

} // namespace

void IntFacetReq::Calc::countSegment(MergeableIntFacet& data, int32_t segnum, DocSet* domain,
                                     ExecutionProfilePieceState* profile) {
  auto& facet = thisOp();
  if (std::holds_alternative<std::monostate>(data.counts)) {
    if (facet.useVector) {
      int64_t range = facet.globalMax - facet.globalMin + 1;
      data.counts = MergeableIntFacet::CountVector(range, 0);
      data.minValue = facet.globalMin;
    } else {
      data.counts = MergeableIntFacet::IntHash();
    }
  }
  int32_t maxDoc = facet.reader.segments()[(size_t)segnum].maxDoc();
  if (profile != nullptr) {
    profile->wire.max_doc = maxDoc;
    profile->wire.domain_size = domain ? domain->card() : maxDoc;
    profile->wire.strategy =
        std::holds_alternative<MergeableIntFacet::CountVector>(data.counts) ? "vector" : "hash";
  }
  const auto* info = facet.numberFieldInfo(segnum);
  const auto* terms = info ? info->terms() : nullptr;
  if (terms) {
    const auto& totals = info->indexedTotals();
    DomainView view(domain, maxDoc);
    const auto& bounds = *info->scalarBounds();
    uint64_t span = (uint64_t)bounds.max - (uint64_t)bounds.min;
    size_t denseSlots = span < (uint64_t)std::max<int64_t>(4 * totals.numTerms, 4096)
        ? (size_t)span + 1 : 0;
    double pairsPerDoc = (double)totals.sumDocFreq / maxDoc;
    // Forced 2M-doc grid: 4 puts postings ahead of hash updates at 30% and
    // 70% on the 1K-value field, and complement ahead at 90% and 99%.
    constexpr double HASH_VALUE_COST = 4;
    // All-docs walk / column, normalized per term: ~6 on both dense 64K
    // values (vector output) and 2M distinct values (hash output).
    constexpr double WALK_TERM_COST = 6;
    double stageCost = denseSlots ? 1.0 : HASH_VALUE_COST;
    auto plan = FieldFacetCountPlan::select({
        .maxDoc = maxDoc,
        .domainCardinality = view.card,
        .complementCardinality = view.compCard,
        .segmentTerms = totals.numTerms,
        .sumDocFreq = totals.sumDocFreq,
        .docsWithField = info->docsWithField,
        .domainHasBitset = view.bits != nullptr,
        .missingRequested = facet.missing,
        .topTermsLegal = false,
        .columnWork = view.card * pairsPerDoc * (facet.useVector ? 1.0 : HASH_VALUE_COST),
        .walkTermCost = WALK_TERM_COST,
        .complementTermCost = WALK_TERM_COST + stageCost,
        .complementValueWork = view.compCard * pairsPerDoc * stageCost,
        // Words inverted in the domain and zeroed in the int32 staging array.
        .complementSetupWork = (double)FixedBitSet::sizeInWords(maxDoc) + (double)denseSlots / 2
    }, forcedFieldFacetStrategy);
    if (profile != nullptr) {
      profile->wire.cardinality = totals.numTerms;
      if (!plan.forcedFallback.empty()) profile->details.emplace_back(plan.forcedFallback);
    }
    if (plan.strategy != FieldFacetCountKind::COLUMN_DOMAIN) {
      if (auto* counts = std::get_if<MergeableIntFacet::CountVector>(&data.counts)) {
        int64_t minValue = data.minValue;
        countFromTerms(facet, data, segnum, view, *info, plan.strategy, denseSlots, profile,
            [&](int64_t value, int64_t count) { (*counts)[value - minValue] += count; });
      } else {
        auto& hash = std::get<MergeableIntFacet::IntHash>(data.counts);
        countFromTerms(facet, data, segnum, view, *info, plan.strategy, denseSlots, profile,
            [&](int64_t value, int64_t count) { hash[value] += count; });
      }
      return;
    }
  }
  if (profile != nullptr) {
    profile->details.emplace_back("parent-count=column-domain");
    profile->details.emplace_back(domainDesc(domain));
  }
  if (auto* countVec = std::get_if<MergeableIntFacet::CountVector>(&data.counts)) {
    int64_t minVal = data.minValue;
    facet.facetSegIntDistinct(domain, segnum, data.missing_num,
      [countVec, minVal](int32_t docid, int64_t val) LUXIR_INLINE {
        unused(docid);
        (*countVec)[val - minVal]++;
      });
  } else {
    auto* countMap = &std::get<MergeableIntFacet::IntHash>(data.counts);
    facet.facetSegIntDistinct(domain, segnum, data.missing_num,
      [countMap](int32_t docid, int64_t val) LUXIR_INLINE {
        unused(docid);
        (*countMap)[val]++;
      });
  }
}

} // namespace luxir
