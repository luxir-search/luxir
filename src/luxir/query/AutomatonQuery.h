// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <optional>
#include <string_view>

#include "FittedTerms.h"
#include "MultiTermQuery.h"
#include "luxir/reader/DfaIntersectEnum.h"
#include "luxir/util/automaton/ByteDfa.h"

namespace luxir {

class ExactTermTermsEnum final : public ScanTermsEnum {
  std::string_view term;

protected:
  bool seekStart() override { return te.seekCeil(term); }
  Status accept() override { return termView() == term ? Status::ACCEPT : Status::END; }

public:
  ExactTermTermsEnum(TermsEnum& te, std::string_view term) : ScanTermsEnum(te), term(term) {}
};

class AutomatonQuery final : public MultiTermQuery {
public:
  enum class Kind { WILDCARD, REGEX };

private:
  Kind automatonKind;
  std::string_view pattern;
  automaton::ByteDfaView dfa;
  automaton::ByteDfaKind classification;
  std::string_view exactOrPrefix;
  DfaScanPlan scanPlan;
  // The field's long-term policy for a general pattern on a field that can
  // hold fitted (hashed or truncated) long values, whose discarded bytes the
  // pattern cannot see; nullopt means no warning applies.
  std::optional<TermPolicy> fittedPolicy;

public:
  AutomatonQuery(std::string_view field, Kind kind, std::string_view pattern,
                 automaton::ByteDfaView dfa, automaton::ByteDfaKind classification,
                 std::string_view exactOrPrefix, DfaScanPlan scanPlan,
                 std::optional<TermPolicy> fittedPolicy = std::nullopt)
      : MultiTermQuery(QueryKind::AUTOMATON, field), automatonKind(kind),
        pattern(pattern), dfa(dfa),
        classification(classification), exactOrPrefix(exactOrPrefix),
        scanPlan(scanPlan), fittedPolicy(fittedPolicy) {}

  bool equalsSameKind(const Query& other) const override {
    const auto& rhs = static_cast<const AutomatonQuery&>(other);
    return automatonKind == rhs.automatonKind && field == rhs.field
        && pattern == rhs.pattern;
  }

  uint64_t hashImpl() const override {
    uint64_t value = mixHash(Query::hashImpl(), automatonKind);
    value = mixHash(value, field);
    return mixHash(value, pattern);
  }

  FilterKeyScope appendFilterKey(FilterKeyBuilder& out,
                                 const FilterKeyContext& ctx) const override {
    out.appendKind(kind);
    out.appendByte((uint8_t)automatonKind);
    out.appendString(field);
    out.appendTerm(pattern);
    unused(ctx);
    return FilterKeyScope::SEGMENT_STABLE;
  }

  FilteredTermsEnum* createFilteredEnum(MemPool& pool, TermsEnum& te) override;

  const DfaScanPlan& getScanPlan() const { return scanPlan; }

protected:
  void validateLogicalImpl(PlanningContext& context, float multiplier) const override {
    checkedBoostProduct(multiplier, 1.0f);
    if (!fittedPolicy) return;
    warnIfFittedTermsReachable(context, field, *fittedPolicy, [&] {
      return dfa.maxAcceptedLength() >= (int32_t)PackedTerm::MIN_FITTED_LEN;
    });
  }
};

} // namespace luxir
