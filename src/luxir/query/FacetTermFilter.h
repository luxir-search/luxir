// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "luxir/util/automaton/ByteDfa.h"

namespace luxir {

// The compiled form of a FieldFacet's TermFilter: a predicate over indexed
// term bytes. Every configured condition must hold. QueryBuilder builds it
// with field normalization already applied to the literals.
class FacetTermFilter {
public:
  std::string prefix;
  std::optional<automaton::ByteDfa> regex;
  std::optional<automaton::ByteDfa> excludeRegex;
  bool hasInclude = false;
  std::vector<std::string> include;  // sorted, unique
  std::vector<std::string> exclude;  // sorted, unique
  uint32_t partition = 0;
  uint32_t numPartitions = 0;

  // Stable across segments, merges and processes, so partitions never move.
  static uint64_t partitionHash(std::string_view term) {
    uint64_t hash = 0xcbf29ce484222325ULL;
    for (unsigned char byte : term) {
      hash ^= byte;
      hash *= 0x100000001b3ULL;
    }
    return hash;
  }

  bool accepts(std::string_view term) const {
    if (!term.starts_with(prefix)) return false;
    if (hasInclude && !std::ranges::binary_search(include, term)) return false;
    if (!exclude.empty() && std::ranges::binary_search(exclude, term)) return false;
    if (regex && !regex->matches(term)) return false;
    if (excludeRegex && excludeRegex->matches(term)) return false;
    if (numPartitions != 0 && partitionHash(term) % numPartitions != partition) return false;
    return true;
  }
};

}  // namespace luxir
