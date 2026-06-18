#include <gtest/gtest.h>

#include <set>
#include <string>
#include <vector>

#include "test/SoluxTest.h"
#include "test/CollectionHelper.h"
#include "test/LocalReq.h"

using namespace solux;
using namespace solux::test;

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

  // Random query generation.

  void genLeaf(proto::Query* q) {
    if (rng.rint(10) < 3) {  // ~30% phrase
      auto& ph = *q->mutable_phrase();
      ph.set_field("body_w");
      int len = (int)rng.rint(2, 4);  // 2 or 3 words
      for (int i = 0; i < len; i++) *ph.mutable_words()->Add() = randTerm();
    } else {
      auto& m = *q->mutable_match();
      m.set_field("body_w");
      m.mutable_val()->set_s(randTerm());
    }
  }

  void genClause(proto::Query* q, int depth) {
    if (depth > 0 && rng.rint(6) == 0) {
      genBool(q, depth - 1);
    } else {
      genLeaf(q);
    }
  }

  void genBool(proto::Query* q, int depth) {
    auto& b = *q->mutable_boolean();
    int nreq = (int)rng.rint(3);     // 0..2
    int nopt = (int)rng.rint(4);     // 0..3
    int nproh = (int)rng.rint(3);    // 0..2
    int nfilter = (int)rng.rint(3);  // 0..2
    if (nreq + nopt + nfilter == 0) nopt = 1;  // guarantee a positive clause
    for (int i = 0; i < nreq; i++) genClause(b.add_required(), depth);
    for (int i = 0; i < nopt; i++) genClause(b.add_optional(), depth);
    for (int i = 0; i < nproh; i++) genClause(b.add_prohibited(), depth);
    for (int i = 0; i < nfilter; i++) genClause(b.add_filter(), depth);
    // min_match is only valid for optional-only clauses (no required/filter).
    if (nopt > 0 && nreq == 0 && nfilter == 0) {
      b.set_min_match((int)rng.rint(nopt + 1));  // 0..nopt
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

  static bool clauseMatches(const proto::Query& q, const std::vector<std::string>& toks) {
    switch (q.kind_case()) {
      case proto::Query::kMatch:
        return tokensContain(toks, q.match().val().s());
      case proto::Query::kPhrase: {
        std::vector<std::string> phrase(q.phrase().words().begin(), q.phrase().words().end());
        return tokensContainPhrase(toks, phrase);
      }
      case proto::Query::kBoolean:
        return boolMatches(q.boolean(), toks);
      case proto::Query::kAll:
        return true;
      default:
        return false;
    }
  }

  static bool boolMatches(const proto::BooleanQuery& b, const std::vector<std::string>& toks) {
    for (const auto& r : b.required()) {
      if (!clauseMatches(r, toks)) return false;
    }
    for (const auto& f : b.filter()) {  // filters constrain like required (no score)
      if (!clauseMatches(f, toks)) return false;
    }
    for (const auto& p : b.prohibited()) {
      if (clauseMatches(p, toks)) return false;
    }
    bool hasPositive = b.required_size() > 0 || b.optional_size() > 0 || b.filter_size() > 0;
    if (!hasPositive) return false;
    // Optional must match unless there is a mandatory (required) clause, which
    // makes the optional side scoring-only (MandOpt). A filter does NOT relax
    // that - filter + optional conjoins them, so the optional is still required.
    if (b.optional_size() > 0 && b.required_size() == 0) {
      int matched = 0;
      for (const auto& o : b.optional()) {
        if (clauseMatches(o, toks)) matched++;
      }
      int mm = b.min_match();
      int eff = mm >= 1 ? std::min(mm, b.optional_size()) : 1;  // unset/0 -> any (>=1)
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
    auto* req = LocalReq::create(helper.getSearchEngine());
    req->collection("main");
    auto& td = req->topDocs("q");
    td.set_limit(100);
    *td.mutable_fields()->Add() = "id";
    auto& b = *td.mutable_query()->mutable_boolean();
    auto& f = *b.add_filter()->mutable_match();
    f.set_field("body_w");
    f.mutable_val()->set_s(filterTerm);
    auto& o = *b.add_optional()->mutable_match();
    o.set_field("body_w");
    o.mutable_val()->set_s(optTerm);
    req->execute();
    std::set<std::string> got;
    for (const auto& d : req->getDocs()) got.insert(std::get<std::string>(*find(d, "id")));
    req->done();
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

  auto* req = LocalReq::create(helper.getSearchEngine());
  req->collection("main");
  auto& td = req->topDocs("q");
  td.set_get_number(true);
  td.set_limit(100);
  *td.mutable_fields()->Add() = "id";
  auto& b = *td.mutable_query()->mutable_boolean();
  { auto& m = *b.add_required()->mutable_match(); m.set_field("body_w"); m.mutable_val()->set_s("b"); }
  { auto& m = *b.add_required()->mutable_match(); m.set_field("body_w"); m.mutable_val()->set_s("d"); }
  req->execute(false);
  std::set<std::string> got;
  for (const auto& d : req->getDocs()) got.insert(std::get<std::string>(*find(d, "id")));
  int64_t count = req->getMatchCount();
  req->done();

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
    auto* req = LocalReq::create(helper.getSearchEngine());
    req->collection("main");
    auto& td = req->topDocs("q");
    td.set_limit(100);
    *td.mutable_fields()->Add() = "id";
    auto& ph = *td.mutable_query()->mutable_phrase();
    ph.set_field("body_w");
    *ph.mutable_words()->Add() = "a";
    *ph.mutable_words()->Add() = "e";
    req->execute(false);
    std::set<std::string> got;
    for (const auto& d : req->getDocs()) got.insert(std::get<std::string>(*find(d, "id")));
    req->done();
    EXPECT_EQ((std::set<std::string>{"p1", "p3"}), got) << "standalone phrase";
  }

  // required e AND phrase "a e"
  {
    auto* req = LocalReq::create(helper.getSearchEngine());
    req->collection("main");
    auto& td = req->topDocs("q");
    td.set_limit(100);
    *td.mutable_fields()->Add() = "id";
    auto& b = *td.mutable_query()->mutable_boolean();
    { auto& m = *b.add_required()->mutable_match(); m.set_field("body_w"); m.mutable_val()->set_s("e"); }
    { auto& ph = *b.add_required()->mutable_phrase(); ph.set_field("body_w");
      *ph.mutable_words()->Add() = "a"; *ph.mutable_words()->Add() = "e"; }
    req->execute(false);
    std::set<std::string> got;
    for (const auto& d : req->getDocs()) got.insert(std::get<std::string>(*find(d, "id")));
    req->done();
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
    auto* req = LocalReq::create(helper.getSearchEngine());
    req->collection("main");
    auto& td = req->topDocs("q");
    td.set_get_number(true);
    td.set_limit(numDocs);          // return every match, not just a top page
    td.set_batch_size(numDocs + 1);  // in one response batch (we only read responses[0])
    *td.mutable_fields()->Add() = "id";
    genBool(td.mutable_query(), 2);

    std::set<std::string> expected;
    for (const auto& [id, toks] : docs) {
      if (boolMatches(td.query().boolean(), toks)) expected.insert(id);
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
                    << "\ndiff docs:\n" << diff << "query=\n" << td.query().DebugString();
      req->done();
      break;
    }
    req->done();
  }
}
