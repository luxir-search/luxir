// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

// End-to-end checks for weight proofs, segment completion and reporting.

#include <algorithm>
#include <atomic>
#include <gtest/gtest.h>
#include "luxir/index/IndexWriter.h"
#include "luxir/query/BooleanQuery.h"
#include "luxir/query/CatalogSummaryView.h"
#include "luxir/query/NumericPredicateQuery.h"
#include "luxir/query/PhraseQuery.h"
#include "luxir/query/PrefixQuery.h"
#include "luxir/query/QueryPruning.h"
#include "luxir/query/TermQuery.h"
#include "luxir/search/FilterCache.h"
#include "luxir/search/IndexReader.h"
#include "luxir/server/JsonRequest.h"
#include "luxir/server/JsonResponse.h"
#include "luxir/store/FSDirectory.h"
#include "luxir/util/Signal.h"
#include "luxir/util/Uuid.h"
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
    auto writer = helper->getIndexWriter();
    writer->mergePolicy->setMergeFactor(100);
    // Let routed filters in the one- and two-document segments enter the cache.
    writer->snapshots.readers.filterCache = std::make_shared<FilterCache>(FilterCacheConfig{.minSegmentDocs = 0});
    QueryPruning::disableForTests = false;
    Signal::listen("searchTopDocsSegmentPruned", [this](void*, void*, void*) -> void* {
      prunedSegments.fetch_add(1);
      return nullptr;
    });
  }

  void TearDown() override {
    Signal::unlisten("searchTopDocsSegmentPruned");
    QueryPruning::disableForTests = false;
    QueryPruning::verifyTermAbsenceForTests = false;
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

  // Complete response bodies, including scores, buckets, warnings and errors.
  std::string render(std::string_view json, std::string_view expectedError = {}) {
    auto req = localReq(helper->getSearchEngine());
    parseQueryRequest(json, req->rawRequest(), req->mr);
    req->collection("prune").execute();
    if (expectedError.empty()) {
      EXPECT_OK(req);
    } else {
      EXPECT_EQ(expectedError, req->errorCode()) << req->errorMsg();
    }
    std::string out;
    for (auto* response : req->responses) {
      auto copy = response->proto;
      // Ops appear in completion order; compare them by name.
      using Entry = std::remove_cvref_t<decltype(*copy.ops.begin())>;
      std::vector<Entry> ops(copy.ops.begin(), copy.ops.end());
      std::ranges::sort(ops, {}, [](const Entry& op) { return op.first; });
      copy.ops = decltype(copy.ops)(std::span<const Entry>(ops));
      out += renderSearchResponseBody(copy);
    }
    return out;
  }

  void clearCache() {
    auto* cache = currentReader()->filterCache();
    cache->clear();
    cache->resetAdmission();
  }

  // Count TopDocs segment skips on the cold plan.
  int64_t prunedBy(std::string_view json) {
    clearCache();
    prunedSegments = 0;
    render(json);
    return prunedSegments.load();
  }

  // One committed segment of JSON documents.
  void indexJson(std::initializer_list<std::string_view> docs) {
    CollectionHelper::UpdateBuilder builder;
    for (std::string_view json : docs) {
      builder.addRaw([json](api::Map& doc, std::pmr::memory_resource& mr) {
        std::string error;
        if (!api::read_json(doc, api::build::arenaStr(mr, json), mr, &error)) {
          throw std::runtime_error("bad test doc: " + error);
        }
      });
    }
    builder.commit();
    ASSERT_TRUE(helper->submit(builder).success);
  }

  // Explain notes of a request.
  std::vector<std::string> notes(std::string_view json) {
    auto req = localReq(helper->getSearchEngine());
    parseQueryRequest(json, req->rawRequest(), req->mr);
    req->collection("prune");
    return helper->getSearchEngine().explain(req->rawRequest(), "");
  }

  void expectSameWithAndWithoutPruning(std::string_view json, std::string_view expectedError = {}) {
    SCOPED_TRACE(std::string(json));
    std::string reference;
    for (bool disabled : {true, false}) {
      clearCache();
      QueryPruning::disableForTests = disabled;
      auto cold = render(json, expectedError);
      if (disabled) reference = cold;
      else EXPECT_EQ(reference, cold);
      // The second sighting admits eligible entries. Check the populated cache
      // from both modes, always against the cold, unpruned response.
      EXPECT_EQ(reference, render(json, expectedError));
      EXPECT_EQ(reference, render(json, expectedError));
      QueryPruning::disableForTests = !disabled;
      EXPECT_EQ(reference, render(json, expectedError));
    }
    QueryPruning::disableForTests = false;
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
  QueryPruning::verifyTermAbsenceForTests = true;
  indexThreeSegments();
  // Only the first segment contributes df for aardvark. Verify each omitted
  // statistics seek directly against the dictionary as well as comparing scores.
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

TEST_F(SegmentPruningTest, reopenedProofsUseTheNewCoresDictionaryBounds) {
  ASSERT_TRUE(helper->indexAll({flatdoc("id", "1", "tag_s", "green"), flatdoc("id", "2", "tag_s", "red"),
                                flatdoc("id", "3", "tag_s", "middle")}, UpdateMessage::COMMIT).success);
  auto writer = helper->getIndexWriter();
  auto snapshot = *writer->snapshots.snapshot();
  snapshot.openCores.clear();
  auto path = std::filesystem::temp_directory_path() / ("luxir-pruning-" + newUuid());
  auto cleanup = scope_guard([&] { std::filesystem::remove_all(path); });
  FSDirectory disk(path);
  for (const auto& file : snapshot.files) disk.copyFile(writer->dir, file.name);
  auto old = std::make_shared<IndexReader>(disk, snapshot);
  const auto* totals = &old->field("tag_s").stats();
  std::weak_ptr<SegmentCore> core = old->segments()[0].segmentCoreShared();

  ASSERT_TRUE(helper->deleteById("3", UpdateMessage::COMMIT).success);
  auto reader = std::make_shared<IndexReader>(writer->dir, *writer->snapshots.snapshot(), old.get());
  ASSERT_EQ(old->coreGen(), reader->coreGen());
  ASSERT_EQ(totals, &reader->field("tag_s").stats());
  old.reset();
  ASSERT_TRUE(core.expired());  // Releases the original mmap.
  for (auto query : {"tag_s:green", "tag_s:red", "tag_s:gr*", "tag_s:[red TO red]"}) {
    auto req = localReq(helper->getSearchEngine());
    req->collection("prune").topDocs().exprQuery(query).countLimit().limit(0);
    req->execute(reader, false);
    ASSERT_OK(req);
    EXPECT_EQ(1, req->getMatchCount()) << query;
  }
}

TEST_F(SegmentPruningTest, emptyRootSkipsTermSeeksInPriceGap) {
  indexThreeSegments();
  auto req = localReq(helper->getSearchEngine());
  req->collection("prune").topDocs().exprQuery("body_t:common AND price_i:[5 TO 8]").getScores().countLimit();
  SkipStatsScope stats;
  req->execute(false);
  ASSERT_OK(req);
  EXPECT_EQ(0, req->getMatchCount());
  EXPECT_EQ(0, SkipStats::termDictionarySeeks);
}

TEST_F(SegmentPruningTest, emptyConjunctionEmitsNamedZeroBucketWithoutStatSeeks) {
  indexThreeSegments();
  auto req = localReq(helper->getSearchEngine());
  parseQueryRequest(R"({"query":"body_t:common AND price_i:[5 TO 8]","get_scores":true,
    "ops":{"tags":{"field_facet":{"field":"tag_s"}},
           "named":{"query_facet":{"buckets":{"all":"*:*"}}}}})", req->rawRequest(), req->mr);
  SkipStatsScope stats;
  req->collection("prune").execute(false);
  ASSERT_OK(req);
  const auto& bucket = *req->docList()->ops.at("named")->facetResult();
  ASSERT_EQ(1u, bucket.counts.size());
  EXPECT_EQ("all", std::get<api::ColStr>(bucket.bucket_ids->kind).v[0]);
  EXPECT_EQ(0, bucket.counts[0]);
  EXPECT_EQ(0, SkipStats::termDictionarySeeks);
}

TEST_F(SegmentPruningTest, impossibleClauseKeepsPreparedRowOpValidation) {
  indexThreeSegments();
  auto req = localReq(helper->getSearchEngine());
  parseQueryRequest(R"({"query":"*:*","row_ops":{"row":{"top_docs":{
    "query":"body_t:common AND price_i:[5 TO 8]"}}}})", req->rawRequest(), req->mr);
  req->testForcePrepare = true;
  req->collection("prune").execute(false);
  EXPECT_TRUE(req->errorMsg().contains("prepared queries and routed filters")) << req->errorMsg();
}

TEST_F(SegmentPruningTest, provenZeroWarmsTheAuthoredWholeCountKey) {
  indexThreeSegments();
  for (int repeat = 0; repeat < 3; repeat++) {
    // The last request must find all three zeros even without proofs.
    QueryPruning::disableForTests = repeat == 2;
    auto req = localReq(helper->getSearchEngine());
    req->collection("prune").topDocs().exprQuery("body_t:common AND price_i:[5 TO 8]").countLimit().limit(0);
    SkipStatsScope stats;
    req->execute(false);
    ASSERT_OK(req);
    EXPECT_EQ(0, req->getMatchCount());
    EXPECT_EQ(repeat == 2, SkipStats::cacheFirstMembershipWeightSkips);
  }
}

TEST_F(SegmentPruningTest, directPrefixScorerHonorsRootProof) {
  indexThreeSegments();
  auto reader = currentReader();
  MemPool pool;
  Query::Context context(pool, *reader);
  PrefixQuery prefix("tag_s", "a");
  prefix.validateLogical(context.planningContext());
  auto* weight = context.createRootWeight(prefix, 0);
  auto& excluded = reader->segments()[1];
  SkipStatsScope stats;
  EXPECT_EQ(nullptr, weight->createScorer(pool, excluded));
  EXPECT_EQ(0, SkipStats::multitermExpansions);
  EXPECT_NE(nullptr, weight->createScorer(pool, reader->segments()[0]));
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
}

TEST_F(SegmentPruningTest, unprunedScorersAgreeWithSegmentEvidence) {
  indexThreeSegments();
  QueryPruning::disableForTests = true;
  auto reader = currentReader();
  MemPool pool;
  Query::Context context(pool, *reader);
  std::string_view words[] = {"common", "zebra"};
  int32_t positions[] = {0, 1};
  PhraseQuery phrase("body_t", words, positions);
  TermQuery a("tag_s", "a");
  TermQuery c("tag_s", "c");
  Query* both[] = {&a, &c};
  BooleanQuery conjunction(both, {}, {}, {});
  QueryPruning pruning;
  for (Query* query : std::initializer_list<Query*>{&phrase, &conjunction, &c}) {
    query->validateLogical(context.planningContext());
    auto* weight = query->createWeight(context, Query::NEED_SCORES);
    for (auto& segment : reader->segments()) {
      CatalogSummaryView evidence(pruning, context.planningContext(), segment.ord);
      bool absent = query->mayMatch(evidence) == Query::MatchState::EMPTY;
      // Only the last segment admits either the phrase or tag c. No segment admits both tags.
      EXPECT_EQ(query == &conjunction || segment.ord < 2, absent);
      if (absent) {
        auto* scorer = weight->createScorer(pool, segment);
        EXPECT_TRUE(!scorer || scorer->next() == PostingsReader::END) << segment.ord;
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

TEST_F(SegmentPruningTest, collectionEmptyListKeepsFiltersForBucketReset) {
  indexThreeSegments();
  // The named bucket exists even when the list is empty. Its children reset
  // inside the bucket, so the list itself needs no routed domain.
  auto req = localReq(helper->getSearchEngine());
  parseQueryRequest(R"({"query":"price_i:>100","filter":["tag_s:a"],"limit":0,"count_limit":-1,
    "ops":{"all":{"query_facet":{"buckets":{"rows":"*:*"},"ops":{
      "filtered":{"domain":{"query":"*:*","apply_parent_filters":true},"top_docs":{"limit":0,"count_limit":-1}},
      "unfiltered":{"domain":{"query":"*:*","apply_parent_filters":false},"top_docs":{"limit":0,"count_limit":-1}}
    }}}}})", req->rawRequest(), req->mr);
  req->collection("prune").execute();
  ASSERT_OK(req);
  EXPECT_EQ(0, req->getMatchCount());
  const auto& bucket = *req->docList()->ops.at("all")->facetResult();
  auto count = [&](std::string_view child) {
    return *std::get<api::ArrVal>(bucket.ops.at(child)->kind).v[0].docList()->found;
  };
  EXPECT_EQ(3, count("filtered"));
  EXPECT_EQ(9, count("unfiltered"));
}

TEST_F(SegmentPruningTest, emptyInputSkipsChildPrefixExpansion) {
  indexThreeSegments();
  // The child's prefix has a match in every segment. Its parent supplies
  // three empty inputs, so even the first expansion is unnecessary.
  auto json = topDocs(R"({"range":{"field":"price_i","gte":100}})", R"(,"ops":{"nested":{
      "top_docs":{"query":{"prefix":{"field":"body_t","prefix":"comm"}},"limit":1}}})");
  auto req = localReq(helper->getSearchEngine());
  parseQueryRequest(json, req->rawRequest(), req->mr);
  SkipStatsScope stats;
  req->collection("prune").execute(false);
  ASSERT_OK(req);
  EXPECT_EQ(6, prunedSegments.load());
  EXPECT_EQ(0, SkipStats::multitermExpansions);
}

TEST_F(SegmentPruningTest, cacheHitWithoutAWeightReportsNoProof) {
  indexThreeSegments();
  auto json = R"({"ops":{"q":{"top_docs":{"query":"body_t:common AND price_i:[5 TO 8]",
                                         "count_limit":-1,"limit":0}}}})";
  render(json);
  render(json);
  prunedSegments = 0;
  auto req = localReq(helper->getSearchEngine());
  parseQueryRequest(json, req->rawRequest(), req->mr);
  {
    SkipStatsScope stats;
    req->collection("prune").profile(true).execute(false);
    ASSERT_OK(req);
    ASSERT_EQ(1, SkipStats::cacheFirstMembershipWeightSkips);
  }
  EXPECT_EQ(0, prunedSegments.load());
  for (const auto& op : req->responses.back()->proto.profile->ops) EXPECT_NE("pruning", op.name);
  EXPECT_TRUE(helper->getSearchEngine().explain(req->rawRequest(), "").empty());
  expectSameWithAndWithoutPruning(json);
}

TEST_F(SegmentPruningTest, survivingDisjunctionDoesNotSupplyTheEmptyReason) {
  indexThreeSegments();
  auto query = R"({"boolean":{"required":[
    {"boolean":{"optional":["tag_s:zzz","body_t:common"]}},
    {"range":{"field":"price_i","gte":100}}]}})";
  auto explained = notes(topDocs(query));
  ASSERT_EQ(1u, explained.size());
  EXPECT_TRUE(explained.front().contains("numeric bounds disjoint")) << explained.front();
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
    EXPECT_EQ("op q: root 1: numeric bounds disjoint", piece.details[0]);
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

TEST_F(SegmentPruningTest, booleanProofsFollowNormalizedMatchCounts) {
  indexThreeSegments();
  auto check = [&](std::string_view query, int pruned) {
    auto json = topDocs(query);
    EXPECT_EQ(pruned, prunedBy(json)) << query;
    expectSameWithAndWithoutPruning(json);
  };
  // A required disjunction is hoisted into two optionals with min_match 1.
  check(R"({"boolean":{"filter":["body_t:common"],"required":[{"boolean":{"optional":["tag_s:a","tag_s:b"]}}]}})", 1);
  // Above half, preserve the miss budget. At half, require two distinct terms.
  check(R"({"boolean":{"optional":["tag_s:a","tag_s:a","tag_s:b"],"min_match":2}})", 1);
  check(R"({"boolean":{"optional":["tag_s:a","tag_s:a","tag_s:b","body_t:common"],"min_match":2}})", 1);
  check(R"({"boolean":{"required":["tag_s:b"]}})", 2);
}

TEST_F(SegmentPruningTest, routedAnyProvesOnlySegmentsWithoutAnySource) {
  indexThreeSegments();
  // The facet resets to a. Both outputs exclude segment c. The list needs
  // both a and b, and the two separately weighted a buckets must stay separate.
  auto json = topDocs(R"({"all":true})", R"(,"ops":{"tags":{
    "domain":{"query":"tag_s:a"},
    "query_facet":{"buckets":{"one":{"match":{"tag_s":"a"},"boost":2e38},
                             "two":{"match":{"tag_s":"a"},"boost":2e38},
                             "three":{"boolean":{"required":["tag_s:b"]}}},
                   "selected":["one","two","three"],"selection_mode":"any"}}})");
  EXPECT_EQ(1, prunedBy(json));
  auto* cache = currentReader()->filterCache();
  auto hits = cache->counters().hits;
  expectSameWithAndWithoutPruning(json);
  EXPECT_GT(cache->counters().hits, hits);
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

TEST_F(SegmentPruningTest, mappedKnnKeepsGlobalWinnerBeforeCollection) {
  SchemaBuilder schema;
  using Type = api::FieldDef::FieldClass;
  schema.field("items").type = Type::NESTED;
  schema.field("items.tag").type = Type::STRING;
  auto& vector = schema.field("items.v");
  vector.type = Type::VECTOR;
  vector.dims = 2;
  vector.metric = api::VectorMetric::L2;
  schema.set(helper->collection());
  indexJson({R"({"id":"nearest","items":{"tag":"x","v":[0,0]}})"});
  indexJson({R"({"id":"second","items":{"tag":"y","v":[1,0]}})"});
  ASSERT_EQ(2u, currentReader()->segments().size());

  for (std::string_view op : {
      R"("top_docs":{"query":"tag:y","count_limit":-1})",
      R"("fusion":{"rrf":{},"count_limit":-1,"sources":{"s":{"query":"tag:y","limit":1}}})"}) {
    SCOPED_TRACE(op);
    std::string json = R"({"ops":{"q":{"domain":{"to":"items","filter":[{"knn":{
      "field":"v","query":[0,0],"k":1,"exact":true}}]},)" + std::string(op) + "}}}";
    auto req = localReq(helper->getSearchEngine());
    parseQueryRequest(json, req->rawRequest(), req->mr);
    req->collection("prune").execute();
    ASSERT_OK(req);
    EXPECT_EQ(0, req->getMatchCount());
    expectSameWithAndWithoutPruning(json);
  }
}

TEST_F(SegmentPruningTest, fusionSharedFiltersDoNotPoisonTheMembershipCache) {
  indexThreeSegments();
  std::string plain = R"({"ops":{"q":{"top_docs":{"query":"body_t:common","limit":0,"count_limit":-1}}}})";
  QueryPruning::disableForTests = true;
  auto reference = render(plain);
  QueryPruning::disableForTests = false;
  clearCache();
  // The shared filter prunes the source's count in two segments; the
  // cached membership of body_t:common must not record them as empty.
  std::string fusion = R"({"ops":{"f":{"fusion":{"rrf":{},"limit":-1,"filter":["price_i:[11 TO 13]"],)"
      R"("sources":{"count":{"query":"body_t:common","limit":0,"count_limit":-1},)"
      R"("text":{"query":"body_t:common","limit":-1}}}}}})";
  for (int repeat = 0; repeat < 3; repeat++) render(fusion);
  EXPECT_EQ(reference, render(plain));
}

TEST_F(SegmentPruningTest, emptyRootsSkipCollection) {
  indexThreeSegments();
  std::string outOfRange = R"({"range":{"field":"price_i","gte":100}})";
  // The term alone would seek every segment's dictionary for statistics.
  std::string withTerm = R"({"boolean":{"required":[{"match":{"body_t":"common"}}],"filter":[)" + outOfRange + "]}}";
  for (const std::string& query : {outOfRange, withTerm, std::string(R"({"match":{"tag_s":"zz"}})"),
                                   std::string(R"({"exists":{"field":"never_i"}})")}) {
    std::string json = topDocs(query);
    EXPECT_EQ(3, prunedBy(json)) << json;
    expectSameWithAndWithoutPruning(json);
    std::string facet = topDocs(query, R"(,"ops":{"tags":{"field_facet":{"field":"tag_s"}}})");
    expectSameWithAndWithoutPruning(facet);
  }
  auto explained = notes(topDocs(outOfRange));
  ASSERT_EQ(1u, explained.size());
  EXPECT_EQ("op q: empty weight for the collection (segment 0, root 1: numeric bounds disjoint)", explained[0]);
  // A filter proven empty, fusion sources, and a request mixing a proven op
  // with one that matches.
  expectSameWithAndWithoutPruning(topDocs(R"({"match":{"body_t":"common"}})", R"(,"filter":[{"match":{"tag_s":"zz"}}])"));
  std::string fusion = R"({"ops":{"f":{"fusion":{"rrf":{},"limit":5,"fields":["id"],"sources":{)"
                       R"("a":{"query":{"range":{"field":"price_i","gte":100}},"limit":5},)"
                       R"("b":{"query":{"match":{"tag_s":"zz"}},"limit":5}}}}}})";
  EXPECT_EQ(6, prunedBy(fusion));
  expectSameWithAndWithoutPruning(fusion);
  std::string mixed = R"({"ops":{"none":{"top_docs":{"query":{"range":{"field":"price_i","gte":100}},)"
                      R"("limit":5,"fields":["id"]}},"some":{"top_docs":{"query":)"
                      R"({"range":{"field":"price_i","gte":11,"lte":13}},"limit":5,"fields":["id"]}}}})";
  EXPECT_EQ(5, prunedBy(mixed));
  expectSameWithAndWithoutPruning(mixed);
  // Only a query inside every bound reaches the segments.
  EXPECT_TRUE(notes(topDocs(R"({"range":{"field":"price_i","gte":1,"lte":23}})")).empty());
}

// kNN prepares over the whole index, so its op keeps its weights; its
// segments are still pruned for collection, and the answer is unchanged.
TEST_F(SegmentPruningTest, emptyRootKeepsWholeIndexPreparation) {
  SchemaBuilder schema;
  auto& vector = schema.field("v_v");
  vector.type = api::FieldDef::FieldClass::VECTOR;
  vector.column = true;
  vector.dims = 2;
  vector.metric = api::VectorMetric::L2;
  schema.set(helper->collection());
  for (int segment = 0; segment < 2; segment++) {
    ASSERT_TRUE(helper->index(flatdoc("id", std::to_string(segment), "price_i", (int64_t)segment,
                                      "v_v", std::vector<float>{(float)segment, 0}),
                              UpdateMessage::COMMIT).success);
  }
  std::string json = R"({"ops":{"q":{"top_docs":{"query":{"boolean":{"required":[)"
                     R"({"knn":{"field":"v_v","query":[0,0],"k":2}},{"range":{"field":"price_i","gte":100}}]}},)"
                     R"("limit":5,"count_limit":-1,"fields":["id"]}}}})";
  auto explained = notes(json);
  ASSERT_EQ(2u, explained.size());
  for (const auto& note : explained) EXPECT_TRUE(note.starts_with("op q: pruned segment")) << note;
  EXPECT_EQ(2, prunedBy(json));
  expectSameWithAndWithoutPruning(json);
}

TEST_F(SegmentPruningTest, mismatchedFieldOffersNoProof) {
  auto setValueType = [&](api::FieldDef::FieldClass type) {
    SchemaBuilder schema;
    schema.field("value").type = type;
    schema.set(helper->collection());
  };
  setValueType(api::FieldDef::FieldClass::INT);
  ASSERT_TRUE(helper->index(flatdoc("id", "one", "value", 7, "tag_s", "a"), UpdateMessage::COMMIT).success);
  setValueType(api::FieldDef::FieldClass::DATE);

  std::string range = R"({"range":{"field":"value","gte":100}})";
  auto withOptional = [&](std::string_view tag) {
    return std::string(R"({"boolean":{"required":[{"match":{"tag_s":")") + std::string(tag)
        + R"("}}],"optional":[)" + range + "]}}";
  };
  auto search = [&](const std::string& query, int limit) {
    auto req = localReq(helper->getSearchEngine());
    std::string json = R"({"count_limit":-1,"limit":)" + std::to_string(limit) + R"(,"query":)" + query + "}";
    parseQueryRequest(json, req->rawRequest(), req->mr);
    req->collection("prune").execute();
    return req;
  };
  for (bool disabled : {true, false}) {
    QueryPruning::disableForTests = disabled;
    auto count = search(withOptional("a"), 0);
    EXPECT_TRUE(count->ok()) << count->errorMsg();
    EXPECT_EQ(1, count->getMatchCount());
    auto scored = search(withOptional("zzz"), 1);
    if (disabled) {
      EXPECT_EQ("field_type_mismatch", scored->errorCode());
    } else {
      EXPECT_TRUE(scored->ok()) << scored->errorMsg();
      EXPECT_EQ(0, scored->getMatchCount());
    }
  }
  expectSameWithAndWithoutPruning(topDocs(range), "field_type_mismatch");
  auto reader = currentReader();
  MemPool pool;
  Query::PlanningContext planning(pool, *reader);
  QueryPruning pruning;
  NumericPredicateQuery disjoint("value", 100, 200);
  EXPECT_EQ(Query::MatchState::UNKNOWN, disjoint.mayMatch(CatalogSummaryView(pruning, planning, 0)));
}

// Dictionary bounds cut long endpoints: a ROUNDED lower is a strict prefix of
// the smallest term and an EXCLUSIVE upper is above every term. Proofs
// compare the bytes execution seeks against those modes.
TEST_F(SegmentPruningTest, termBoundsRespectRoundedAndExclusiveEndpoints) {
  std::string base(40, 'k');
  std::string low = base + "a", high = base + "z";
  ASSERT_TRUE(helper->indexAll({flatdoc("id", "1", "tag_s", low), flatdoc("id", "2", "tag_s", high)},
                               UpdateMessage::COMMIT).success);
  ASSERT_TRUE(helper->indexAll({flatdoc("id", "3", "tag_s", "m"), flatdoc("id", "4", "tag_s", "n")},
                               UpdateMessage::COMMIT).success);
  std::string rounded(32, 'k');
  std::string exclusive = std::string(31, 'k') + "l";
  auto match = [](std::string_view term) {
    return std::string(R"({"match":{"tag_s":")") + std::string(term) + R"("}})";
  };
  auto prefix = [](std::string_view text) {
    return std::string(R"({"prefix":{"field":"tag_s","prefix":")") + std::string(text) + R"("}})";
  };
  auto range = [](std::string_view gte, std::string_view lte) {
    return std::string(R"({"range":{"field":"tag_s","gte":")") + std::string(gte) + R"(","lte":")"
        + std::string(lte) + R"("}})";
  };
  // The rounded lower and the exclusive upper themselves are absent.
  EXPECT_EQ(2, prunedBy(topDocs(match(rounded))));
  EXPECT_EQ(2, prunedBy(topDocs(match(exclusive))));
  EXPECT_EQ(1, prunedBy(topDocs(match(low))));
  EXPECT_EQ(1, prunedBy(topDocs(match(high))));
  EXPECT_EQ(1, prunedBy(topDocs(prefix(base))));
  // At or above the exclusive upper in the first segment, below "m" in the second.
  EXPECT_EQ(2, prunedBy(topDocs(prefix(exclusive))));
  EXPECT_EQ(2, prunedBy(topDocs(range(exclusive, "l"))));
  for (const auto& query : {match(rounded), match(exclusive), match(low), match(high), match("m"),
                            prefix(base), prefix(rounded), prefix(exclusive), prefix("kk"),
                            range(rounded, rounded), range(exclusive, "l"), range("a", rounded),
                            range("a", base + "a"), range(high, "zz")}) {
    expectSameWithAndWithoutPruning(topDocs(query));
  }
}

TEST_F(SegmentPruningTest, intervalBoundsProveRangeFieldsEmpty) {
  SchemaBuilder schema;
  auto& bkd = schema.field("bkd");
  bkd.type = api::FieldDef::FieldClass::LONG_RANGE;
  bkd.index = api::FieldDef::IndexMode::RANGE;
  schema.field("col").type = api::FieldDef::FieldClass::LONG_RANGE;
  schema.set(helper->collection());
  indexJson({R"({"id":"a","bkd":{"gte":10,"lte":20},"col":{"gte":10,"lte":20}})",
             R"({"id":"b","bkd":{"gte":15,"lte":30},"col":{"gte":15,"lte":30}})"});
  indexJson({R"({"id":"c","bkd":{"gte":100,"lte":200},"col":{"gte":100,"lte":200}})"});
  for (std::string_view field : {"bkd", "col"}) {
    auto range = [&](std::string_view bounds, std::string_view relation) {
      return std::string(R"({"range":{"field":")") + std::string(field) + R"(",)" + std::string(bounds)
          + R"(,"relation":")" + std::string(relation) + R"("}})";
    };
    EXPECT_EQ(2, prunedBy(topDocs(range(R"("gte":50,"lte":60)", "intersects")))) << field;
    EXPECT_EQ(1, prunedBy(topDocs(range(R"("gte":12,"lte":18)", "contains")))) << field;
    EXPECT_EQ(1, prunedBy(topDocs(range(R"("gte":0,"lte":40)", "within")))) << field;
    for (std::string_view bounds : {R"("gte":50,"lte":60)", R"("gte":12,"lte":18)", R"("gte":0,"lte":40)",
                                    R"("gte":20,"lte":100)", R"("gte":31)", R"("lte":9)"}) {
      for (std::string_view relation : {"intersects", "contains", "within"}) {
        expectSameWithAndWithoutPruning(topDocs(range(bounds, relation)));
      }
    }
  }
}

// A segment without the child space proves a required nested clause empty;
// one with it is pruned when the child query is proven empty over its rows.
TEST_F(SegmentPruningTest, nestedClausesPrunePerRowSpace) {
  SchemaBuilder schema;
  schema.field("items").type = api::FieldDef::FieldClass::NESTED;
  schema.field("items.color").type = api::FieldDef::FieldClass::STRING;
  schema.set(helper->collection());
  indexJson({R"({"id":"a","tag_s":"x","items":[{"color":"red"},{"color":"blue"}]})",
             R"({"id":"b","tag_s":"x","items":{"color":"green"}})"});
  indexJson({R"({"id":"c","tag_s":"x"})", R"({"id":"d","tag_s":"y"})"});
  indexJson({R"({"id":"e","tag_s":"y","items":{"color":"yellow"}})"});
  std::string red = R"({"to_parent":{"path":"items","score":"none","query":{"boolean":{"required":["color:red"]}}}})";
  auto explained = notes(topDocs(red));
  EXPECT_EQ(2, prunedBy(topDocs(red)));
  EXPECT_TRUE(std::ranges::any_of(explained, [](const auto& note) { return note.ends_with("nested space absent"); }));
  for (std::string_view query : {
         std::string_view(red),
         std::string_view(R"("items.color:red")"),
         std::string_view(R"("items:*")"),
         std::string_view(R"({"to_parent":{"path":"items","score":"max","query":{"match":{"field":"color","val":"green"}}}})"),
         std::string_view(R"({"boolean":{"required":[{"match":{"tag_s":"x"}}],"optional":[)"
                          R"({"to_parent":{"path":"items","score":"max","query":{"match":{"field":"color","val":"red"}}}}]}})"),
         std::string_view(R"({"boolean":{"required":[{"all":true}],"prohibited":[)"
                          R"({"to_parent":{"path":"items","score":"none","query":{"match":{"field":"color","val":"red"}}}}]}})"),
       }) {
    expectSameWithAndWithoutPruning(topDocs(query));
  }
  // A child query no row matches is proven empty for the collection.
  std::string none = R"({"to_parent":{"path":"items","score":"none","query":{"match":{"field":"color","val":"zz"}}}})";
  EXPECT_EQ(3, prunedBy(topDocs(none)));
  expectSameWithAndWithoutPruning(topDocs(none));
}
