#pragma once

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
  Kind kind;
  std::string_view pattern;
  automaton::ByteDfaView dfa;
  automaton::ByteDfaKind classification;
  std::string_view exactOrPrefix;
  DfaScanPlan scanPlan;

public:
  AutomatonQuery(std::string_view field, Kind kind, std::string_view pattern,
                 automaton::ByteDfaView dfa, automaton::ByteDfaKind classification,
                 std::string_view exactOrPrefix, DfaScanPlan scanPlan)
      : MultiTermQuery(field), kind(kind), pattern(pattern), dfa(dfa),
        classification(classification), exactOrPrefix(exactOrPrefix),
        scanPlan(scanPlan) {}

  FilterKeyScope appendFilterKey(FilterKeyBuilder& out,
                                 const FilterKeyContext& ctx) const override {
    unused(ctx);
    out.appendTag(kind == Kind::WILDCARD ? FilterKeyTag::WILDCARD : FilterKeyTag::REGEX);
    out.appendString(field);
    out.appendTerm(pattern);
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
};

} // namespace luxir
