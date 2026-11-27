// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include <gtest/gtest.h>

#include <atomic>
#include <future>
#include <mutex>
#include <thread>
#include <vector>

#include "luxir/reader/TermsEnum.h"
#include "luxir/plugin/Registries.h"
#include "luxir/store/DelegatingDirectory.h"
#include "luxir/store/StorageRegistry.h"
#include "luxir/util/IoContext.h"
#include "test/CollectionHelper.h"
#include "test/LocalReq.h"
#include "test/TestUtils.h"

using namespace luxir;
using namespace luxir::test;
using namespace std::chrono_literals;

int exampleMembershipProbeBatches();

namespace {

// How the io_deferred layer completes acquires, shared by the files it opens.
struct Deferral {
  std::mutex mutex;
  std::condition_variable released;
  bool hold = false;         // batches and single acquires wait for release()
  std::atomic<uint64_t> batchesElsewhere{0};

  void wait() {
    std::unique_lock lock(mutex);
    released.wait(lock, [&] { return !hold; });
  }
  void set(bool held) {
    {
      std::lock_guard lock(mutex);
      hold = held;
    }
    released.notify_all();
  }
};
Deferral deferral;

// Reports held ranges and completes each batch on a thread of its own, as a
// remote backend would; while deferral.hold is set, acquires wait.
class DeferredInputFile : public InputFile {
  std::shared_ptr<InputFile> inner;
  std::mutex mutex;
  std::vector<std::jthread> threads;

public:
  explicit DeferredInputFile(std::shared_ptr<InputFile> inner) : inner(std::move(inner)) {}
  ~DeferredInputFile() override {
    std::lock_guard lock(mutex);
    threads.clear();
  }
  size_t size() override { return inner->size(); }
  bool holdsFree() const override { return false; }
  HeldRange acquire(ByteRange range) override {
    deferral.wait();
    return inner->acquire(range);
  }
  void acquireBatch(std::span<const ByteRange> ranges, AcquireDone done) noexcept override {
    deferral.batchesElsewhere.fetch_add(1);
    std::lock_guard lock(mutex);
    threads.emplace_back([this, requested = std::vector<ByteRange>(ranges.begin(), ranges.end()), done] {
      deferral.wait();
      std::vector<HeldRange> holds;
      try {
        for (auto range : requested) holds.push_back(inner->acquire(range));
      } catch (...) {
        return done({}, std::current_exception());
      }
      done(std::move(holds), nullptr);
    });
  }
};

class DeferredDirectory : public DelegatingDirectory {
public:
  using DelegatingDirectory::DelegatingDirectory;
  std::shared_ptr<InputFile> openFile(std::string_view name, OpenOptions options = {}) override {
    auto file = delegate->openFile(name, options);
    return file ? std::make_shared<DeferredInputFile>(std::move(file)) : file;
  }
};

class DeferredLayer : public DelegatingDirFactory {
public:
  using DelegatingDirFactory::DelegatingDirFactory;
  std::shared_ptr<Directory> directory(StorageRoot root, std::string_view path, ViewOptions options = {}) override {
    return std::make_shared<DeferredDirectory>(delegate->directory(root, path, std::move(options)));
  }
  std::string describe() const override { return "io_deferred(" + delegate->describe() + ")"; }
};

const bool deferredLayerRegistered = [] {
  StorageRegistry::global().addLayer(
      "io_deferred",
      [](std::unique_ptr<DirectoryFactory> inner, const StorageContext&) -> std::unique_ptr<DirectoryFactory> {
        return std::make_unique<DeferredLayer>(std::move(inner));
      });
  return true;
}();

// Replies through a promise, so a test can wait for a dispatched request.
class DispatchReq : public LocalReq {
public:
  std::promise<void> replied;
  using LocalReq::LocalReq;
  ReplyStatus reply(SearchResponse& response) override {
    ReplyStatus status = LocalReq::reply(response);
    replied.set_value();
    return status;
  }
};

class DeclaredReadsTest : public ::testing::Test {
protected:
  void TearDown() override { deferral.set(false); }

  static void index(CollectionHelper& h, int segments) {
    for (int i = 0; i < segments; ++i) {
      ASSERT_TRUE(h.indexAll({flatdoc("id", std::to_string(i), "body_w", "needle hay " + std::to_string(i)),
                              flatdoc("id", "x" + std::to_string(i), "body_w", "hay")},
                             UpdateMessage::COMMIT).success);
    }
  }
};

}  // namespace

// Over storage whose batches complete on other threads, a dispatched search
// resumes after each round instead of a worker waiting for it, and scores
// as it does on RAM.
TEST_F(DeclaredReadsTest, dispatchedSearchResumesAfterRounds) {
  auto search = [](CollectionHelper& h, bool dispatch) {
    auto* arena = createArena();
    auto req = LocalReqHandle(luxir::arenaCreate<DispatchReq>(*arena, h.getSearchEngine(), *arena));
    req->collection("main").profile().topDocs("q").matchQuery("body_w", "needle").fields({"id", "_score_"});
    auto done = static_cast<DispatchReq*>(req.get())->replied.get_future();
    if (dispatch) {
      h.getSearchEngine().dispatch(*req.get(), 0, SearchEngine::Receiver::OFFLOAD);
      EXPECT_EQ(std::future_status::ready, done.wait_for(60s));
    } else {
      req->execute(0);
    }
    EXPECT_OK(req);
    std::vector<std::string> docs;
    for (const auto& doc : req->getDocs("q")) docs.push_back(docToString(doc));
    return std::pair(docs, req->responses.back()->proto.profile.value_or(api::ExecutionProfile{}));
  };
  LuxirConfig ramConfig;
  LuxirNode ram(ramConfig);
  CollectionHelper ramHelper(ram, "main");
  index(ramHelper, 3);
  auto [expected, ramProfile] = search(ramHelper, false);
  ASSERT_EQ(3u, expected.size());
  EXPECT_EQ(0u, ramProfile.io_acquire_rounds);  // RAM declares nothing

  LuxirConfig config;
  config.store.backend = "ram";
  config.store.layers = {"io_deferred"};
  LuxirNode node(config);
  CollectionHelper h(node, "main");
  index(h, 3);
  auto elsewhere = deferral.batchesElsewhere.load();
  auto [hits, profile] = search(h, true);
  EXPECT_EQ(expected, hits);
  EXPECT_GT(deferral.batchesElsewhere.load(), elsewhere);
  // Routing and a small dictionary, postings, then norms: the stored id
  // fields of the hits are read on demand.
  EXPECT_GE(profile.io_acquire_rounds, 3u);
  EXPECT_GT(profile.io_acquire_batches, 3u);
}

// A round still pending at the request's deadline fails the search with
// deadline_exceeded, and a cancel fails it with cancelled; neither waits for
// the storage.
TEST_F(DeclaredReadsTest, pendingRoundEndsAtDeadlineOrCancel) {
  LuxirConfig config;
  config.store.backend = "ram";
  config.store.layers = {"io_deferred"};
  LuxirNode node(config);
  CollectionHelper h(node, "main");
  index(h, 2);
  // Opens the reader, whose catalogs a snapshot open waits for, without
  // reading the queried field.
  auto warm = localReq(h.getSearchEngine());
  warm->collection("main").topDocs().matchQuery("id", "0").withStats();
  warm->execute();
  ASSERT_OK(warm);
  auto dispatch = [&](auto&& setup) {
    auto* arena = createArena();
    auto req = LocalReqHandle(luxir::arenaCreate<DispatchReq>(*arena, h.getSearchEngine(), *arena));
    req->collection("main").topDocs().matchQuery("body_w", "needle").withStats();
    auto done = static_cast<DispatchReq*>(req.get())->replied.get_future();
    setup(*req.get(), [&] { h.getSearchEngine().dispatch(*req.get(), 0, SearchEngine::Receiver::OFFLOAD); });
    EXPECT_EQ(std::future_status::ready, done.wait_for(30s));
    return req;
  };

  deferral.set(true);
  auto late = dispatch([](SearchRequest& req, auto go) {
    req.io.setDeadline(IoContext::Clock::now() + 300ms);
    go();
  });
  ASSERT_FALSE(late->ok());
  EXPECT_NE(std::string::npos, late->errorMsg().find("deadline")) << late->errorMsg();

  auto cancelled = dispatch([](SearchRequest& req, auto go) {
    req.waitCancellation = std::stop_source();
    go();
    std::this_thread::sleep_for(50ms);
    req.waitCancellation.request_stop();
  });
  ASSERT_FALSE(cancelled->ok());
  EXPECT_NE(std::string::npos, cancelled->errorMsg().find("cancel")) << cancelled->errorMsg();
  deferral.set(false);

  // The storage caught up: the same search succeeds.
  auto req = localReq(h.getSearchEngine());
  req->collection("main").topDocs().matchQuery("body_w", "needle").withStats();
  req->execute();
  EXPECT_OK(req);
  EXPECT_EQ(2, req->getMatchCount());
}

// A slice that misses does not hold the segment's lock while it acquires:
// a part the segment holds is still served meanwhile.
TEST_F(DeclaredReadsTest, missingSliceDoesNotBlockHeldParts) {
  LuxirConfig config;
  config.store.backend = "ram";
  config.store.layers = {"io_deferred"};
  LuxirNode node(config);
  CollectionHelper h(node, "main");
  index(h, 1);
  auto reader = h.getIndexWriter()->snapshots.readers.getReader();
  auto& segment = reader->segments().front();
  const auto* record = segment.postingsReader().fieldInfo("body_w");
  ASSERT_TRUE(record && record->terms());
  auto& core = segment.segmentCore();
  auto held = TermsEnum::navigationPart(*record);
  auto missing = record->normsValues(segment.maxDoc());
  ASSERT_FALSE(core.held(missing));
  auto bytes = core.slice(held, ReadClass::METADATA);
  deferral.set(true);
  auto blocked = std::async(std::launch::async, [&] { return core.slice(missing); });
  ASSERT_EQ(std::future_status::timeout, blocked.wait_for(50ms));
  auto again = std::async(std::launch::async, [&] { return core.slice(held, ReadClass::METADATA); });
  ASSERT_EQ(std::future_status::ready, again.wait_for(10s));
  EXPECT_EQ(bytes.data(), again.get().data());
  deferral.set(false);
  EXPECT_EQ(missing.bytes, blocked.get().size());
  EXPECT_TRUE(core.held(missing));
}

// Overlay probes are the first round: a segment proven to lack the term has
// no dictionary or postings declared, so the search issues fewer batches.
TEST_F(DeclaredReadsTest, membershipProbesDropSegmentsBeforeTheDictionaryRounds) {
  initPlugins(addConfiguredPlugins);
  LuxirConfig config;
  config.store.backend = "ram";
  config.store.layers = {"io_deferred"};
  LuxirNode node(config);
  CollectionHelper h(node, "main");
  std::pmr::monotonic_buffer_resource arena;
  api::SchemaDef def;
  ASSERT_TRUE(api::read_json(def, R"({"segment_overlays":[
      {"name":"m","kind":"example_absent_prefix","fields":["body_w"],"params":{"prefix":"mm"}}]})", arena));
  h.collection().updateSchema(def, api::SchemaRequest_::Mode::SET);
  index(h, 3);
  auto batches = [&](std::string_view term) {
    auto req = localReq(h.getSearchEngine());
    req->collection("main").profile().topDocs("q").matchQuery("body_w", term).fields({"id"});
    req->execute(0);
    EXPECT_OK(req);
    return req->responses.back()->proto.profile.value_or(api::ExecutionProfile{}).io_acquire_batches;
  };
  auto probes = exampleMembershipProbeBatches();
  auto proven = batches("mmgone");
  EXPECT_GT(exampleMembershipProbeBatches(), probes);
  EXPECT_LT(proven, batches("nngone"));
}
