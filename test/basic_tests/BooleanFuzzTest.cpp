#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <memory_resource>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "test/SoluxTest.h"
#include "test/CollectionHelper.h"
#include "test/LocalReq.h"
#include "test/QueryBuild.h"
#include "solux/query/BooleanQuery.h"
#include "solux/query/TermQuery.h"
#include "solux/search/DocSet.h"

using namespace solux;
using namespace solux::test;

namespace api = solux::api;
namespace build = solux::api::build;

namespace {

struct ApproxFlattenGuard {
  bool saved = BooleanQuery::ConjunctionScorer::disableApproxFlattenForTests;

  explicit ApproxFlattenGuard(bool disabled) {
    BooleanQuery::ConjunctionScorer::disableApproxFlattenForTests = disabled;
  }

  ~ApproxFlattenGuard() {
    BooleanQuery::ConjunctionScorer::disableApproxFlattenForTests = saved;
  }
};

struct DisjGroupBulkGuard {
  bool saved = BooleanQuery::ConjunctionBulkScorer::disableDisjGroupBulkForTests;

  explicit DisjGroupBulkGuard(bool disabled) {
    BooleanQuery::ConjunctionBulkScorer::disableDisjGroupBulkForTests = disabled;
  }

  ~DisjGroupBulkGuard() {
    BooleanQuery::ConjunctionBulkScorer::disableDisjGroupBulkForTests = saved;
  }
};

struct DisjConjBulkGuard {
  bool saved = BooleanQuery::MaxScoreBulkScorer::disableDisjConjBulkForTests;

  explicit DisjConjBulkGuard(bool disabled) {
    BooleanQuery::MaxScoreBulkScorer::disableDisjConjBulkForTests = disabled;
  }

  ~DisjConjBulkGuard() {
    BooleanQuery::MaxScoreBulkScorer::disableDisjConjBulkForTests = saved;
  }
};

struct NotTwoPhaseGuard {
  bool saved = BooleanQuery::MandNotScorer::disableNotTwoPhaseForTests;

  explicit NotTwoPhaseGuard(bool disabled) {
    BooleanQuery::MandNotScorer::disableNotTwoPhaseForTests = disabled;
  }

  ~NotTwoPhaseGuard() {
    BooleanQuery::MandNotScorer::disableNotTwoPhaseForTests = saved;
  }
};

struct DisjTwoPhaseGuard {
  bool saved = BooleanQuery::DisjunctionScorer::disableDisjTwoPhaseForTests;

  explicit DisjTwoPhaseGuard(bool disabled) {
    BooleanQuery::DisjunctionScorer::disableDisjTwoPhaseForTests = disabled;
  }

  ~DisjTwoPhaseGuard() {
    BooleanQuery::DisjunctionScorer::disableDisjTwoPhaseForTests = saved;
  }
};

struct UnscoredOptionalDropGuard {
  bool saved = BooleanQuery::Weight::disableUnscoredOptionalDropForTests;

  explicit UnscoredOptionalDropGuard(bool disabled) {
    BooleanQuery::Weight::disableUnscoredOptionalDropForTests = disabled;
  }

  ~UnscoredOptionalDropGuard() {
    BooleanQuery::Weight::disableUnscoredOptionalDropForTests = saved;
  }
};

} // namespace

// Randomized differential test for boolean matching. body_w uses identity
// analysis for this vocabulary, so the oracle can evaluate raw tokens.
class BooleanFuzzTest : public SoluxTest {
public:
  CollectionHelper helper;
  // A wide vocabulary over short docs keeps terms sparse, so a clause is often
  // absent from a whole segment - which is what exercises the per-segment
  // "scorer is null this segment" paths (e.g. filter + absent optional).
  static constexpr const char* VOCAB[] = {
    "a", "b", "c", "d", "e", "f", "g", "h", "i", "j", "k", "l", "m",
    "n", "o", "p", "q", "r", "s", "t", "u", "v", "w", "x", "y", "z"};
  static constexpr int VOCAB_SIZE = 26;

  std::string randTerm() { return VOCAB[rng.rint(VOCAB_SIZE)]; }

  // Random query generation. Each builder returns a Query by value with its nested
  // data allocated into `mr` (the request build arena), so the tree is assembled
  // bottom-up and the root is assigned to the op via cur.rawQuery().

  api::Query genLeaf(std::pmr::memory_resource& mr) {
    if (rng.rint(10) < 3) {  // ~30% phrase
      int len = (int)rng.rint(2, 4);  // 2 or 3 words
      api::Query q;
      auto& ph = q.kind.emplace<api::PhraseQuery>();
      ph.field = build::arenaStr(mr, "body_w");
      std::string_view* a = build::allocArray(ph.words, (size_t)len, mr);
      for (int i = 0; i < len; i++) a[i] = build::arenaStr(mr, randTerm());
      return q;
    }
    return qb::match(mr, "body_w", randTerm());
  }

  api::Query genClause(std::pmr::memory_resource& mr, int depth) {
    if (depth > 0 && rng.rint(6) == 0) {
      return genBool(mr, depth - 1);
    }
    return genLeaf(mr);
  }

  api::Query genBool(std::pmr::memory_resource& mr, int depth) {
    if (depth > 0 && rng.rint(5) == 0) {
      int groupCount = (int) rng.rint(2, 4);
      bool disjunctionOfConjunctions = rng.rint(2) != 0;
      std::vector<api::Query> groups;
      for (int group = 0; group < groupCount; group++) {
        int termCount = (int) rng.rint(2, 4);
        std::vector<api::Query> terms;
        for (int term = 0; term < termCount; term++) {
          terms.push_back(qb::match(mr, "body_w", randTerm()));
        }
        groups.push_back(disjunctionOfConjunctions
            ? qb::boolean(mr, terms)
            : qb::boolean(mr, {}, terms));
      }
      return disjunctionOfConjunctions
          ? qb::boolean(mr, {}, groups)
          : qb::boolean(mr, groups);
    }
    int nreq = (int)rng.rint(4);     // 0..3
    int nopt = (int)rng.rint(4);     // 0..3
    int nproh = (int)rng.rint(3);    // 0..2
    int nfilter = (int)rng.rint(3);  // 0..2
    if (nreq + nopt + nfilter == 0) nopt = 1;  // guarantee a positive clause
    std::vector<api::Query> required, optional, prohibited, filter;
    for (int i = 0; i < nreq; i++) required.push_back(genClause(mr, depth));
    for (int i = 0; i < nopt; i++) optional.push_back(genClause(mr, depth));
    for (int i = 0; i < nproh; i++) prohibited.push_back(genClause(mr, depth));
    for (int i = 0; i < nfilter; i++) filter.push_back(genClause(mr, depth));
    int minMatch = 0;
    // min_match composes with required/filter clauses (the optional group
    // becomes a constraint); it just needs optional clauses to apply to.
    if (nopt > 0) {
      minMatch = (int)rng.rint(nopt + 1);  // 0..nopt
    }
    return qb::boolean(mr, required, optional, prohibited, filter, minMatch);
  }

  static std::string querySummary(const api::Query& q) {
    std::string out;
    appendQuerySummary(q, out);
    return out;
  }

  static void appendQuerySummary(const api::Query& q, std::string& out) {
    if (const auto* m = std::get_if<api::Match>(&q.kind)) {
      out += "match(" + std::string(m->field) + ":";
      if (m->val.has_value() && std::holds_alternative<std::string_view>(m->val->kind)) {
        out += std::get<std::string_view>(m->val->kind);
      }
      out += ")";
    } else if (const auto* ph = std::get_if<api::PhraseQuery>(&q.kind)) {
      out += "phrase(" + std::string(ph->field) + ":";
      for (size_t i = 0; i < ph->words.size(); i++) {
        if (i != 0) out += " ";
        out += ph->words[i];
      }
      out += ")";
    } else if (const auto* b = std::get_if<api::BooleanQuery>(&q.kind)) {
      out += "bool(required=[";
      appendQueriesSummary(b->required, out);
      out += "], optional=[";
      appendQueriesSummary(b->optional, out);
      out += "], prohibited=[";
      appendQueriesSummary(b->prohibited, out);
      out += "], filter=[";
      appendQueriesSummary(b->filter, out);
      out += "], min_match=" + std::to_string(b->min_match) + ")";
    } else if (const auto* boost = std::get_if<api::BoostQuery>(&q.kind)) {
      out += "boost(";
      if (boost->query) appendQuerySummary(*boost->query, out);
      out += ", " + std::to_string(boost->boost.value_or(1.0f)) + ")";
    } else if (std::holds_alternative<bool>(q.kind)) {
      out += "all";
    } else {
      out += "unknown";
    }
  }

  static void appendQueriesSummary(std::span<const api::Query> qs, std::string& out) {
    for (size_t i = 0; i < qs.size(); i++) {
      if (i != 0) out += ", ";
      appendQuerySummary(qs[i], out);
    }
  }

  // Brute-force oracle.

  static bool tokensContain(const std::vector<std::string>& toks, std::string_view term) {
    for (const auto& t : toks) {
      if (std::string_view(t) == term) return true;
    }
    return false;
  }

  static bool tokensContainPhrase(const std::vector<std::string>& toks,
                                  const std::vector<std::string>& phrase) {
    if (phrase.empty() || phrase.size() > toks.size()) return false;
    for (size_t i = 0; i + phrase.size() <= toks.size(); i++) {
      bool ok = true;
      for (size_t j = 0; j < phrase.size(); j++) {
        if (toks[i + j] != phrase[j]) { ok = false; break; }
      }
      if (ok) return true;
    }
    return false;
  }

  static bool clauseMatches(const api::Query& q, const std::vector<std::string>& toks) {
    if (const auto* m = std::get_if<api::Match>(&q.kind)) {
      if (!m->val.has_value() || !std::holds_alternative<std::string_view>(m->val->kind)) return false;
      return tokensContain(toks, std::get<std::string_view>(m->val->kind));
    }
    if (const auto* ph = std::get_if<api::PhraseQuery>(&q.kind)) {
      std::vector<std::string> phrase(ph->words.begin(), ph->words.end());
      return tokensContainPhrase(toks, phrase);
    }
    if (const auto* b = std::get_if<api::BooleanQuery>(&q.kind)) {
      return boolMatches(*b, toks);
    }
    if (const auto* boost = std::get_if<api::BoostQuery>(&q.kind)) {
      return boost->query && clauseMatches(*boost->query, toks);
    }
    if (const auto* constant = std::get_if<api::ConstantScoreQuery>(&q.kind)) {
      return constant->query && clauseMatches(*constant->query, toks);
    }
    return std::holds_alternative<bool>(q.kind);
  }

  static const api::Match* dedupableMatch(const api::Query& q) {
    const api::Query* query = &q;
    while (const auto* boost = std::get_if<api::BoostQuery>(&query->kind)) {
      if (!boost->query) return nullptr;
      query = &*boost->query;
    }
    const auto* m = std::get_if<api::Match>(&query->kind);
    if (m == nullptr || !m->val.has_value()
        || !std::holds_alternative<std::string_view>(m->val->kind)) {
      return nullptr;
    }
    return m;
  }

  static bool sameMatchIdentity(const api::Match& lhs, const api::Match& rhs) {
    return lhs.field == rhs.field
        && std::get<std::string_view>(lhs.val->kind)
           == std::get<std::string_view>(rhs.val->kind);
  }

  static std::vector<const api::Query*> dedupOptionalOracle(std::span<const api::Query> clauses,
                                                            int32_t& removed) {
    std::vector<const api::Query*> out;
    std::vector<bool> consumed(clauses.size(), false);
    for (size_t i = 0; i < clauses.size(); i++) {
      if (consumed[i]) continue;
      const api::Query* q = &clauses[i];
      if (const auto* m = dedupableMatch(*q)) {
        for (size_t j = i + 1; j < clauses.size(); j++) {
          if (consumed[j]) continue;
          const auto* other = dedupableMatch(clauses[j]);
          if (other != nullptr && sameMatchIdentity(*m, *other)) {
            consumed[j] = true;
          }
        }
      }
      out.push_back(q);
    }
    removed = (int32_t) clauses.size() - (int32_t) out.size();
    return out;
  }

  static bool boolMatches(const api::BooleanQuery& b, const std::vector<std::string>& toks) {
    for (const auto& r : b.required) {
      if (!clauseMatches(r, toks)) return false;
    }
    for (const auto& f : b.filter) {  // filters constrain like required (no score)
      if (!clauseMatches(f, toks)) return false;
    }
    for (const auto& p : b.prohibited) {
      if (clauseMatches(p, toks)) return false;
    }
    int32_t removedOptional = 0;
    std::vector<const api::Query*> optional = dedupOptionalOracle(b.optional, removedOptional);
    int minMatch = b.min_match;
    if (minMatch > 1 && removedOptional > 0) {
      if ((int64_t) minMatch * 2 > (int64_t) b.optional.size()) {
        minMatch = std::max(1, minMatch - removedOptional);
      } else {
        minMatch = std::min(minMatch, (int32_t) optional.size());
      }
    }
    bool hasPositive = b.required.size() > 0 || !optional.empty() || b.filter.size() > 0;
    if (!hasPositive) return !b.prohibited.empty();
    // The optional group constrains when min_match >= 1, or when nothing else
    // (required/filter) carries the match; otherwise optionals only rank.
    bool constrains = minMatch >= 1 || (b.required.empty() && b.filter.empty());
    if (!optional.empty() && constrains) {
      int matched = 0;
      for (const api::Query* o : optional) {
        if (clauseMatches(*o, toks)) matched++;
      }
      int eff = std::max(1, std::min(minMatch, (int)optional.size()));
      if (matched < eff) return false;
    }
    return true;
  }

  struct ExplicitCase {
    int kind;
    std::array<std::string, 6> terms;
    float factor;
    int minMatch;
  };

  static api::Query nestedBoost(std::pmr::memory_resource& mr, const api::Query& query,
                                float factor) {
    return qb::boost(mr, qb::boost(mr, query, 2.0f), factor);
  }

  static std::pair<api::Query, api::Query> explicitCase(
      std::pmr::memory_resource& mr, const ExplicitCase& spec) {
    auto term = [&](int index) {
      return qb::match(mr, "body_w", spec.terms[(size_t) index]);
    };
    float totalBoost = 2.0f * spec.factor;

    if (spec.kind == 0) {
      // R1 under mandatory, including inner filter/prohibited clauses.
      auto inner = qb::boolean(mr, {term(1), term(2)}, {}, {term(4)}, {term(3)});
      auto nested = qb::boolean(mr, {term(0), nestedBoost(mr, inner, spec.factor)});
      auto twin = qb::boolean(
        mr, {term(0), qb::boost(mr, term(1), totalBoost),
             qb::boost(mr, term(2), totalBoost)}, {}, {term(4)}, {term(3)});
      return {nested, twin};
    }
    if (spec.kind == 1) {
      // R1 under filter: mandatory maps to filter, rank-only optional drops,
      // and the wrapping boost is stripped.
      auto inner = qb::boolean(mr, {term(1)}, {term(3)}, {term(4)}, {term(2)});
      auto nested = qb::boolean(
        mr, {term(0)}, {}, {}, {nestedBoost(mr, inner, spec.factor)});
      auto twin = qb::boolean(mr, {term(0)}, {}, {term(4)}, {term(1), term(2)});
      return {nested, twin};
    }
    if (spec.kind == 2) {
      // R2 with a duplicate spanning the nested/outer boundary.
      auto inner = qb::boolean(mr, {}, {term(1), term(2)}, {}, {}, spec.minMatch);
      auto nested = qb::boolean(
        mr, {}, {term(0), nestedBoost(mr, inner, spec.factor), term(1)}, {}, {},
        spec.minMatch);
      auto twin = qb::boolean(
        mr, {}, {term(0), qb::boost(mr, term(1), totalBoost),
                 qb::boost(mr, term(2), totalBoost), term(1)}, {}, {}, spec.minMatch);
      return {nested, twin};
    }
    if (spec.kind == 3) {
      // R3 beside outer filters/prohibited. For msm <= 1 this also reruns R2
      // and dedups across the newly flattened boundary; msm=2 uses five direct
      // optionals to exercise absolute-count duplicate handling.
      api::Query inner;
      std::vector<api::Query> flatOptional;
      if (spec.minMatch <= 1) {
        auto subgroup = qb::boolean(mr, {}, {term(1), term(2)}, {}, {}, 1);
        inner = qb::boolean(mr, {}, {subgroup, term(1), term(3)}, {}, {},
                            spec.minMatch);
        flatOptional = {
          qb::boost(mr, term(1), totalBoost),
          qb::boost(mr, term(2), totalBoost),
          qb::boost(mr, term(1), totalBoost),
          qb::boost(mr, term(3), totalBoost)};
      } else {
        inner = qb::boolean(mr, {}, {term(1), term(1), term(2), term(3), term(4)},
                            {}, {}, spec.minMatch);
        flatOptional = {
          qb::boost(mr, term(1), totalBoost),
          qb::boost(mr, term(1), totalBoost),
          qb::boost(mr, term(2), totalBoost),
          qb::boost(mr, term(3), totalBoost),
          qb::boost(mr, term(4), totalBoost)};
      }
      auto nested = qb::boolean(
        mr, {nestedBoost(mr, inner, spec.factor)}, {}, {term(5)}, {term(0)});
      auto twin = qb::boolean(
        mr, {}, flatOptional, {term(5)}, {term(0)}, std::max(1, spec.minMatch));
      return {nested, twin};
    }
    if (spec.kind == 4) {
      // Empty required children are deliberately opaque and unsatisfiable.
      auto empty1 = qb::boolean(mr, {});
      auto empty2 = qb::boolean(mr, {});
      return {
        qb::boolean(mr, {term(0), nestedBoost(mr, empty1, spec.factor)}),
        qb::boolean(mr, {term(0), nestedBoost(mr, empty2, spec.factor)})};
    }
    if (spec.kind == 5) {
      // A required pure-negative child is a complement: its boost disappears
      // and its prohibited clause joins the parent.
      auto negative = qb::boolean(mr, {}, {}, {term(1)});
      return {
        qb::boolean(mr, {term(0), nestedBoost(mr, negative, spec.factor)}),
        qb::boolean(mr, {term(0)}, {}, {term(1)})};
    }
    // Parser complement form under a filter parent: the AllQuery carrier and
    // boost both disappear, leaving the exclusion at the outer level.
    auto complement = qb::boolean(mr, {}, {qb::all()}, {term(1)}, {}, 1);
    return {
      qb::boolean(mr, {term(0)}, {}, {},
                  {nestedBoost(mr, complement, spec.factor)}),
      qb::boolean(mr, {term(0)}, {}, {term(1)})};
  }
};

TEST_F(BooleanFuzzTest, optionalRanksUnlessMinMatchConstrains) {
  helper.index(flatdoc("id", "x1", "body_w", "f a"), UpdateMessage::NO_COMMIT);  // f + a
  helper.index(flatdoc("id", "x2", "body_w", "f"), UpdateMessage::COMMIT);        // f, no a; z nowhere

  auto run = [&](const char* filterTerm, const char* optTerm, int minMatch) {
    auto req = localReq(helper.getSearchEngine());
    req->collection("main");
    auto& cur = req->topDocs("q");
    cur.limit(100).fields({"id"});
    cur.rawQuery() = qb::boolean(cur.mr(),
        /*required=*/{},
        /*optional=*/{qb::match(cur.mr(), "body_w", optTerm)},
        /*prohibited=*/{},
        /*filter=*/{qb::match(cur.mr(), "body_w", filterTerm)}, minMatch);
    req->execute();
    std::set<std::string> got;
    for (const auto& d : req->getDocs()) got.insert(std::get<std::string>(*find(d, "id")));
    return got;
  };

  // min_match unset: the filter carries the match and the optional only ranks
  // (Lucene bool semantics) - even when the optional term is absent everywhere.
  EXPECT_EQ((std::set<std::string>{"x1", "x2"}), run("f", "a", 0));
  EXPECT_EQ((std::set<std::string>{"x1", "x2"}), run("f", "z", 0));
  // min_match=1 makes the optional group a real constraint.
  EXPECT_EQ((std::set<std::string>{"x1"}), run("f", "a", 1));
  EXPECT_EQ((std::set<std::string>{}), run("f", "z", 1));
}

TEST_F(BooleanFuzzTest, minMatchComposesWithRequired) {
  helper.index(flatdoc("id", "y1", "body_w", "r a b"), UpdateMessage::NO_COMMIT);  // r + both opts
  helper.index(flatdoc("id", "y2", "body_w", "r a"), UpdateMessage::NO_COMMIT);    // r + one opt
  helper.index(flatdoc("id", "y3", "body_w", "r"), UpdateMessage::COMMIT);          // r only

  auto run = [&](int minMatch) {
    auto req = localReq(helper.getSearchEngine());
    req->collection("main");
    auto& cur = req->topDocs("q");
    cur.limit(100).fields({"id"});
    cur.rawQuery() = qb::boolean(cur.mr(),
        /*required=*/{qb::match(cur.mr(), "body_w", "r")},
        /*optional=*/{qb::match(cur.mr(), "body_w", "a"), qb::match(cur.mr(), "body_w", "b")},
        /*prohibited=*/{}, /*filter=*/{}, minMatch);
    req->execute();
    std::set<std::string> got;
    for (const auto& d : req->getDocs()) got.insert(std::get<std::string>(*find(d, "id")));
    return got;
  };

  EXPECT_EQ((std::set<std::string>{"y1", "y2", "y3"}), run(0));  // optionals rank only
  EXPECT_EQ((std::set<std::string>{"y1", "y2"}), run(1));
  EXPECT_EQ((std::set<std::string>{"y1"}), run(2));
}

TEST_F(BooleanFuzzTest, twoTermConjunctionAcrossSegments) {
  helper.index(flatdoc("id", "x1", "body_w", "b d"), UpdateMessage::COMMIT);        // seg0
  helper.index(flatdoc("id", "x2", "body_w", "b d c g b"), UpdateMessage::COMMIT);  // seg1
  helper.index(flatdoc("id", "x3", "body_w", "b d b f"), UpdateMessage::COMMIT);    // seg2
  helper.index(flatdoc("id", "x4", "body_w", "b c"), UpdateMessage::COMMIT);        // only b

  auto req = localReq(helper.getSearchEngine());
  req->collection("main");
  auto& cur = req->topDocs("q");
  cur.getNumber().limit(100).fields({"id"});
  cur.rawQuery() = qb::boolean(cur.mr(),
      /*required=*/{qb::match(cur.mr(), "body_w", "b"), qb::match(cur.mr(), "body_w", "d")});
  req->execute(false);
  std::set<std::string> got;
  for (const auto& d : req->getDocs()) got.insert(std::get<std::string>(*find(d, "id")));
  int64_t count = req->getMatchCount();

  EXPECT_EQ((std::set<std::string>{"x1", "x2", "x3"}), got);
  EXPECT_EQ(3, count);
}

TEST_F(BooleanFuzzTest, phraseMultiDoc) {
  helper.index(flatdoc("id", "p0", "body_w", "x y z"), UpdateMessage::NO_COMMIT);
  helper.index(flatdoc("id", "p1", "body_w", "a e g c e d"), UpdateMessage::NO_COMMIT);  // "a e" @0
  helper.index(flatdoc("id", "p2", "body_w", "e a"), UpdateMessage::NO_COMMIT);          // "e a", not "a e"
  helper.index(flatdoc("id", "p3", "body_w", "a e"), UpdateMessage::COMMIT);             // "a e"

  // standalone phrase "a e"
  {
    auto req = localReq(helper.getSearchEngine());
    req->collection("main").topDocs("q").phraseQuery("body_w", {"a", "e"}).limit(100).fields({"id"});
    req->execute(false);
    std::set<std::string> got;
    for (const auto& d : req->getDocs()) got.insert(std::get<std::string>(*find(d, "id")));
    EXPECT_EQ((std::set<std::string>{"p1", "p3"}), got) << "standalone phrase";
  }

  // required e AND phrase "a e"
  {
    auto req = localReq(helper.getSearchEngine());
    req->collection("main");
    auto& cur = req->topDocs("q");
    cur.limit(100).fields({"id"});
    cur.rawQuery() = qb::boolean(cur.mr(),
        /*required=*/{qb::match(cur.mr(), "body_w", "e"),
                      qb::phraseWords(cur.mr(), "body_w", {"a", "e"})});
    req->execute(false);
    std::set<std::string> got;
    for (const auto& d : req->getDocs()) got.insert(std::get<std::string>(*find(d, "id")));
    EXPECT_EQ((std::set<std::string>{"p1", "p3"}), got) << "required e + phrase a e";
  }
}

// Multi-member exclusion and rank-only optional sides advance their
// disjunction per candidate (DisjunctionScorer::advance): drive that over
// full postings blocks with members that exhaust mid-stream, against a
// counted oracle.
TEST_F(BooleanFuzzTest, negatedAndRankOnlyDisjunctionAdvanceMatchesOracle) {
  const int32_t numDocs = 2 * DocsEnumMeta::L1_DOCS + 300;
  int64_t expectNegated = 0;
  int64_t expectMandOpt = 0;
  std::vector<Doc> docs;
  docs.reserve((size_t) numDocs);
  for (int32_t doc = 0; doc < numDocs; doc++) {
    std::string body;
    bool req = (doc % 3) == 1;
    bool ex1 = (doc % 4) != 2;
    // ex2 only exists in the first quarter: its enum hits END mid-stream.
    bool ex2 = doc < numDocs / 4 && (doc % 5) == 0;
    if (req) body += " na_req";
    if (ex1) body += " na_ex1";
    if (ex2) body += " na_ex2";
    if ((doc % 7) == 3) body += " na_opt1";
    if ((doc % 11) == 6) body += " na_opt2";
    body += " filler";
    docs.push_back(flatdoc("id", "na" + std::to_string(doc), "body_w", body));
    if (req && !ex1 && !ex2) expectNegated++;
    if (req) expectMandOpt++;
  }
  helper.indexAll(docs, UpdateMessage::COMMIT);

  auto countOf = [&](api::Query q) {
    auto req = localReq(helper.getSearchEngine());
    req->collection("main");
    auto& cur = req->topDocs("q");
    cur.getNumber().limit(10);
    cur.rawQuery() = q;
    req->execute(false);
    return req->getMatchCount();
  };

  {
    auto req = localReq(helper.getSearchEngine());
    auto& cur = req->topDocs("q");
    auto q = qb::boolean(cur.mr(),
        /*required=*/{qb::match(cur.mr(), "body_w", "na_req")},
        /*optional=*/{},
        /*prohibited=*/{qb::match(cur.mr(), "body_w", "na_ex1"),
                        qb::match(cur.mr(), "body_w", "na_ex2")});
    EXPECT_EQ(expectNegated, countOf(q));
  }
  {
    auto req = localReq(helper.getSearchEngine());
    auto& cur = req->topDocs("q");
    // Rank-only optionals: match count is the required side's alone, but
    // scoring probes the optional disjunction per candidate.
    auto q = qb::boolean(cur.mr(),
        /*required=*/{qb::match(cur.mr(), "body_w", "na_req")},
        /*optional=*/{qb::match(cur.mr(), "body_w", "na_opt1"),
                      qb::match(cur.mr(), "body_w", "na_opt2")});
    EXPECT_EQ(expectMandOpt, countOf(q));
  }
  {
    // Nested OR under AND, count-only (no scores): the nested clause is a
    // plain DisjunctionScorer inside a ConjunctionScorer, driven with
    // interleaved next()/advance() including advance-before-first-next.
    int64_t expectNested = 0;
    for (int32_t doc = 0; doc < numDocs; doc++) {
      bool isReq = (doc % 3) == 1;
      bool o1 = (doc % 7) == 3;
      bool o2 = (doc % 11) == 6;
      if (isReq && (o1 || o2)) expectNested++;
    }
    auto req = localReq(helper.getSearchEngine());
    auto& cur = req->topDocs("q");
    std::vector<api::Query> nestedOr = {qb::match(cur.mr(), "body_w", "na_opt1"),
                                        qb::match(cur.mr(), "body_w", "na_opt2")};
    auto q = qb::boolean(cur.mr(),
        /*required=*/{qb::match(cur.mr(), "body_w", "na_req"),
                      qb::boolean(cur.mr(), /*required=*/{}, /*optional=*/nestedOr)});
    EXPECT_EQ(expectNested, countOf(q));
  }
}

TEST_F(BooleanFuzzTest, randomBooleanMatchesOracle) {

  const int numDocs = 48;
  std::vector<std::pair<std::string, std::vector<std::string>>> docs;
  for (int i = 0; i < numDocs; i++) {
    int len = (int)rng.rint(1, 5);  // 1..4 tokens (short, so terms stay sparse)
    std::vector<std::string> toks;
    std::string text;
    for (int j = 0; j < len; j++) {
      std::string t = randTerm();
      toks.push_back(t);
      if (j) text += ' ';
      text += t;
    }
    std::string id = "d" + std::to_string(i);
    docs.emplace_back(id, std::move(toks));
    // Multiple segments exercise per-segment scorer creation.
    auto commit = ((i + 1) % 16 == 0) ? UpdateMessage::COMMIT : UpdateMessage::NO_COMMIT;
    helper.index(flatdoc("id", id, "body_w", text), commit);
  }

  for (int iter = 0; iter < 300; iter++) {
    std::pmr::monotonic_buffer_resource queryArena;
    api::Query rootQuery = genBool(queryArena, 2);

    std::set<std::string> expected;
    for (const auto& [id, toks] : docs) {
      if (boolMatches(std::get<api::BooleanQuery>(rootQuery.kind), toks)) expected.insert(id);
    }

    auto run = [&](bool disableFlatten, bool disableDisjGroupBulk,
                   bool disableNotTwoPhase = false,
                   bool disableDisjTwoPhase = false,
                   bool disableOptionalDrop = false,
                   bool disableDisjConjBulk = false) {
      ApproxFlattenGuard flattenGuard(disableFlatten);
      DisjGroupBulkGuard disjGroupGuard(disableDisjGroupBulk);
      DisjConjBulkGuard disjConjGuard(disableDisjConjBulk);
      NotTwoPhaseGuard notGuard(disableNotTwoPhase);
      DisjTwoPhaseGuard disjGuard(disableDisjTwoPhase);
      UnscoredOptionalDropGuard optionalDropGuard(disableOptionalDrop);
      auto req = localReq(helper.getSearchEngine());
      req->collection("main");
      auto& cur = req->topDocs("q");
      cur.getNumber()
         .limit(numDocs)
         .batchSize(numDocs + 1)
         .fields({"id"});
      cur.rawQuery() = rootQuery;
      req->execute();
      EXPECT_TRUE(req->ok()) << req->errorMsg();

      std::set<std::string> result;
      for (const auto& doc : req->getDocs()) {
        const auto* v = find(doc, "id");
        if (v == nullptr) {
          ADD_FAILURE() << "missing id field";
          continue;
        }
        result.insert(std::get<std::string>(*v));
      }
      return std::pair<int64_t, std::set<std::string>>{
        req->getMatchCount(), std::move(result)};
    };

    auto opaque = run(true, true);
    auto groupBulk = run(true, false);
    auto flattened = run(false, false);
    auto disjConjOpaque = run(false, false, false, false, false, true);
    auto eagerNot = run(false, false, true);
    auto eagerDisj = run(false, false, false, true);
    auto retainedOptionals = run(false, false, false, false, true);

    if (expected != opaque.second || opaque != groupBulk || groupBulk != flattened
        || flattened != disjConjOpaque || flattened != eagerNot || flattened != eagerDisj
        || flattened != retainedOptionals
        || opaque.first != (int64_t) expected.size()) {
      std::string diff;
      for (const auto& [id, toks] : docs) {
        bool brute = expected.contains(id);
        bool oldPath = opaque.second.contains(id);
        bool groupPath = groupBulk.second.contains(id);
        bool flatPath = flattened.second.contains(id);
        bool disjConjOpaquePath = disjConjOpaque.second.contains(id);
        bool eagerNotPath = eagerNot.second.contains(id);
        bool eagerDisjPath = eagerDisj.second.contains(id);
        bool retainedOptionalsPath = retainedOptionals.second.contains(id);
        if (!brute && !oldPath && !groupPath && !flatPath
            && !disjConjOpaquePath && !eagerNotPath && !eagerDisjPath
            && !retainedOptionalsPath) continue;
        diff += "  " + id + " [brute=" + (brute ? "Y" : "N")
            + " nested=" + (oldPath ? "Y" : "N")
            + " group=" + (groupPath ? "Y" : "N")
            + " flat=" + (flatPath ? "Y" : "N")
            + " disj_conj_opaque=" + (disjConjOpaquePath ? "Y" : "N")
            + " eager_not=" + (eagerNotPath ? "Y" : "N")
            + " eager_disj=" + (eagerDisjPath ? "Y" : "N")
            + " retained_opt=" + (retainedOptionalsPath ? "Y" : "N") + "] toks:";
        for (const auto& t : toks) diff += " " + t;
        diff += "\n";
      }
      ADD_FAILURE() << "iter=" << iter << " bruteCount=" << expected.size()
                    << " nestedCount=" << opaque.first
                    << " groupCount=" << groupBulk.first
                    << " flatCount=" << flattened.first
                    << " disjConjOpaqueCount=" << disjConjOpaque.first
                    << " eagerNotCount=" << eagerNot.first
                    << " eagerDisjCount=" << eagerDisj.first
                    << " retainedOptCount=" << retainedOptionals.first
                    << "\ndiff docs:\n" << diff << "query=\n" << querySummary(rootQuery);
      break;
    }
  }
}

TEST_F(BooleanFuzzTest, explicitFlatteningTransformsMatchOracleAndTwin) {
  using ScoreMap = std::map<std::string, float>;

  // Every subset of six terms gives each transformation both matching and
  // non-matching docs, and four commits exercise multi-segment planning.
  std::vector<std::pair<std::string, std::vector<std::string>>> docs;
  for (int bits = 0; bits < 64; bits++) {
    std::string id = "d" + std::to_string(bits);
    std::string text = "z";
    std::vector<std::string> tokens = {"z"};
    for (int term = 0; term < 6; term++) {
      if ((bits & (1 << term)) == 0) continue;
      text += " ";
      text += VOCAB[term];
      tokens.push_back(VOCAB[term]);
    }
    docs.emplace_back(id, std::move(tokens));
    helper.index(flatdoc("id", id, "body_w", text),
                 (bits + 1) % 16 == 0 ? UpdateMessage::COMMIT
                                      : UpdateMessage::NO_COMMIT);
  }

  auto expectedDocs = [&](const api::Query& query) {
    std::set<std::string> expected;
    for (const auto& [id, tokens] : docs) {
      if (clauseMatches(query, tokens)) expected.insert(id);
    }
    return expected;
  };

  auto readScores = [&](LocalReq& req, std::string_view name) {
    ScoreMap result;
    const auto* list = req.docList(name);
    if (list == nullptr) {
      ADD_FAILURE() << "missing doc list for " << name;
      return result;
    }
    if (list->found.value_or(0) == 0) return result;
    const auto* ids = list->columns.find("id");
    const auto* scores = list->columns.find("_score_");
    if (ids == nullptr || scores == nullptr) {
      ADD_FAILURE() << "missing id/score columns for " << name;
      return result;
    }
    const auto& idValues = std::get<api::ColStr>(ids->kind).v;
    const auto& scoreValues = std::get<api::ColFloat>(scores->kind).v;
    if (idValues.size() != scoreValues.size()) {
      ADD_FAILURE() << "id/score column size mismatch for " << name;
      return result;
    }
    for (size_t i = 0; i < idValues.size(); i++) {
      result[std::string(idValues[i])] = scoreValues[i];
    }
    return result;
  };

  auto mapDocs = [](const ScoreMap& scores) {
    std::set<std::string> result;
    for (const auto& [id, score] : scores) {
      unused(score);
      result.insert(id);
    }
    return result;
  };

  auto expectScoresNear = [](const ScoreMap& expected, const ScoreMap& actual,
                             const ExplicitCase& spec, const char* comparison) {
    ASSERT_EQ(expected.size(), actual.size())
      << comparison << " kind=" << spec.kind;
    auto expectedIt = expected.begin();
    auto actualIt = actual.begin();
    for (; expectedIt != expected.end(); expectedIt++, actualIt++) {
      ASSERT_EQ(expectedIt->first, actualIt->first)
        << comparison << " kind=" << spec.kind;
      float scale = std::max({std::fabs(expectedIt->second),
                              std::fabs(actualIt->second), 1.0f});
      EXPECT_NEAR(expectedIt->second, actualIt->second, 8e-6f * scale)
        << comparison << " kind=" << spec.kind << " doc=" << expectedIt->first;
    }
  };

  for (int iter = 0; iter < 42; iter++) {
    ExplicitCase spec;
    spec.kind = iter % 7;
    int offset = (int) rng.rint(6);
    int step = rng.rint(2) == 0 ? 1 : 5;
    for (int i = 0; i < 6; i++) {
      spec.terms[(size_t) i] = VOCAB[(offset + i * step) % 6];
    }
    spec.factor = std::array<float, 3>{0.5f, 1.5f, 2.0f}[(size_t) (iter % 3)];
    spec.minMatch = spec.kind == 3 ? (iter / 6) % 3 : iter % 2;

    auto runScored = [&](bool prepared) {
      auto req = localReq(helper.getSearchEngine());
      req->collection("main");
      auto& nestedCursor = req->topDocs("nested");
      auto queries = explicitCase(nestedCursor.mr(), spec);
      nestedCursor.getNumber().getScores().limit(64).batchSize(65).fields({"id"});
      nestedCursor.rawQuery() = queries.first;
      auto& twinCursor = req->topDocs("twin");
      twinCursor.getNumber().getScores().limit(64).batchSize(65).fields({"id"});
      twinCursor.rawQuery() = queries.second;
      req->testForcePrepare = prepared;
      req->execute(iter % 2 == 0);
      EXPECT_TRUE(req->ok()) << req->errorMsg();

      auto expected = expectedDocs(queries.first);
      auto twinExpected = expectedDocs(queries.second);
      EXPECT_EQ(expected, twinExpected)
        << "token oracle/twin kind=" << spec.kind
        << " nested=" << querySummary(queries.first)
        << " twin=" << querySummary(queries.second);
      ScoreMap nestedScores = readScores(*req, "nested");
      ScoreMap twinScores = readScores(*req, "twin");
      EXPECT_EQ(expected, mapDocs(nestedScores))
        << "nested token oracle kind=" << spec.kind
        << " query=" << querySummary(queries.first);
      EXPECT_EQ(expected, mapDocs(twinScores))
        << "twin token oracle kind=" << spec.kind
        << " query=" << querySummary(queries.second);
      expectScoresNear(twinScores, nestedScores, spec, "nested vs twin");
      return std::pair<ScoreMap, ScoreMap>{std::move(nestedScores),
                                           std::move(twinScores)};
    };

    auto runNoScores = [&](bool prepared, bool disableOptionalDrop) {
      UnscoredOptionalDropGuard optionalDropGuard(disableOptionalDrop);
      auto req = localReq(helper.getSearchEngine());
      req->collection("main");
      auto& nestedCursor = req->topDocs("nested");
      auto queries = explicitCase(nestedCursor.mr(), spec);
      nestedCursor.getNumber().limit(0);
      nestedCursor.rawQuery() = queries.first;
      auto& twinCursor = req->topDocs("twin");
      twinCursor.getNumber().limit(0);
      twinCursor.rawQuery() = queries.second;
      req->testForcePrepare = prepared;
      req->execute(iter % 2 != 0);
      EXPECT_TRUE(req->ok()) << req->errorMsg();
      int64_t expected = (int64_t) expectedDocs(queries.first).size();
      EXPECT_EQ(expected, req->getMatchCount("nested"))
        << "no-scores nested kind=" << spec.kind;
      EXPECT_EQ(expected, req->getMatchCount("twin"))
        << "no-scores twin kind=" << spec.kind;
      return std::pair<int64_t, int64_t>{req->getMatchCount("nested"),
                                         req->getMatchCount("twin")};
    };

    auto liveScores = runScored(false);
    auto preparedScores = runScored(true);
    expectScoresNear(liveScores.first, preparedScores.first, spec,
                     "live vs prepared nested");
    expectScoresNear(liveScores.second, preparedScores.second, spec,
                     "live vs prepared twin");
    auto liveCounts = runNoScores(false, false);
    auto preparedCounts = runNoScores(true, false);
    auto retainedLiveCounts = runNoScores(false, true);
    auto retainedPreparedCounts = runNoScores(true, true);
    EXPECT_EQ(liveCounts, preparedCounts) << "count live/prepared kind=" << spec.kind;
    EXPECT_EQ(liveCounts, retainedLiveCounts) << "count reduced/retained live kind=" << spec.kind;
    EXPECT_EQ(liveCounts, retainedPreparedCounts)
      << "count reduced/retained prepared kind=" << spec.kind;
  }
}

TEST_F(BooleanFuzzTest, conjunctionBulkCountMatchesPullOnMixedBlockShapes) {
  const int32_t numDocs = 2 * DocsEnumMeta::L1_DOCS + 513;
  // bc_rare stays below the dense gate so the sparse combo hits the fallback.
  const int32_t rareMax =
      numDocs / BooleanQuery::ConjunctionBulkScorer::kDenseThresholdInverse - 1;
  ASSERT_GT(rareMax, 1);
  std::vector<Doc> docs;
  docs.reserve((size_t) numDocs);
  int32_t rareCount = 0;
  for (int32_t doc = 0; doc < numDocs; doc++) {
    std::string body = "bc_contig";
    if ((doc % 4) != 1) body += " bc_word";
    if ((doc % 10) == 0) body += " bc_packed";
    if (rareCount < rareMax && (doc % 500) == 0) {
      body += " bc_rare";
      rareCount++;
    }
    body += " filler";
    docs.push_back(flatdoc("id", "bc" + std::to_string(doc), "body_w", body));
  }
  helper.indexAll(docs, UpdateMessage::COMMIT);
  auto reader = helper.getIndexWriter()->getIndexReader();

  auto makeFilter = [](int32_t maxDoc, int32_t mode) -> std::unique_ptr<DocSet> {
    if (mode == 0) {
      return nullptr;
    }
    if (mode == 1) {
      auto filter = std::make_unique<RAMBitDocSet>(maxDoc);
      for (int32_t doc = 0; doc < maxDoc; doc++) {
        if ((doc % 3) != 1) {
          filter->mutableBits().set(doc);
        }
      }
      return filter;
    }
    std::vector<int32_t> filterDocs;
    for (int32_t doc = 0; doc < maxDoc; doc++) {
      if ((doc % 7) == 0) {
        filterDocs.push_back(doc);
      }
    }
    return std::make_unique<ArrDocSet>(std::move(filterDocs));
  };

  auto runCount = [&](std::span<const std::string_view> terms, bool bulk,
                      int32_t filterMode) -> int64_t {
    MemPool pool;
    Query::Context qContext(pool, *reader);
    std::vector<TermQuery> queries;
    queries.reserve(terms.size());
    std::vector<Query*> mandatory;
    mandatory.reserve(terms.size());
    for (auto term : terms) {
      queries.emplace_back("body_w", term);
      mandatory.push_back(&queries.back());
    }
    std::span<Query*> empty;
    BooleanQuery query(std::span<Query*>(mandatory.data(), mandatory.size()),
                       empty, empty, empty);
    auto* weight = query.createWeight(qContext, Query::NEED_SCORES);
    int64_t total = 0;
    auto segments = qContext.topReader.segments();
    for (auto& segment : segments) {
      auto filter = makeFilter(segment.maxDoc(), filterMode);
      if (bulk) {
        auto* supplier = weight->scorerSupplier(pool, segment);
        if (supplier == nullptr) continue;
        auto* bulkScorer = supplier->bulkScorer(pool);
        if (bulkScorer == nullptr) {
          ADD_FAILURE() << "bulkScorer returned null";
          return -1;
        }
        for (int32_t cursor = 0; cursor != PostingsReader::END && cursor < segment.maxDoc(); ) {
          int32_t next = bulkScorer->countNextWindow(total, nullptr, filter.get(), cursor, segment.maxDoc());
          if (next == PostingsReader::END) break;
          if (next <= cursor) {
            ADD_FAILURE() << "countNextWindow made no progress";
            return -1;
          }
          cursor = next;
        }
      } else {
        auto* scorer = weight->createScorer(pool, segment);
        if (scorer == nullptr) continue;
        for (int32_t doc = scorer->next(); doc != PostingsReader::END; doc = scorer->next()) {
          if (filter == nullptr || filter->get(doc)) {
            total++;
          }
        }
      }
    }
    return total;
  };

  std::array<std::string_view, 2> dense2 = {"bc_contig", "bc_word"};
  std::array<std::string_view, 3> dense3 = {"bc_contig", "bc_word", "bc_packed"};
  std::array<std::string_view, 2> sparse = {"bc_contig", "bc_rare"};

  bool savedStats = SkipStats::enabled;
  SkipStats::enabled = true;
  SkipStats::reset();
  for (int32_t filterMode : {0, 1, 2}) {
    EXPECT_EQ(runCount(dense2, true, filterMode), runCount(dense2, false, filterMode))
        << "dense2 filter=" << filterMode;
    EXPECT_EQ(runCount(dense3, true, filterMode), runCount(dense3, false, filterMode))
        << "dense3 filter=" << filterMode;
    EXPECT_EQ(runCount(sparse, true, filterMode), runCount(sparse, false, filterMode))
        << "sparse filter=" << filterMode;
  }
  EXPECT_GT(SkipStats::conjDenseCountWindows, 0);
  EXPECT_GT(SkipStats::conjCountFallbacks, 0);
  SkipStats::enabled = savedStats;
}

TEST_F(BooleanFuzzTest, mandOptBulkCountMatchesPullOnMixedBlockShapes) {
  const int32_t numDocs = 2 * DocsEnumMeta::L1_DOCS + 513;
  std::vector<Doc> docs;
  docs.reserve((size_t) numDocs);
  for (int32_t doc = 0; doc < numDocs; doc++) {
    std::string body = "bm_mand_contig";
    if ((doc % 17) == 0) body += " bm_mand_sparse";
    if ((doc % 4) != 1) body += " bm_opt_word";
    if ((doc % 10) == 0) body += " bm_opt_packed";
    if (doc < DocsEnumMeta::L1_DOCS && (doc % 257) == 17) body += " bm_opt_tail";
    body += " filler";
    docs.push_back(flatdoc("id", "bm" + std::to_string(doc), "body_w", body));
  }
  helper.indexAll(docs, UpdateMessage::COMMIT);
  auto reader = helper.getIndexWriter()->getIndexReader();

  auto makeFilter = [](int32_t maxDoc, int32_t mode) -> std::unique_ptr<DocSet> {
    if (mode == 0) {
      return nullptr;
    }
    if (mode == 1) {
      auto filter = std::make_unique<RAMBitDocSet>(maxDoc);
      for (int32_t doc = 0; doc < maxDoc; doc++) {
        if ((doc % 3) != 1) {
          filter->mutableBits().set(doc);
        }
      }
      return filter;
    }
    std::vector<int32_t> filterDocs;
    for (int32_t doc = 0; doc < maxDoc; doc++) {
      if ((doc % 7) == 0) {
        filterDocs.push_back(doc);
      }
    }
    return std::make_unique<ArrDocSet>(std::move(filterDocs));
  };

  auto runCount = [&](std::string_view mandTerm, std::span<const std::string_view> optTerms,
                      bool bulk, int32_t filterMode) -> int64_t {
    MemPool pool;
    Query::Context qContext(pool, *reader);
    TermQuery mandQuery("body_w", mandTerm);
    std::array<Query*, 1> mandatory = {&mandQuery};
    std::vector<TermQuery> optQueries;
    std::vector<Query*> optional;
    optQueries.reserve(optTerms.size());
    optional.reserve(optTerms.size());
    for (auto term : optTerms) {
      optQueries.emplace_back("body_w", term);
      optional.push_back(&optQueries.back());
    }
    std::span<Query*> empty;
    BooleanQuery query(std::span<Query*>(mandatory.data(), mandatory.size()),
                       std::span<Query*>(optional.data(), optional.size()),
                       empty, empty);
    auto* weight = query.createWeight(qContext, Query::NEED_SCORES);
    int64_t total = 0;
    auto segments = qContext.topReader.segments();
    for (auto& segment : segments) {
      auto filter = makeFilter(segment.maxDoc(), filterMode);
      if (bulk) {
        auto* supplier = weight->scorerSupplier(pool, segment);
        if (supplier == nullptr) continue;
        auto* bulkScorer = supplier->bulkScorer(pool);
        if (bulkScorer == nullptr) {
          ADD_FAILURE() << "bulkScorer returned null";
          return -1;
        }
        for (int32_t cursor = 0; cursor != PostingsReader::END && cursor < segment.maxDoc(); ) {
          int32_t next = bulkScorer->countNextWindow(total, nullptr, filter.get(),
                                                     cursor, segment.maxDoc());
          if (next == PostingsReader::END) break;
          if (next <= cursor) {
            ADD_FAILURE() << "countNextWindow made no progress";
            return -1;
          }
          cursor = next;
        }
      } else {
        auto* scorer = weight->createScorer(pool, segment);
        if (scorer == nullptr) continue;
        for (int32_t doc = scorer->next(); doc != PostingsReader::END; doc = scorer->next()) {
          if (filter == nullptr || filter->get(doc)) {
            total++;
          }
        }
      }
    }
    return total;
  };

  std::array<std::string_view, 1> oneOpt = {"bm_opt_word"};
  std::array<std::string_view, 3> threeOpts = {"bm_opt_word", "bm_opt_packed", "bm_opt_tail"};

  bool savedStats = SkipStats::enabled;
  SkipStats::enabled = true;
  SkipStats::reset();
  for (int32_t filterMode : {0, 1, 2}) {
    EXPECT_EQ(runCount("bm_mand_contig", oneOpt, true, filterMode),
              runCount("bm_mand_contig", oneOpt, false, filterMode))
        << "contig oneOpt filter=" << filterMode;
    EXPECT_EQ(runCount("bm_mand_contig", threeOpts, true, filterMode),
              runCount("bm_mand_contig", threeOpts, false, filterMode))
        << "contig threeOpts filter=" << filterMode;
    EXPECT_EQ(runCount("bm_mand_sparse", threeOpts, true, filterMode),
              runCount("bm_mand_sparse", threeOpts, false, filterMode))
        << "sparse threeOpts filter=" << filterMode;
  }
  EXPECT_GT(SkipStats::mandOptBulkWindows, 0);
  SkipStats::enabled = savedStats;
}
