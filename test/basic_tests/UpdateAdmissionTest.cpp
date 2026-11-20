// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include <atomic>
#include <chrono>
#include <future>
#include <memory>
#include <string>
#include <thread>
#include <gtest/gtest.h>

#include "test/CollectionHelper.h"
#include "test/GrpcClient.h"
#include "test/GrpcLuxirTest.h"
#include "test/HttpReq.h"
#include "test/TestUtils.h"
#include "luxir/server/HttpServer.h"
#include "luxir/server/ProtoUpdateMessage.h"
#include "luxir/server/UpdateAdmission.h"
#include "luxir/util/Signal.h"

using namespace luxir;
using namespace luxir::test;

namespace {

// Holds the first update to `collection` inside the update graph until
// released, so it stays admitted.  Releases on destruction.
class HeldUpdate {
  struct State {
    std::string collection;
    std::atomic<bool> taken{false};
    std::promise<void> entered;
    std::promise<void> release;
    std::shared_future<void> released = release.get_future().share();
  };
  std::shared_ptr<State> state = std::make_shared<State>();
  bool releasedOnce = false;

public:
  explicit HeldUpdate(std::string collection) {
    state->collection = std::move(collection);
    Signal::listen("protoUpdateHandle", [state = state](void* message, void*, void*) -> void* {
      auto* msg = (ProtoUpdateMessage*)message;
      if (msg->req->collection != state->collection || state->taken.exchange(true)) return nullptr;
      state->entered.set_value();
      state->released.wait_for(std::chrono::seconds(30));
      return nullptr;
    });
  }
  ~HeldUpdate() { release(); }
  bool waitEntered() {
    return state->entered.get_future().wait_for(std::chrono::seconds(10)) == std::future_status::ready;
  }
  void release() {
    if (releasedOnce) return;
    releasedOnce = true;
    state->release.set_value();
  }
};

// Polls for a condition the server reaches asynchronously.
template <class Pred>
bool eventually(Pred pred) {
  for (int i = 0; i < 1000; i++) {
    if (pred()) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return pred();
}

grpc::Status unaryUpdate(grpc::Channel* channel, std::string_view collection, std::string_view id) {
  CollectionHelper::UpdateBuilder b;
  b.collection(collection).add(flatdoc("id", std::string(id), "t_w", std::string("admitted")));
  grpc::ClientContext context;
  Reply<api::UpdateResponse> response;
  return hppUnaryCall(channel, rpc::Update, &context, b.finish(), &response);
}

}  // namespace

TEST(UpdateAdmissionUnitTest, limitsWakeAndCharge) {
  IndexRamBudget ram(1 << 20);
  UpdateAdmission admission(2, 100, &ram);
  auto a = admission.tryAdmit(60);
  ASSERT_TRUE(a);
  EXPECT_EQ(60, ram.reservedBytes());
  EXPECT_EQ(60, ram.drainingBytesCount());  // transient: not inverter pressure
  EXPECT_FALSE(admission.tryAdmit(41));     // over the byte limit
  auto b = admission.tryAdmit(40);
  ASSERT_TRUE(b);
  EXPECT_FALSE(admission.tryAdmit(0));      // over the count limit
  EXPECT_EQ(2u, admission.snapshot().rejected);

  int resumed = 0;
  EXPECT_FALSE(admission.whenAvailable([&] { resumed++; }));
  EXPECT_EQ(1u, admission.snapshot().paused);
  b->release();
  b->release();  // idempotent
  EXPECT_EQ(1, resumed);
  EXPECT_TRUE(admission.whenAvailable([&] { resumed++; }));

  a.reset();
  EXPECT_EQ(0, admission.snapshot().updates);
  EXPECT_EQ(0, ram.reservedBytes());
  // Alone, an update larger than the byte limit is admitted.
  EXPECT_TRUE(admission.tryAdmit(1000));
}

class UpdateAdmissionTest : public GrpcLuxirTest {
public:
  std::shared_ptr<grpc::Channel> channel = getChannel();
  UpdateAdmission& admission = luxirNode->getUpdateAdmission();

  void TearDown() override {
    const auto& ingest = luxirNode->getConfig().ingest;
    admission.setLimits(ingest.max_inflight_updates, ingest.max_inflight_update_bytes);
    GrpcLuxirTest::TearDown();
  }
};

// Past the node's limit a unary Update is refused, and admits again once the
// update holding the slot completes.
TEST_F(UpdateAdmissionTest, unaryUpdateOverLimitIsResourceExhausted) {
  admission.setLimits(1, 0);
  HeldUpdate held("adm_unary_hold");
  auto first = std::async(std::launch::async, [&] { return unaryUpdate(channel.get(), "adm_unary_hold", "h"); });
  ASSERT_TRUE(held.waitEntered());

  auto rejectedBefore = admission.snapshot().rejected;
  auto refused = unaryUpdate(channel.get(), "adm_unary", "r");
  EXPECT_EQ(grpc::StatusCode::RESOURCE_EXHAUSTED, refused.error_code()) << refused.error_message();
  EXPECT_EQ(rejectedBefore + 1, admission.snapshot().rejected);

  held.release();
  EXPECT_TRUE(first.get().ok());
  EXPECT_TRUE(unaryUpdate(channel.get(), "adm_unary", "a").ok());
  EXPECT_EQ(0, admission.snapshot().updates);
}

// An UpdateStream stops reading while the node is full and resumes when the
// slot frees; nothing is refused.
TEST_F(UpdateAdmissionTest, updateStreamPausesAndResumes) {
  admission.setLimits(1, 0);
  HeldUpdate held("adm_stream_hold");
  auto first = std::async(std::launch::async, [&] { return unaryUpdate(channel.get(), "adm_stream_hold", "h"); });
  ASSERT_TRUE(held.waitEntered());
  auto pausedBefore = admission.snapshot().paused;
  auto rejectedBefore = admission.snapshot().rejected;

  grpc::ClientContext context;
  HppClientReaderWriter<api::UpdateRequest, api::UpdateResponse> stream(channel.get(), rpc::UpdateStream, &context);
  for (int i = 0; i < 3; i++) {
    CollectionHelper::UpdateBuilder b;
    b.collection("adm_stream").add(flatdoc("id", std::to_string(i), "t_w", std::string("streamed")));
    ASSERT_TRUE(stream.Write(b.finish()));
  }
  ASSERT_TRUE(stream.WritesDone());
  ASSERT_TRUE(eventually([&] { return admission.snapshot().paused > pausedBefore; }));
  EXPECT_EQ(1, admission.snapshot().updates);  // the stream read nothing

  held.release();
  int responses = 0;
  Reply<api::UpdateResponse> reply;
  while (stream.Read(&reply)) {
    EXPECT_EQ(api::UpdateResponse_::Status::OK, reply.msg.status);
    responses++;
  }
  EXPECT_TRUE(stream.Finish().ok());
  EXPECT_EQ(3, responses);
  EXPECT_TRUE(first.get().ok());
  EXPECT_EQ(0, admission.snapshot().updates);
  EXPECT_EQ(rejectedBefore, admission.snapshot().rejected);
}

// Every way an admitted update ends gives its slot back: a refused
// collection, a document error, and a failed writer.
TEST_F(UpdateAdmissionTest, errorsReleaseAdmission) {
  admission.setLimits(1, 0);
  EXPECT_EQ(grpc::StatusCode::INVALID_ARGUMENT, unaryUpdate(channel.get(), "../bad", "x").error_code());
  EXPECT_EQ(0, admission.snapshot().updates);

  CollectionHelper::UpdateBuilder bad;
  bad.collection("adm_errors").allOrNone().add(flatdoc("id", std::string("a"), "n_i", std::string("nan")));
  grpc::ClientContext context;
  Reply<api::UpdateResponse> response;
  EXPECT_TRUE(hppUnaryCall(channel.get(), rpc::Update, &context, bad.finish(), &response).ok());
  EXPECT_EQ(api::UpdateResponse_::Status::ERROR, response.msg.status);
  EXPECT_EQ(0, admission.snapshot().updates);

  auto collection = luxirNode->getOrCreateCollection("adm_failed");
  collection->getShard()->requireIndexWriter()->fail("test failure");
  EXPECT_FALSE(unaryUpdate(channel.get(), "adm_failed", "f").ok());
  EXPECT_EQ(0, admission.snapshot().updates);
  EXPECT_EQ(0, admission.snapshot().bytes);
  EXPECT_TRUE(unaryUpdate(channel.get(), "adm_errors", "ok").ok());
  luxirNode->deleteCollection("adm_failed");
}

// HTTP on its own node: small NDJSON batches, so a stream cuts batches
// before its EOF barrier.
class UpdateAdmissionHttpTest : public LuxirTest {
protected:
  LuxirNode node{[] {
    LuxirConfig config;
    config.ingest.stream_batch_docs = 2;
    config.ingest.max_inflight_updates = 1;
    return config;
  }()};
  HttpServer server{node, 2, 0};

  void SetUp() override { server.start(); }
  void TearDown() override { server.shutdown(); }

  http::response<http::string_body> post(std::string_view collection, std::string body,
                                         std::string_view contentType = "application/json") {
    return httpRequest(server.getPort(), http::verb::post,
                       "/collections/" + std::string(collection) + "/_update", std::move(body), contentType);
  }
};

TEST_F(UpdateAdmissionHttpTest, bufferedUpdateOverLimitIs429) {
  HeldUpdate held("http_hold");
  auto first = std::async(std::launch::async, [&] { return post("http_hold", R"({"docs":[{"id":"h"}]})"); });
  ASSERT_TRUE(held.waitEntered());

  auto refused = post("http_other", R"({"docs":[{"id":"r"}]})");
  EXPECT_EQ(429, refused.result_int()) << refused.body();
  EXPECT_EQ("1", refused[http::field::retry_after]);
  EXPECT_NE(std::string::npos, refused.body().find("too_many_updates")) << refused.body();

  held.release();
  EXPECT_EQ(200, first.get().result_int());
  EXPECT_EQ(400, post("http_other", "{not json").result_int());
  EXPECT_EQ(0, node.getUpdateAdmission().snapshot().updates);
  EXPECT_EQ(200, post("http_other", R"({"docs":[{"id":"a"}]})").result_int());
}

// An NDJSON stream waits on the node's admission (pausing its reads) rather
// than failing, and indexes everything once there is room.
TEST_F(UpdateAdmissionHttpTest, ndjsonRespectsNodeAdmission) {
  auto& admission = node.getUpdateAdmission();
  HeldUpdate held("ndjson_hold");
  auto first = std::async(std::launch::async, [&] { return post("ndjson_hold", R"({"docs":[{"id":"h"}]})"); });
  ASSERT_TRUE(held.waitEntered());

  std::string body;
  for (int i = 0; i < 6; i++) body += R"({"id":"n)" + std::to_string(i) + R"(","t_w":"gated"})" "\n";
  body += R"({"_end_":{"commit":{}}})" "\n";
  auto stream = std::async(std::launch::async, [&] { return post("ndjson_gate", body, "application/x-ndjson"); });
  ASSERT_TRUE(eventually([&] { return admission.snapshot().paused > 0; }));
  EXPECT_EQ(1, admission.snapshot().updates);

  held.release();
  auto response = stream.get();
  ASSERT_EQ(200, response.result_int()) << response.body();
  EXPECT_EQ(200, first.get().result_int());
  EXPECT_EQ(0u, admission.snapshot().rejected);
  EXPECT_EQ(0, admission.snapshot().updates);

  HttpReq query(server.getPort());
  query.collection("ndjson_gate").matchQuery("t_w", "gated").fields({"id"}).limit(10).withStats().execute();
  ASSERT_EQ(200, query.status()) << query.rawResponse();
  EXPECT_EQ(6, query.found()) << query.rawResponse();
}
