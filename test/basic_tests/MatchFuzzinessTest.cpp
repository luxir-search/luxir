// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include <gtest/gtest.h>

#include <algorithm>
#include <functional>
#include <string>
#include <vector>

#include "test/CollectionHelper.h"
#include "test/LocalReq.h"
#include "test/LuxirTest.h"

using namespace luxir;
using namespace luxir::test;

// match with fuzziness: each analyzed term matches like a fuzzy query, and
// operator / min_match combine them as they do exact terms.
class MatchFuzzinessTest : public LuxirTest {
public:
  CollectionHelper helper;

  MatchFuzzinessTest() {
    helper.index(flatdoc("id", "d1", "body_w", "quick brown fox", "color_s", "red"),
                 UpdateMessage::NO_COMMIT);
    helper.index(flatdoc("id", "d2", "body_w", "quikc brwon fox", "color_s", "green"),
                 UpdateMessage::NO_COMMIT);
    helper.index(flatdoc("id", "d3", "body_w", "quick red fox", "count_i", 7),
                 UpdateMessage::NO_COMMIT);
    helper.index(flatdoc("id", "d4", "body_w", "slow green turtle"), UpdateMessage::COMMIT);
  }

  using Setup = std::function<void(api::Match&)>;

  std::vector<std::string> ids(std::string_view field, std::string_view text, const Setup& setup,
                               std::string* error = nullptr) {
    auto req = localReq(helper.getSearchEngine());
    auto& cur = req->collection("main").topDocs("q");
    cur.matchQuery(field, text).fields({"id"}).limit(100);
    setup(std::get<api::Match>(cur.rawQuery().kind));
    req->execute();
    if (error != nullptr) *error = req->ok() ? "" : req->errorMsg();
    EXPECT_TRUE(error != nullptr || req->ok()) << req->errorMsg();
    std::vector<std::string> out;
    for (auto& doc : req->getDocs()) {
      if (auto* v = find(doc, "id")) out.push_back(std::get<std::string>(*v));
    }
    std::sort(out.begin(), out.end());
    return out;
  }
};

using S = std::vector<std::string>;
using Op = api::Match_::Operator;

TEST_F(MatchFuzzinessTest, operatorCombinesFuzzyTerms) {
  auto andAuto = [](api::Match& m) {
    m.operator_ = Op::AND;
    m.fuzziness = "AUTO";
  };
  // "quikc" and "brwon" are one swap each from the query terms.
  EXPECT_EQ(ids("body_w", "quick brown", andAuto), (S{"d1", "d2"}));
  EXPECT_EQ(ids("body_w", "quick brown", [](api::Match& m) { m.fuzziness = "AUTO"; }),
            (S{"d1", "d2", "d3"}));
  // Without fuzziness the same AND keeps only the exact spelling.
  EXPECT_EQ(ids("body_w", "quick brown", [](api::Match& m) { m.operator_ = Op::AND; }),
            (S{"d1"}));
  // Without transpositions a swap costs two edits.
  EXPECT_EQ(ids("body_w", "quick brown", [&](api::Match& m) {
              andAuto(m);
              m.transpositions = false;
            }),
            (S{"d1"}));
}

TEST_F(MatchFuzzinessTest, minMatchCountsFuzzyTerms) {
  auto twoOfThree = [](api::Match& m) {
    m.fuzziness = "1";
    m.min_match = 2;
  };
  EXPECT_EQ(ids("body_w", "quick brown turtle", twoOfThree), (S{"d1", "d2"}));
  EXPECT_EQ(ids("body_w", "quick brown turtle", [](api::Match& m) { m.min_match = 2; }),
            (S{"d1"}));
  // The prefix_length option pins the first code points: "wuick" cannot reach "quick".
  EXPECT_EQ(ids("body_w", "wuick brawn", [](api::Match& m) {
              m.fuzziness = "1";
              m.operator_ = Op::AND;
              m.prefix_length = 0;
            }),
            (S{"d1"}));
  EXPECT_TRUE(ids("body_w", "wuick brawn", [](api::Match& m) {
                m.fuzziness = "1";
                m.operator_ = Op::AND;
              }).empty());
}

TEST_F(MatchFuzzinessTest, stringFieldAndErrors) {
  // The default prefix_length of 1 keeps the first letter exact.
  EXPECT_EQ(ids("color_s", "ted", [](api::Match& m) { m.fuzziness = "1"; }), S{});
  EXPECT_EQ(ids("color_s", "ted", [](api::Match& m) {
              m.fuzziness = "1";
              m.prefix_length = 0;
            }),
            (S{"d1"}));

  std::string error;
  ids("count_i", "7", [](api::Match& m) { m.fuzziness = "AUTO"; }, &error);
  EXPECT_NE(error.find("fuzziness"), std::string::npos) << error;
  ids("body_w", "quick", [](api::Match& m) { m.fuzziness = "AUTO:9"; }, &error);
  EXPECT_NE(error.find("fuzziness"), std::string::npos) << error;
  ids("body_w", "quick", [](api::Match& m) { m.prefix_length = 0; }, &error);
  EXPECT_NE(error.find("need fuzziness"), std::string::npos) << error;
}
