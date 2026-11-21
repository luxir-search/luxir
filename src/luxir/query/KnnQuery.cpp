// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include "luxir/query/KnnQuery.h"
#include "luxir/query/KnnTuning.h"

namespace luxir {

KnnQuery::Tuning KnnQuery::tuning;

void setKnnScanWindows(int64_t windowBytes, int64_t aheadBytes) {
  KnnQuery::tuning.scanWindowBytes = windowBytes;
  KnnQuery::tuning.scanAheadBytes = aheadBytes;
}

Query::Weight* KnnQuery::createWeight(Query::Context& context, int32_t flags,
                                      float multiplier) {
  return context.pool.make<KnnQuery::Weight>(context, *this, flags, multiplier);
}

} // namespace luxir
