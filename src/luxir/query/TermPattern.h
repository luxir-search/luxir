// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <optional>
#include <string>

#include "luxir/util/StrRef.h"
#include "luxir/util/automaton/ByteDfa.h"

namespace luxir {

// A wildcard or regex pattern compiled against one field's indexed terms:
// field folding applied, classified, and checked against the field's fitted
// long-term form. QueryBuilder turns it into a query; term-by-term callers
// (facet term filters) test terms against it directly.
struct TermPattern {
  automaton::ByteDfaKind kind = automaton::ByteDfaKind::NONE;
  // SINGLE: the one term. PREFIX: the prefix. NORMAL: the literal every match
  // starts with (possibly empty).
  std::string literal;
  // Set when every match starts with a literal longer than fitted long terms
  // keep. A query then matches every term with this shorter prefix, an
  // over-approximation, instead of running the pattern.
  std::optional<std::string> fittedPrefix;
  automaton::ByteDfa dfa;
  // NORMAL: the DFA state after `literal`, and the suffix every match ends with.
  automaton::ByteDfa::State afterLiteral = 0;
  std::string commonSuffix;
  // NORMAL on a field whose long values are fitted: the pattern sees fitted
  // bytes, not the discarded ones.
  std::optional<TermPolicy> fittedPolicy;
};

}  // namespace luxir
