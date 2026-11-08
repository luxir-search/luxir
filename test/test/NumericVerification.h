// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <functional>
#include "luxir/query/NumericPredicateQuery.h"
#include "test/TestUtils.h"

namespace luxir::test {

class NumericVerificationObserver {
#ifndef NDEBUG
  static inline thread_local NumericVerificationObserver* active = nullptr;
  SearchOverridesGuard<decltype(NumericPredicateQuery::verificationObserver),
                       NumericVerificationObserver*> saved{
      NumericPredicateQuery::verificationObserver, active};
  std::function<void(const NumericPredicateQuery&, int32_t)> callback;

public:
  explicit NumericVerificationObserver(std::function<void(const NumericPredicateQuery&, int32_t)> callback)
      : callback(std::move(callback)) {
    active = this;
    NumericPredicateQuery::verificationObserver = [](const NumericPredicateQuery& query, int32_t doc) {
      active->callback(query, doc);
    };
  }
  explicit NumericVerificationObserver(std::function<void(int32_t)> callback)
      : NumericVerificationObserver([callback = std::move(callback)](const NumericPredicateQuery&, int32_t doc) {
          callback(doc);
        }) {}
#else
public:
  explicit NumericVerificationObserver(std::function<void(int32_t)>) {}
  explicit NumericVerificationObserver(std::function<void(const NumericPredicateQuery&, int32_t)>) {}
#endif
};

} // namespace luxir::test
