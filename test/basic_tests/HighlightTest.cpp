// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include "luxir/server/JsonRequest.h"
#include "luxir/server/JsonResponse.h"
#include "test/CollectionHelper.h"
#include "test/LocalReq.h"
#include "test/LuxirTest.h"
#include "test/TestUtils.h"

using namespace luxir;
using namespace luxir::test;

class HighlightTest : public LuxirTest {
protected:
  LocalReqHandle run(std::string_view body, bool ok = true) {
    auto request = localReq(luxirNode->getSearchEngine());
    parseQueryRequest(body, request->rawRequest(), request->mr);
    request->collection("highlight");
    request->execute();
    EXPECT_EQ(ok, request->ok()) << request->errorMsg() << body;
    return request;
  }

  // Fragments of `field` for row `row` of op `op` (empty when absent).
  static std::vector<std::string> fragments(const LocalReq& request, size_t row,
                                            std::string_view field = "body_t",
                                            std::string_view op = "q") {
    std::vector<std::string> out;
    const auto* list = request.docList(op);
    if (!list || row >= list->highlight.size()) return out;
    if (const auto* val = list->highlight[row].fields.find(field)) {
      for (auto fragment : std::get<api::ArrStr>((*val)->kind).v) out.emplace_back(fragment);
    }
    return out;
  }

  void SetUp() override {
    CollectionHelper helper("highlight");
    std::string filler;
    for (int i = 0; i < 30; i++) filler += "filler ";
    ASSERT_TRUE(helper.indexAll({
        flatdoc("id", "1", "body_t", "a lazy cat, a dog and a lazy dog jumps"),
        flatdoc("id", "2", "body_t", "wildcard prefix fuzzy matching", "tag_s", "x"),
        flatdoc("id", "3", "body_t", filler + "needle <x> " + filler + "needle"),
    }, UpdateMessage::COMMIT).success);
  }
};

TEST_F(HighlightTest, termsStemsAndExactPhrases) {
  auto request = run(R"({"query":"body_t:jumping OR body_t:\"lazy dog\"","fields":["id"],)"
      R"("highlight":{"fields":["body_t"],"number_of_fragments":0}})");
  ASSERT_EQ(1, request->docList()->row_count);
  // The stemmed term marks "jumps"; the phrase marks only its adjacent pair.
  EXPECT_EQ(std::vector<std::string>{"a lazy cat, a dog and a <em>lazy</em> <em>dog</em> <em>jumps</em>"},
            fragments(*request, 0));
}

TEST_F(HighlightTest, multiTermQueriesMatchAnalyzedTokens) {
  auto request = run(R"({"query":{"boolean":{"optional":[)"
      R"({"prefix":{"field":"body_t","prefix":"pref"}},)"
      R"({"wildcard":{"field":"body_t","pattern":"wild*rd"}},)"
      R"({"fuzzy":{"field":"body_t","term":"fuzzi","max_edits":1}}]}},)"
      R"("fields":["id"],"highlight":{"fields":["*"],"number_of_fragments":0}})");
  ASSERT_EQ(1, request->docList()->row_count);
  EXPECT_EQ(std::vector<std::string>{"<em>wildcard</em> <em>prefix</em> <em>fuzzy</em> matching"},
            fragments(*request, 0));
}

TEST_F(HighlightTest, fragmentsAreBoundedEscapedAndOrdered) {
  std::string spec = R"("highlight":{"fields":["body_t"],"fragment_size":40,"pre_tag":"[",)"
      R"("post_tag":"]","encoder":"html","number_of_fragments":)";
  auto one = run(R"({"query":"body_t:needle","fields":["id"],)" + spec + "1}}");
  auto best = fragments(*one, 0);
  ASSERT_EQ(1u, best.size());
  // Equal scores keep the earlier window; edges snap to whitespace.
  EXPECT_TRUE(best[0].starts_with("filler [needle] &lt;x&gt; filler")) << best[0];
  EXPECT_FALSE(best[0].ends_with(" "));
  EXPECT_LT(best[0].size(), 60u);
  auto two = run(R"({"query":"body_t:needle","fields":["id"],)" + spec + "2}}");
  auto both = fragments(*two, 0);
  ASSERT_EQ(2u, both.size());
  EXPECT_EQ(best[0], both[0]);
  EXPECT_TRUE(both[1].ends_with("filler [needle]")) << both[1];
}

TEST_F(HighlightTest, responseShapesFusionAndErrors) {
  auto request = run(R"({"query":"body_t:dog OR tag_s:x","fields":["id"],"sort":"id asc",)"
      R"("highlight":{"fields":["body_t"],"number_of_fragments":0}})");
  ASSERT_EQ(2, request->docList()->row_count);
  EXPECT_EQ(1u, fragments(*request, 0).size());
  EXPECT_TRUE(fragments(*request, 1).empty());  // matched by tag_s only
  auto body = renderSearchResponseBody(request->responses[0]->proto, true);
  EXPECT_NE(std::string::npos, body.find(
      R"({"id":"1","_highlight_":{"body_t":["a lazy cat, a <em>dog</em> and a lazy <em>dog</em> jumps"]}})"))
      << body;
  EXPECT_NE(std::string::npos, body.find(R"({"id":"2"})")) << body;

  auto fusion = run(R"({"ops":{"f":{"fusion":{"sources":{"a":{"query":"body_t:cat"},)"
      R"("b":{"query":"body_t:jumping"}},"rrf":{},"fields":["id"],)"
      R"("highlight":{"fields":["body_t"],"number_of_fragments":0}}}}})");
  EXPECT_EQ(std::vector<std::string>{"a lazy <em>cat</em>, a dog and a lazy dog <em>jumps</em>"},
            fragments(*fusion, 0, "body_t", "f"));

  auto unstored = run(R"({"query":"tag_s:x","highlight":{"fields":["tag_s"]}})", false);
  EXPECT_EQ("invalid_field", unstored->errorCode());
  auto badSize = run(R"({"query":"body_t:dog","highlight":{"fragment_size":0}})", false);
  EXPECT_EQ("invalid_highlight", badSize->errorCode());
}
