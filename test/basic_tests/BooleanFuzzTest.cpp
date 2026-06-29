#include <gtest/gtest.h>

#include <set>
#include <string>
#include <vector>

#include "test/SoluxTest.h"
#include "test/CollectionHelper.h"
#include "test/LocalReq.h"
#include "test/QueryBuild.h"

using namespace solux;
using namespace solux::test;

namespace api = solux::api;
namespace build = solux::api::build;

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
    int nreq = (int)rng.rint(3);     // 0..2
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
    // min_match is only valid for optional-only clauses (no required/filter).
    if (nopt > 0 && nreq == 0 && nfilter == 0) {
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
    return std::holds_alternative<bool>(q.kind);
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
    bool hasPositive = b.required.size() > 0 || b.optional.size() > 0 || b.filter.size() > 0;
    if (!hasPositive) return false;
    // Optional must match unless there is a mandatory (required) clause, which
    // makes the optional side scoring-only (MandOpt). A filter does NOT relax
    // that - filter + optional conjoins them, so the optional is still required.
    if (!b.optional.empty() && b.required.empty()) {
      int matched = 0;
      for (const auto& o : b.optional) {
        if (clauseMatches(o, toks)) matched++;
      }
      int mm = b.min_match;
      int eff = mm >= 1 ? std::min(mm, (int)b.optional.size()) : 1;  // unset/0 -> any (>=1)
      if (matched < eff) return false;
    }
    return true;
  }
};

TEST_F(BooleanFuzzTest, filterRequiresOptionalToMatch) {
  helper.clear();
  helper.index(flatdoc("id", "x1", "body_w", "f a"), UpdateMessage::NO_COMMIT);  // f + a
  helper.index(flatdoc("id", "x2", "body_w", "f"), UpdateMessage::COMMIT);        // f, no a; z nowhere

  auto run = [&](const char* filterTerm, const char* optTerm) {
    auto req = localReq(helper.getSearchEngine());
    req->collection("main");
    auto& cur = req->topDocs("q");
    cur.limit(100).fields({"id"});
    cur.rawQuery() = qb::boolean(cur.mr(),
        /*required=*/{},
        /*optional=*/{qb::match(cur.mr(), "body_w", optTerm)},
        /*prohibited=*/{},
        /*filter=*/{qb::match(cur.mr(), "body_w", filterTerm)});
    req->execute();
    std::set<std::string> got;
    for (const auto& d : req->getDocs()) got.insert(std::get<std::string>(*find(d, "id")));
    return got;
  };

  // No mandatory clause, so the optional is required (conjoined with the filter).
  EXPECT_EQ((std::set<std::string>{"x1"}), run("f", "a"));  // x2 has f but not a
  // Optional term absent everywhere -> filter must NOT match on its own.
  EXPECT_EQ((std::set<std::string>{}), run("f", "z"));
}

TEST_F(BooleanFuzzTest, twoTermConjunctionAcrossSegments) {
  helper.clear();
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
  helper.clear();
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

TEST_F(BooleanFuzzTest, randomBooleanMatchesOracle) {
  helper.clear();

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
    auto req = localReq(helper.getSearchEngine());
    req->collection("main");
    auto& cur = req->topDocs("q");
    cur.getNumber()
       .limit(numDocs)          // return every match, not just a top page
       .batchSize(numDocs + 1)  // in one response batch (we only read responses[0])
       .fields({"id"});
    api::Query rootQuery = genBool(cur.mr(), 2);
    cur.rawQuery() = rootQuery;

    std::set<std::string> expected;
    for (const auto& [id, toks] : docs) {
      if (boolMatches(std::get<api::BooleanQuery>(rootQuery.kind), toks)) expected.insert(id);
    }

    req->execute();
    int64_t engineCount = req->getMatchCount();
    std::set<std::string> got;
    for (const auto& doc : req->getDocs()) {
      const auto* v = find(doc, "id");
      ASSERT_NE(v, nullptr);
      got.insert(std::get<std::string>(*v));
    }

    if (expected != got) {
      std::string diff;
      for (const auto& [id, toks] : docs) {
        bool e = expected.count(id), g = got.count(id);
        if (!e && !g) continue;
        diff += "  " + id + " [oracle=" + (e ? "Y" : "N") + " engine=" + (g ? "Y" : "N") + "] toks:";
        for (const auto& t : toks) diff += " " + t;
        diff += "\n";
      }
      ADD_FAILURE() << "iter=" << iter << " oracleCount=" << expected.size()
                    << " engineCount=" << engineCount << " gotDocs=" << got.size()
                    << "\ndiff docs:\n" << diff << "query=\n" << querySummary(rootQuery);
      break;
    }
  }
}
