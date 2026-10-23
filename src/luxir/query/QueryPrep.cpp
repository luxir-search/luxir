// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include "luxir/query/QueryPrep.h"

namespace luxir::QueryPrep {

DomainHandle materializeEffectiveFilter(const PreparedSource& source, IndexReader& reader,
                                        IndexReader::Segment& segment, DocSet* domain,
                                        FilterCache::Probe probe) {
  auto& weight = *source.weight;
  auto* prepared = source.prepared.get();
  auto* use = source.cacheUse;
  if (source.domainDependence != PreparedDomainDependence::QUERY_CANONICAL && use != nullptr &&
      use->scope() != FilterKeyScope::READER_STABLE) {
    use = nullptr;
  }
  if (use == nullptr) {
    return DomainHandle(materialize(weight, prepared, segment, domain));
  }

  if (use->scope() == FilterKeyScope::READER_STABLE) {
    if (auto* cached = dynamic_cast<ReaderStablePreparedWeight*>(prepared)) {
      [[maybe_unused]] DocSet* canonical =
          !segment.hasDeletes() ? nullptr : &segment.liveDocs()->docset();
      assert(domain == nullptr || domain == canonical);
      return DomainHandle::borrowed(cached->docSet(segment));
    }
    return DomainHandle(materialize(weight, prepared, segment, domain));
  }

  if (probe.kind() == FilterCache::Probe::Kind::HIT) {
    return use->effectiveDomain((size_t)segment.ord, reader, domain);
  } else if (probe.kind() == FilterCache::Probe::Kind::BUILD) {
    auto buildStart = std::chrono::steady_clock::now();
    auto raw = materializeRawFilter(weight, prepared, segment);
    uint32_t buildCostMicros = elapsedBuildMicros(buildStart);
    use->publishRawByproduct((size_t)segment.ord, probe, std::move(raw), buildCostMicros);
  } else {
    // A non-folded raw materialization is free by-product population. A
    // domain-composed set must never enter the raw cache.
    if (domain == nullptr && use->wasAdmitted() && !weight.needsScores() && !weight.allowsPruning()) {
      auto buildStart = std::chrono::steady_clock::now();
      auto raw = materializeRawFilter(weight, prepared, segment);
      uint32_t buildCostMicros = elapsedBuildMicros(buildStart);
      use->offerRaw((size_t)segment.ord, std::move(raw), buildCostMicros);
      DocSet* docs = use->effectiveDocSet((size_t)segment.ord, reader);
      return DomainHandle::borrowed(docs);
    }
    if (!disableFilterOwnForTests && !weight.needsScores() && !weight.allowsPruning()) {
      auto raw = materializeRawFilter(weight, prepared, segment);
      use->adoptOwnedRaw((size_t)segment.ord, probe, std::move(raw));
      return use->effectiveDomain((size_t)segment.ord, reader, domain);
    }
    return DomainHandle(materialize(weight, prepared, segment, domain));
  }

  return use->effectiveDomain((size_t)segment.ord, reader, domain);
}

}  // namespace luxir::QueryPrep
