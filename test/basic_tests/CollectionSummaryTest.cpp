// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

// The collection summary in the manifest: the union of the segment
// summaries, its size budget, its reuse across delete-only commits, and the
// collection-level proof that answers a search before any segment opens.

#include <atomic>
#include <memory_resource>
#include <gtest/gtest.h>
#include "luxir/index/IndexWriter.h"
#include "luxir/index/SummaryInfo.h"
#include "luxir/query/QueryPruning.h"
#include "luxir/search/IndexReader.h"
#include "luxir/server/JsonRequest.h"
#include "luxir/server/JsonResponse.h"
#include "luxir/util/Signal.h"
#include "test/CollectionHelper.h"
#include "test/DurableIndexInfo.h"
#include "test/LocalReq.h"
#include "test/LuxirTest.h"
#include "test/SchemaBuilder.h"
#include "test/TestUtils.h"

using namespace luxir;
using namespace luxir::test;

namespace {

using F = SegmentSummary;

// A segment summary over hand-written records.
struct Records {
  std::pmr::monotonic_buffer_resource arena;
  std::vector<api::FieldSummary> fields;

  api::FieldSummary& add(std::string_view name, uint32_t type, uint32_t flags) {
    auto& record = fields.emplace_back();
    record.fieldname = name;
    record.type = type;
    record.summary_flags = flags;
    record.docs_with_field = 2;
    return record;
  }
  static void terms(api::FieldSummary& record, std::string_view min, std::string_view max) {
    record.summary_flags |= F::HAS_TERM_BOUNDS;
    record.term_min = ::hpp_proto::bytes_view((const std::byte*)min.data(), min.size());
    record.term_max = ::hpp_proto::bytes_view((const std::byte*)max.data(), max.size());
  }
  std::shared_ptr<const SegmentSummary> summary(bool complete = true) {
    std::ranges::sort(fields, {}, &api::FieldSummary::fieldname);
    return SegmentSummary::fromAggregate(fields, complete);
  }
};

const OwnedFieldSummary* find(const CollectionSummary& summary, std::string_view name) {
  for (const auto& record : summary.fields) {
    if (record.fieldname == name) return &record;
  }
  return nullptr;
}

class CollectionSummaryTest : public LuxirTest {
protected:
  std::unique_ptr<CollectionHelper> helper;
  std::atomic<int> opened{0};

  void SetUp() override {
    LuxirTest::SetUp();
    helper = std::make_unique<CollectionHelper>("collsum");
    helper->clear();
    helper->getIndexWriter()->mergePolicy->setMergeFactor(100);
    helper->getIndexWriter()->collectionSummaryMaxBytes = 64 * 1024;
    QueryPruning::disableForTests = false;
    Signal::listen("segmentMaterialized", [this](void*, void*, void*) -> void* {
      opened++;
      return nullptr;
    });
  }

  void TearDown() override {
    Signal::unlisten("segmentMaterialized");
    QueryPruning::disableForTests = false;
    helper.reset();
  }

  IndexWriter& writer() { return *helper->getIndexWriter(); }

  // Three segments: prices 1..3, 11..13 and 21..23; tags a, b, c; only the
  // last has `extra_s`.
  void indexThreeSegments() {
    for (int segment = 0; segment < 3; segment++) {
      std::vector<Doc> batch;
      for (int i = 0; i < 3; i++) {
        int price = segment * 10 + 1 + i;
        auto doc = flatdoc("id", std::to_string(price), "price_i", (int64_t)price,
                           "tag_s", std::string(1, (char)('a' + segment)), "body_t", "common word");
        if (segment == 2) doc = flatdoc("id", std::to_string(price), "price_i", (int64_t)price,
                                        "tag_s", "c", "body_t", "common word", "extra_s", "x");
        batch.push_back(doc);
      }
      ASSERT_TRUE(helper->indexAll(batch, UpdateMessage::COMMIT).success);
    }
  }

  CollectionSummary manifestSummary() {
    auto info = readDurableIndexInfo(writer().dir);
    CollectionSummary summary;
    summary.complete = info->summary_complete;
    summary.fields = summariesFromWire(info->field_summaries);
    return summary;
  }

  // A restarted collection: a lazy reader with no segment open.
  void reopenLazily() {
    auto& snapshots = helper->collection().getShard()->getSnapshots();
    snapshots.readers.indexReader.store(nullptr);
    snapshots.openLocalSnapshot(false);
    for (const auto& segment : snapshots.readers.getReader()->segments()) ASSERT_FALSE(segment.isOpen());
  }

  std::string render(std::string_view json, std::vector<std::string>* notes = nullptr) {
    auto req = localReq(helper->getSearchEngine());
    parseQueryRequest(json, req->rawRequest(), req->mr);
    req->collection("collsum");
    req->execute();
    EXPECT_TRUE(req->ok()) << req->errorMsg();
    if (notes) *notes = helper->getSearchEngine().explain(req->rawRequest(), "");
    std::string out;
    for (auto* response : req->responses) {
      auto copy = response->proto;
      copy.profile.reset();
      using Entry = std::remove_cvref_t<decltype(*copy.ops.begin())>;
      std::vector<Entry> ops(copy.ops.begin(), copy.ops.end());
      std::ranges::sort(ops, {}, [](const Entry& op) { return op.first; });
      copy.ops = decltype(copy.ops)(std::span<const Entry>(ops));
      out += renderSearchResponseBody(copy);
    }
    return out;
  }

  // Runs `json` on a fresh lazy reader, returns the segments it opened, and
  // expects the response of a run with pruning disabled.
  int searchOpens(std::string_view json) {
    SCOPED_TRACE(std::string(json));
    reopenLazily();
    opened = 0;
    auto pruned = render(json);
    int count = opened.load();
    QueryPruning::disableForTests = true;
    EXPECT_EQ(render(json), pruned);
    QueryPruning::disableForTests = false;
    return count;
  }
};

} // namespace

TEST_F(CollectionSummaryTest, aggregateSumsCountsAndUnionsBounds) {
  indexThreeSegments();
  auto summary = manifestSummary();
  ASSERT_TRUE(summary.complete);
  auto reader = writer().snapshots.readers.getReader();
  ASSERT_EQ(3u, reader->segments().size());
  const auto* price = find(summary, "price_i");
  ASSERT_NE(nullptr, price);
  EXPECT_EQ(9u, price->docsWithField);
  EXPECT_EQ(3u, price->segmentsWithField);
  ASSERT_TRUE(price->summaryFlags & F::HAS_NUMERIC);
  EXPECT_EQ(reader->segments()[0].summary()->field("price_i")->num_min, price->numMin);
  EXPECT_EQ(reader->segments()[2].summary()->field("price_i")->num_max, price->numMax);
  const auto* tag = find(summary, "tag_s");
  ASSERT_NE(nullptr, tag);
  ASSERT_TRUE(tag->summaryFlags & F::HAS_TERM_BOUNDS);
  auto bytes = [](auto view) { return std::string((const char*)view.data(), view.size()); };
  EXPECT_EQ(bytes(reader->segments()[0].summary()->field("tag_s")->term_min), tag->termMin);
  EXPECT_EQ(bytes(reader->segments()[2].summary()->field("tag_s")->term_max), tag->termMax);
  const auto* extra = find(summary, "extra_s");
  ASSERT_NE(nullptr, extra);
  EXPECT_EQ(1u, extra->segmentsWithField);
  EXPECT_EQ(3u, extra->docsWithField);
  // The reader decodes it; a field no segment holds is absent from it.
  ASSERT_NE(nullptr, reader->collectionSummary());
  EXPECT_NE(nullptr, reader->collectionSummary()->field("extra_s"));
  EXPECT_EQ(nullptr, reader->collectionSummary()->field("never_s"));
}

TEST_F(CollectionSummaryTest, aggregateRules) {
  Records first, second;
  auto& a = first.add("x", 1, F::HAS_NUMERIC);
  a.num_min = 5;
  a.num_max = 9;
  Records::terms(first.add("t", 2, 0), "b", "d");
  first.add("only", 2, F::EMPTY).docs_with_field = 0;
  auto& b = second.add("x", 1, F::HAS_NUMERIC);
  b.num_min = -3;
  b.num_max = 7;
  Records::terms(second.add("t", 2, 0), "a", "c");
  auto s1 = first.summary(), s2 = second.summary();
  std::vector<const SegmentSummary*> both{s1.get(), s2.get()};
  auto summary = CollectionSummary::aggregate(both);
  ASSERT_TRUE(summary.complete);
  EXPECT_EQ(CollectionSummary::Tier::FULL, summary.tier);
  const auto* x = find(summary, "x");
  EXPECT_EQ(-3, x->numMin);
  EXPECT_EQ(9, x->numMax);
  EXPECT_EQ(4u, x->docsWithField);
  const auto* t = find(summary, "t");
  EXPECT_EQ("a", t->termMin);
  EXPECT_EQ("d", t->termMax);
  EXPECT_TRUE(find(summary, "only")->summaryFlags & F::EMPTY);

  // Disagreeing types or type flags drop the field's bounds, not its record.
  Records third;
  auto& c = third.add("x", 3, F::HAS_NUMERIC);
  c.num_min = 0;
  c.num_max = 1;
  auto& t3 = third.add("t", 2, 0);
  Records::terms(t3, "a", "z");
  t3.type_flags = 1;
  auto s3 = third.summary();
  std::vector<const SegmentSummary*> mixed{s1.get(), s3.get()};
  summary = CollectionSummary::aggregate(mixed);
  ASSERT_TRUE(summary.complete);
  EXPECT_FALSE(find(summary, "x")->summaryFlags & F::HAS_NUMERIC);
  EXPECT_FALSE(find(summary, "t")->summaryFlags & F::HAS_TERM_BOUNDS);
  EXPECT_EQ(2u, find(summary, "x")->segmentsWithField);

  // A segment holding values without a bound leaves the union unbounded.
  Records fourth;
  fourth.add("x", 1, 0);
  auto s4 = fourth.summary();
  std::vector<const SegmentSummary*> unbounded{s1.get(), s4.get()};
  EXPECT_FALSE(find(CollectionSummary::aggregate(unbounded), "x")->summaryFlags & F::HAS_NUMERIC);

  // A segment without a summary, or with an incomplete one, proves nothing.
  auto incomplete = second.summary(false);
  for (const SegmentSummary* missing : {(const SegmentSummary*)nullptr, incomplete.get()}) {
    std::vector<const SegmentSummary*> partial{s1.get(), missing};
    summary = CollectionSummary::aggregate(partial);
    EXPECT_FALSE(summary.complete);
    EXPECT_TRUE(summary.fields.empty());
    EXPECT_EQ(CollectionSummary::Tier::INCOMPLETE, summary.tier);
  }
}

TEST_F(CollectionSummaryTest, budgetDropsTermBoundsThenBoundsThenCompleteness) {
  Records records;
  std::vector<std::string> names;
  for (int i = 0; i < 50; i++) names.push_back(std::format("field_{:02}", i));
  std::string longMin(32, 'a'), longMax(32, 'z');
  for (const auto& name : names) {
    auto& record = records.add(name, 1, F::HAS_NUMERIC);
    record.num_min = -1000000;
    record.num_max = 1000000;
    Records::terms(record, longMin, longMax);
  }
  auto segment = records.summary();
  std::vector<const SegmentSummary*> one{segment.get()};
  auto full = CollectionSummary::aggregate(one);
  auto noTerms = full;
  for (auto& record : noTerms.fields) {
    record.summaryFlags &= ~(uint32_t)F::HAS_TERM_BOUNDS;
    record.termMin.clear();
    record.termMax.clear();
  }
  auto presence = noTerms;
  for (auto& record : presence.fields) {
    record.summaryFlags &= ~(uint32_t)F::HAS_NUMERIC;
    record.numMin = record.numMax = 0;
  }
  size_t fullBytes = full.encodedBytes(), noTermBytes = noTerms.encodedBytes(), presenceBytes = presence.encodedBytes();
  ASSERT_GT(fullBytes, noTermBytes);
  ASSERT_GT(noTermBytes, presenceBytes);

  auto fitted = [&](size_t budget) {
    auto summary = CollectionSummary::aggregate(one);
    summary.fit(budget);
    if (summary.complete) {
      EXPECT_LE(summary.encodedBytes(), budget);
    }
    return summary;
  };
  EXPECT_EQ(CollectionSummary::Tier::FULL, fitted(fullBytes).tier);
  auto tier2 = fitted(fullBytes - 1);
  EXPECT_EQ(CollectionSummary::Tier::NO_TERM_BOUNDS, tier2.tier);
  EXPECT_EQ(noTerms.fields, tier2.fields);
  auto tier3 = fitted(noTermBytes - 1);
  EXPECT_EQ(CollectionSummary::Tier::PRESENCE, tier3.tier);
  EXPECT_EQ(presence.fields, tier3.fields);
  EXPECT_TRUE(tier3.complete);
  auto over = fitted(presenceBytes - 1);
  EXPECT_EQ(CollectionSummary::Tier::OVER_BUDGET, over.tier);
  EXPECT_FALSE(over.complete);
  EXPECT_TRUE(over.fields.empty());
}

TEST_F(CollectionSummaryTest, overBudgetSummaryProvesNothing) {
  writer().collectionSummaryMaxBytes = 0;
  indexThreeSegments();
  auto summary = manifestSummary();
  EXPECT_FALSE(summary.complete);
  EXPECT_TRUE(summary.fields.empty());
  EXPECT_EQ(nullptr, writer().snapshots.readers.getReader()->collectionSummary());
  // Segment proofs still answer.
  std::string json = R"({"ops":{"q":{"top_docs":{"query":{"range":{"field":"price_i","gte":100}},"limit":10,"count_limit":-1}}}})";
  std::vector<std::string> notes;
  render(json, &notes);
  for (const auto& note : notes) EXPECT_FALSE(note.contains("collection summary")) << note;
}

TEST_F(CollectionSummaryTest, recomputedOnlyWhenSegmentsChange) {
  indexThreeSegments();
  auto before = manifestSummary();
  uint64_t builds = writer().collectionSummaryBuilds.load();
  // A delete-only commit keeps core_gen and copies the summary: deleted
  // documents still count, as they still widen their segment's bounds.
  ASSERT_TRUE(helper->deleteById("1", UpdateMessage::COMMIT).success);
  EXPECT_EQ(builds, writer().collectionSummaryBuilds.load());
  auto afterDelete = manifestSummary();
  EXPECT_EQ(before.complete, afterDelete.complete);
  EXPECT_EQ(before.fields, afterDelete.fields);
  // A new segment changes core_gen: recomputed.
  ASSERT_TRUE(helper->index(flatdoc("id", "99", "price_i", (int64_t)99), UpdateMessage::COMMIT).success);
  EXPECT_EQ(builds + 1, writer().collectionSummaryBuilds.load());
  EXPECT_EQ(10u, find(manifestSummary(), "price_i")->docsWithField);
  // A merge too.
  CollectionHelper::UpdateBuilder merge;
  merge.commit(true, 1);
  ASSERT_TRUE(helper->submit(merge).success);
  EXPECT_EQ(builds + 2, writer().collectionSummaryBuilds.load());
  EXPECT_EQ(1u, find(manifestSummary(), "price_i")->segmentsWithField);
}

// An op the collection summary proves empty answers as an empty match
// without opening a segment, and identically to a run with pruning disabled.
TEST_F(CollectionSummaryTest, provenEmptySearchOpensNoSegment) {
  indexThreeSegments();
  std::string outOfRange = R"({"range":{"field":"price_i","gte":100}})";
  // The term alone would seek every segment's dictionary for statistics.
  std::string withTerm = R"({"boolean":{"required":[{"match":{"body_t":"common"}}],"filter":[)" + outOfRange + "]}}";
  for (const std::string& query : {outOfRange, withTerm, std::string(R"({"match":{"tag_s":"zz"}})")}) {
    std::string json = std::string(R"({"ops":{"q":{"top_docs":{"query":)") + query
        + R"(,"limit":10,"count_limit":-1,"get_scores":true,"fields":["id"])";
    EXPECT_EQ(0, searchOpens(json + "}}}}")) << json;
    // A string facet below still builds its global ordinal map over every
    // segment (a first-use path), but answers as an empty match.
    searchOpens(json + R"(,"ops":{"tags":{"field_facet":{"field":"tag_s"}}}}}}})");
  }
  // Filters, fusion, and a mixed request: only the op that may match opens
  // segments, and only its own. Every op names its fields: projecting every
  // stored field reads each segment's field catalog.
  EXPECT_EQ(0, searchOpens(R"({"ops":{"q":{"top_docs":{"query":{"match":{"body_t":"common"}},)"
                           R"("filter":[{"match":{"tag_s":"zz"}}],"limit":10,"count_limit":-1,"fields":["id"]}}}})"));
  EXPECT_EQ(0, searchOpens(R"({"ops":{"f":{"fusion":{"rrf":{},"limit":5,"fields":["id"],"sources":{)"
                           R"("a":{"query":{"range":{"field":"price_i","gte":100}},"limit":5},)"
                           R"("b":{"query":{"match":{"tag_s":"zz"}},"limit":5}}}}}})"));
  EXPECT_EQ(1, searchOpens(R"({"ops":{"none":{"top_docs":{"query":{"range":{"field":"price_i","gte":100}},)"
                           R"("limit":5,"fields":["id"]}},"some":{"top_docs":{"query":)"
                           R"({"range":{"field":"price_i","gte":11,"lte":13}},"limit":5,"fields":["id"]}}}})"));

  std::vector<std::string> notes;
  render(std::string(R"({"ops":{"q":{"top_docs":{"query":)") + outOfRange + R"(,"limit":5,"fields":["id"]}}}})", &notes);
  ASSERT_EQ(1u, notes.size());
  EXPECT_EQ("op q: proven empty by the collection summary: numeric bounds disjoint", notes[0]);
}

// Whole-index preparation is not short-circuited: kNN still prepares over
// every segment, and the answer is that of an empty match.
TEST_F(CollectionSummaryTest, wholeIndexPreparationStillRuns) {
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
  EXPECT_EQ(2, searchOpens(json));
}


// A schema that types a field differently from its persisted segments fails
// the requests using that field (field_type_mismatch), also when the
// collection summary would prove them empty before any weight exists.
TEST_F(CollectionSummaryTest, schemaTypeMismatchIsNotHiddenByTheProof) {
  auto setValueType = [&](api::FieldDef::FieldClass type) {
    SchemaBuilder schema;
    schema.field("value").type = type;
    schema.set(helper->collection());
  };
  setValueType(api::FieldDef::FieldClass::INT);
  ASSERT_TRUE(helper->index(flatdoc("id", "one", "value", 7), UpdateMessage::COMMIT).success);
  auto search = [&](std::string_view query) {
    auto req = localReq(helper->getSearchEngine());
    parseQueryRequest(std::string(R"({"ops":{"q":{"top_docs":{"query":)") + std::string(query)
                          + R"(,"limit":10,"fields":["id"]}}}})",
                      req->rawRequest(), req->mr);
    req->collection("collsum");
    req->execute();
    return req;
  };
  std::string outOfRange = R"({"range":{"field":"value","gte":100}})";
  ASSERT_NE(nullptr, writer().snapshots.readers.getReader()->collectionSummary());
  EXPECT_TRUE(search(outOfRange)->ok());

  setValueType(api::FieldDef::FieldClass::DATE);
  EXPECT_EQ(nullptr, writer().snapshots.readers.getReader()->collectionSummary());
  auto mismatched = search(outOfRange);
  ASSERT_FALSE(mismatched->ok());
  EXPECT_NE(std::string::npos, mismatched->errorMsg().find("persisted type 'int'")) << mismatched->errorMsg();
  // Requests that do not use the field still answer, through segment proofs.
  auto other = search(R"({"match":{"id":"zz"}})");
  ASSERT_TRUE(other->ok()) << other->errorMsg();
  EXPECT_EQ(0u, other->getDocs("q").size());
}
