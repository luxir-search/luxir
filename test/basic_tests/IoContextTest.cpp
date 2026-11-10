// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <map>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "luxir/store/DelegatingDirectory.h"
#include "luxir/store/StorageRegistry.h"
#include "luxir/util/IoContext.h"
#include "luxir/util/Uuid.h"
#include "luxir/util/luxir_util.h"
#include "luxir/util/thread.h"
#include "test/CollectionHelper.h"
#include "test/LocalReq.h"
#include "test/LuxirTest.h"
#include "test/TestUtils.h"

using namespace luxir;

namespace {

// Fetches across every chunked file, with or without a context.
struct FetchTotals {
  std::atomic<uint64_t> requests{0};
  std::atomic<uint64_t> bytes{0};
};
FetchTotals fetchTotals;

// A file read the way a lazy backend reads one: fixed-size chunks are fetched
// on first touch and counted into the request's context, as a backend's lazy
// files do. The bytes come from a resident inner file; the counts and the
// context each fetch saw are what the tests check.
class ChunkedFile : public InputFile {
public:
  struct State {
    std::mutex mutex;
    std::vector<bool> resident;
    std::vector<std::optional<IoContext>> seen;  // the context of each fetch
  };

private:
  std::shared_ptr<InputFile> inner;
  std::shared_ptr<State> state;
  uint64_t chunkBytes;

  void fetch(uint64_t offset, uint64_t length) {
    if (length == 0) return;
    uint64_t total = inner->size();
    size_t first = (size_t)(offset / chunkBytes), last = (size_t)((offset + length - 1) / chunkBytes);
    std::lock_guard lock(state->mutex);
    if (state->resident.empty()) state->resident.resize((size_t)((total + chunkBytes - 1) / chunkBytes));
    for (size_t c = first; c <= last;) {
      if (state->resident[c]) {
        c++;
        continue;
      }
      size_t runEnd = c;
      while (runEnd < last && !state->resident[runEnd + 1]) runEnd++;
      uint64_t bytes = std::min<uint64_t>((runEnd + 1) * chunkBytes, total) - c * chunkBytes;
      state->seen.push_back(captureIoContext());
      fetchTotals.requests.fetch_add(1, std::memory_order_relaxed);
      fetchTotals.bytes.fetch_add(bytes, std::memory_order_relaxed);
      if (const auto* io = IoContext::current()) io->countFetch(bytes);
      for (size_t i = c; i <= runEnd; i++) state->resident[i] = true;
      c = runEnd + 1;
    }
  }
  void touch(uint64_t offset, uint64_t length) {
    if (length == 0) return;
    if (const auto* io = IoContext::current()) io->countTouch(length);
    fetch(offset, length);
  }
  void whole() {
    if (const auto* io = IoContext::current()) io->countWholeFileRead();
    touch(0, inner->size());
  }

public:
  ChunkedFile(std::shared_ptr<InputFile> inner, std::shared_ptr<State> state, uint64_t chunkBytes)
      : inner(std::move(inner)), state(std::move(state)), chunkBytes(chunkBytes) {}

  size_t size() override { return inner->size(); }
  std::string_view read() override {
    whole();
    return inner->read();
  }
  InputStream getInputStream() override {
    whole();
    return inner->getInputStream();
  }
  InputStream rangeStream(uint64_t offset, uint64_t length) override {
    touch(offset, length);
    return inner->rangeStream(offset, length);
  }
  void prefetchRanges(std::span<const ByteRange> ranges) override {
    for (const auto& range : ranges) fetch(range.offset, range.length);
  }
  bool lazy() const override { return true; }
};

// Residency by view and file name, shared by every view of one layer so that
// reopening a file finds its chunks resident, as a backend's cache would.
struct ChunkedStates {
  std::mutex mutex;
  std::map<std::string, std::shared_ptr<ChunkedFile::State>, std::less<>> byName;

  std::shared_ptr<ChunkedFile::State> of(const std::string& key) {
    std::lock_guard lock(mutex);
    auto& state = byName[key];
    if (!state) state = std::make_shared<ChunkedFile::State>();
    return state;
  }
  void forget(const std::string& key) {
    std::lock_guard lock(mutex);
    byName.erase(key);
  }
};

constexpr uint64_t kChunk = 1024;

class ChunkedDirectory : public DelegatingDirectory {
  std::shared_ptr<ChunkedStates> states;
  std::string label;

  std::string key(std::string_view name) const { return label + "/" + std::string(name); }

public:
  ChunkedDirectory(std::shared_ptr<Directory> inner, std::shared_ptr<ChunkedStates> states, std::string label)
      : DelegatingDirectory(std::move(inner)), states(std::move(states)), label(std::move(label)) {}

  std::shared_ptr<InputFile> openFile(std::string_view name, bool expectSynced = false) override {
    auto file = delegate->openFile(name, expectSynced);
    if (!file) return file;
    return std::make_shared<ChunkedFile>(std::move(file), states->of(key(name)), kChunk);
  }
  std::unique_ptr<File> createFile(std::string_view name) override {
    states->forget(key(name));
    return delegate->createFile(name);
  }
  std::unique_ptr<File> createFile(std::string_view name, FileCreateOptions options) override {
    states->forget(key(name));
    return delegate->createFile(name, std::move(options));
  }
  bool deleteFile(std::string_view name) override {
    states->forget(key(name));
    return delegate->deleteFile(name);
  }
  std::optional<PublicationVersion> publishFile(std::string_view name, std::span<const std::byte> bytes,
                                                PublishCondition condition) override {
    states->forget(key(name));
    return delegate->publishFile(name, bytes, condition);
  }
};

class ChunkedLayer : public DelegatingDirFactory {
  std::shared_ptr<ChunkedStates> states = std::make_shared<ChunkedStates>();

public:
  using DelegatingDirFactory::DelegatingDirFactory;
  std::shared_ptr<Directory> directory(StorageRoot root, std::string_view path, ViewOptions options = {}) override {
    std::string label = (root == StorageRoot::NODE_PRIVATE ? "node/" : "") + std::string(path);
    return std::make_shared<ChunkedDirectory>(delegate->directory(root, path, std::move(options)), states, label);
  }
  std::string describe() const override { return "io_chunked(" + delegate->describe() + ")"; }
};

// Registered before the registries freeze at test startup.
const bool chunkedLayerRegistered = [] {
  StorageRegistry::global().addLayer(
      "io_chunked", [](std::unique_ptr<DirectoryFactory> inner, const StorageContext&) -> std::unique_ptr<DirectoryFactory> {
        return std::make_unique<ChunkedLayer>(std::move(inner));
      });
  return true;
}();

IoContext context(uint64_t queryId, IoCounters* counters = nullptr) {
  IoContext io;
  io.queryId = queryId;
  io.counters = counters;
  return io;
}

constexpr size_t kChunks = 8;

struct LazyFixture {
  RAMDir ram;
  std::shared_ptr<ChunkedFile::State> state = std::make_shared<ChunkedFile::State>();
  std::unique_ptr<ChunkedFile> file;
  LazyFixture() {
    {
      auto out = ram.createFile("f");
      OutputStream stream(out.get());
      stream.writeBytes(std::string(kChunk * kChunks, 'x'));
      stream.close();
      ram.finishFile(*out);
    }
    file = std::make_unique<ChunkedFile>(ram.openFile("f"), state, kChunk);
  }
  // Touches 100 bytes of each chunk, one task per chunk.
  void readEachChunkInTasks() {
    TaskGroupRunner runner;
    task_group_fanout(runner.group(), kChunks, [&](size_t i) {
      return [this, i] { file->rangeStream(i * kChunk, 100); };
    });
    runner.join();
  }
};

}  // namespace

TEST(IoContextTest, tasksSeeTheSpawnersContextInStorageReads) {
  LazyFixture f;
  IoCounters counters;
  auto deadline = std::chrono::steady_clock::now() + std::chrono::hours(1);
  IoContext io = context(7, &counters);
  io.requestId = "r7";
  io.deadline = deadline;
  io.fillCache = false;
  {
    IoScope scope(&io);
    f.readEachChunkInTasks();
  }
  EXPECT_EQ(nullptr, IoContext::current());
  ASSERT_EQ(kChunks, f.state->seen.size());
  for (const auto& seen : f.state->seen) {
    ASSERT_TRUE(seen.has_value());
    EXPECT_EQ(7u, seen->queryId);
    EXPECT_EQ("r7", seen->requestId);
    EXPECT_EQ(deadline, seen->deadline);
    EXPECT_FALSE(seen->fillCache);
    EXPECT_EQ(&counters, seen->counters);
  }
  auto totals = counters.snapshot();
  EXPECT_EQ(kChunks, totals.requests);
  EXPECT_EQ(kChunks, totals.rounds);
  EXPECT_EQ(kChunks * kChunk, totals.fetchedBytes);
  EXPECT_EQ(kChunks * 100, totals.touchedBytes);
  EXPECT_EQ(0u, totals.wholeFileReads);

  // Resident now: touching again costs no requests. A whole-file read counts.
  {
    IoScope scope(&io);
    f.file->read();
  }
  totals = counters.snapshot();
  EXPECT_EQ(kChunks, totals.requests);
  EXPECT_EQ(kChunks * 100 + kChunks * kChunk, totals.touchedBytes);
  EXPECT_EQ(1u, totals.wholeFileReads);
}

TEST(IoContextTest, readsOutsideAnyRequestSeeNoContext) {
  LazyFixture f;
  IoCounters counters;
  IoContext io = context(8, &counters);
  {
    IoScope scope(&io);
    f.file->rangeStream(0, 1);
  }
  // The same worker threads, now without a context: nothing leaked to them.
  f.state->seen.clear();
  f.readEachChunkInTasks();
  ASSERT_EQ(kChunks - 1, f.state->seen.size());  // chunk 0 is resident
  for (const auto& seen : f.state->seen) EXPECT_FALSE(seen.has_value());
  EXPECT_EQ(1u, counters.snapshot().requests);
}

TEST(IoContextTest, boundWorkRestoresTheContextOfTheThreadThatRunsIt) {
  IoContext request = context(1);
  IoContext other = context(2);
  auto queryId = [] { return IoContext::current() ? IoContext::current()->queryId : 0; };
  std::vector<uint64_t> inside;
  auto task = [&] {
    IoScope scope(&request);
    return bindIoContext([&] { inside.push_back(queryId()); });
  }();
  auto failing = [&] {
    IoScope scope(&request);
    return bindIoContext([] { throw std::runtime_error("boom"); });
  }();
  uint64_t before = 9, after = 9, afterNested = 9, afterThrow = 9;
  std::thread([&] {
    before = queryId();
    task();
    after = queryId();
    IoScope scope(&other);
    task();
    afterNested = queryId();
    try { failing(); } catch (const std::exception&) {}
    afterThrow = queryId();
  }).join();
  EXPECT_EQ(0u, before);
  EXPECT_EQ((std::vector<uint64_t>{1, 1}), inside);
  EXPECT_EQ(0u, after);
  EXPECT_EQ(2u, afterNested);
  EXPECT_EQ(2u, afterThrow);

  // Shared work keeps the request's counters but none of its policy.
  IoCounters counters;
  IoContext io = context(3, &counters);
  io.deadline = std::chrono::steady_clock::now();
  io.fillCache = false;
  auto shared = io.shared();
  EXPECT_EQ(3u, shared.queryId);
  EXPECT_EQ(&counters, shared.counters);
  EXPECT_FALSE(shared.hasDeadline());
  EXPECT_TRUE(shared.fillCache);
}

// A search over a collection on lazily read storage reports the requests and
// bytes its reads fetched, exactly as the storage counted them.
TEST(IoContextTest, searchProfileCountsStorageIo) {
  using namespace luxir::test;
  auto path = std::filesystem::temp_directory_path() / ("luxir-io-" + newUuid());
  std::filesystem::remove_all(path);
  auto cleanup = scope_guard([&] { std::filesystem::remove_all(path); });
  LuxirConfig config;
  config.store.backend = "fs";
  config.store.data_dir = path.string();
  config.store.layers = {"io_chunked"};
  std::string filler(200, 'a');
  {
    LuxirNode node(config);
    CollectionHelper h(node, "main");
    for (int seg = 0; seg < 3; seg++) {
      std::vector<Doc> docs;
      for (int i = 0; i < 100; i++) {
        std::string id = std::to_string(seg) + "_" + std::to_string(i);
        docs.push_back(flatdoc("id", id, "text_un", (i == 50 ? "needle " : "hay ") + filler + id));
      }
      ASSERT_TRUE(h.indexAll(docs, UpdateMessage::COMMIT).success);
    }
  }
  {  // reopened: a new layer, nothing resident; the segments are read lazily
    LuxirNode node(config);
    CollectionHelper h(node, "main");
    auto search = [&] {
      auto req = localReq(h.getSearchEngine());
      req->collection("main").profile().topDocs("q").matchQuery("text_un", "needle").fields({"id", "text_un"});
      req->execute(true);
      EXPECT_OK(req);
      return req->responses.back()->proto.profile.value_or(api::ExecutionProfile{});
    };
    auto requests = fetchTotals.requests.load();
    auto bytes = fetchTotals.bytes.load();
    auto metricRequests = node.metrics->counts().counters[(size_t)MetricCounter::SEARCH_IO_REQUESTS];
    auto cold = search();
    EXPECT_GT(cold.io_requests, 0u);
    EXPECT_EQ(fetchTotals.requests.load() - requests, cold.io_requests);
    EXPECT_EQ(fetchTotals.bytes.load() - bytes, cold.io_fetched_bytes);
    EXPECT_EQ(cold.io_requests, cold.io_rounds);
    EXPECT_GE(cold.io_touched_bytes, 3 * filler.size());  // three returned documents at least
    EXPECT_EQ(metricRequests + cold.io_requests,
              node.metrics->counts().counters[(size_t)MetricCounter::SEARCH_IO_REQUESTS]);

    auto warm = search();  // everything it needs is resident now
    EXPECT_EQ(0u, warm.io_requests);
    EXPECT_EQ(0u, warm.io_fetched_bytes);
    EXPECT_LE(warm.io_touched_bytes, cold.io_touched_bytes);
  }
}
