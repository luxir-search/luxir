// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

// In-process updates (UpdateSubmission.h): the path plugins write through.

#include <atomic>
#include <chrono>
#include <filesystem>
#include <future>
#include <memory>
#include <string>
#include <thread>
#include <gtest/gtest.h>

#include "luxir/search/SearchEngine.h"
#include "test/LocalReq.h"
#include "test/TestUtils.h"
#include "luxir/server/LuxirNode.h"
#include "luxir/server/UpdateSubmission.h"
#include "luxir/util/luxir_util.h"

using namespace luxir;
using namespace luxir::test;

namespace {

using Status = api::UpdateResponse_::Status;

// One submission's outcome; `calls` counts how often its callback ran.
struct Outcome {
  std::atomic<int> calls{0};
  std::promise<std::shared_ptr<UpdateResult>> promise;
  std::shared_future<std::shared_ptr<UpdateResult>> future = promise.get_future().share();

  bool ready(std::chrono::milliseconds wait = std::chrono::seconds(30)) const {
    return future.wait_for(wait) == std::future_status::ready;
  }
  const api::UpdateResponse& response() const { return future.get()->response; }
  std::string errorCode() const { return response().error ? std::string(response().error->code) : ""; }
};

std::shared_ptr<Outcome> submitJson(LuxirNode& node, std::string_view json,
                                    UpdateAdmissionPolicy policy = UpdateAdmissionPolicy::WAIT) {
  auto outcome = std::make_shared<Outcome>();
  auto submission = std::make_unique<UpdateSubmission>();
  submission->parseJson(json);
  submitUpdate(node, std::move(submission), [outcome](std::shared_ptr<UpdateResult> result) {
    if (outcome->calls.fetch_add(1) == 0) outcome->promise.set_value(std::move(result));
  }, policy);
  return outcome;
}

size_t liveDocs(LuxirNode& node, std::string_view collection) {
  auto request = localReq(node.getSearchEngine());
  request->collection(collection).topDocs("q").allQuery().fields({"id"}).limit(-1);
  request->execute();
  return request->getDocs().size();
}

}  // namespace

TEST(UpdateSubmissionTest, indexesAndCommits) {
  LuxirNode node;
  auto outcome = submitJson(node, R"({"collection":"sub","docs":[{"id":"a"},{"id":"b"}],"return_ids":true,"commit":{}})");
  ASSERT_TRUE(outcome->ready());
  EXPECT_EQ(Status::OK, outcome->response().status);
  EXPECT_EQ(2u, outcome->response().ids.size());
  EXPECT_FALSE(outcome->response().commit.empty());
  EXPECT_EQ(2u, liveDocs(node, "sub"));
  EXPECT_EQ(1, outcome->calls.load());
  EXPECT_EQ(0, node.getUpdateAdmission().snapshot().updates);
}

TEST(UpdateSubmissionTest, documentErrorsArePartial) {
  LuxirNode node;
  auto outcome = submitJson(node, R"({"docs":[{"id":"a"},{"id":"b","_version_":7}]})");
  ASSERT_TRUE(outcome->ready());
  EXPECT_EQ(Status::PARTIAL, outcome->response().status);
  ASSERT_EQ(1u, outcome->response().errors.size());
  EXPECT_EQ(1, outcome->response().errors[0].index);
  EXPECT_EQ(1, outcome->calls.load());
}

TEST(UpdateSubmissionTest, refusedBeforeSubmission) {
  {
    LuxirConfig config;
    config.ingest.auto_create_collection = false;
    LuxirNode node(config);
    auto outcome = submitJson(node, R"({"collection":"missing","docs":[{"id":"a"}]})");
    ASSERT_TRUE(outcome->ready());
    EXPECT_EQ(Status::ERROR, outcome->response().status);
    EXPECT_EQ("collection_not_found", outcome->errorCode());
    EXPECT_EQ(0, node.getUpdateAdmission().snapshot().updates);
    EXPECT_EQ(1, outcome->calls.load());
  }
  auto dir = std::filesystem::temp_directory_path() /
      ("luxir_update_submission_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  auto cleanup = scope_guard([&]() { std::filesystem::remove_all(dir); });
  LuxirConfig config;
  config.store.backend = "fs";
  config.store.data_dir = dir.string();
  { LuxirNode writer(config); }
  config.read_only = true;
  LuxirNode node(config);
  auto outcome = submitJson(node, R"({"docs":[{"id":"a"}]})");
  ASSERT_TRUE(outcome->ready());
  EXPECT_EQ(Status::ERROR, outcome->response().status);
  EXPECT_EQ("read_only", outcome->errorCode());
  EXPECT_EQ(1, outcome->calls.load());
}

// Over the node's limit REFUSE completes at once; WAIT parks until a slot
// frees and then indexes.
TEST(UpdateSubmissionTest, admissionRefusesOrWaits) {
  LuxirNode node;
  auto& admission = node.getUpdateAdmission();
  admission.setLimits(1, 0);
  auto held = admission.tryAdmit(1);
  ASSERT_TRUE(held);

  auto refused = submitJson(node, R"({"docs":[{"id":"r"}]})", UpdateAdmissionPolicy::REFUSE);
  ASSERT_TRUE(refused->ready(std::chrono::milliseconds(0)));
  EXPECT_EQ(Status::ERROR, refused->response().status);
  EXPECT_EQ("too_many_updates", refused->errorCode());

  auto parked = submitJson(node, R"({"collection":"parked","docs":[{"id":"w"}],"commit":{}})");
  EXPECT_FALSE(parked->ready(std::chrono::milliseconds(100)));
  EXPECT_EQ(1u, admission.snapshot().paused);
  held.reset();
  ASSERT_TRUE(parked->ready());
  EXPECT_EQ(Status::OK, parked->response().status);
  EXPECT_EQ(1u, liveDocs(node, "parked"));
  EXPECT_EQ(1, refused->calls.load());
  EXPECT_EQ(1, parked->calls.load());
  EXPECT_EQ(0, admission.snapshot().updates);
}

// An update still parked or queued as the node shuts down completes once:
// indexed, refused by its closing writer, or refused with node_closing.
TEST(UpdateSubmissionTest, completesOnceAtShutdown) {
  std::shared_ptr<Outcome> outcome;
  {
    LuxirNode node;
    node.getUpdateAdmission().setLimits(1, 0);
    auto held = node.getUpdateAdmission().tryAdmit(1);
    outcome = submitJson(node, R"({"docs":[{"id":"s"}]})");
    EXPECT_FALSE(outcome->ready(std::chrono::milliseconds(0)));
    held.reset();
  }
  ASSERT_TRUE(outcome->ready());
  auto code = outcome->errorCode();
  EXPECT_TRUE(outcome->response().status == Status::OK || code == "node_closing" || code == "writer_closed") << code;
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  EXPECT_EQ(1, outcome->calls.load());
}
