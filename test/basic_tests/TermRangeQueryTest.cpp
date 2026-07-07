#include <gtest/gtest.h>

#include <algorithm>
#include <optional>
#include <string>
#include <vector>

#include "test/SoluxTest.h"
#include "test/TestIndex.h"
#include "test/CollectionHelper.h"
#include "test/LocalReq.h"
#include "solux/query/TermRangeQuery.h"
#include "solux/query/QueryBuilder.h"
#include "solux/schema/Schema.h"

using namespace solux;
using namespace solux::test;

// Drive TermRangeQuery::Weight::createScorer directly to assert per-segment
// doc ids (the PrefixQueryTest shape; the two queries share the multi-term
// machinery and differ only in endpoint specification).
class TermRangeQueryTest : public SoluxTest {
protected:
  std::vector<int32_t> rangeDocs(TestIndex& ti, std::string_view field,
                                 std::optional<std::string_view> lower, bool includeLower,
                                 std::optional<std::string_view> upper, bool includeUpper,
                                 int segOrd) {
    auto g = ti.pool.rewindScopeGuard();
    TermRangeQuery trq(field, lower, includeLower, upper, includeUpper);
    Query::Context ctx(ti.pool, *ti.reader);
    auto* weight = trq.createWeight(ctx, 0);
    Query::Scorer* scorer = weight->createScorer(ti.pool, ctx.topReader.segments()[segOrd]);
    std::vector<int32_t> docs;
    if (scorer != nullptr) {
      for (int32_t d = scorer->next(); d != PostingsReader::END; d = scorer->next()) {
        docs.push_back(d);
      }
    }
    return docs;
  }
};

TEST_F(TermRangeQueryTest, endpoints) {
  TestIndex ti;
  TestField f(ti, "foo_w");
  f.startIndexing();
  f.add(0, "apple");
  f.add(1, "banana");
  f.add(2, "cherry");
  f.add(3, "apricot");
  f.add(4, "banana cherry");
  ti.flush();
  f.startReading();

  using D = std::vector<int32_t>;
  // inclusive both ends
  EXPECT_EQ((D{0, 1, 3, 4}), rangeDocs(ti, "foo_w", "apple", true, "banana", true, 0));
  // exclusive lower drops the exact term
  EXPECT_EQ((D{1, 3, 4}), rangeDocs(ti, "foo_w", "apple", false, "banana", true, 0));
  // exclusive upper drops the exact term
  EXPECT_EQ((D{0, 3}), rangeDocs(ti, "foo_w", "apple", true, "banana", false, 0));
  // open ends
  EXPECT_EQ((D{1, 2, 4}), rangeDocs(ti, "foo_w", "banana", true, std::nullopt, true, 0));
  EXPECT_EQ((D{0, 3}), rangeDocs(ti, "foo_w", std::nullopt, true, "b", false, 0));
  EXPECT_EQ((D{0, 1, 2, 3, 4}),
            rangeDocs(ti, "foo_w", std::nullopt, true, std::nullopt, true, 0));
  // endpoints between terms bound by byte order
  EXPECT_EQ((D{1, 4}), rangeDocs(ti, "foo_w", "b", true, "c", false, 0));
  // empty range: lower past every candidate
  EXPECT_TRUE(rangeDocs(ti, "foo_w", "zebra", true, std::nullopt, true, 0).empty());
  EXPECT_TRUE(rangeDocs(ti, "foo_w", "banana", false, "banana", true, 0).empty());
}

TEST_F(TermRangeQueryTest, multiSegment) {
  TestIndex ti;
  TestField f(ti, "foo_w");
  f.startIndexing();
  f.add(0, "alpha");
  f.add(1, "mike");
  ti.flush();
  f.startIndexing();
  f.add(0, "november");
  f.add(1, "zulu");
  ti.flush();
  f.startReading();

  using D = std::vector<int32_t>;
  EXPECT_EQ((D{1}), rangeDocs(ti, "foo_w", "m", true, "n", false, 0));
  EXPECT_EQ((D{0}), rangeDocs(ti, "foo_w", "m", true, "nz", false, 1));
}

// End-to-end through the range arm: the builder picks term ranges for
// term-backed fields and numeric ranges for columns from the same wire shape.
class TermRangeE2ETest : public SoluxTest {
public:
  CollectionHelper helper;

  TermRangeE2ETest() {
    helper.clear();
    helper.index(flatdoc("id", "d1", "tag_s", "action", "title_wl", "Alpha"),
                 UpdateMessage::NO_COMMIT);
    helper.index(flatdoc("id", "d2", "tag_s", "drama", "title_wl", "Mike"),
                 UpdateMessage::NO_COMMIT);
    helper.index(flatdoc("id", "d3", "tag_s", "scifi", "title_wl", "Zulu"),
                 UpdateMessage::COMMIT);
  }

  std::vector<std::string> rangeIds(std::string_view field, const char* gteV, const char* lteV,
                                    bool loIncl = true, bool hiIncl = true) {
    auto req = localReq(helper.getSearchEngine());
    auto& cur = req->collection("main").topDocs("q");
    auto& mr = cur.mr();
    auto& r = cur.rawQuery().kind.emplace<solux::api::RangeQuery>();
    r.field = build::arenaStr(mr, field);
    auto mkVal = [&](const char* text) {
      auto* v = (solux::api::Val*)mr.allocate(sizeof(solux::api::Val), alignof(solux::api::Val));
      new (v) solux::api::Val();
      v->kind = build::arenaStr(mr, text);
      return v;
    };
    if (gteV != nullptr) {
      if (loIncl) r.gte = mkVal(gteV); else r.gt = mkVal(gteV);
    }
    if (lteV != nullptr) {
      if (hiIncl) r.lte = mkVal(lteV); else r.lt = mkVal(lteV);
    }
    cur.fields({"id"}).limit(100);
    req->execute();
    EXPECT_TRUE(req->ok()) << req->errorMsg();
    std::vector<std::string> ids;
    for (auto& doc : req->getDocs()) {
      if (auto* v = find(doc, "id")) ids.push_back(std::get<std::string>(*v));
    }
    std::sort(ids.begin(), ids.end());
    return ids;
  }
};

TEST_F(TermRangeE2ETest, stringField) {
  using S = std::vector<std::string>;
  EXPECT_EQ((S{"d1", "d2"}), rangeIds("tag_s", "action", "drama"));
  EXPECT_EQ((S{"d2"}), rangeIds("tag_s", "action", "drama", false, true));
  EXPECT_EQ((S{"d2", "d3"}), rangeIds("tag_s", "b", nullptr));
  EXPECT_EQ((S{"d1"}), rangeIds("tag_s", nullptr, "b", true, false));
}

TEST_F(TermRangeE2ETest, textEndpointsAreNormalized) {
  using S = std::vector<std::string>;
  // TEXT endpoints fold like the field folds: [Alpha TO Mike] finds the
  // lowercased indexed terms
  EXPECT_EQ((S{"d1", "d2"}), rangeIds("title_wl", "Alpha", "Mike"));
  EXPECT_EQ((S{"d2", "d3"}), rangeIds("title_wl", "M", nullptr));
}

TEST_F(TermRangeE2ETest, constantScoring) {
  auto req = localReq(helper.getSearchEngine());
  auto& cur = req->collection("main").topDocs("q");
  auto& mr = cur.mr();
  auto& r = cur.rawQuery().kind.emplace<solux::api::RangeQuery>();
  r.field = build::arenaStr(mr, "tag_s");
  cur.withStats().fields({"id"}).limit(100);
  req->execute();
  ASSERT_TRUE(req->ok()) << req->errorMsg();
  const auto& docs = *req->docList("q");
  const auto& scores = std::get<solux::api::ColFloat>(docs.columns.at("_score_").kind).v;
  ASSERT_EQ(3u, scores.size());
  for (float s : scores) {
    EXPECT_FLOAT_EQ(scores[0], s);  // every match scores the same
  }
}
