// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "luxir/schema/FieldType.h"

namespace luxir::handler {

// One mutable chain per physical handler. Returned views last until the next
// value; callers validate before appending to postings or column streams.
class StringValue {
  std::unique_ptr<TokenChain> normalizer;
  std::string normalized;

public:
  explicit StringValue(const FieldType& type) {
    if (auto* stringType = dynamic_cast<const StrFieldType*>(&type)) {
      if (stringType->normalizer) normalizer = stringType->normalizer->createChain();
    }
  }

  bool hasNormalizer() const { return normalizer != nullptr; }

  std::string_view normalize(std::string_view value) {
    if (normalizer) {
      normalized.assign(value);
      normalizer->normalizeTerm(normalized);
      value = normalized;
    }
    return value;
  }
};

} // namespace luxir::handler
