// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "luxir/query/BooleanQuery.h"

namespace luxir::test {

class ConjunctionPlanControl {
  const BooleanQuery::ConjunctionControlForTests* previous;

public:
  explicit ConjunctionPlanControl(const BooleanQuery::ConjunctionControlForTests& control)
      : previous(BooleanQuery::conjunctionControlForTests) {
    using Consumption = Query::MembershipConsumption;
    for (auto consumption : control.consumption) {
      if (consumption != Consumption::WINDOWS && consumption != Consumption::CANDIDATES) {
        throw std::invalid_argument("conjunction consumption must be WINDOWS or CANDIDATES");
      }
    }
    if (!control.order.empty()) {
      std::vector<bool> seen(control.order.size());
      for (size_t ordinal : control.order) {
        if (ordinal >= seen.size() || seen[ordinal]) {
          throw std::invalid_argument("conjunction order must be a permutation of entry ordinals");
        }
        seen[ordinal] = true;
      }
      if (!control.consumption.empty()
          && (control.consumption.size() != control.order.size()
              || control.consumption[control.order[0]] != Consumption::WINDOWS)) {
        throw std::invalid_argument("conjunction consumption must cover every entry and keep the lead WINDOWS");
      }
    }
    BooleanQuery::conjunctionControlForTests = &control;
  }

  ~ConjunctionPlanControl() { BooleanQuery::conjunctionControlForTests = previous; }
};

} // namespace luxir::test
