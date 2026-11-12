// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

// CombinedFieldQuery batch scoring and block-max pruning: top-k with a
// competitive threshold must match exhaustive scoring.

#include <charconv>
#include <cmath>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "luxir/util/random.h"
#include "test/CollectionHelper.h"
#include "test/LocalReq.h"
#include "test/LuxirTest.h"
#include "test/QueryBuild.h"

using namespace luxir;
using namespace luxir::test;

using Mode = api::MultiMatchQuery_::Mode;
using MatchOp = api::Match_::Operator;

namespace {

struct Hit {
  int64_t id;
  float score;
};

class CombinedFieldScorerTest : public LuxirTest {
public:
  CollectionHelper helper{"combined_scorer"};

  // Docs 0..199 hold "hot" often in short fields, so they fill the top-k
  // early; later docs hold it once in long bodies, whose block bounds fall
  // below the threshold. Other terms are skewed draws from a small vocab.
  void SetUp() override {
    helper.clear();
    auto iw = helper.getIndexWriter();
    for (int seg = 0; seg < 2; seg++) {
      Inverter& inverter = iw->obtainInverter();
      auto& id = inverter.getIndexHandler("id");
      auto& title = inverter.getIndexHandler("title_w");
      auto& body = inverter.getIndexHandler("body_w");
      auto& tag = inverter.getIndexHandler("tag_w");
      std::string text;
      auto words = [&](SplitMix64& r, int n) {
        for (int t = 0; t < n; t++) {
          text += " w";
          int64_t a = r.rint(300), b = r.rint(300);
          text += std::to_string(std::min(a, b));
        }
      };
      for (int64_t doc = seg * 20000; doc < (seg + 1) * 20000; doc++) {
        SplitMix64 r((uint64_t)doc * 7919);
        bool early = doc % 20000 < 200;
        inverter.startDoc();
        id.index(inverter, std::to_string(doc));
        text = early ? "hot hot" : "";
        words(r, 1 + (int)r.rint(6));
        title.index(inverter, text);
        text = early ? "hot hot hot" : (r.rint(2) == 0 ? "hot" : "");
        words(r, early ? 3 : 40 + (int)r.rint(40));
        body.index(inverter, text);
        if (r.rint(4) == 0) {
          text = "";
          words(r, 1 + (int)r.rint(3));
          tag.index(inverter, text);
        }
        inverter.finishDoc();
      }
      iw->releaseInverter(inverter, true);
    }
    helper.commit();
  }

  std::vector<Hit> run(std::string_view text, bool threeFields, MatchOp op, bool exhaustive) {
    auto req = localReq(helper.getSearchEngine());
    auto& cur = req->collection("combined_scorer").topDocs("q");
    if (threeFields) {
      cur.rawQuery() = qb::multiMatch(cur.mr(), text,
          {{"title_w", 2.0f}, {"body_w", {}}, {"tag_w", 1.5f}}, Mode::COMBINED, op);
    } else {
      cur.rawQuery() = qb::multiMatch(cur.mr(), text, {{"title_w", 2.0f}, {"body_w", {}}},
                                      Mode::COMBINED, op);
    }
    cur.fields({"id"}).limit(20).getScores();
    if (exhaustive) cur.countLimit();
    req->execute(false);
    EXPECT_TRUE(req->ok()) << req->errorMsg();
    std::vector<Hit> hits;
    const auto* docs = req->docList();
    if (docs == nullptr || docs->row_count == 0) return hits;
    const auto& ids = std::get<api::ColStr>(docs->columns.at("id").kind).v;
    const auto& scores = std::get<api::ColFloat>(docs->columns.at("_score_").kind).v;
    for (size_t i = 0; i < ids.size(); i++) {
      int64_t id = 0;
      std::from_chars(ids[i].data(), ids[i].data() + ids[i].size(), id);
      hits.push_back({id, scores[i]});
    }
    return hits;
  }

  // Equal scores per rank, and equal ids except within tie groups.
  void expectSameTopK(std::string_view text, bool threeFields, MatchOp op) {
    auto expected = run(text, threeFields, op, true);
    auto actual = run(text, threeFields, op, false);
    ASSERT_EQ(expected.size(), actual.size()) << text;
    ASSERT_FALSE(expected.empty()) << text;
    for (size_t i = 0; i < expected.size(); i++) {
      EXPECT_NEAR(expected[i].score, actual[i].score, 1e-5f * expected[i].score) << text << " rank " << i;
      bool tied = (i > 0 && expected[i - 1].score == expected[i].score)
          || (i + 1 < expected.size() && expected[i + 1].score == expected[i].score);
      if (!tied) {
        EXPECT_EQ(expected[i].id, actual[i].id) << text << " rank " << i;
      }
    }
  }
};

TEST_F(CombinedFieldScorerTest, prunedTopKMatchesExhaustive) {
  for (bool threeFields : {false, true}) {
    expectSameTopK("hot", threeFields, MatchOp::OPERATOR_UNSPECIFIED);
    expectSameTopK("w3", threeFields, MatchOp::OPERATOR_UNSPECIFIED);
    expectSameTopK("hot w5", threeFields, MatchOp::OPERATOR_UNSPECIFIED);
    expectSameTopK("w1 w40", threeFields, MatchOp::OPERATOR_UNSPECIFIED);
    expectSameTopK("w2 w9 w150", threeFields, MatchOp::OPERATOR_UNSPECIFIED);
    expectSameTopK("hot w2", threeFields, MatchOp::AND);
  }
}

}  // namespace
