// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <map>
#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "luxir/query/KnnQuery.h"
#include "luxir/server/LuxirNode.h"
#include "luxir/store/DelegatingDirectory.h"
#include "luxir/store/FileIOException.h"
#include "luxir/store/StorageRegistry.h"
#include "luxir/index/PostingsWriter.h"
#include "luxir/util/Uuid.h"
#include "luxir/util/luxir_util.h"
#include "test/CollectionHelper.h"
#include "test/KnnTestUtils.h"
#include "test/LocalReq.h"
#include "test/QueryBuild.h"
#include "test/SchemaBuilder.h"
#include "test/TestUtils.h"

using namespace luxir;
using namespace luxir::test;

namespace {

// A lazy view of a local file (its holds are not free) that logs the ranges
// acquired from it. Batches complete from another thread, and can be made to fail.
class LoggedLazyFile : public InputFile {
  std::shared_ptr<InputFile> inner;
  std::mutex mutex;
  std::vector<ByteRange> acquired;
  std::vector<size_t> batches;  // ranges per acquireBatch() call
  std::vector<std::jthread> workers;

public:
  std::atomic<bool> failing = false;

  explicit LoggedLazyFile(std::shared_ptr<InputFile> inner) : inner(std::move(inner)) {}

  size_t size() override { return inner->size(); }
  HeldRange acquire(ByteRange range) override {
    {
      std::lock_guard lock(mutex);
      acquired.push_back(range);
    }
    if (failing) throw FileIOException("injected acquire failure");
    return inner->acquire(range);
  }
  void acquireBatch(std::span<const ByteRange> ranges, AcquireDone done) noexcept override {
    std::vector<ByteRange> copy(ranges.begin(), ranges.end());
    std::lock_guard lock(mutex);
    batches.push_back(copy.size());
    acquired.insert(acquired.end(), copy.begin(), copy.end());
    workers.emplace_back([this, copy = std::move(copy), done = std::move(done)]() mutable {
      std::vector<HeldRange> holds;
      std::exception_ptr failure;
      try {
        if (failing) throw FileIOException("injected acquire failure");
        for (const auto& range : copy) holds.push_back(inner->acquire(range));
      } catch (...) {
        holds.clear();
        failure = std::current_exception();
      }
      done(std::move(holds), failure);
    });
  }
  // The ranges acquired inside [offset, offset + length), sorted.
  std::vector<ByteRange> within(uint64_t offset, uint64_t length) {
    std::lock_guard lock(mutex);
    std::vector<ByteRange> result;
    for (const auto& range : acquired) {
      if (offset <= range.offset && range.offset + range.length <= offset + length) result.push_back(range);
    }
    std::ranges::sort(result, {}, &ByteRange::offset);
    return result;
  }
  std::vector<size_t> batchSizes() {
    std::lock_guard lock(mutex);
    return batches;
  }
  void clear() {
    std::lock_guard lock(mutex);
    acquired.clear();
    batches.clear();
  }
};

class LoggedLazyDirectory : public DelegatingDirectory {
public:
  using DelegatingDirectory::DelegatingDirectory;
  std::shared_ptr<InputFile> openFile(std::string_view name, OpenOptions options = {}) override {
    auto file = delegate->openFile(name, options);
    if (!file) return file;
    return std::make_shared<LoggedLazyFile>(std::move(file));
  }
};

class LoggedLazyLayer : public DelegatingDirFactory {
public:
  using DelegatingDirFactory::DelegatingDirFactory;
  std::shared_ptr<Directory> directory(StorageRoot root, std::string_view path, ViewOptions options = {}) override {
    return std::make_shared<LoggedLazyDirectory>(delegate->directory(root, path, std::move(options)));
  }
  std::string describe() const override { return "knn_logged_lazy(" + delegate->describe() + ")"; }
};

// Registered before the registries freeze at test startup.
const bool loggedLazyLayerRegistered = [] {
  StorageRegistry::global().addLayer(
      "knn_logged_lazy", [](std::unique_ptr<DirectoryFactory> inner, const StorageContext&) -> std::unique_ptr<DirectoryFactory> {
        return std::make_unique<LoggedLazyLayer>(std::move(inner));
      });
  return true;
}();

// Large enough that each vector column spills to a physical file of its own,
// as a production one does.
constexpr int32_t kDims = 256;
constexpr int kDocs = 7000;
constexpr int64_t kVectorBytes = kDims * (int64_t)sizeof(float);
constexpr int64_t kWindowBytes = 10 * kVectorBytes;
constexpr int64_t kAheadBytes = 4 * kWindowBytes;

std::vector<float> vectorOf(int seed) {
  std::vector<float> v(kDims);
  for (int d = 0; d < kDims; d++) v[(size_t)d] = (float)(((uint64_t)seed * 2654435761u + (uint64_t)d * 40503u) % 1001) / 100.0f;
  return v;
}

// Every doc has dense_v; two docs in three have sparse_v; four in five have
// one to three multi_vs vectors. p_s selects most docs, c_s about one in seven,
// t_s one in three (more domain docs than a look-ahead batch holds).
std::vector<Doc> corpus() {
  std::vector<Doc> docs;
  for (int i = 0; i < kDocs; i++) {
    int n = i;
    Doc doc = flatdoc("id", "d" + std::to_string(n), "dense_v", vectorOf(n),
                      "c_s", "c" + std::to_string(i % 7), "p_s", i % 10 == 0 ? "no" : "yes",
                      "t_s", i % 3 == 0 ? "y" : "n");
    if (i % 3 != 0) appendFlat(doc, "sparse_v", vectorOf(n + 7));
    if (i % 5 != 0) {
      std::vector<std::vector<float>> multi;
      for (int k = 0; k <= i % 3; k++) multi.push_back(vectorOf(n * 3 + k));
      appendFlat(doc, "multi_vs", multi);
    }
    docs.push_back(std::move(doc));
  }
  return docs;
}

struct Hits {
  std::vector<std::string> ids;
  std::vector<float> scores;
  bool operator==(const Hits&) const = default;
};

Hits exactKnn(LuxirNode& node, std::string_view field, std::string_view filterField = {},
              std::string_view filterValue = {}, bool ok = true) {
  auto req = localReq(node.getSearchEngine());
  auto& cur = req->collection("main").topDocs("q");
  cur.getScores().countLimit().fields({"id"});
  cur.rawQuery() = qb::knn(cur.mr(), field, vectorOf(5), 12, 0, true, 0, 0.0f);
  if (!filterField.empty()) {
    auto& td = std::get<api::TopDocs>(cur.rawOp().kind);
    api::Filter* filter = build::allocArray(td.filter, 1, cur.mr());
    auto* q = (api::Query*)cur.mr().allocate(sizeof(api::Query), alignof(api::Query));
    new (q) api::Query(qb::match(cur.mr(), filterField, filterValue));
    filter[0].query = q;
  }
  req->execute(true);
  if (ok) {
    EXPECT_OK(req);
  } else {
    EXPECT_FALSE(req->ok());
  }
  Hits hits;
  const auto* list = req->docList("q");
  if (!list) return hits;
  if (const auto* ids = list->columns.find("id")) {
    for (const auto& id : std::get<api::ColStr>(ids->kind).v) hits.ids.emplace_back(id);
  }
  if (const auto* scores = list->columns.find("_score_")) {
    for (float s : std::get<api::ColFloat>(scores->kind).v) hits.scores.push_back(s);
  }
  return hits;
}

void indexCorpus(LuxirNode& node, bool defineSchema = true) {
  CollectionHelper h(node, "main");
  if (!defineSchema) {
    ASSERT_TRUE(h.indexAll(corpus(), UpdateMessage::COMMIT).success);
    return;
  }
  SchemaBuilder schema;
  for (const char* suffix : {"_v", "_vs"}) {
    auto& f = schema.templ(suffix);
    f.type = api::FieldDef::FieldClass::VECTOR;
    f.column = true;
    f.metric = api::VectorMetric::L2;
    if (std::string_view(suffix) == "_vs") f.multi = api::FieldDef::MultiMode::DEFAULT;
  }
  schema.set(h.collection());
  ASSERT_TRUE(h.indexAll(corpus(), UpdateMessage::COMMIT).success);
}

// A vector field's value column in one segment, and the file holding it.
struct Column {
  uint32_t filenum;
  ByteRange values;
  InputFile* file;
};
std::vector<Column> columns(LuxirNode& node, std::string_view field) {
  CollectionHelper h(node, "main");
  auto reader = h.getIndexWriter()->snapshots.readers.getReader();
  std::vector<Column> result;
  for (auto& segment : reader->segments()) {
    auto& postings = segment.postingsReader();
    const auto* info = postings.fieldInfo(field);
    if (!info || !info->bytesColumn()) continue;
    const auto& slice = info->bytesColumn()->values;
    result.push_back({slice.location.filenum(), {slice.location.offset(), slice.bytes},
                      postings.core().getFile(slice.location.filenum())});
  }
  return result;
}

// Every byte was acquired once: sorted ranges do not overlap.
void expectEachByteOnce(const std::vector<ByteRange>& ranges) {
  for (size_t i = 1; i < ranges.size(); i++) {
    EXPECT_LE(ranges[i - 1].offset + ranges[i - 1].length, ranges[i].offset) << "range " << i << " overlaps";
  }
}

uint64_t bytesOf(const std::vector<ByteRange>& ranges) {
  uint64_t bytes = 0;
  for (const auto& range : ranges) bytes += range.length;
  return bytes;
}

// No structure of another field shares the file of a large vector column.
void expectVectorColumnsApart(LuxirNode& node) {
  std::map<std::string, std::vector<uint32_t>> files;
  for (const char* field : {"dense_v", "sparse_v", "multi_vs"}) {
    for (const auto& column : columns(node, field)) {
      ASSERT_GE(column.values.length, (uint64_t)PostingsWriter::RAM_SPILL_BYTES);
      // The file holds the column and its header, nothing else.
      EXPECT_LE(column.file->size() - column.values.length, 64u) << field;
      files[field].push_back(column.filenum);
    }
  }
  EXPECT_NE(files["dense_v"], files["sparse_v"]);
  EXPECT_NE(files["dense_v"], files["multi_vs"]);
}

}  // namespace

// A large vector column has its segment file to itself, flushed and merged.
TEST(KnnScanWindowTest, largeVectorColumnsKeepTheirFiles) {
  auto path = std::filesystem::temp_directory_path() / ("luxir-knnwin-" + newUuid());
  auto cleanup = scope_guard([&] { std::filesystem::remove_all(path); });
  LuxirConfig config;
  config.store.backend = "fs";
  config.store.data_dir = path.string();
  LuxirNode node(config);
  indexCorpus(node);
  expectVectorColumnsApart(node);
  indexCorpus(node, false);
  CollectionHelper h(node, "main");
  CollectionHelper::UpdateBuilder merge;
  merge.commit(true, 1);
  ASSERT_TRUE(h.submit(merge).success);
  ASSERT_EQ(1u, h.getIndexWriter()->snapshots.readers.getReader()->segments().size());
  expectVectorColumnsApart(node);
}

// An exact kNN scan over lazily read vector files returns what the in-memory
// path returns, and never acquires the values whole: a scan of most of the
// vectors reads bounded windows, a selective walk acquires just its vectors.
TEST(KnnScanWindowTest, lazyFlatScansReadBoundedWindows) {
  auto saved = KnnQuery::tuning;
  auto restore = scope_guard([&] { KnnQuery::tuning = saved; });
  KnnQuery::tuning.scanWindowBytes = kWindowBytes;
  KnnQuery::tuning.scanAheadBytes = kAheadBytes;

  LuxirNode memory;
  indexCorpus(memory);

  auto path = std::filesystem::temp_directory_path() / ("luxir-knnwin-" + newUuid());
  std::filesystem::remove_all(path);
  auto cleanup = scope_guard([&] { std::filesystem::remove_all(path); });
  LuxirConfig config;
  config.store.backend = "fs";
  config.store.data_dir = path.string();
  config.store.layers = {"knn_logged_lazy"};
  {
    LuxirNode node(config);
    indexCorpus(node);
  }
  LuxirNode node(config);  // reopened: every segment file is lazy

  struct Case {
    std::string field, filterField, filterValue;
    bool windows;  // reads most of the vectors
    int64_t matches = -1;  // single-valued: the documents the filter selects, when known
  };
  auto count = [](auto&& pred) { int64_t n = 0; for (int i = 0; i < kDocs; i++) n += pred(i); return n; };
  std::vector<Case> cases = {
      {"dense_v", "", "", true},              // sequential
      {"dense_v", "p_s", "yes", true},        // filtered sequential
      {"dense_v", "c_s", "c3", false, count([](int i) { return i % 7 == 3; })},        // domain walk
      {"sparse_v", "", "", true},             // sparse sequential
      {"sparse_v", "c_s", "c3", false},       // sparse walk
      {"multi_vs", "", "", true},             // multi-valued sequential
      {"multi_vs", "p_s", "yes", true},       // multi-valued walk of most documents
      {"multi_vs", "c_s", "c3", false},       // multi-valued walk
      {"dense_v", "id", "d1007", false, 1},      // a walk of one document
      {"dense_v", "t_s", "y", false, count([](int i) { return i % 3 == 0; })},  // more documents than a look-ahead batch
      {"multi_vs", "t_s", "y", false},        // multi-valued, the same
  };
  for (const auto& c : cases) {
    SCOPED_TRACE(c.field + " " + c.filterField + ":" + c.filterValue);
    auto cols = columns(node, c.field);
    ASSERT_FALSE(cols.empty());
    for (const auto& column : cols) static_cast<LoggedLazyFile*>(column.file)->clear();
    auto expected = exactKnn(memory, c.field, c.filterField, c.filterValue);
    ASSERT_FALSE(expected.ids.empty());
    EXPECT_EQ(expected, exactKnn(node, c.field, c.filterField, c.filterValue));
    uint64_t acquiredBytes = 0;
    for (const auto& column : cols) {
      auto ranges = static_cast<LoggedLazyFile*>(column.file)->within(column.values.offset, column.values.length);
      uint64_t total = bytesOf(ranges);
      acquiredBytes += total;
      expectEachByteOnce(ranges);
      for (const auto& range : ranges) {
        EXPECT_LT(range.length, column.values.length);
        EXPECT_EQ(0u, range.length % (uint64_t)kVectorBytes);
      }
      if (c.windows) {
        for (const auto& range : ranges) EXPECT_LE(range.length, (uint64_t)kWindowBytes);
        EXPECT_GE(ranges.size(), 10u);
      } else {
        EXPECT_LT(total * 2, column.values.length);
        if (c.filterField == "t_s") {
          EXPECT_GE(static_cast<LoggedLazyFile*>(column.file)->batchSizes().size(), 2u);
        }
      }
    }
    if (c.matches >= 0) {
      EXPECT_EQ(acquiredBytes, (uint64_t)c.matches * (uint64_t)kVectorBytes);
    }
  }
}

// A failed acquire fails the search that needed it, and leaves later searches alone.
TEST(KnnScanWindowTest, failedAcquiresFailTheSearch) {
  auto saved = KnnQuery::tuning;
  auto restore = scope_guard([&] { KnnQuery::tuning = saved; });
  KnnQuery::tuning.scanWindowBytes = kWindowBytes;
  KnnQuery::tuning.scanAheadBytes = kAheadBytes;
  auto path = std::filesystem::temp_directory_path() / ("luxir-knnwin-" + newUuid());
  auto cleanup = scope_guard([&] { std::filesystem::remove_all(path); });
  LuxirConfig config;
  config.store.backend = "fs";
  config.store.data_dir = path.string();
  config.store.layers = {"knn_logged_lazy"};
  {
    LuxirNode node(config);
    indexCorpus(node);
  }
  LuxirNode node(config);
  auto cols = columns(node, "dense_v");
  ASSERT_FALSE(cols.empty());
  auto expected = exactKnn(node, "dense_v");
  ASSERT_FALSE(expected.ids.empty());
  for (const auto& column : cols) static_cast<LoggedLazyFile*>(column.file)->failing = true;
  // Window scan (a batch fails) and per-vector walk (a batch fails).
  exactKnn(node, "dense_v", "", "", false);
  exactKnn(node, "dense_v", "c_s", "c3", false);
  for (const auto& column : cols) static_cast<LoggedLazyFile*>(column.file)->failing = false;
  EXPECT_EQ(expected, exactKnn(node, "dense_v"));
}

// Exact rescoring of approximate candidates acquires them in one batch per segment.
TEST(KnnScanWindowTest, ivfRescoreAcquiresCandidatesInBatches) {
  auto path = std::filesystem::temp_directory_path() / ("luxir-knnwin-" + newUuid());
  auto cleanup = scope_guard([&] { std::filesystem::remove_all(path); });
  LuxirConfig config;
  config.store.backend = "fs";
  config.store.data_dir = path.string();
  config.store.layers = {"knn_logged_lazy"};
  ForceIvfGuard forceIvf;
  IvfTestGuard ivf(/*minTraining=*/16);
  {
    LuxirNode node(config);
    CollectionHelper h(node, "main");
    SchemaBuilder schema;
    auto& f = schema.templ("_v");
    f.type = api::FieldDef::FieldClass::VECTOR;
    f.column = true;
    f.dims = 4;
    f.metric = api::VectorMetric::L2;
    f.ann = pqAnn(4, 2, 2);
    schema.set(h.collection());
    std::vector<Doc> docs;
    for (int i = 0; i < 400; i++) docs.push_back(flatdoc("id", "d" + std::to_string(i), "iv_v", std::vector<float>{(float)(i % 40), (float)(i / 40), 0, 0}));
    ASSERT_TRUE(h.indexAll(docs, UpdateMessage::COMMIT).success);
    h.commit({"vec.iv_v"});
  }
  LuxirNode node(config);
  auto cols = columns(node, "iv_v");
  ASSERT_FALSE(cols.empty());
  auto req = localReq(node.getSearchEngine());
  auto& cur = req->collection("main").topDocs("q");
  cur.getScores().countLimit().fields({"id"});
  cur.rawQuery() = qb::knn(cur.mr(), "iv_v", {3.0f, 3.0f, 0.0f, 0.0f}, 5, 0, false, 40, 1.0f);
  req->execute(true);
  EXPECT_OK(req);
  for (const auto& column : cols) {
    auto* file = static_cast<LoggedLazyFile*>(column.file);
    auto ranges = file->within(column.values.offset, column.values.length);
    ASSERT_FALSE(ranges.empty());
    expectEachByteOnce(ranges);
    for (const auto& range : ranges) EXPECT_EQ(16u, range.length);  // single vectors, not the column
    EXPECT_FALSE(file->batchSizes().empty());
  }
}
