// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include "luxir/query/KnnQuery.h"

namespace luxir {

Query::Weight* KnnQuery::createWeight(Query::Context& context, int32_t flags,
                                      float multiplier) {
  return context.pool.make<KnnQuery::Weight>(context, *this, flags, multiplier);
}

} // namespace luxir
