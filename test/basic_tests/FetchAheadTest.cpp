// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

// A search fetches ahead what its reads will need, on storage that fetches
// lazily: only the ranges a segment open reads (the hotcache's, or each file's
// header and file 0's tail), capped per request, bounded by the request's
// deadline and stopped by its cancellation.

#include <gtest/gtest.h>

#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <future>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "luxir/reader/Hotcache.h"
#include "luxir/reader/SegmentCore.h"
#include "luxir/store/DelegatingDirectory.h"
#include "luxir/store/StorageRegistry.h"
#include "luxir/util/IoContext.h"
#include "luxir/util/Uuid.h"
#include "luxir/util/luxir_util.h"
#include "test/CollectionHelper.h"
#include "test/LocalReq.h"
#include "test/LuxirTest.h"
#include "test/TestUtils.h"

using namespace luxir;
using namespace luxir::test;

namespace {

// How fetches ahead complete: at once, at the deadline with FetchTimeoutError
// (never before), or once the gate opens.
enum class Mode { INLINE, STALL, GATED };

// What every held file of the probe layers saw, across nodes.
struct Probe {
  struct Fetch {
    std::string name;
    uint64_t size = 0;
    std::vector<ByteRange> ranges;
  };
  std::mutex mutex;
  std::condition_variable changed;
  std::vector<Fetch> fetches;
  std::vector<std::thread> threads;
  Mode mode = Mode::INLINE;
  bool gateOpen = false;

  void reset(Mode next) {
    finish();
    std::lock_guard lock(mutex);
    fetches.clear();
    mode = next;
    gateOpen = false;
  }
  // Opens the gate and joins the fetches still waiting on it.
  void finish() {
    std::vector<std::thread> waiting;
    {
      std::lock_guard lock(mutex);
      gateOpen = true;
      waiting.swap(threads);
    }
    changed.notify_all();
    for (auto& thread : waiting) thread.join();
  }
  uint64_t bytes() {
    std::lock_guard lock(mutex);
    uint64_t total = 0;
    for (const auto& fetch : fetches) {
      for (const auto& range : fetch.ranges) total += range.length;
    }
    return total;
  }
};
Probe probe;

class ProbeFile : public InputFile {
  std::shared_ptr<InputFile> inner;
  std::string name;

public:
  ProbeFile(std::shared_ptr<InputFile> inner, std::string name) : inner(std::move(inner)), name(std::move(name)) {}

  size_t size() override { return inner->size(); }
  std::string_view read() override { return inner->read(); }
  InputStream getInputStream() override { return inner->getInputStream(); }
  InputStream rangeStream(uint64_t offset, uint64_t length) override { return inner->rangeStream(offset, length); }
  bool lazy() const override { return true; }

  void fetchRanges(std::span<const ByteRange> ranges, ReadDeadline deadline, FetchDone done) override {
    std::unique_lock lock(probe.mutex);
    probe.fetches.push_back({name, (uint64_t)inner->size(), {ranges.begin(), ranges.end()}});
    probe.changed.notify_all();
    if (probe.mode == Mode::INLINE) {
      lock.unlock();
      done(nullptr);
      return;
    }
    bool stall = probe.mode == Mode::STALL;
    probe.threads.emplace_back([stall, deadline, done = std::move(done)] {
      std::unique_lock lock(probe.mutex);
      auto open = [] { return probe.gateOpen; };
      bool opened = stall && deadline != kNoReadDeadline ? probe.changed.wait_until(lock, deadline, open)
                                                         : (probe.changed.wait(lock, open), true);
      lock.unlock();
      done(stall && !opened ? std::make_exception_ptr(FetchTimeoutError("fetch passed its deadline")) : nullptr);
    });
  }
};

class ProbeHandle : public ChecksumReader {
  std::shared_ptr<ChecksumReader> inner;
  std::string name;

public:
  ProbeHandle(std::shared_ptr<ChecksumReader> inner, std::string name) : inner(std::move(inner)), name(std::move(name)) {}
  uint64_t size() const override { return inner->size(); }
  std::shared_ptr<InputFile> map() override {
    auto file = inner->map();
    return file ? std::make_shared<ProbeFile>(std::move(file), name) : file;
  }
  FileCheck checksum(const std::function<void(size_t)>& progress = {}) override { return inner->checksum(progress); }
};

// Held files fetch through the probe. The remote variant says its bytes come
// over a network, so finished segments get a hotcache.
class ProbeDirectory : public DelegatingDirectory {
  bool remote;

public:
  ProbeDirectory(std::shared_ptr<Directory> inner, bool remote) : DelegatingDirectory(std::move(inner)), remote(remote) {}
  Capabilities capabilities() const override { return delegate->capabilities() | (remote ? REMOTE : 0); }
  std::shared_ptr<ChecksumReader> openChecksum(std::string_view name, bool direct = false) override {
    auto handle = delegate->openChecksum(name, direct);
    return handle ? std::make_shared<ProbeHandle>(std::move(handle), std::string(name)) : handle;
  }
};

class ProbeLayer : public DelegatingDirFactory {
  bool remote;

public:
  ProbeLayer(std::unique_ptr<DirectoryFactory> inner, bool remote) : DelegatingDirFactory(std::move(inner)), remote(remote) {}
  std::shared_ptr<Directory> directory(StorageRoot root, std::string_view path, ViewOptions options = {}) override {
    return std::make_shared<ProbeDirectory>(delegate->directory(root, path, std::move(options)), remote);
  }
  std::string describe() const override { return "fetch_probe(" + delegate->describe() + ")"; }
};

// Registered before the registries freeze at test startup.
const bool probeLayersRegistered = [] {
  for (bool remote : {false, true}) {
    StorageRegistry::global().addLayer(
        remote ? "fetch_probe_remote" : "fetch_probe",
        [remote](std::unique_ptr<DirectoryFactory> inner, const StorageContext&) -> std::unique_ptr<DirectoryFactory> {
          return std::make_unique<ProbeLayer>(std::move(inner), remote);
        });
  }
  return true;
}();

// Replies on whichever thread finishes the request.
class AsyncReq : public LocalReq {
public:
  std::promise<void> replied;
  using LocalReq::LocalReq;
  ReplyStatus reply(SearchResponse& response) override {
    ReplyStatus status = LocalReq::reply(response);
    replied.set_value();
    return status;
  }
};

class FetchAheadTest : public LuxirTest {
protected:
  std::filesystem::path path = std::filesystem::temp_directory_path() / ("luxir-fetch-" + newUuid());
  LuxirConfig config;
  std::unique_ptr<LuxirNode> node;

  void TearDown() override {
    probe.reset(Mode::INLINE);
    node.reset();
    std::filesystem::remove_all(path);
    LuxirTest::TearDown();
  }

  // Three segments of large stored text, then a node reopened over them, so
  // nothing is open: the next search opens every segment it needs.
  void indexAndReopen(bool remote) {
    config.store.backend = "fs";
    config.store.data_dir = path.string();
    config.store.layers = {remote ? "fetch_probe_remote" : "fetch_probe"};
    std::string filler(40, 'a');
    {
      LuxirNode writer(config);
      CollectionHelper h(writer, "main");
      for (int seg = 0; seg < 3; seg++) {
        std::vector<Doc> docs;
        for (int i = 0; i < 3000; i++) {
          std::string id = std::to_string(seg) + "_" + std::to_string(i);
          docs.push_back(flatdoc("id", id, "n_i", (int64_t)i,
                                 "text_un", (i == 7 ? "needle w" : "hay w") + id + " x" + id + " " + filler));
        }
        ASSERT_TRUE(h.indexAll(docs, UpdateMessage::COMMIT).success);
      }
    }
    probe.reset(Mode::INLINE);
    node = std::make_unique<LuxirNode>(config);
  }

  // Runs a needle search through dispatch, which fetches ahead, and waits for
  // its reply.
  LocalReqHandle search(const std::function<void(AsyncReq&)>& setup = {}) {
    CollectionHelper h(*node, "main");
    auto* arena = createArena();
    auto req = LocalReqHandle(luxir::arenaCreate<AsyncReq>(*arena, h.getSearchEngine(), *arena));
    auto& async = static_cast<AsyncReq&>(*req.get());
    auto replied = async.replied.get_future();
    req->collection("main").topDocs("q").matchQuery("text_un", "needle").fields({"id"});
    if (setup) setup(async);
    h.getSearchEngine().dispatch(*req.get(), 0, SearchEngine::Receiver::OFFLOAD);
    EXPECT_EQ(std::future_status::ready, replied.wait_for(std::chrono::seconds(60)));
    return req;
  }
};

}  // namespace

TEST_F(FetchAheadTest, aSegmentWithAHotcacheDeclaresItsRangesOnly) {
  indexAndReopen(true);
  auto req = search();
  ASSERT_OK(req);
  EXPECT_EQ(3u, req->getDocs("q").size());
  std::lock_guard lock(probe.mutex);
  size_t hotcaches = 0, data = 0;
  for (const auto& fetch : probe.fetches) {
    if (fetch.name.ends_with("__hot")) {
      hotcaches++;  // small: fetched whole, in a round of its own
      continue;
    }
    auto parsed = Hotcache::parseDataFile(fetch.name);
    ASSERT_TRUE(parsed) << fetch.name;
    data++;
    for (const auto& range : fetch.ranges) {
      EXPECT_EQ(0u, range.offset % Hotcache::kAlign) << fetch.name;
      // Never the whole of a file larger than a hotcache page.
      if (fetch.size > 2 * Hotcache::kAlign) {
        EXPECT_LT(range.length, fetch.size) << fetch.name;
      }
    }
  }
  EXPECT_EQ(3u, hotcaches);
  EXPECT_GE(data, 3u);
}

TEST_F(FetchAheadTest, aSegmentWithoutAHotcacheDeclaresHeadersAndTheCatalogTail) {
  indexAndReopen(false);
  auto req = search();
  ASSERT_OK(req);
  EXPECT_EQ(3u, req->getDocs("q").size());
  std::lock_guard lock(probe.mutex);
  ASSERT_FALSE(probe.fetches.empty());
  bool partial = false;
  for (const auto& fetch : probe.fetches) {
    auto parsed = Hotcache::parseDataFile(fetch.name);
    ASSERT_TRUE(parsed) << fetch.name;  // nothing but the open's data files
    auto expected = SegmentCore::openReads(parsed->fileNum, fetch.size);
    ASSERT_EQ(expected.size(), fetch.ranges.size()) << fetch.name;
    for (size_t i = 0; i < expected.size(); i++) {
      EXPECT_EQ(expected[i].offset, fetch.ranges[i].offset) << fetch.name;
      EXPECT_EQ(expected[i].length, fetch.ranges[i].length) << fetch.name;
    }
    uint64_t declared = 0;
    for (const auto& range : fetch.ranges) declared += range.length;
    partial = partial || declared < fetch.size;
  }
  EXPECT_TRUE(partial);  // the stored text is never declared
}

TEST_F(FetchAheadTest, aRequestFetchesAheadNoMoreThanItsCap) {
  config.search.fetch_ahead_max_bytes = 10000;
  indexAndReopen(false);
  auto req = search();
  ASSERT_OK(req);  // what the cap left out is read on demand
  EXPECT_EQ(3u, req->getDocs("q").size());
  EXPECT_GT(probe.bytes(), 0u);
  EXPECT_LE(probe.bytes(), 10000u);
}

TEST_F(FetchAheadTest, aFetchPastTheDeadlineFailsTheSearchAsRetryable) {
  indexAndReopen(false);
  probe.reset(Mode::STALL);
  auto start = std::chrono::steady_clock::now();
  auto req = search([](AsyncReq& r) { r.io.setDeadline(std::chrono::steady_clock::now() + std::chrono::milliseconds(300)); });
  EXPECT_FALSE(req->ok());
  EXPECT_EQ("fetch_timeout", req->errorCode()) << req->errorMsg();
  EXPECT_EQ(ErrorKind::UNAVAILABLE, req->errorKind());
  // The wait ended a margin before the deadline (15 ms of 300).
  EXPECT_GE(std::chrono::steady_clock::now() - start, std::chrono::milliseconds(280));
  probe.finish();
}

TEST_F(FetchAheadTest, aCancelledRequestStopsAfterItsFetch) {
  indexAndReopen(false);
  probe.reset(Mode::GATED);
  std::stop_source cancel;
  std::jthread canceller([&] {
    std::unique_lock lock(probe.mutex);
    probe.changed.wait(lock, [] { return !probe.fetches.empty(); });
    lock.unlock();
    cancel.request_stop();
    probe.finish();
  });
  auto req = search([&](AsyncReq& r) { r.waitCancellation = cancel; });
  EXPECT_FALSE(req->ok());
  EXPECT_EQ("cancelled", req->errorCode()) << req->errorMsg();
}
