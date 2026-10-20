// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

// Elasticsearch-style sort options: `missing` (_first, _last, or a literal)
// and multi-value `mode` (min, max, sum, avg, median).

#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "luxir/server/JsonRequest.h"
#include "test/CollectionHelper.h"
#include "test/LocalReq.h"
#include "test/LuxirTest.h"
#include "test/TestUtils.h"

using namespace luxir;
using namespace luxir::test;

class SortOptionsTest : public LuxirTest {
protected:
  // Run one top-docs request whose sort list is `sort` (JSON).
  LocalReqHandle search(CollectionHelper& helper, const std::string& sort,
                        int64_t limit = 10, bool parallel = true) {
    auto req = localReq(helper.getSearchEngine());
    std::string body = R"({"limit":)" + std::to_string(limit)
        + R"(,"fields":["id"],"sort":)" + sort + "}";
    parseQueryRequest(body, req->rawRequest(), req->mr);
    req->collection("main");
    req->execute(parallel);
    return req;
  }

  // The ranked ids, which the parallel lane (per-segment collectors merged)
  // and the serial lane (one collector across segments) must agree on.
  std::vector<std::string> ids(CollectionHelper& helper, const std::string& sort,
                               int64_t limit = 10) {
    std::vector<std::string> lanes[2];
    for (bool parallel : {true, false}) {
      auto req = search(helper, sort, limit, parallel);
      EXPECT_TRUE(req->ok()) << sort << ": " << req->errorMsg();
      const auto* docs = req->docList("q");
      const auto* col = docs == nullptr ? nullptr : docs->columns.find("id");
      if (col == nullptr) continue;
      for (auto id : std::get<api::ColStr>(col->kind).v) lanes[parallel].emplace_back(id);
    }
    EXPECT_EQ(lanes[1], lanes[0]) << sort;
    return lanes[1];
  }

  std::string error(CollectionHelper& helper, const std::string& sort) {
    auto req = search(helper, sort);
    EXPECT_FALSE(req->ok()) << sort;
    EXPECT_EQ(ErrorKind::INVALID_REQUEST, req->errorKind()) << sort;
    return req->errorMsg();
  }

  void checkStringMissing(bool segmented);

  // One doc per segment, so every comparison also crosses segments.
  static void indexSegments(CollectionHelper& helper, const std::vector<Doc>& docs) {
    for (const auto& doc : docs) helper.index(doc, UpdateMessage::COMMIT);
  }
};

using V = std::vector<std::string>;

TEST_F(SortOptionsTest, numericMissingEdgesAndLiteral) {
  CollectionHelper helper;
  indexSegments(helper, {flatdoc("id", "ten", "price_i", 10),
                         flatdoc("id", "five", "price_i", 5),
                         flatdoc("id", "none")});

  auto clause = [](std::string dir, std::string missing) {
    return R"({"field":"price_i","dir":")" + dir + R"(","missing":)" + missing + "}";
  };
  EXPECT_EQ((V{"five", "ten", "none"}), ids(helper, clause("asc", R"("_last")")));
  EXPECT_EQ((V{"ten", "five", "none"}), ids(helper, clause("desc", R"("_last")")));
  EXPECT_EQ((V{"none", "five", "ten"}), ids(helper, clause("asc", R"("_first")")));
  EXPECT_EQ((V{"none", "ten", "five"}), ids(helper, clause("desc", R"("_first")")));
  EXPECT_EQ((V{"five", "none", "ten"}), ids(helper, clause("asc", "7")));
  EXPECT_EQ((V{"ten", "none", "five"}), ids(helper, clause("desc", "7")));
  // A literal equal to a value ties with it; index order breaks the tie.
  EXPECT_EQ((V{"five", "none", "ten"}), ids(helper, clause("asc", "5")));
  EXPECT_EQ((V{"none", "five", "ten"}), ids(helper, clause("asc", "-1")));
  // Paging over the literal keeps one consistent order.
  EXPECT_EQ((V{"five"}), ids(helper, clause("asc", "7"), 1));
}

TEST_F(SortOptionsTest, dateAndDoubleLiterals) {
  CollectionHelper helper;
  indexSegments(helper, {flatdoc("id", "y2020", "when_dt", "2020-01-01", "x_d", 1.5),
                         flatdoc("id", "y2024", "when_dt", "2024-01-01", "x_d", 2.5),
                         flatdoc("id", "none")});
  EXPECT_EQ((V{"y2020", "none", "y2024"}),
            ids(helper, R"({"field":"when_dt","missing":"2022-06-01"})"));
  EXPECT_EQ((V{"y2020", "none", "y2024"}),
            ids(helper, R"({"field":"x_d","missing":2.0})"));
  EXPECT_EQ((V{"none", "y2024", "y2020"}),
            ids(helper, R"({"field":"x_d","dir":"desc","missing":9})"));
}

// Global ords cannot place a literal no segment holds, so string literals
// compare per segment; check both a single-segment and a multi-segment index.
void SortOptionsTest::checkStringMissing(bool segmented) {
  CollectionHelper helper;
  std::vector<Doc> docs{flatdoc("id", "pear", "fruit_s", "pear"),
                        flatdoc("id", "apple", "fruit_s", "apple"),
                        flatdoc("id", "none")};
  if (segmented) {
    indexSegments(helper, docs);
  } else {
    for (const auto& doc : docs) helper.index(doc, UpdateMessage::NO_COMMIT);
    helper.commit();
  }

  auto clause = [](std::string dir, std::string missing) {
    return R"({"field":"fruit_s","dir":")" + dir + R"(","missing":)" + missing + "}";
  };
  EXPECT_EQ((V{"apple", "pear", "none"}), ids(helper, clause("asc", R"("_last")")));
  EXPECT_EQ((V{"none", "apple", "pear"}), ids(helper, clause("asc", R"("_first")")));
  EXPECT_EQ((V{"none", "pear", "apple"}), ids(helper, clause("desc", R"("_first")")));
  EXPECT_EQ((V{"apple", "none", "pear"}), ids(helper, clause("asc", R"("banana")")));
  EXPECT_EQ((V{"pear", "none", "apple"}), ids(helper, clause("desc", R"("banana")")));
  // A literal the index holds ties with that value.
  EXPECT_EQ((V{"apple", "pear", "none"}), ids(helper, clause("asc", R"("pear")")));
  EXPECT_EQ((V{"none", "apple", "pear"}), ids(helper, clause("asc", R"("aaa")")));
  EXPECT_EQ((V{"apple", "pear", "none"}), ids(helper, clause("asc", R"("zzz")")));
  EXPECT_EQ((V{"apple"}), ids(helper, clause("asc", R"("banana")"), 1));
}

TEST_F(SortOptionsTest, stringMissingAcrossSegments) { checkStringMissing(true); }

TEST_F(SortOptionsTest, stringMissingInOneSegment) { checkStringMissing(false); }

TEST_F(SortOptionsTest, numericModes) {
  CollectionHelper helper;
  indexSegments(helper, {flatdoc("id", "a", "v_is", vec_i(9, 4)),
                         flatdoc("id", "b", "v_is", vec_i(8, 7)),
                         flatdoc("id", "c", "v_is", vec_i(1, 20)),
                         flatdoc("id", "e", "v_is", vec_i(7)),
                         flatdoc("id", "f", "v_is", vec_i(1, 2, 100)),
                         flatdoc("id", "none")});

  auto clause = [](std::string dir, std::string mode) {
    std::string out = R"({"field":"v_is","dir":")" + dir + "\"";
    if (!mode.empty()) out += R"(,"mode":")" + mode + "\"";
    return out + "}";
  };
  // Defaults: min ascending, max descending.
  EXPECT_EQ((V{"c", "f", "a", "b", "e", "none"}), ids(helper, clause("asc", "")));
  EXPECT_EQ((V{"c", "f", "a", "b", "e", "none"}), ids(helper, clause("asc", "min")));
  EXPECT_EQ((V{"f", "c", "a", "b", "e", "none"}), ids(helper, clause("desc", "")));
  EXPECT_EQ((V{"f", "c", "a", "b", "e", "none"}), ids(helper, clause("desc", "max")));
  EXPECT_EQ((V{"e", "b", "a", "c", "f", "none"}), ids(helper, clause("asc", "max")));
  EXPECT_EQ((V{"b", "e", "a", "c", "f", "none"}), ids(helper, clause("desc", "min")));
  // sum: a13 b15 c21 e7 f103; avg: a6.5 b7.5 c10.5 e7 f34.3; median: f2.
  EXPECT_EQ((V{"e", "a", "b", "c", "f", "none"}), ids(helper, clause("asc", "sum")));
  EXPECT_EQ((V{"f", "c", "b", "a", "e", "none"}), ids(helper, clause("desc", "sum")));
  EXPECT_EQ((V{"a", "e", "b", "c", "f", "none"}), ids(helper, clause("asc", "avg")));
  EXPECT_EQ((V{"f", "a", "e", "b", "c", "none"}), ids(helper, clause("asc", "median")));
  // Missing combines with reducing modes.
  EXPECT_EQ((V{"e", "a", "none", "b", "c", "f"}),
            ids(helper, R"({"field":"v_is","mode":"sum","missing":14})"));
  EXPECT_EQ((V{"none", "e", "a", "b", "c", "f"}),
            ids(helper, R"({"field":"v_is","mode":"sum","missing":"_first"})"));
  // An array-valued expression takes a mode like a bare field.
  EXPECT_EQ((V{"f", "c", "a", "b", "e", "none"}),
            ids(helper, R"({"expr":"v_is * 2","dir":"desc","mode":"max"})"));
}

TEST_F(SortOptionsTest, stringModes) {
  CollectionHelper helper;
  indexSegments(helper, {flatdoc("id", "a", "tags_ss", vecs("m", "x")),
                         flatdoc("id", "b", "tags_ss", vecs("b", "y"))});
  EXPECT_EQ((V{"b", "a"}), ids(helper, R"({"field":"tags_ss"})"));
  EXPECT_EQ((V{"a", "b"}), ids(helper, R"({"field":"tags_ss","mode":"max"})"));
  EXPECT_EQ((V{"b", "a"}), ids(helper, R"({"field":"tags_ss","dir":"desc"})"));
  EXPECT_EQ((V{"a", "b"}), ids(helper, R"({"field":"tags_ss","dir":"desc","mode":"min"})"));
}

TEST_F(SortOptionsTest, expressionMissing) {
  CollectionHelper helper;
  indexSegments(helper, {flatdoc("id", "ten", "price_i", 10),
                         flatdoc("id", "five", "price_i", 5),
                         flatdoc("id", "none")});
  EXPECT_EQ((V{"none", "five", "ten"}),
            ids(helper, R"({"expr":"price_i + 1","missing":"_first"})"));
  EXPECT_EQ((V{"none", "ten", "five"}),
            ids(helper, R"({"expr":"price_i + 1","dir":"desc","missing":"_first"})"));
  EXPECT_EQ((V{"five", "none", "ten"}),
            ids(helper, R"({"expr":"price_i + 1","missing":8})"));
  EXPECT_EQ((V{"five", "none", "ten"}),
            ids(helper, R"({"expr":"price_i / 1","missing":7.5})"));
}

TEST_F(SortOptionsTest, invalidCombinationsAreRequestErrors) {
  CollectionHelper helper;
  indexSegments(helper, {flatdoc("id", "a", "fruit_s", "pear", "v_is", vec_i(1, 2),
                                 "when_dt", "2020-01-01", "price_i", 3)});
  EXPECT_NE(std::string::npos,
            error(helper, R"({"field":"fruit_s","mode":"sum"})").find("requires a numeric"));
  EXPECT_NE(std::string::npos,
            error(helper, R"({"field":"fruit_s","mode":"avg"})").find("requires a numeric"));
  EXPECT_NE(std::string::npos,
            error(helper, R"({"field":"when_dt","mode":"sum"})").find("DATE"));
  EXPECT_NE(std::string::npos,
            error(helper, R"({"expr":"score","mode":"max"})").find("one value per document"));
  EXPECT_NE(std::string::npos,
            error(helper, R"({"expr":"_docid_","missing":"_first"})").find("one value"));
  error(helper, R"({"field":"price_i","missing":"cheap"})");
  error(helper, R"({"expr":"price_i + 1","missing":1.5})");
  // An unknown mode fails to parse.
  auto req = localReq(helper.getSearchEngine());
  EXPECT_ANY_THROW(parseQueryRequest(R"({"sort":{"field":"price_i","mode":"middle"}})",
                                     req->rawRequest(), req->mr));
}
