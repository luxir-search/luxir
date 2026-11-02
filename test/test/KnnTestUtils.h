// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "luxir/query/KnnQuery.h"
#include "TestUtils.h"

namespace luxir::test {
// Routing is independent of the tiny-corpus IVF build policy.
class ForceIvfGuard {
  SearchOverridesGuard<KnnQuery::Routing, KnnQuery::Completion> saved{KnnQuery::routing, KnnQuery::completion};
public:
  ForceIvfGuard() {
    KnnQuery::routing = KnnQuery::Routing::FORCE_IVF;
    KnnQuery::completion = KnnQuery::Completion::OFF; // Mechanics/calibration must stay on IVF.
  }
};
}
