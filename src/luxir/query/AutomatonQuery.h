// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <format>
#include <string_view>

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
  // Set when the pattern can accept a term long enough to be a fitted (hashed
  // or truncated) long value, whose discarded bytes it cannot see. Holds the
  // field's policy for the warning text; nullopt means no warning applies.
  std::optional<TermPolicy> fittedPolicy;

public:
  static constexpr std::string_view FITTED_TERMS_WARNING = "fitted_terms_pattern";

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

  FilteredTermsEnum* createFilteredEnum(MemPool& pool, TermsEnum& te) override {
    switch (classification) {
      case automaton::ByteDfaKind::SINGLE:
        return pool.make<ExactTermTermsEnum>(te, exactOrPrefix);
      case automaton::ByteDfaKind::PREFIX:
      case automaton::ByteDfaKind::ALL:
        return pool.make<PrefixTermsEnum>(te, exactOrPrefix);
      case automaton::ByteDfaKind::NORMAL:
        return pool.make<DfaIntersectEnum>(pool, te, dfa, scanPlan);
      case automaton::ByteDfaKind::NONE:
        assert(false);
    }
    return nullptr;
  }

  const DfaScanPlan& getScanPlan() const { return scanPlan; }

protected:
  // Warn once per request and field when a segment holds possibly fitted terms.
  void validateLogicalImpl(PlanningContext& context, float multiplier) const override {
    checkedBoostProduct(multiplier, 1.0f);
    if (!fittedPolicy || context.warnings == nullptr) return;
    bool found = false;
    for (auto& segment : context.topReader.segments()) {
      FieldReader reader(segment.postingsReader());
      if (!reader.seek(field)) continue;
      SegFieldInfo info;
      reader.readFieldInfo(info);
      if ((info.flags & FieldType::MAY_HAVE_FITTED) != 0) {
        found = true;
        break;
      }
    }
    if (!found) return;
    std::string message = std::format(
        "pattern on field {} may miss or falsely match values longer than {} bytes ({})",
        field, PackedTerm::MAX_LEN,
        *fittedPolicy == TermPolicy::HASH128 ? "stored as prefix+hash" : "stored truncated");
    for (const auto& warning : *context.warnings) {
      if (warning.code == FITTED_TERMS_WARNING && warning.message == message) return;
    }
    context.warn(FITTED_TERMS_WARNING, message);
  }
};

} // namespace luxir
