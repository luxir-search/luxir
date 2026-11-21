// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

// Segment pruning end to end: one root mask per op from the segment
// summaries, pruned execution answering exactly as unpruned execution
// (scores included), and the reporting around it.

#include <algorithm>
#include <atomic>
#include <gtest/gtest.h>
#include "luxir/index/IndexWriter.h"
#include "luxir/query/BooleanQuery.h"
#include "luxir/query/PhraseQuery.h"
#include "luxir/query/QueryPruning.h"
#include "luxir/query/TermQuery.h"
#include "luxir/search/FilterCache.h"
#include "luxir/search/IndexReader.h"
#include "luxir/server/JsonRequest.h"
#include "luxir/server/JsonResponse.h"
#include "luxir/util/Signal.h"
#include "test/CollectionHelper.h"
#include "test/LocalReq.h"
#include "test/LuxirTest.h"
#include "test/SchemaBuilder.h"
#include "test/TestUtils.h"

using namespace luxir;
using namespace luxir::test;

namespace {

class SegmentPruningTest : public LuxirTest {
protected:
  std::unique_ptr<CollectionHelper> helper;
  std::atomic<int64_t> prunedSegments{0};

  void SetUp() override {
    LuxirTest::SetUp();
    helper = std::make_unique<CollectionHelper>("prune");
    helper->clear();
    helper->getIndexWriter()->mergePolicy->setMergeFactor(100);
    QueryPruning::disableForTests = false;
    Signal::listen("searchTopDocsSegmentPruned", [this](void*, void*, void*) -> void* {
      prunedSegments.fetch_add(1);
      return nullptr;
    });
  }

  void TearDown() override {
    Signal::unlisten("searchTopDocsSegmentPruned");
    QueryPruning::disableForTests = false;
    helper.reset();
  }

  // Three segments: prices 1..3 with tag a, 11..13 with tag b, 21..23 with
  // tag c. Every body holds "common"; the first segment also "aardvark", the
  // second "kiwi", the third "zebra", so the term bounds prove aardvark and
  // zebra absent from every other segment (kiwi only from the first).
  void indexThreeSegments() {
    const char* words[] = {"aardvark common", "common kiwi", "common zebra"};
    for (int segment = 0; segment < 3; segment++) {
      std::vector<Doc> batch;
      for (int i = 0; i < 3; i++) {
        int price = segment * 10 + 1 + i;
        batch.push_back(flatdoc("id", std::to_string(price), "price_i", (int64_t)price,
                                "tag_s", std::string(1, (char)('a' + segment)),
                                "body_t", std::string(words[segment]) + (i == 0 ? " common" : "")));
      }
      ASSERT_TRUE(helper->indexAll(batch, UpdateMessage::COMMIT).success);
    }
  }

  std::shared_ptr<IndexReader> currentReader() {
    return helper->getIndexWriter()->snapshots.readers.getReader();
  }

  // The complete JSON response, scores included.
  std::string render(std::string_view json, bool profile = false) {
    auto req = localReq(helper->getSearchEngine());
    parseQueryRequest(json, req->rawRequest(), req->mr);
    req->collection("prune").profile(profile);
    req->execute();
    EXPECT_TRUE(req->ok()) << req->errorMsg();
    std::string out;
    for (auto* response : req->responses) {
      auto copy = response->proto;
      copy.profile.reset();
      // Ops appear in completion order; compare them by name.
      using Entry = std::remove_cvref_t<decltype(*copy.ops.begin())>;
      std::vector<Entry> ops(copy.ops.begin(), copy.ops.end());
      std::ranges::sort(ops, {}, [](const Entry& op) { return op.first; });
      copy.ops = decltype(copy.ops)(std::span<const Entry>(ops));
      out += renderSearchResponseBody(copy);
    }
    return out;
  }

  // Segments the request pruned, from the TopDocs pruned path.
  int64_t prunedBy(std::string_view json) {
    prunedSegments = 0;
    render(json);
    return prunedSegments.load();
  }

  void expectSameWithAndWithoutPruning(std::string_view json) {
    SCOPED_TRACE(std::string(json));
    QueryPruning::disableForTests = true;
    auto reference = render(json);
    QueryPruning::disableForTests = false;
    EXPECT_EQ(reference, render(json));
  }
};

std::string topDocs(std::string_view query, std::string_view extra = "") {
  return std::string(R"({"ops":{"q":{"top_docs":{"query":)") + std::string(query)
      + R"(,"limit":-1,"count_limit":-1,"get_scores":true,"fields":["id"])" + std::string(extra) + "}}}}";
}

} // namespace

TEST_F(SegmentPruningTest, resultsMatchUnprunedExecution) {
  indexThreeSegments();
  // A deleted document keeps widening its segment's bounds until a merge:
  // queries for it reach the segment and must still not match it.
  ASSERT_TRUE(helper->indexAll({flatdoc("id", "gone", "price_i", (int64_t)99, "tag_s", "z"),
                                flatdoc("id", "kept", "price_i", (int64_t)50, "tag_s", "y")},
                               UpdateMessage::COMMIT).success);
  ASSERT_TRUE(helper->deleteById("gone", UpdateMessage::COMMIT).success);
  ASSERT_EQ(4u, currentReader()->segments().size());

  for (std::string_view query : {
         R"({"all":true})",
         R"({"range":{"field":"price_i","gte":11,"lte":13}})",
         R"({"range":{"field":"price_i","gte":100}})",
         R"({"range":{"field":"price_i","gte":99,"lte":99}})",
         R"({"match":{"tag_s":"z"}})",
         R"({"match":{"tag_s":"b"}})",
         R"({"match":{"tag_s":"nope"}})",
         R"({"match":{"body_t":"kiwi"}})",
         R"({"match":{"body_t":"zebra"}})",
         R"({"match":{"body_t":"common"}})",
         R"({"prefix":{"field":"tag_s","prefix":"c"}})",
         R"({"exists":{"field":"never_i"}})",
         R"({"exists":{"field":"price_i"}})",
         R"({"phrase":{"field":"body_t","text":"common kiwi"}})",
         R"({"boolean":{"required":[{"match":{"body_t":"common"}}],"filter":[{"range":{"field":"price_i","gte":20}}]}})",
         R"({"boolean":{"optional":[{"match":{"tag_s":"a"}},{"match":{"tag_s":"nope"}}]}})",
         R"({"boolean":{"optional":[{"match":{"tag_s":"a"}},{"match":{"tag_s":"c"}}],"min_match":2}})",
         R"({"boolean":{"required":[{"match":{"body_t":"common"}}],"prohibited":[{"match":{"tag_s":"a"}}]}})",
       }) {
    expectSameWithAndWithoutPruning(topDocs(query));
  }
  EXPECT_EQ(3, prunedBy(topDocs(R"({"range":{"field":"price_i","gte":11,"lte":13}})")));
  EXPECT_EQ(0, prunedBy(topDocs(R"({"all":true})")));
}

TEST_F(SegmentPruningTest, scoresUseStatisticsOfEverySegment) {
  indexThreeSegments();
  // aardvark is provably absent from two segments: they skip its dictionary
  // seek and contribute zero frequency, so scores equal unpruned ones.
  std::string absentElsewhere = topDocs(R"({"match":{"body_t":"aardvark"}})");
  EXPECT_EQ(2, prunedBy(absentElsewhere));
  expectSameWithAndWithoutPruning(absentElsewhere);
  // A sibling filter prunes two segments; "common" is still seeked there,
  // so its statistics, and the field totals, cover all three.
  std::string conjunction = topDocs(
      R"({"boolean":{"required":[{"match":{"body_t":"common"}}],"filter":[{"range":{"field":"price_i","gte":11,"lte":13}}]}})");
  EXPECT_EQ(2, prunedBy(conjunction));
  expectSameWithAndWithoutPruning(conjunction);
  // The same with the filter as a TopDocs filter, a phrase, and a disjunction.
  std::string routed = topDocs(R"({"match":{"body_t":"common"}})",
                               R"(,"filter":[{"match":{"tag_s":"c"}}])");
  EXPECT_EQ(2, prunedBy(routed));
  expectSameWithAndWithoutPruning(routed);
  expectSameWithAndWithoutPruning(topDocs(R"({"phrase":{"field":"body_t","text":"common zebra"}})"));
  std::string disjunction =
      topDocs(R"({"boolean":{"optional":[{"match":{"body_t":"aardvark"}},{"match":{"body_t":"zebra"}}]}})");
  EXPECT_EQ(1, prunedBy(disjunction));
  expectSameWithAndWithoutPruning(disjunction);
}

TEST_F(SegmentPruningTest, statisticsSeeksSkipOnlyProvenAbsence) {
  indexThreeSegments();
  auto reader = currentReader();
  MemPool pool;
  Query::Context context(pool, *reader);
  auto* field = context.getCachedFieldInfo("body_t");
  ASSERT_NE(nullptr, field);
  auto* aardvark = context.getCachedTerminfo(*field, "aardvark");
  ASSERT_NE(nullptr, aardvark);
  EXPECT_EQ(3, aardvark->termStats.docFreq);
  for (size_t ord = 0; ord < 3; ord++) {
    // Only the first segment's term bounds admit aardvark: no other
    // dictionary is even opened for it.
    EXPECT_EQ(ord == 0, field->termsEnums[ord] != nullptr) << ord;
    EXPECT_EQ(ord == 0, aardvark->postingsStates[ord] != nullptr) << ord;
  }
  // A term inside every segment's bounds is seeked everywhere.
  auto* common = context.getCachedTerminfo(*field, "common");
  ASSERT_NE(nullptr, common);
  EXPECT_EQ(9, common->termStats.docFreq);
  EXPECT_EQ(12, common->termStats.totalTermFreq);

  // Scorers on proven-absent segments are null, never crashes.
  std::string_view words[] = {"common", "zebra"};
  int32_t positions[] = {0, 1};
  PhraseQuery phrase("body_t", words, positions);
  TermQuery a("tag_s", "a");
  TermQuery c("tag_s", "c");
  Query* both[] = {&a, &c};
  BooleanQuery conjunction(both, {}, {}, {});
  for (Query* query : std::initializer_list<Query*>{&phrase, &conjunction, &c}) {
    auto* weight = query->createWeight(context, Query::NEED_SCORES);
    for (auto& segment : reader->segments()) {
      bool absent = query->mayMatch(SummaryView(segment.summary())) == Query::MatchState::EMPTY;
      if (absent) {
        EXPECT_EQ(nullptr, weight->createScorer(pool, segment)) << segment.ord;
      }
    }
  }
}

TEST_F(SegmentPruningTest, childrenOfPrunedOpsSeeEmptyMatches) {
  indexThreeSegments();
  // A facet under the list counts its matches only.
  std::string facet = topDocs(R"({"range":{"field":"price_i","gte":11,"lte":13}})",
                              R"(,"ops":{"tags":{"field_facet":{"field":"tag_s"}}})");
  EXPECT_EQ(2, prunedBy(facet));
  expectSameWithAndWithoutPruning(facet);
  // A child resetting its domain needs the segments its own query matches:
  // the list keeps segment 0 for it.
  std::string reset = topDocs(R"({"range":{"field":"price_i","gte":11,"lte":13}})",
                              R"(,"ops":{"tags":{"field_facet":{"field":"tag_s"},"domain":{"query":"tag_s:a"}}})");
  EXPECT_EQ(1, prunedBy(reset));
  expectSameWithAndWithoutPruning(reset);
  // A filter routed away from a child keeps the segments of the query alone.
  std::string routed = topDocs(R"({"match":{"body_t":"common"}})",
      R"(,"filter":[{"query":"tag_s:a","except_ops":["tags"]}],"ops":{"tags":{"field_facet":{"field":"tag_s"}}})");
  EXPECT_EQ(0, prunedBy(routed));
  expectSameWithAndWithoutPruning(routed);
  // Root-level facets: one over every document, one with its own domain.
  std::string root = std::string(R"({"ops":{"q":{"top_docs":{"query":"tag_s:b","limit":-1,"fields":["id"]}},)")
      + R"("all":{"field_facet":{"field":"tag_s"}},)"
      + R"("own":{"field_facet":{"field":"price_i"},"domain":{"query":"tag_s:c"}}}})";
  EXPECT_EQ(2, prunedBy(root));
  expectSameWithAndWithoutPruning(root);
  // Fusion: the shared filter bounds every source.
  std::string fusion = R"({"ops":{"f":{"fusion":{"rrf":{},"limit":-1,"filter":["price_i:[11 TO 13]"],)"
      R"("sources":{"text":{"query":"body_t:common","limit":-1},"tag":{"query":"tag_s:b OR tag_s:c","limit":-1}}}}}})";
  EXPECT_EQ(4, prunedBy(fusion));
  expectSameWithAndWithoutPruning(fusion);
}

TEST_F(SegmentPruningTest, reportsPruningInProfileAndExplain) {
  indexThreeSegments();
  std::string json = topDocs(R"({"range":{"field":"price_i","gte":11,"lte":13}})");
  auto req = localReq(helper->getSearchEngine());
  parseQueryRequest(json, req->rawRequest(), req->mr);
  req->collection("prune").profile(true);
  req->execute();
  ASSERT_TRUE(req->ok()) << req->errorMsg();
  EXPECT_EQ(3u, req->getDocs("q").size());
  const auto& response = req->responses.back()->proto;
  ASSERT_TRUE(response.profile.has_value());
  const api::ExecutionProfileOp* pruning = nullptr;
  for (const auto& op : response.profile->ops) {
    if (op.name == "pruning") pruning = &op;
  }
  ASSERT_NE(nullptr, pruning);
  size_t prunedPieces = 0;
  for (const auto& piece : pruning->pieces) {
    if (piece.strategy != "pruned") continue;
    prunedPieces++;
    ASSERT_FALSE(piece.details.empty());
    EXPECT_EQ("op q: numeric bounds disjoint", piece.details[0]);
  }
  EXPECT_EQ(2u, prunedPieces);
  auto notes = helper->getSearchEngine().explain(req->rawRequest(), "");
  size_t prunedNotes = 0;
  for (const auto& note : notes) prunedNotes += note.starts_with("op q: pruned segment");
  EXPECT_EQ(2u, prunedNotes);

  // Nothing pruned: no pruning op and no notes.
  auto all = localReq(helper->getSearchEngine());
  parseQueryRequest(topDocs(R"({"all":true})"), all->rawRequest(), all->mr);
  all->collection("prune").profile(true);
  all->execute();
  ASSERT_TRUE(all->ok()) << all->errorMsg();
  for (const auto& op : all->responses.back()->proto.profile->ops) EXPECT_NE("pruning", op.name);
  for (const auto& note : helper->getSearchEngine().explain(all->rawRequest(), "")) {
    EXPECT_FALSE(note.contains("pruned segment")) << note;
  }
}

TEST_F(SegmentPruningTest, numericBoundEdgesAndMultiValuedFields) {
  // Bounds are inclusive at both ends; a multi-valued field's bounds cover
  // every value, and a range between two of a document's values still misses.
  ASSERT_TRUE(helper->indexAll({flatdoc("id", "neg", "nums_is", vec_i(-100, -1)),
                                flatdoc("id", "low", "nums_is", vec_i(-1, 7))},
                               UpdateMessage::COMMIT).success);
  ASSERT_TRUE(helper->indexAll({flatdoc("id", "high", "nums_is", vec_i(50, INT64_MAX))},
                               UpdateMessage::COMMIT).success);
  ASSERT_TRUE(helper->indexAll({flatdoc("id", "min", "nums_is", vec_i(INT64_MIN))},
                               UpdateMessage::COMMIT).success);
  for (std::string_view query : {
         R"({"range":{"field":"nums_is","gte":7,"lte":7}})",
         R"({"range":{"field":"nums_is","gt":7,"lt":50}})",
         R"({"range":{"field":"nums_is","gte":-100,"lte":-100}})",
         R"({"range":{"field":"nums_is","lt":-100}})",
         R"({"range":{"field":"nums_is","gte":9223372036854775807}})",
         R"({"range":{"field":"nums_is","lte":-9223372036854775808}})",
         R"({"range":{"field":"nums_is","gte":8,"lte":49}})",
         R"({"exists":{"field":"nums_is"}})",
       }) {
    expectSameWithAndWithoutPruning(topDocs(query));
  }
  EXPECT_EQ(2, prunedBy(topDocs(R"({"range":{"field":"nums_is","gte":9223372036854775807}})")));
  EXPECT_EQ(2, prunedBy(topDocs(R"({"range":{"field":"nums_is","lte":-9223372036854775808}})")));
  EXPECT_EQ(3, prunedBy(topDocs(R"({"range":{"field":"nums_is","gte":8,"lte":49}})")));
}

TEST_F(SegmentPruningTest, fieldMissingFromSomeSegments) {
  indexThreeSegments();
  ASSERT_TRUE(helper->index(flatdoc("id", "late", "late_i", (int64_t)5), UpdateMessage::COMMIT).success);
  ASSERT_EQ(4u, currentReader()->segments().size());
  EXPECT_EQ(3, prunedBy(topDocs(R"({"exists":{"field":"late_i"}})")));
  for (std::string_view query : {
         R"({"exists":{"field":"late_i"}})",
         R"({"range":{"field":"late_i","gte":0}})",
         R"({"boolean":{"optional":[{"range":{"field":"late_i","gte":0}},{"match":{"tag_s":"a"}}]}})",
         R"({"boolean":{"required":[{"all":true}],"prohibited":[{"exists":{"field":"late_i"}}]}})",
       }) {
    expectSameWithAndWithoutPruning(topDocs(query));
  }
}

TEST_F(SegmentPruningTest, nanosecondDates) {
  SchemaBuilder schema;
  auto& stamp = schema.field("stamp");
  stamp.type = api::FieldDef::FieldClass::DATE;
  stamp.unit = api::FieldDef::DateUnit::NS;
  schema.set(helper->collection());
  ASSERT_TRUE(helper->index(flatdoc("id", "early", "stamp", (int64_t)1000), UpdateMessage::COMMIT).success);
  ASSERT_TRUE(helper->index(flatdoc("id", "late", "stamp", (int64_t)5'000'000'000), UpdateMessage::COMMIT).success);
  expectSameWithAndWithoutPruning(topDocs(R"({"range":{"field":"stamp","gte":4000000000}})"));
  expectSameWithAndWithoutPruning(topDocs(R"({"range":{"field":"stamp","lte":1000}})"));
}

TEST_F(SegmentPruningTest, emptyCollectionAndFullyPrunedRequests) {
  expectSameWithAndWithoutPruning(topDocs(R"({"all":true})"));
  expectSameWithAndWithoutPruning(topDocs(R"({"range":{"field":"price_i","gte":1}})"));
  indexThreeSegments();
  // Every segment pruned: exact zero counts, facets and limits included.
  for (std::string_view extra : {"", R"(,"ops":{"tags":{"field_facet":{"field":"tag_s"}}})"}) {
    std::string none = topDocs(R"({"range":{"field":"price_i","gte":100}})", extra);
    EXPECT_EQ(3, prunedBy(none));
    expectSameWithAndWithoutPruning(none);
  }
  expectSameWithAndWithoutPruning(
      R"({"ops":{"q":{"top_docs":{"query":{"match":{"tag_s":"nope"}},"limit":10,"count_limit":-1,"fields":["id"]}}}})");
}

TEST_F(SegmentPruningTest, duplicateOptionalClausesCountOnce) {
  indexThreeSegments();
  // The plan drops the duplicate and lowers min_match 2 to 1, so tag b's
  // segment matches; only tag c's segment is pruned.
  std::string json = topDocs(
      R"({"boolean":{"optional":[{"match":{"tag_s":"a"}},{"match":{"tag_s":"a"}},{"match":{"tag_s":"b"}}],"min_match":2}})");
  EXPECT_EQ(1, prunedBy(json));
  expectSameWithAndWithoutPruning(json);
}

TEST_F(SegmentPruningTest, knnPreparationSeesPrunedSegments) {
  SchemaBuilder schema;
  auto& vector = schema.field("v_v");
  vector.type = api::FieldDef::FieldClass::VECTOR;
  vector.column = true;
  vector.dims = 2;
  vector.metric = api::VectorMetric::L2;
  schema.set(helper->collection());
  ASSERT_TRUE(helper->indexAll({flatdoc("id", "x1", "tag_s", "x", "v_v", std::vector<float>{1, 0}),
                                flatdoc("id", "x2", "tag_s", "x", "v_v", std::vector<float>{2, 0})},
                               UpdateMessage::COMMIT).success);
  ASSERT_TRUE(helper->indexAll({flatdoc("id", "y1", "tag_s", "y", "v_v", std::vector<float>{0, 0}),
                                flatdoc("id", "y2", "tag_s", "y", "v_v", std::vector<float>{0.1f, 0})},
                               UpdateMessage::COMMIT).success);
  // kNN picks its global top 2 (both in the y segment) before the required
  // term filters them, even though the term prunes the y segment.
  std::string json = topDocs(
      R"({"boolean":{"required":[{"knn":{"field":"v_v","query":[0,0],"k":2,"exact":true}},{"match":{"tag_s":"x"}}]}})");
  EXPECT_EQ(1, prunedBy(json));
  expectSameWithAndWithoutPruning(json);
}

TEST_F(SegmentPruningTest, fusionSharedFiltersDoNotPoisonTheMembershipCache) {
  indexThreeSegments();
  std::string plain = R"({"ops":{"q":{"top_docs":{"query":"body_t:common","limit":0,"count_limit":-1}}}})";
  QueryPruning::disableForTests = true;
  auto reference = render(plain);
  QueryPruning::disableForTests = false;
  auto* cache = currentReader()->filterCache();
  ASSERT_NE(nullptr, cache);
  cache->clear();
  cache->resetAdmission();
  // The shared filter prunes the source's count in two segments; the
  // cached membership of body_t:common must not record them as empty.
  std::string fusion = R"({"ops":{"f":{"fusion":{"rrf":{},"limit":-1,"filter":["price_i:[11 TO 13]"],)"
      R"("sources":{"count":{"query":"body_t:common","limit":0,"count_limit":-1},)"
      R"("text":{"query":"body_t:common","limit":-1}}}}}})";
  for (int repeat = 0; repeat < 3; repeat++) render(fusion);
  EXPECT_EQ(reference, render(plain));
}
