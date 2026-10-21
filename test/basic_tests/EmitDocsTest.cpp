// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include <array>
#include <atomic>
#include <functional>
#include <latch>
#include <memory_resource>
#include <string>
#include <thread>
#include <utility>
#include <vector>
#include <oneapi/tbb/global_control.h>
#include <oneapi/tbb/task_group.h>

#include "luxir/reader/FieldMetadataMemory.h"
#include "luxir/search/EmitDocs.h"
#include "luxir/search/ops/SearchOp.h"
#include "luxir/util/Signal.h"
#include "test/CollectionHelper.h"
#include "test/LocalReq.h"
#include "test/LuxirTest.h"
#include "test/QueryBuild.h"
#include "test/SchemaBuilder.h"
#include "test/TestUtils.h"

namespace luxir::test {

namespace {

class PausingReq : public LocalReq {
public:
  std::function<void()> resume;
  std::latch parked{1};
  bool paused = false;
  using LocalReq::LocalReq;

  ReplyStatus reply(SearchResponse& response) override {
    LocalReq::reply(response);
    if (!response.last && !paused) {
      paused = true;
      return ReplyStatus::PAUSE;
    }
    return ReplyStatus::OK;
  }

  void resumeWhenDrained(std::function<void()> callback) override {
    resume = std::move(callback);
    parked.count_down();
  }
};

}

class EmitDocsTest : public LuxirTest {
protected:
  static std::string bodyValue(int64_t i) { return std::string(300, 'x') + std::to_string(i); }

  void indexDocs(CollectionHelper& helper, int count, bool stored = false) {
    if (stored) {
      SchemaBuilder schema;
      auto& field = schema.field("body_s");
      field.type = api::FieldDef::FieldClass::STRING;
      field.column = true;
      field.stored = true;
      field.long_terms = api::FieldDef::LongTerms::HASH128;
      schema.set(helper.collection());
    }
    int segments = stored ? 2 : 1;
    for (int seg = 0; seg < segments; seg++) {
      std::vector<Doc> docs;
      for (int i = seg; i < count; i += segments) {
        docs.push_back(flatdoc("id", "d" + std::to_string(i), "value_i", (int64_t)i,
                              "body_s", bodyValue(i)));
      }
      helper.indexAll(docs, UpdateMessage::COMMIT);
    }
    ASSERT_EQ((size_t)segments, helper.getIndexWriter()->snapshots.readers.getReader()->segments().size());
  }

  void addDocsOp(LocalReq& req, const char* name, int count, bool stored = false) {
    auto& top = req.topDocs(name).allQuery().limit(count).batchSize(2)
        .documentFormat(api::DocFormat::COLUMNS);
    if (stored) top.fields({"id", "value_i", "body_s"});
    else top.fields({"id", "value_i"});
    qb::sort(top, "value_i", qb::ASC);
  }

  void assertBatch(const SearchResponse& response, std::string_view opName, bool stored = false) {
    const auto* docs = response.proto.ops.at(opName)->docList();
    ASSERT_NE(nullptr, docs);
    const auto& ids = std::get<api::ColStr>(docs->columns.at("id").kind);
    const auto& values = std::get<api::ColInt>(docs->columns.at("value_i").kind);
    ASSERT_EQ((size_t)docs->row_count, ids.v.size());
    ASSERT_EQ(ids.v.size(), values.v.size());
    for (int i = 0; i < docs->row_count; i++) {
      int64_t expected = docs->offset + i;
      EXPECT_EQ("d" + std::to_string(expected), ids.v[i]);
      EXPECT_EQ(expected, values.v[i]);
      if (stored) {
        const auto& body = std::get<api::ColStr>(docs->columns.at("body_s").kind);
        EXPECT_EQ(bodyValue(expected), body.v[i]);
      }
    }
  }

  void assertError(const LocalReq& req) {
    ASSERT_FALSE(req.responses.empty());
    const auto& final = *req.responses.back();
    EXPECT_TRUE(final.last);
    EXPECT_FALSE(final.proto.more);
    EXPECT_EQ("invalid_request", errorCode(final.proto));
    EXPECT_EQ("sibling failure", errorMessage(final.proto));
    EXPECT_TRUE(final.proto.ops.empty());
    int finals = 0;
    for (const auto* response : req.responses) finals += response->last;
    EXPECT_EQ(1, finals);
  }

  void siblingFailure(bool onCaller, bool finalBatch, bool stored) {
    SCOPED_TRACE(::testing::Message() << "caller=" << onCaller << ", final=" << finalBatch
                 << ", stored=" << stored);
    CollectionHelper helper("emit_" + std::to_string(onCaller) + std::to_string(finalBatch)
                            + std::to_string(stored));
    int count = finalBatch ? 2 : 5;
    indexDocs(helper, count, stored);
    auto req = localReq(helper.getSearchEngine());
    req->collection(helper.collection().getId().name);
    addDocsOp(*req.get(), "a", count, stored);
    addDocsOp(*req.get(), "b", count, stored);
    oneapi::tbb::global_control width(oneapi::tbb::global_control::max_allowed_parallelism, 2);
    auto caller = std::this_thread::get_id();
    std::string emitter;
    std::latch loadersPublished{1}, failed{1};
    std::atomic<int> waits{0};
    Signal::listen("searchRootChildStart", [&](void* ptr, void*, void*) -> void* {
      if ((std::this_thread::get_id() == caller) == onCaller) {
        emitter = std::string(((SearchOp::Calculator*)ptr)->getOp().name);
        return nullptr;
      }
      loadersPublished.wait();
      // Cancel before releasing the held wait so queued loaders cannot race
      // the sibling's cancellation. The exception still follows submitBody.
      req->tg->cancel();
      failed.count_down();
      throw RequestError("sibling failure", "invalid_request");
    });
    const char* signal = stored ? "emitDocsStoredWait" : "emitDocsColumnsWait";
    Signal::listen(signal, [&](void*, void*, void*) -> void* {
      if (waits.fetch_add(1) == 0) {
        // With one worker, the emitter has published loaders that cannot run
        // until this wait; the other thread is held in the sibling op.
        loadersPublished.count_down();
        failed.wait();
      }
      return nullptr;
    });
    helper.getSearchEngine().dispatch(*req.get(), -1, SearchEngine::Receiver::MAY_EXECUTE);
    Signal::unlisten(signal);
    Signal::unlisten("searchRootChildStart");
    ASSERT_GE(waits.load(), 1);
    if (finalBatch) EXPECT_EQ(1u, req->responses.size());
    else EXPECT_EQ(3u, req->responses.size());
    for (const auto* response : req->responses) {
      if (response->proto.more) assertBatch(*response, emitter, stored);
    }
    assertError(*req.get());
  }
};

TEST_F(EmitDocsTest, siblingFailureDuringColumnLoads) {
  for (bool onCaller : {false, true}) {
    for (bool finalBatch : {false, true}) siblingFailure(onCaller, finalBatch, false);
  }
}

TEST_F(EmitDocsTest, defaultWideProjectionDecodesOnlyHitSegments) {
  CollectionHelper helper;
  helper.getIndexWriter()->mergePolicy->setMergeFactor(100);
  constexpr int FIELDS = 200;
  for (int seg = 0; seg < 4; seg++) {
    std::vector<Doc> docs;
    for (int doc = 0; doc < 20; doc++) {
      Doc values = flatdoc("id", std::format("{}-{}", seg, doc));
      for (int f = 0; f < FIELDS; f++) {
        values.push_back({std::format("f{:04}_i", f), (int64_t)(1000 * seg + doc)});
      }
      docs.push_back(std::move(values));
    }
    ASSERT_TRUE(helper.indexAll(docs, UpdateMessage::COMMIT).success);
  }
  auto reader = helper.getIndexWriter()->snapshots.readers.getReader();
  ASSERT_EQ(4u, reader->segments().size());
  std::vector<PostingsReader::FieldCacheStats> before;
  for (auto& seg : reader->segments()) before.push_back(seg.postingsReader().fieldCacheStats());
  for (int parallel : {-1, 0, 1}) {
    auto req = localReq(helper.getSearchEngine());
    req->collection("main").topDocs("q").allQuery().limit(10)
        .documentFormat(api::DocFormat::COLUMNS);
    req->execute(parallel);
    ASSERT_OK(req);
    auto docs = req->getDocs();
    ASSERT_EQ(10u, docs.size());
    for (int doc = 0; doc < 10; doc++) {
      auto* id = find(docs[(size_t)doc], "id");
      auto* value = find(docs[(size_t)doc], "f0000_i");
      ASSERT_NE(nullptr, id);
      ASSERT_NE(nullptr, value);
      EXPECT_EQ(std::format("0-{}", doc), std::get<std::string>(*id));
      EXPECT_EQ(doc, std::get<int64_t>(*value));
    }
    EXPECT_GE(reader->segments()[0].postingsReader().fieldCacheStats().entries, (size_t)FIELDS);
    for (size_t seg = 1; seg < reader->segments().size(); seg++) {
      auto stats = reader->segments()[seg].postingsReader().fieldCacheStats();
      EXPECT_EQ(before[seg].entries, stats.entries) << seg;
      EXPECT_EQ(before[seg].pages, stats.pages) << seg;
    }
  }
}

TEST_F(EmitDocsTest, countOnlyResponsesSkipReturnFieldMetadata) {
  CollectionHelper helper;
  constexpr int FIELDS = 200;
  Doc values = flatdoc("id", std::string("one"));
  for (int f = 0; f < FIELDS; f++) values.push_back({std::format("f{:04}_i", f), (int64_t)f});
  ASSERT_TRUE(helper.indexAll({values}, UpdateMessage::COMMIT).success);
  auto names = [] { return (int64_t)fieldMetadataMemory.names.load(); };
  int64_t before = names();
  auto count = localReq(helper.getSearchEngine());
  count->collection("main").topDocs("q").allQuery().limit(0).countLimit();
  count->execute(0);
  ASSERT_OK(count);
  EXPECT_LT(names() - before, FIELDS);
  int64_t afterCount = names();
  auto page = localReq(helper.getSearchEngine());
  page->collection("main").topDocs("q").allQuery().limit(1);
  page->execute(0);
  ASSERT_OK(page);
  ASSERT_EQ(1u, page->getDocs().size());
  EXPECT_GE(names() - afterCount, FIELDS);
}

TEST_F(EmitDocsTest, siblingFailureDuringStoredRereads) {
  siblingFailure(false, false, true);
}

TEST_F(EmitDocsTest, siblingFailureWhilePausedPreservesOriginalError) {
  CollectionHelper helper;
  indexDocs(helper, 5);
  auto* arena = createArena();
  auto* paused = arenaCreate<PausingReq>(*arena, helper.getSearchEngine(), *arena);
  auto req = LocalReqHandle(paused);
  req->collection(helper.collection().getId().name);
  addDocsOp(*req.get(), "a", 5);
  addDocsOp(*req.get(), "b", 5);
  oneapi::tbb::global_control width(oneapi::tbb::global_control::max_allowed_parallelism, 2);
  auto caller = std::this_thread::get_id();
  std::string emitter;
  Signal::listen("searchRootChildStart", [&](void* ptr, void*, void*) -> void* {
    if (std::this_thread::get_id() != caller) {
      emitter = std::string(((SearchOp::Calculator*)ptr)->getOp().name);
      return nullptr;
    }
    paused->parked.wait();
    throw RequestError("sibling failure", "invalid_request");
  });
  helper.getSearchEngine().dispatch(*req.get(), -1, SearchEngine::Receiver::MAY_EXECUTE);
  Signal::unlisten("searchRootChildStart");
  ASSERT_EQ(1u, req->responses.size());
  ASSERT_TRUE(req->responses[0]->proto.more);
  assertBatch(*req->responses[0], emitter);
  auto resume = std::move(paused->resume);
  ASSERT_TRUE(resume);
  resume();
  ASSERT_EQ(2u, req->responses.size());
  assertError(*req.get());
}

TEST_F(EmitDocsTest, resumedLoadsIgnoreUnrelatedContextCancellation) {
  CollectionHelper helper;
  indexDocs(helper, 5);
  auto* arena = createArena();
  auto* paused = arenaCreate<PausingReq>(*arena, helper.getSearchEngine(), *arena);
  auto req = LocalReqHandle(paused);
  req->collection(helper.collection().getId().name);
  addDocsOp(*req.get(), "q", 5);
  helper.getSearchEngine().dispatch(*req.get(), -1, SearchEngine::Receiver::MAY_EXECUTE);
  ASSERT_EQ(1u, req->responses.size());
  auto resume = std::move(paused->resume);
  ASSERT_TRUE(resume);
  oneapi::tbb::task_group_context context(oneapi::tbb::task_group_context::isolated);
  oneapi::tbb::task_group group(context);
  EXPECT_EQ(oneapi::tbb::canceled, group.run_and_wait([&] {
    context.cancel_group_execution();
    resume();
  }));
  ASSERT_EQ(3u, req->responses.size());
  for (const auto* response : req->responses) {
    EXPECT_FALSE(hasError(response->proto));
    assertBatch(*response, "q");
  }
  EXPECT_TRUE(req->responses.back()->last);
}


// A col_val column (a type with no typed arm) follows the same placement as
// typed columns: an unset slot is missing, ROWS scatters the set values,
// ADAPTIVE keeps a dense one, COLUMNS leaves out an all-missing discovered one.
TEST(EmitDocsPlacement, valueColumnPlacement) {
  using Pair = std::pair<std::string_view, api::Column>;
  std::pmr::monotonic_buffer_resource mr;
  api::Val vals[3];
  vals[0].kind = true;
  vals[2].kind = false;
  ReturnField rf{"flag_b", "flag_b", nullptr, ReturnField::Kind::PHYSICAL, true, {}, {}};
  std::array<const ReturnField*, 1> fields{&rf};
  std::array<const uint8_t*, 1> present{nullptr};
  std::vector<uint8_t> fate;
  std::vector<uint32_t> rowCaps;
  auto place = [&](api::DocFormat format, std::span<const api::Val> v, api::DocList& docList) {
    auto* column = (Pair*)mr.allocate(sizeof(Pair), alignof(Pair));
    new (column) Pair{"flag_b", {}};
    column->second.kind = api::ColVal{v};
    SearchResponse::ColumnsType columns(std::span<const Pair>(column, 1)), moved;
    placeBatchColumns(format, fields, present, columns, moved, v.size(), false, fate, rowCaps, mr);
    if (!rowCaps.empty()) scatterColumnsToRows(moved, docList, v.size(), rowCaps, mr);
    docList.columns = columns;
  };

  api::DocList rows;
  place(api::DocFormat::ROWS, vals, rows);
  EXPECT_TRUE(rows.columns.empty());
  EXPECT_EQ((std::vector<uint32_t>{1, 0, 1}), rowCaps);
  ASSERT_EQ(3u, rows.docs.size());
  for (size_t i : {0, 2}) {
    const auto* flag = rows.docs[i].fields.find("flag_b");
    ASSERT_NE(nullptr, flag);
    EXPECT_EQ(i == 0, std::get<bool>((**flag).kind));
  }
  EXPECT_TRUE(rows.docs[1].fields.empty());

  api::DocList adaptive;
  place(api::DocFormat::ADAPTIVE, vals, adaptive);
  EXPECT_TRUE(adaptive.docs.empty());
  EXPECT_NE(nullptr, adaptive.columns.find("flag_b"));

  api::Val unset[3];
  api::DocList columns;
  place(api::DocFormat::COLUMNS, unset, columns);
  EXPECT_TRUE(columns.columns.empty());
  EXPECT_TRUE(columns.docs.empty());
}

}
