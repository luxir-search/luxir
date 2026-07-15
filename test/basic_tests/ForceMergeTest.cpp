#include <algorithm>
#include <atomic>
#include <chrono>
#include <future>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "solux/index/IndexWriter.h"
#include "solux/schema/Schema.h"
#include "solux/util/Signal.h"
#include "test/CollectionHelper.h"
#include "test/LocalReq.h"
#include "test/SoluxTest.h"
#include "test/TestUtils.h"

namespace solux::test {
namespace {

using namespace std::chrono_literals;

IndexResult commit(CollectionHelper& helper, uint32_t maxSegments = 0,
                   bool waitForMerges = false) {
  CollectionHelper::UpdateBuilder request;
  request.commit(waitForMerges, maxSegments);
  return helper.submit(request);
}

bool addSegment(CollectionHelper& helper, std::string_view prefix, int count) {
  CollectionHelper::UpdateBuilder request;
  for (int i = 0; i < count; i++) {
    request.add(flatdoc("id", std::string(prefix) + std::to_string(i),
                        "text_w", std::string("force merge")));
  }
  request.commit();
  return helper.submit(request).success;
}

void enableL2VectorSuffix(Collection& collection) {
  std::pmr::monotonic_buffer_resource arena;
  solux::api::SchemaDef definition;
  auto* field = solux::api::build::allocArray(definition.fields, 1, arena);
  field->name = "_v";
  field->field_class = solux::api::FieldDef_::FieldClass::VECTOR;
  field->abstract = true;
  field->column_stored = true;
  field->vector.emplace().metric = solux::api::VectorParams_::Metric::L2;
  auto base = collection.getSchema();
  collection.setSchema(Schema::fromProto(definition, base.get()));
}

class AsyncForceCommit final : public UpdateMessage {
  std::promise<bool> completed;

public:
  AsyncForceCommit(uint32_t maxSegments, bool waitForMerges) {
    commit = COMMIT;
    this->maxSegments = (int32_t)maxSegments;
    this->waitForMerges = waitForMerges;
  }

  std::future<bool> getFuture() { return completed.get_future(); }
  void handle(IndexWriter& iw) override { unused(iw); }
  void done(IndexWriter& iw) override {
    unused(iw);
    completed.set_value(!result.errored());
    delete this;
  }
};

std::future<bool> submitForce(IndexWriter& writer, uint32_t maxSegments,
                              bool waitForMerges = false) {
  auto* message = new AsyncForceCommit(maxSegments, waitForMerges);
  auto future = message->getFuture();
  if (!writer.submitUpdate(message)) {
    delete message;
    throw std::runtime_error("Force-merge test submission rejected");
  }
  return future;
}

class MergeGate {
  std::promise<void> startedPromise;
  std::shared_future<void> started;
  std::promise<void> releasePromise;
  std::shared_future<void> release;
  std::atomic_bool first = true;
  bool opened = false;

public:
  MergeGate()
    : started(startedPromise.get_future().share()),
      release(releasePromise.get_future().share()) {
    Signal::listen("mergeStart", [this](void*, void*, void*) -> void* {
      if (first.exchange(false)) {
        startedPromise.set_value();
        release.wait();
      }
      return nullptr;
    });
  }

  ~MergeGate() {
    open();
    Signal::unlisten("mergeStart");
  }

  bool waitStarted() { return started.wait_for(10s) == std::future_status::ready; }
  void open() {
    if (!opened) {
      opened = true;
      releasePromise.set_value();
    }
  }
};

} // namespace

class ForceMergeTest : public SoluxTest {};

TEST_F(ForceMergeTest, concurrentUncommittedIngestionDoesNotBlockPublish) {
  CollectionHelper helper("main");
  auto writer = helper.getIndexWriter();
  writer->mergePolicy->setMergeFactor(100);
  ASSERT_TRUE(addSegment(helper, "base-a-", 2));
  ASSERT_TRUE(addSegment(helper, "base-b-", 2));

  MergeGate gate;
  auto force = submitForce(*writer, 1);
  ASSERT_TRUE(gate.waitStarted());

  ASSERT_TRUE(helper.index(flatdoc("id", "later-uncommitted", "text_w",
                                   "later uncommitted document"),
                           UpdateMessage::NO_COMMIT).success);

  gate.open();
  ASSERT_EQ(std::future_status::ready, force.wait_for(10s));
  EXPECT_TRUE(force.get());
  EXPECT_EQ(1u, helper.durableSegmentCount());
  EXPECT_EQ((std::vector<std::string>{"base-a-0", "base-a-1", "base-b-0", "base-b-1"}),
            allIds(helper));

  writer->commit();
  EXPECT_EQ((std::vector<std::string>{"base-a-0", "base-a-1", "base-b-0", "base-b-1",
                                      "later-uncommitted"}),
            allIds(helper));
}

TEST_F(ForceMergeTest, overlappingRequestsSerializeThroughDurablePublish) {
  CollectionHelper helper("main");
  auto writer = helper.getIndexWriter();
  writer->mergePolicy->setMergeFactor(100);
  ASSERT_TRUE(addSegment(helper, "overlap-a-", 2));
  ASSERT_TRUE(addSegment(helper, "overlap-b-", 2));

  MergeGate gate;
  auto first = submitForce(*writer, 1);
  ASSERT_TRUE(gate.waitStarted());
  auto second = submitForce(*writer, 1);
  gate.open();

  ASSERT_EQ(std::future_status::ready, first.wait_for(10s));
  ASSERT_EQ(std::future_status::ready, second.wait_for(10s));
  EXPECT_TRUE(first.get());
  EXPECT_TRUE(second.get());
  EXPECT_EQ(1u, helper.durableSegmentCount());
}

TEST_F(ForceMergeTest, waitForMergesComposesWithMaxSegments) {
  CollectionHelper helper("main");
  auto writer = helper.getIndexWriter();
  writer->mergePolicy->setMergeFactor(100);
  ASSERT_TRUE(addSegment(helper, "wait-a-", 1));
  writer->mergePolicy->setMergeFactor(2);

  MergeGate gate;
  ASSERT_TRUE(addSegment(helper, "wait-b-", 1));
  ASSERT_TRUE(gate.waitStarted());
  auto force = submitForce(*writer, 1, /*waitForMerges=*/true);
  gate.open();

  ASSERT_EQ(std::future_status::ready, force.wait_for(10s));
  EXPECT_TRUE(force.get());
  EXPECT_FALSE(writer->testMergeRunning());
  EXPECT_EQ(1u, helper.durableSegmentCount());
}

TEST_F(ForceMergeTest, mergeFailureReturnsErrorAndPreservesSources) {
  CollectionHelper helper("main");
  enableL2VectorSuffix(helper.collection());
  auto writer = helper.getIndexWriter();
  writer->mergePolicy->setMergeFactor(100);
  ASSERT_TRUE(addSegment(helper, "failure-a-", 2));
  ASSERT_TRUE(addSegment(helper, "failure-b-", 2));
  helper.commit({"vec.embedding_v"});
  ASSERT_TRUE(writer->testActiveVectorOverlayName("vec.embedding_v"));

  Signal::listen("mergedPostingsReader", [](void*, void*, void*) -> void* {
    throw std::runtime_error("injected client merge failure");
  });
  IndexResult result;
  {
    ExpectLog quiet("injected client merge failure");
    result = commit(helper, 1);
  }
  Signal::unlisten("mergedPostingsReader");

  EXPECT_FALSE(result.success);
  EXPECT_EQ(IndexResult::Status::ERROR, result.status);
  EXPECT_NE(std::string::npos, result.error_message.find("Data commit succeeded"));
  EXPECT_EQ(2u, helper.durableSegmentCount());
  EXPECT_EQ((std::vector<std::string>{"failure-a-0", "failure-a-1", "failure-b-0", "failure-b-1"}),
            allIds(helper));
}

TEST_F(ForceMergeTest, targetAboveOneSelectsSmallestLiveSegmentsAndPolicyRecovers) {
  CollectionHelper helper("main");
  auto writer = helper.getIndexWriter();
  writer->mergePolicy->setMergeFactor(100);
  ASSERT_TRUE(addSegment(helper, "large-", 5));
  ASSERT_TRUE(addSegment(helper, "small-", 2));
  ASSERT_TRUE(addSegment(helper, "medium-", 4));
  ASSERT_TRUE(addSegment(helper, "tiny-", 1));

  ASSERT_TRUE(commit(helper, 3).success);
  auto info = readDurableIndexInfo(writer->dir);
  ASSERT_EQ(3u, info.info.segments.size());
  std::vector<int32_t> liveDocs;
  for (const auto& segment : info.info.segments) liveDocs.push_back(segment.live_docs);
  std::sort(liveDocs.begin(), liveDocs.end());
  EXPECT_EQ((std::vector<int32_t>{3, 4, 5}), liveDocs);

  writer->mergePolicy->setMergeFactor(2);
  ASSERT_TRUE(addSegment(helper, "policy-", 1));
  EXPECT_TRUE(commit(helper, 0, /*waitForMerges=*/true).success);
  EXPECT_FALSE(writer->testMergeRunning());
}

TEST_F(ForceMergeTest, dirtySingletonRewritesAndCleanSingletonStillPublishes) {
  CollectionHelper helper("main");
  auto writer = helper.getIndexWriter();
  writer->mergePolicy->setMergeFactor(100);
  ASSERT_TRUE(addSegment(helper, "singleton-", 3));
  EXPECT_TRUE(helper.deleteById("singleton-1", UpdateMessage::COMMIT).success);

  auto dirty = readDurableIndexInfo(writer->dir);
  ASSERT_EQ(1u, dirty.info.segments.size());
  uint64_t dirtySegId = dirty.info.segments[0].seg_id;
  ASSERT_LT(dirty.info.segments[0].live_docs, dirty.info.segments[0].max_doc);

  ASSERT_TRUE(commit(helper, 1).success);
  auto rewritten = readDurableIndexInfo(writer->dir);
  ASSERT_EQ(1u, rewritten.info.segments.size());
  EXPECT_NE(dirtySegId, rewritten.info.segments[0].seg_id);
  EXPECT_EQ(2, rewritten.info.segments[0].max_doc);
  EXPECT_EQ(2, rewritten.info.segments[0].live_docs);
  uint64_t cleanSegId = rewritten.info.segments[0].seg_id;
  uint64_t beforeNoOpGen = rewritten.info.index_gen;

  ASSERT_TRUE(commit(helper, 1).success);
  auto noOp = readDurableIndexInfo(writer->dir);
  ASSERT_EQ(1u, noOp.info.segments.size());
  EXPECT_EQ(cleanSegId, noOp.info.segments[0].seg_id);
  EXPECT_GT(noOp.info.index_gen, beforeNoOpGen);
}

TEST_F(ForceMergeTest, emptyCommitIsResumeForm) {
  CollectionHelper helper("main");
  helper.getIndexWriter()->mergePolicy->setMergeFactor(100);
  ASSERT_TRUE(addSegment(helper, "resume-a-", 1));
  ASSERT_TRUE(addSegment(helper, "resume-b-", 1));

  CollectionHelper::UpdateBuilder empty;
  empty.commit(false, 1);
  EXPECT_TRUE(helper.submit(empty).success);
  EXPECT_EQ(1u, helper.durableSegmentCount());
}

} // namespace solux::test
