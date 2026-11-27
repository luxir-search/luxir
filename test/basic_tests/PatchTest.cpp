// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

// Partial updates against the writer: ordering with writes that commit between
// a patch's resolution and the indexing of its result, and versions across a
// writer reopen. The operations themselves are in SourcePatchTest and the
// HTTP surface in PatchHttpTest.

#include <gtest/gtest.h>

#include <chrono>
#include <future>
#include <latch>
#include <memory_resource>

#include "luxir/index/IndexWriter.h"
#include "luxir/reader/Postings.h"
#include "luxir/server/LuxirNode.h"
#include "luxir/server/ProtoUpdateMessage.h"
#include "luxir/util/Signal.h"
#include "test/CollectionHelper.h"
#include "test/LocalReq.h"
#include "test/LuxirTest.h"
#include "test/SchemaBuilder.h"
#include "test/TestUtils.h"

namespace luxir::test {

using namespace std::chrono_literals;

namespace {

// The request a JsonUpdate parses, constructed before its message base.
struct ParsedRequest {
  std::pmr::monotonic_buffer_resource arena;
  api::UpdateRequest request;
  explicit ParsedRequest(std::string_view json) {
    std::string err;
    EXPECT_TRUE(api::read_json(request, json, arena, &err)) << err;
  }
};

// An update request written as JSON, kept alive with its message, which
// reports completion instead of deleting itself.
class JsonUpdate final : private ParsedRequest, public ProtoUpdateMessage {
public:
  std::promise<void> completed;

  explicit JsonUpdate(std::string_view json) : ParsedRequest(json), ProtoUpdateMessage(&request) {}
  void done(IndexWriter&) override { completed.set_value(); }
  bool waitFor(std::chrono::milliseconds timeout) {
    return completed.get_future().wait_for(timeout) == std::future_status::ready;
  }
};

void enableSource(CollectionHelper& helper) {
  SchemaBuilder b;
  b.storedResource(Postings::SOURCE_RESOURCE);
  b.set(helper.collection());
}

// The n_i values of the live documents with id `id`.
std::vector<int64_t> values(CollectionHelper& helper, std::string_view id) {
  auto req = localReq(helper.getSearchEngine());
  req->collection("main").topDocs("q").matchQuery("id", std::string(id)).fields({"n_i"}).limit(-1);
  req->execute();
  EXPECT_TRUE(req->ok()) << req->toString();
  std::vector<int64_t> out;
  for (const auto& doc : req->getDocs()) {
    const auto* value = find(doc, "n_i");
    out.push_back(value ? std::get<int64_t>(*value) : -1);
  }
  return out;
}

}  // namespace

class PatchTest : public LuxirTest {};

// An overwrite that commits after a patch resolved, but before the patched
// document is indexed, must still win: its delete stays pending until the
// patched document is in a segment it reaches.
TEST_F(PatchTest, overwriteBetweenResolutionAndIndexingWins) {
  LuxirNode node;
  CollectionHelper helper(node);
  enableSource(helper);
  auto iw = helper.getIndexWriter();
  ASSERT_TRUE(helper.index(flatdoc("id", "a", "n_i", 1)).success);

  JsonUpdate patch(R"({"patches":[{"id":"a","ops":[{"op":"increment","path":"n_i","value":1}]}]})");
  JsonUpdate overwrite(R"({"docs":[{"id":"a","n_i":100}],"commit":{}})");
  std::latch release(1);
  std::promise<void> resolved, overwriteAdmitted;
  auto cleanup = scope_guard([&] {
    if (!release.try_wait()) release.count_down();
    iw->updateGraph->wait_for_all();
    Signal::unlisten("patchesResolved");
    Signal::unlisten("initiateCommit");
  });
  Signal::listen("initiateCommit", [&](void* msg, void*, void*) -> void* {
    if (msg == static_cast<UpdateMessage*>(&overwrite)) overwriteAdmitted.set_value();
    return nullptr;
  });
  Signal::listen("patchesResolved", [&](void* msg, void*, void*) -> void* {
    if (msg == static_cast<UpdateMessage*>(&patch)) {
      resolved.set_value();
      release.wait();
    }
    return nullptr;
  });

  ASSERT_TRUE(iw->submitUpdate(&patch));
  ASSERT_EQ(std::future_status::ready, resolved.get_future().wait_for(5s));
  // Its commit is admitted before the patch's follow-up message exists, so it
  // neither flushes nor sees the patched document.
  ASSERT_TRUE(iw->submitUpdate(&overwrite));
  ASSERT_EQ(std::future_status::ready, overwriteAdmitted.get_future().wait_for(5s));
  release.count_down();
  ASSERT_TRUE(patch.waitFor(10s));
  ASSERT_TRUE(overwrite.waitFor(10s));
  iw->updateGraph->wait_for_all();
  ASSERT_FALSE(patch.result.errored()) << patch.result.what();
  ASSERT_EQ(1u, patch.patchOutcomes.size());
  EXPECT_EQ(api::PatchResult::Status::UPDATED, patch.patchOutcomes[0].status);
  helper.commit();
  EXPECT_EQ(std::vector<int64_t>{100}, values(helper, "a"));
}

// A second message's patch of the same id, resolved before the first
// message's patched document is indexed, builds on that document.
TEST_F(PatchTest, concurrentPatchesOfOneIdBothApply) {
  LuxirNode node;
  CollectionHelper helper(node);
  enableSource(helper);
  auto iw = helper.getIndexWriter();
  ASSERT_TRUE(helper.index(flatdoc("id", "a", "n_i", 1)).success);

  JsonUpdate first(R"({"patches":[{"id":"a","ops":[{"op":"increment","path":"n_i","value":1}]}]})");
  JsonUpdate second(R"({"patches":[{"id":"a","ops":[{"op":"increment","path":"n_i","value":10}]}]})");
  std::latch release(1);
  std::promise<void> resolved, secondAdmitted;
  auto cleanup = scope_guard([&] {
    if (!release.try_wait()) release.count_down();
    iw->updateGraph->wait_for_all();
    Signal::unlisten("patchesResolved");
    Signal::unlisten("initiateCommit");
  });
  Signal::listen("initiateCommit", [&](void* msg, void*, void*) -> void* {
    if (msg == static_cast<UpdateMessage*>(&second)) secondAdmitted.set_value();
    return nullptr;
  });
  Signal::listen("patchesResolved", [&](void* msg, void*, void*) -> void* {
    if (msg == static_cast<UpdateMessage*>(&first)) {
      resolved.set_value();
      release.wait();
    }
    return nullptr;
  });

  ASSERT_TRUE(iw->submitUpdate(&first));
  ASSERT_EQ(std::future_status::ready, resolved.get_future().wait_for(5s));
  // The second commit is admitted before the first's follow-up exists.
  ASSERT_TRUE(iw->submitUpdate(&second));
  ASSERT_EQ(std::future_status::ready, secondAdmitted.get_future().wait_for(5s));
  release.count_down();
  ASSERT_TRUE(first.waitFor(10s));
  ASSERT_TRUE(second.waitFor(10s));
  iw->updateGraph->wait_for_all();
  ASSERT_FALSE(first.result.errored()) << first.result.what();
  ASSERT_FALSE(second.result.errored()) << second.result.what();
  helper.commit();
  EXPECT_EQ(std::vector<int64_t>{12}, values(helper, "a"));
}

// A version handed out before a reopen is never handed out again, so a stale
// if_version cannot match a document written after the reopen.
TEST_F(PatchTest, versionsAreNotReusedAfterReopen) {
  LuxirNode node;
  CollectionHelper helper(node);
  enableSource(helper);
  auto iw = helper.getIndexWriter();
  ASSERT_TRUE(helper.index(flatdoc("id", "a", "n_i", 1)).success);
  uint64_t before = iw->stats(false).updateVersion;
  iw->close();
  IndexWriter reopened(iw->snapshots);
  EXPECT_GT(reopened.stats(false).updateVersion, before + (1ull << 31));
}

}  // namespace luxir::test
