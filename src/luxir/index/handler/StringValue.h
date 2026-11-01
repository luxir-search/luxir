// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "luxir/schema/FieldType.h"
#include "luxir/util/ApiError.h"
#include "luxir/util/IpAddress.h"

#include <fmt/format.h>

namespace luxir::handler {

// One mutable chain per physical handler. Returned views last until the next
// value; callers validate before appending to postings or column streams.
// An IP field normalizes address text to its 16-byte term.
class StringValue {
  std::unique_ptr<TokenChain> normalizer;
  std::unique_ptr<Collator> collator;  // after the normalizer: the value becomes its sort key
  std::string normalized;
  std::string key;
  bool ip;

public:
  explicit StringValue(const FieldType& type) : ip(type.logical == FieldType::Logical::IP) {
    if (auto* stringType = dynamic_cast<const StrFieldType*>(&type)) {
      if (stringType->normalizer) normalizer = stringType->normalizer->createChain();
      if (stringType->collation) collator = stringType->collation->create();
    }
  }

  // True when values are rewritten before they are stored (normalized, collated, IP).
  bool hasNormalizer() const { return normalizer != nullptr || collator != nullptr || ip; }

  size_t retainedBytes() const {
    auto heap = [](const std::string& s) { return s.capacity() > std::string{}.capacity() ? s.capacity() + 1 : 0; };
    return heap(normalized) + heap(key);
  }

  // Normalize caller-owned materialization storage without another byte copy.
  void normalizeInPlace(std::string& value) {
    if (ip) {
      auto addr = ip::parse(value);
      if (!addr) {
        throw DocumentError(fmt::format("cannot parse '{}' as an IPv4 or IPv6 address", value));
      }
      value.assign(ip::bytes(*addr));
      return;
    }
    if (normalizer) normalizer->normalizeTerm(value);
    if (collator) {
      key.clear();
      collator->appendKey(value, key);
      value.swap(key);
    }
  }

  std::string_view normalize(std::string_view value) {
    if (hasNormalizer()) {
      normalized.assign(value);
      normalizeInPlace(normalized);
      value = normalized;
    }
    return value;
  }
};

} // namespace luxir::handler
