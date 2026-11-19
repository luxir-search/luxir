// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include <gtest/gtest.h>

#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include "luxir/query/KnnQuery.h"
#include "luxir/server/LuxirNode.h"
#include "luxir/store/DelegatingDirectory.h"
#include "luxir/store/StorageRegistry.h"
#include "luxir/util/Uuid.h"
#include "luxir/util/luxir_util.h"
#include "test/CollectionHelper.h"
#include "test/LocalReq.h"
#include "test/QueryBuild.h"
#include "test/SchemaBuilder.h"
#include "test/TestUtils.h"

using namespace luxir;
using namespace luxir::test;

namespace {

// What the searches did to each file, by name.
struct FileReads {
  int wholeReads = 0;  // getInputStream() or read()
  std::vector<ByteRange> ranges;      // rangeStream()
  std::vector<ByteRange> prefetches;  // prefetchRanges()
};

struct ReadLog {
  std::mutex mutex;
  std::map<std::string, FileReads> byName;

  FileReads of(const std::string& name) {
    std::lock_guard lock(mutex);
    return byName[name];
  }
  void clear() {
    std::lock_guard lock(mutex);
    byName.clear();
  }
  void clearRanges() {
    std::lock_guard lock(mutex);
    for (auto& [name, reads] : byName) {
      reads.ranges.clear();
      reads.prefetches.clear();
    }
  }
};
ReadLog readLog;

// A lazy view of a local file that logs how it is read.
class LoggedLazyFile : public InputFile {
  std::shared_ptr<InputFile> inner;
  std::string name;

  FileReads& reads() { return readLog.byName[name]; }

public:
  LoggedLazyFile(std::shared_ptr<InputFile> inner, std::string name) : inner(std::move(inner)), name(std::move(name)) {}

  size_t size() override { return inner->size(); }
  std::string_view read() override {
    std::lock_guard lock(readLog.mutex);
    reads().wholeReads++;
    return inner->read();
  }
  InputStream getInputStream() override {
    std::lock_guard lock(readLog.mutex);
    reads().wholeReads++;
    return inner->getInputStream();
  }
  InputStream rangeStream(uint64_t offset, uint64_t length) override {
    std::lock_guard lock(readLog.mutex);
    reads().ranges.push_back({offset, length});
    return inner->rangeStream(offset, length);
  }
  void prefetchRanges(std::span<const ByteRange> ranges) override {
    std::lock_guard lock(readLog.mutex);
    for (const auto& range : ranges) reads().prefetches.push_back(range);
  }
  bool lazy() const override { return true; }
};

// Readers secure segment files by handle and map them when a segment opens.
class LoggedLazyHandle : public ChecksumReader {
  std::shared_ptr<ChecksumReader> inner;
  std::string name;

public:
  LoggedLazyHandle(std::shared_ptr<ChecksumReader> inner, std::string name) : inner(std::move(inner)), name(std::move(name)) {}
  uint64_t size() const override { return inner->size(); }
  std::shared_ptr<InputFile> map() override {
    auto file = inner->map();
    if (!file) return file;
    return std::make_shared<LoggedLazyFile>(std::move(file), name);
  }
  FileCheck checksum(const std::function<void(size_t)>& progress = {}) override { return inner->checksum(progress); }
};

class LoggedLazyDirectory : public DelegatingDirectory {
public:
  using DelegatingDirectory::DelegatingDirectory;
  std::shared_ptr<InputFile> openFile(std::string_view name, bool expectSynced = false) override {
    auto file = delegate->openFile(name, expectSynced);
    if (!file) return file;
    return std::make_shared<LoggedLazyFile>(std::move(file), std::string(name));
  }
  std::shared_ptr<ChecksumReader> openChecksum(std::string_view name, bool direct = false) override {
    auto handle = delegate->openChecksum(name, direct);
    if (!handle) return handle;
    return std::make_shared<LoggedLazyHandle>(std::move(handle), std::string(name));
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
constexpr int64_t kWindowBytes = 10 * kDims * (int64_t)sizeof(float);  // ten vectors

std::vector<float> vectorOf(int seed) {
  std::vector<float> v(kDims);
  for (int d = 0; d < kDims; d++) v[(size_t)d] = (float)(((uint64_t)seed * 2654435761u + (uint64_t)d * 40503u) % 1001) / 100.0f;
  return v;
}

// Every doc has dense_v; two docs in three have sparse_v; four in five have
// one to three multi_vs vectors. p_s selects most docs, c_s about one in seven.
std::vector<Doc> corpus() {
  std::vector<Doc> docs;
  for (int i = 0; i < kDocs; i++) {
    int n = i;
    Doc doc = flatdoc("id", "d" + std::to_string(n), "dense_v", vectorOf(n),
                      "c_s", "c" + std::to_string(i % 7), "p_s", i % 10 == 0 ? "no" : "yes");
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
              std::string_view filterValue = {}) {
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
  EXPECT_OK(req);
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

void indexCorpus(LuxirNode& node) {
  CollectionHelper h(node, "main");
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

// The files that hold a vector field's values, in every segment.
std::set<std::string> valueFiles(LuxirNode& node, std::string_view field) {
  CollectionHelper h(node, "main");
  auto reader = h.getIndexWriter()->snapshots.readers.getReader();
  std::set<std::string> names;
  for (auto& segment : reader->segments()) {
    auto& postings = segment.postingsReader();
    if (const auto* info = postings.fieldInfo(field)) names.insert(postings.core().fileName(info->columnLoc.filenum()));
  }
  return names;
}

}  // namespace

// An exact kNN scan over lazily read vector files returns what the in-memory
// path returns, reads the values only by bounded ranges and never whole.
TEST(KnnScanWindowTest, lazyFlatScansReadBoundedWindows) {
  auto saved = KnnQuery::tuning;
  auto restore = scope_guard([&] { KnnQuery::tuning = saved; });
  KnnQuery::tuning.scanWindowBytes = kWindowBytes;

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
  readLog.clear();  // what the indexing node read
  LuxirNode node(config);  // reopened: every segment file is lazy

  struct Case {
    std::string field, filterField, filterValue;
  };
  std::vector<Case> cases = {
      {"dense_v", "", ""},              // sequential
      {"dense_v", "p_s", "yes"},        // filtered sequential
      {"dense_v", "c_s", "c3"},         // domain walk
      {"sparse_v", "", ""},             // sparse sequential
      {"sparse_v", "c_s", "c3"},        // sparse walk
      {"multi_vs", "", ""},             // multi-valued sequential
      {"multi_vs", "p_s", "yes"},       // multi-valued walk
      {"dense_v", "id", "d1007"},       // a walk of one document reads by vector
  };
  for (const auto& c : cases) {
    SCOPED_TRACE(c.field + " " + c.filterField + ":" + c.filterValue);
    auto files = valueFiles(node, c.field);
    ASSERT_FALSE(files.empty());
    readLog.clearRanges();
    auto expected = exactKnn(memory, c.field, c.filterField, c.filterValue);
    ASSERT_FALSE(expected.ids.empty());
    EXPECT_EQ(expected, exactKnn(node, c.field, c.filterField, c.filterValue));
    size_t ranges = 0;
    for (const auto& name : files) {
      auto reads = readLog.of(name);
      EXPECT_EQ(0, reads.wholeReads) << name;
      for (const auto& range : reads.ranges) EXPECT_LE(range.length, (uint64_t)kWindowBytes) << name;
      for (const auto& range : reads.prefetches) EXPECT_LE(range.length, (uint64_t)kWindowBytes) << name;
      ranges += reads.ranges.size();
    }
    EXPECT_GT(ranges, 0u);
  }
  // An unfiltered scan reads several windows and declares the next as it goes.
  readLog.clearRanges();
  exactKnn(node, "dense_v");
  for (const auto& name : valueFiles(node, "dense_v")) {
    auto reads = readLog.of(name);
    EXPECT_GE(reads.ranges.size(), 10u) << name;
    EXPECT_GE(reads.prefetches.size(), 9u) << name;
  }
}

// A reader that still takes a whole lazy file is logged once per file when the
// file is over the limit.
TEST(KnnScanWindowTest, largeLazyMaterializationsAreCounted) {
  auto savedLimit = SegmentCore::maxMaterializeBytes.load();
  auto restore = scope_guard([&] { SegmentCore::maxMaterializeBytes = savedLimit; });
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
  LuxirNode node(config);
  CollectionHelper h(node, "main");
  auto reader = h.getIndexWriter()->snapshots.readers.getReader();
  auto& postings = reader->segments()[0].postingsReader();
  const auto* info = postings.fieldInfo("dense_v");
  ASSERT_NE(nullptr, info);
  uint32_t fnum = info->columnLoc.filenum();
  ASSERT_FALSE(postings.core().residentStream(fnum).has_value());
  SegmentCore::maxMaterializeBytes = 1;
  auto before = SegmentCore::largeMaterializations.load();
  postings.core().getInputStream(fnum, "dense_v");
  postings.core().getInputStream(fnum, "dense_v");
  EXPECT_EQ(before + 1, SegmentCore::largeMaterializations.load());
  EXPECT_TRUE(postings.core().residentStream(fnum).has_value());
}
