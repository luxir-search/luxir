// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

// Segment pruning end to end: summaries written at flush and merge, queries
// that skip segments without opening them, pruned and unpruned execution
// agreeing, and the reporting around it.

#include <algorithm>
#include <mutex>
#include <set>
#include <gtest/gtest.h>
#include "luxir/index/IndexWriter.h"
#include "luxir/query/BooleanQuery.h"
#include "luxir/query/NumericPredicateQuery.h"
#include "luxir/query/PhraseQuery.h"
#include "luxir/query/QueryPruning.h"
#include "luxir/query/TermQuery.h"
#include "luxir/reader/Postings.h"
#include "luxir/search/IndexReader.h"
#include "luxir/server/JsonRequest.h"
#include "luxir/util/Metrics.h"
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

  void SetUp() override {
    LuxirTest::SetUp();
    helper = std::make_unique<CollectionHelper>("prune");
    helper->clear();
    helper->getIndexWriter()->mergePolicy->setMergeFactor(100);
    QueryPruning::disableForTests = false;
  }

  void TearDown() override {
    QueryPruning::disableForTests = false;
    Signal::unlisten("directoryOpenFile");
    helper.reset();
  }

  // Three segments: prices 1..3 with tag a, 11..13 with tag b, 21..23 with tag c.
  void indexThreeSegments() {
    for (int segment = 0; segment < 3; segment++) {
      std::vector<Doc> batch;
      for (int i = 0; i < 3; i++) {
        int price = segment * 10 + 1 + i;
        batch.push_back(flatdoc("id", std::to_string(price), "price_i", (int64_t)price,
                                "tag_s", std::string(1, (char)('a' + segment)),
                                "body_t", "common word" + std::to_string(segment)));
      }
      ASSERT_TRUE(helper->indexAll(batch, UpdateMessage::COMMIT).success);
    }
  }

  std::shared_ptr<IndexReader> currentReader() {
    return helper->getIndexWriter()->snapshots.readers.getReader();
  }

  // A reader of the current snapshot with every segment closed: the
  // publisher's open readers are not handed over, so cells open lazily.
  std::shared_ptr<IndexReader> lazyReader() {
    auto& writer = *helper->getIndexWriter();
    CommitSnapshot snapshot = *writer.snapshots.snapshot();
    snapshot.openReaders.clear();
    auto dir = std::shared_ptr<Directory>(std::shared_ptr<void>{}, &writer.dir);
    return std::make_shared<IndexReader>(dir, snapshot, nullptr, nullptr, nullptr, nullptr);
  }

  struct OpenRecorder {
    std::mutex mutex;
    std::vector<std::string> names;
    OpenRecorder() {
      Signal::listen("directoryOpenFile", [this](void*, void* name, void*) -> void* {
        std::lock_guard lock(mutex);
        names.push_back(std::string(*(std::string_view*)name));
        return nullptr;
      });
    }
    ~OpenRecorder() { Signal::unlisten("directoryOpenFile"); }
    bool openedDataOf(uint64_t segId) {
      std::lock_guard lock(mutex);
      std::string prefix = Postings::getIndexFileNamePrefix(segId);
      for (const auto& name : names) {
        if (name.starts_with(prefix) && name.find("__summary_") == std::string::npos) return true;
      }
      return false;
    }
  };

  static std::vector<std::string> run(SearchEngine& engine, std::string_view json) {
    auto req = localReq(engine);
    parseQueryRequest(json, req->rawRequest(), req->mr);
    req->collection("prune");
    req->execute();
    EXPECT_TRUE(req->ok()) << req->errorMsg();
    std::vector<std::string> docs;
    for (const auto& doc : req->getDocs("q")) docs.push_back(docToString(doc));
    std::ranges::sort(docs);
    return docs;
  }

  // "id score" per hit of op q, sorted; getDocs() leaves scores out.
  static std::vector<std::string> runScored(SearchEngine& engine, std::string_view json) {
    auto req = localReq(engine);
    parseQueryRequest(json, req->rawRequest(), req->mr);
    req->collection("prune");
    req->execute();
    EXPECT_TRUE(req->ok()) << req->errorMsg();
    const auto* docs = req->responses.back()->proto.ops.at("q")->docList();
    if (docs == nullptr) return {};
    auto ids = std::get<api::ColStr>(docs->columns.at("id").kind).v;
    auto scores = std::get<api::ColFloat>(docs->columns.at("_score_").kind).v;
    std::vector<std::string> out;
    for (size_t i = 0; i < ids.size(); i++) out.push_back(fmt::format("{} {:.7g}", ids[i], scores[i]));
    std::ranges::sort(out);
    return out;
  }

  void expectSameWithAndWithoutPruning(std::string_view json) {
    SCOPED_TRACE(std::string(json));
    QueryPruning::disableForTests = true;
    auto reference = run(helper->getSearchEngine(), json);
    QueryPruning::disableForTests = false;
    auto pruned = run(helper->getSearchEngine(), json);
    EXPECT_EQ(reference, pruned);
  }
};

constexpr std::string_view kTopDocs = R"({"ops":{"q":{"top_docs":{"query":QUERY,"limit":-1,"count_limit":-1,"fields":["id"]}}}})";

std::string topDocs(std::string_view query) {
  std::string out(kTopDocs);
  return out.replace(out.find("QUERY"), 5, query);
}

} // namespace

TEST_F(SegmentPruningTest, everySegmentCarriesASummary) {
  indexThreeSegments();
  auto reader = currentReader();
  ASSERT_EQ(3u, reader->segments().size());
  for (auto& segment : reader->segments()) {
    const SegmentSummary* summary = segment.summary();
    ASSERT_NE(nullptr, summary) << segment.segInfo.seg_id;
    EXPECT_TRUE(summary->complete());
    const auto* price = summary->field("price_i");
    ASSERT_NE(nullptr, price);
    EXPECT_EQ(3u, price->docs_with_field);
    EXPECT_EQ(price->num_max - price->num_min, 2);
    ASSERT_NE(nullptr, summary->field("tag_s"));
    ASSERT_NE(nullptr, summary->field("body_t"));
    EXPECT_EQ(nullptr, summary->field("never_i"));
  }
  const SegmentSummary* aggregate = reader->collectionSummary();
  ASSERT_NE(nullptr, aggregate);
  EXPECT_TRUE(aggregate->complete());
  const auto* price = aggregate->field("price_i");
  ASSERT_NE(nullptr, price);
  EXPECT_EQ(1, price->num_min);
  EXPECT_EQ(23, price->num_max);
  EXPECT_EQ(9u, price->docs_with_field);
  EXPECT_EQ(3u, price->segments_with_field);
}

TEST_F(SegmentPruningTest, prunedSegmentsStayClosed) {
  indexThreeSegments();
  auto reader = lazyReader();
  ASSERT_EQ(3u, reader->segments().size());
  for (auto& segment : reader->segments()) EXPECT_FALSE(segment.isOpen());

  OpenRecorder recorder;
  // Field presence comes from the summaries: no segment opens for a field
  // nothing holds.
  EXPECT_FALSE(reader->field("never_i").exists());
  for (auto& segment : reader->segments()) EXPECT_FALSE(segment.isOpen());

  MemPool pool;
  Query::Context context(pool, *reader);
  NumericPredicateQuery range("price_i", 11, 13);
  auto* weight = range.createWeight(context, 0);
  const SegmentMask& viable = weight->viableSegments();
  EXPECT_EQ(1u, viable.popcount());
  for (auto& segment : reader->segments()) {
    bool middle = viable.test((size_t)segment.ord);
    EXPECT_EQ(middle, weight->scorerSupplier(pool, segment) != nullptr);
    EXPECT_EQ(middle, segment.isOpen());
    EXPECT_EQ(middle, recorder.openedDataOf(segment.segInfo.seg_id));
    auto shortcut = weight->countShortcut(segment, nullptr);
    if (!middle) {
      EXPECT_EQ(0, shortcut.maxMatches);
      EXPECT_EQ(Query::CountHow::METADATA, shortcut.how);
    }
  }

  // A term only one segment holds: the others are never seeked.
  TermQuery term("tag_s", "c");
  auto* termWeight = term.createWeight(context, Query::NEED_SCORES);
  EXPECT_EQ(1u, termWeight->viableSegments().popcount());
  for (auto& segment : reader->segments()) {
    bool last = termWeight->viableSegments().test((size_t)segment.ord);
    bool middle = viable.test((size_t)segment.ord);
    EXPECT_EQ(last || middle, segment.isOpen());
  }
}

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
         R"({"match":{"body_t":"word1"}})",
         R"({"match":{"body_t":"common"}})",
         R"({"prefix":{"field":"tag_s","prefix":"c"}})",
         R"({"exists":{"field":"never_i"}})",
         R"({"exists":{"field":"price_i"}})",
         R"({"boolean":{"required":[{"match":{"body_t":"common"}}],"filter":[{"range":{"field":"price_i","gte":20}}]}})",
         R"({"boolean":{"optional":[{"match":{"tag_s":"a"}},{"match":{"tag_s":"nope"}}]}})",
         R"({"boolean":{"optional":[{"match":{"tag_s":"a"}},{"match":{"tag_s":"c"}}],"min_match":2}})",
         R"({"boolean":{"required":[{"match":{"body_t":"common"}}],"prohibited":[{"match":{"tag_s":"a"}}]}})",
       }) {
    expectSameWithAndWithoutPruning(topDocs(query));
  }
  // Scores of a term every live-bearing segment holds are untouched by pruning.
  std::string scored = R"({"ops":{"q":{"top_docs":{"query":{"match":{"body_t":"common"}},"limit":-1,"get_scores":true,"fields":["id"]}}}})";
  QueryPruning::disableForTests = true;
  auto reference = runScored(helper->getSearchEngine(), scored);
  QueryPruning::disableForTests = false;
  EXPECT_EQ(9u, reference.size());
  EXPECT_EQ(reference, runScored(helper->getSearchEngine(), scored));
}

TEST_F(SegmentPruningTest, mergeRecomputesSummaries) {
  indexThreeSegments();
  ASSERT_TRUE(helper->deleteByIds({"1", "23"}, UpdateMessage::COMMIT).success);
  {
    // Delete-only commits leave the summaries alone: bounds stay conservative.
    auto reader = currentReader();
    const auto* price = reader->collectionSummary()->field("price_i");
    ASSERT_NE(nullptr, price);
    EXPECT_EQ(1, price->num_min);
    EXPECT_EQ(23, price->num_max);
  }
  helper->getIndexWriter()->mergeSegments();
  auto reader = currentReader();
  ASSERT_EQ(1u, reader->segments().size());
  const SegmentSummary* summary = reader->segments()[0].summary();
  ASSERT_NE(nullptr, summary);
  const auto* price = summary->field("price_i");
  ASSERT_NE(nullptr, price);
  EXPECT_EQ(2, price->num_min);
  EXPECT_EQ(22, price->num_max);
  EXPECT_EQ(7u, price->docs_with_field);
  EXPECT_EQ(SegmentSummary::Presence::MAYBE, summary->mayContainTerm(*summary->field("tag_s"), "a"));
  EXPECT_EQ(SegmentSummary::Presence::NO, summary->mayContainTerm(*summary->field("tag_s"), "z"));
  expectSameWithAndWithoutPruning(topDocs(R"({"range":{"field":"price_i","gte":1,"lte":1}})"));
}

TEST_F(SegmentPruningTest, reportsPruningInProfileExplainAndMetrics) {
  indexThreeSegments();
  auto& metrics = *LuxirTest::luxirNode->metrics;
  auto before = metrics.counts().counters[(size_t)MetricCounter::SEGMENTS_PRUNED];

  auto req = localReq(helper->getSearchEngine());
  parseQueryRequest(topDocs(R"({"range":{"field":"price_i","gte":11,"lte":13}})"), req->rawRequest(), req->mr);
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
    if (piece.strategy == "pruned") {
      prunedPieces++;
      EXPECT_FALSE(piece.details.empty());
    }
  }
  EXPECT_EQ(2u, prunedPieces);
  auto after = metrics.counts().counters[(size_t)MetricCounter::SEGMENTS_PRUNED];
  EXPECT_EQ(before + 2, after);

  auto notes = helper->getSearchEngine().explain(req->rawRequest(), "");
  size_t prunedNotes = 0;
  for (const auto& note : notes) prunedNotes += note.starts_with("pruned segment");
  EXPECT_EQ(2u, prunedNotes);
}

TEST_F(SegmentPruningTest, collectionCanMatch) {
  indexThreeSegments();
  auto& engine = helper->getSearchEngine();
  auto ask = [&](std::string_view query) {
    auto req = localReq(engine);
    parseQueryRequest(topDocs(query), req->rawRequest(), req->mr);
    req->collection("prune");
    return engine.canMatch(req->rawRequest(), "", true);
  };
  auto hit = ask(R"({"range":{"field":"price_i","gte":11,"lte":13}})");
  EXPECT_TRUE(hit.mayMatch);
  EXPECT_TRUE(hit.summaryComplete);
  EXPECT_EQ(3u, hit.segments);
  EXPECT_EQ(2u, hit.prunedSegments.size());
  EXPECT_EQ(currentReader()->commitId(), CommitId::parse(hit.commit).index_gen);

  auto miss = ask(R"({"range":{"field":"price_i","gte":100}})");
  EXPECT_FALSE(miss.mayMatch);
  ASSERT_EQ(1u, miss.ops.size());
  EXPECT_FALSE(miss.ops[0].mayMatch);
  EXPECT_FALSE(miss.ops[0].reason.empty());
  EXPECT_EQ(3u, miss.prunedSegments.size());

  EXPECT_FALSE(ask(R"({"match":{"tag_s":"nope"}})").mayMatch);
  EXPECT_FALSE(ask(R"({"exists":{"field":"never_i"}})").mayMatch);
  EXPECT_TRUE(ask(R"({"all":true})").mayMatch);
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
  auto reader = currentReader();
  MemPool pool;
  Query::Context context(pool, *reader);
  NumericPredicateQuery gap("nums_is", 8, 49);
  EXPECT_EQ(0u, gap.createWeight(context, 0)->viableSegments().popcount());
}

TEST_F(SegmentPruningTest, fieldMissingFromSomeSegments) {
  indexThreeSegments();
  ASSERT_TRUE(helper->index(flatdoc("id", "late", "late_i", (int64_t)5), UpdateMessage::COMMIT).success);
  auto reader = lazyReader();
  ASSERT_EQ(4u, reader->segments().size());
  // Presence comes from the summaries alone: nothing opens to find the field.
  auto late = reader->field("late_i");
  ASSERT_TRUE(late.exists());
  for (size_t i = 0; i < 3; i++) EXPECT_FALSE(late.presentIn(i));
  EXPECT_TRUE(late.presentIn(3));
  EXPECT_EQ(1, late.stats().docsWithField);
  for (auto& segment : reader->segments()) EXPECT_FALSE(segment.isOpen());

  for (std::string_view query : {
         R"({"exists":{"field":"late_i"}})",
         R"({"range":{"field":"late_i","gte":0}})",
         R"({"boolean":{"optional":[{"range":{"field":"late_i","gte":0}},{"match":{"tag_s":"a"}}]}})",
         R"({"boolean":{"required":[{"all":true}],"prohibited":[{"exists":{"field":"late_i"}}]}})",
       }) {
    expectSameWithAndWithoutPruning(topDocs(query));
  }
}

TEST_F(SegmentPruningTest, prunedScorersAreNullNotCrashes) {
  // Direct createScorer calls on a pruned segment answer null for every
  // weight kind, including phrases and booleans whose clauses all vanish.
  indexThreeSegments();
  auto reader = lazyReader();
  MemPool pool;
  Query::Context context(pool, *reader);
  std::string_view words[] = {"common", "word1"};
  int32_t positions[] = {0, 1};
  PhraseQuery phrase("body_t", words, positions);
  TermQuery a("tag_s", "a");
  TermQuery c("tag_s", "c");
  Query* both[] = {&a, &c};
  BooleanQuery conjunction(both, {}, {}, {});
  BooleanQuery disjunction({}, both, {}, {});
  for (Query* query : std::initializer_list<Query*>{&phrase, &conjunction, &disjunction, &a}) {
    auto* weight = query->createWeight(context, Query::NEED_SCORES);
    for (auto& segment : reader->segments()) {
      if (weight->viableSegments().test((size_t)segment.ord)) continue;
      EXPECT_EQ(nullptr, weight->createScorer(pool, segment)) << segment.ord;
      EXPECT_EQ(nullptr, weight->scorerSupplier(pool, segment)) << segment.ord;
    }
  }
  expectSameWithAndWithoutPruning(topDocs(R"({"phrase":{"field":"body_t","text":"common word1"}})"));
}

TEST_F(SegmentPruningTest, globalStatsScoresMatchUnprunedExecution) {
  indexThreeSegments();
  // word1 is only in the middle segment, so mask-scoped statistics see a
  // third of the documents; global_stats sees them all, as without pruning.
  auto scored = [](std::string_view requestFlags, std::string_view opFlags) {
    return std::string("{") + std::string(requestFlags) + R"("ops":{"q":{)" + std::string(opFlags)
        + R"("top_docs":{"query":{"match":{"body_t":"word1"}},"limit":-1,"get_scores":true,"fields":["id"]}}}})";
  };
  auto& engine = helper->getSearchEngine();
  QueryPruning::disableForTests = true;
  auto reference = runScored(engine, scored("", ""));
  QueryPruning::disableForTests = false;
  ASSERT_EQ(3u, reference.size());
  EXPECT_NE(reference, runScored(engine, scored("", "")));
  EXPECT_EQ(reference, runScored(engine, scored(R"("flags":["global_stats"],)", "")));
  EXPECT_EQ(reference, runScored(engine, scored("", R"("flags":["global_stats"],)")));
  // Matching still follows the mask.
  EXPECT_EQ(3u, run(engine, std::string(R"({"flags":["global_stats"],"ops":{"q":{"top_docs":)")
      + R"({"query":{"match":{"tag_s":"b"}},"limit":-1,"fields":["id"]}}}})").size());
}

TEST_F(SegmentPruningTest, dateUnitTravelsInSummaries) {
  // A nanosecond date persists type flags; a reader resolving the field from
  // summaries alone must agree with the schema without opening a segment.
  SchemaBuilder schema;
  auto& stamp = schema.field("stamp");
  stamp.type = api::FieldDef::FieldClass::DATE;
  stamp.unit = api::FieldDef::DateUnit::NS;
  schema.set(helper->collection());
  ASSERT_TRUE(helper->index(flatdoc("id", "early", "stamp", (int64_t)1000), UpdateMessage::COMMIT).success);
  ASSERT_TRUE(helper->index(flatdoc("id", "late", "stamp", (int64_t)5'000'000'000), UpdateMessage::COMMIT).success);

  auto reader = lazyReader();
  auto field = reader->field("stamp");
  ASSERT_TRUE(field.exists());
  EXPECT_EQ(FieldType::DATE, field.persistedType());
  for (auto& segment : reader->segments()) {
    EXPECT_FALSE(segment.isOpen());
    const auto* record = segment.summary()->field("stamp");
    ASSERT_NE(nullptr, record);
    EXPECT_NE(0u, record->type_flags);
  }
  EXPECT_NE(0u, reader->collectionSummary()->field("stamp")->type_flags);
  expectSameWithAndWithoutPruning(topDocs(R"({"range":{"field":"stamp","gte":4000000000}})"));
  expectSameWithAndWithoutPruning(topDocs(R"({"range":{"field":"stamp","lte":1000}})"));
}

TEST_F(SegmentPruningTest, emptyCollectionAndAllPrunedRequests) {
  expectSameWithAndWithoutPruning(topDocs(R"({"all":true})"));
  expectSameWithAndWithoutPruning(topDocs(R"({"range":{"field":"price_i","gte":1}})"));
  indexThreeSegments();
  // Every segment pruned: exact zero counts and nothing opened.
  auto reader = lazyReader();
  MemPool pool;
  Query::Context context(pool, *reader);
  NumericPredicateQuery none("price_i", 100, 200);
  auto* weight = none.createWeight(context, 0);
  EXPECT_TRUE(weight->viableSegments().isNone());
  for (auto& segment : reader->segments()) {
    EXPECT_EQ(0, weight->countShortcut(segment, nullptr).maxMatches);
    EXPECT_FALSE(segment.isOpen());
  }
  expectSameWithAndWithoutPruning(
      R"({"ops":{"q":{"top_docs":{"query":{"range":{"field":"price_i","gte":100}},"limit":10,"count_limit":-1,"fields":["id"]}}}})");
}
