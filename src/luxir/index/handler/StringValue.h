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
  std::string normalized;
  bool ip;

public:
  explicit StringValue(const FieldType& type) : ip(type.logical == FieldType::Logical::IP) {
    if (auto* stringType = dynamic_cast<const StrFieldType*>(&type)) {
      if (stringType->normalizer) normalizer = stringType->normalizer->createChain();
    }
  }

  bool hasNormalizer() const { return normalizer != nullptr || ip; }

  size_t retainedBytes() const {
    return normalized.capacity() > std::string{}.capacity() ? normalized.capacity() + 1 : 0;
  }

  // Normalize caller-owned materialization storage without another byte copy.
  void normalizeInPlace(std::string& value) {
    if (normalizer) normalizer->normalizeTerm(value);
  }

  std::string_view normalize(std::string_view value) {
    if (ip) {
      auto addr = ip::parse(value);
      if (!addr) {
        throw DocumentError(fmt::format("cannot parse '{}' as an IPv4 or IPv6 address", value));
      }
      normalized.assign(ip::bytes(*addr));
      return normalized;
    }
    if (normalizer) {
      normalized.assign(value);
      normalizer->normalizeTerm(normalized);
      value = normalized;
    }
    return value;
  }
};

} // namespace luxir::handler
