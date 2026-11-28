// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

// Patches that change only source-only fields (no index, column, stored copy
// or variant): the document's record is replaced in its segment's source map
// and the document is not reindexed.

#include <gtest/gtest.h>

#include <chrono>
#include <future>
#include <latch>
#include <memory_resource>
#include <thread>

#include "luxir/index/IndexWriter.h"
#include "luxir/reader/Postings.h"
#include "luxir/server/LuxirNode.h"
#include "luxir/server/ProtoUpdateMessage.h"
#include "luxir/store/SourceFiles.h"
#include "luxir/util/Signal.h"
#include "test/CollectionHelper.h"
#include "test/LocalReq.h"
#include "test/LuxirTest.h"
#include "test/SchemaBuilder.h"
#include "test/TestUtils.h"

namespace luxir::test {

using namespace std::chrono_literals;

namespace {

using Status = api::PatchResult::Status;

struct ParsedRequest {
  std::pmr::monotonic_buffer_resource arena;
  api::UpdateRequest request;
  explicit ParsedRequest(std::string_view json) {
    std::string err;
    EXPECT_TRUE(api::read_json(request, json, arena, &err)) << err;
  }
};

// An update request written as JSON, kept alive with its message.
class JsonUpdate final : private ParsedRequest, public ProtoUpdateMessage {
public:
  std::promise<void> completed;

  explicit JsonUpdate(std::string_view json) : ParsedRequest(json), ProtoUpdateMessage(&request) {}
  void done(IndexWriter&) override { completed.set_value(); }
  bool waitFor(std::chrono::milliseconds timeout) {
    return completed.get_future().wait_for(timeout) == std::future_status::ready;
  }
};

class SourceOnlyPatchTest : public LuxirTest {
protected:
  // `note` lives only in the source; n_i is indexed.
  static void schema(CollectionHelper& helper, int32_t maxFiles = 0) {
    SchemaBuilder b;
    auto& source = b.storedResource(Postings::SOURCE_RESOURCE);
    if (maxFiles) source.max_files = maxFiles;
    auto& note = b.field("note");
    note.type = api::FieldDef::FieldClass::STRING;
    note.index = api::FieldDef::IndexMode::NONE;
    note.column = false;
    note.multi = api::FieldDef::MultiMode::DEFAULT;
    b.set(helper.collection());
  }

  // Runs `json` on the writer and returns its patch outcomes.
  static std::vector<UpdateMessage::PatchOutcome> patch(IndexWriter& iw, std::string_view json) {
    JsonUpdate update(json);
    EXPECT_TRUE(iw.submitUpdate(&update));
    EXPECT_TRUE(update.waitFor(10s));
    EXPECT_FALSE(update.result.errored()) << update.result.what();
    return update.patchOutcomes;
  }

  struct Doc {
    std::string source;
    int64_t version = 0;
    bool operator==(const Doc&) const = default;
  };

  // _source and _version_ of the live documents, in id order.
  static std::vector<Doc> docs(CollectionHelper& helper) {
    auto req = localReq(helper.getSearchEngine());
    req->collection("main").topDocs("q").allQuery().fields({"_source", "_version_"}).limit(-1);
    req->execute();
    EXPECT_TRUE(req->ok()) << req->toString();
    std::vector<Doc> out;
    for (const auto& doc : req->getDocs()) {
      const auto* source = find(doc, "_source");
      const auto* version = find(doc, "_version_");
      out.push_back({source ? std::get<std::string>(*source) : std::string(),
                     version ? std::get<int64_t>(*version) : 0});
    }
    // Every source starts with its id.
    std::ranges::sort(out, {}, &Doc::source);
    return out;
  }

  static uint64_t maxDocs(IndexWriter& iw) { return iw.stats(false).maxDocs; }

  // Patches the writer's node wrote without reindexing.
  static uint64_t sourceOnly(IndexWriter& iw) {
    return iw.metrics->counts().counters[(size_t)MetricCounter::PATCHES_SOURCE_ONLY];
  }
};

}  // namespace

TEST_F(SourceOnlyPatchTest, replacesTheRecordWithoutReindexing) {
  CollectionHelper helper;
  schema(helper);
  auto iw = helper.getIndexWriter();
  uint64_t counted = sourceOnly(*iw);  // the node's counter, shared with other tests
  ASSERT_TRUE(helper.index(flatdoc("id", "a", "n_i", 1, "note", "x"), UpdateMessage::COMMIT).success);
  ASSERT_EQ(1u, maxDocs(*iw));
  auto birth = docs(helper)[0].version;

  auto out = patch(*iw, R"({"patches":[{"id":"a","ops":[{"op":"set","path":"note","value":["y","z"]}]}]})");
  ASSERT_EQ(1u, out.size());
  EXPECT_EQ(Status::UPDATED, out[0].status);
  EXPECT_GT(out[0].version, (uint64_t)birth);
  // No document was indexed: the segment and its postings are as they were.
  EXPECT_EQ(1u, maxDocs(*iw));
  EXPECT_EQ((std::vector<Doc>{{R"({"id":"a","n_i":1,"note":["y","z"]})", (int64_t)out[0].version}}), docs(helper));

  // _version_ is the patch's: if_version sees it.
  auto stale = patch(*iw, R"({"patches":[{"id":"a","if_version":)" + std::to_string(birth) +
                              R"(,"ops":[{"op":"unset","path":"note"}]}]})");
  EXPECT_EQ(Status::CONFLICT, stale[0].status);
  auto current = patch(*iw, R"({"patches":[{"id":"a","if_version":)" + std::to_string(out[0].version) +
                                R"(,"ops":[{"op":"unset","path":"note"}]}]})");
  EXPECT_EQ(Status::UPDATED, current[0].status);
  EXPECT_EQ(1u, maxDocs(*iw));
  EXPECT_EQ((std::vector<Doc>{{R"({"id":"a","n_i":1})", (int64_t)current[0].version}}), docs(helper));
  EXPECT_EQ(2u, sourceOnly(*iw) - counted);
}

TEST_F(SourceOnlyPatchTest, indexedChangesStillReindex) {
  CollectionHelper helper;
  schema(helper);
  auto iw = helper.getIndexWriter();
  uint64_t counted = sourceOnly(*iw);  // the node's counter, shared with other tests
  ASSERT_TRUE(helper.index(flatdoc("id", "a", "n_i", 1, "note", "x"), UpdateMessage::COMMIT).success);
  auto out = patch(*iw, R"({"patches":[{"id":"a","ops":[{"op":"set","path":"note","value":"y"},)"
                        R"({"op":"increment","path":"n_i","value":1}]}]})");
  EXPECT_EQ(Status::UPDATED, out[0].status);
  EXPECT_EQ(0u, sourceOnly(*iw) - counted);
  EXPECT_EQ(R"({"id":"a","n_i":2,"note":"y"})", docs(helper)[0].source);
  // A value a scalar field rejects is left to reindexing, which reports it.
  out = patch(*iw, R"({"patches":[{"id":"a","ops":[{"op":"set","path":"note","value":[["x"]]}]}]})");
  EXPECT_EQ(Status::ERROR, out[0].status);
}

TEST_F(SourceOnlyPatchTest, mergesCarryPatchedRecordsAndVersions) {
  CollectionHelper helper;
  schema(helper);
  auto iw = helper.getIndexWriter();
  uint64_t counted = sourceOnly(*iw);  // the node's counter, shared with other tests
  ASSERT_TRUE(helper.indexAll({flatdoc("id", "a", "note", "x"), flatdoc("id", "b", "note", "x")},
                              UpdateMessage::COMMIT).success);
  auto out = patch(*iw, R"({"patches":[{"id":"b","ops":[{"op":"set","path":"note","value":"y"}]}]})");
  ASSERT_EQ(Status::UPDATED, out[0].status);
  ASSERT_TRUE(helper.index(flatdoc("id", "c", "note", "x"), UpdateMessage::COMMIT).success);
  auto before = docs(helper);
  ASSERT_EQ(R"({"id":"b","note":"y"})", before[1].source);

  CollectionHelper::UpdateBuilder merge;
  merge.commit(true, 1);
  ASSERT_TRUE(helper.submit(merge).success);
  ASSERT_EQ(1u, helper.durableSegmentCount());
  EXPECT_EQ(before, docs(helper));
  EXPECT_EQ((int64_t)out[0].version, docs(helper)[1].version);
  // And the merged segment takes further source-only patches.
  out = patch(*iw, R"({"patches":[{"id":"b","ops":[{"op":"set","path":"note","value":"z"}]}]})");
  EXPECT_EQ(Status::UPDATED, out[0].status);
  EXPECT_EQ(2u, sourceOnly(*iw) - counted);
  EXPECT_EQ(R"({"id":"b","note":"z"})", docs(helper)[1].source);
}

// A segment a merge is reading takes the reindexing path: the merge would not
// carry a new map into its output.
TEST_F(SourceOnlyPatchTest, aMergeInProgressMakesThePatchReindex) {
  LuxirNode node;
  CollectionHelper helper(node);
  schema(helper);
  auto iw = helper.getIndexWriter();
  uint64_t counted = sourceOnly(*iw);  // the node's counter, shared with other tests
  ASSERT_TRUE(helper.index(flatdoc("id", "a", "note", "x"), UpdateMessage::COMMIT).success);
  ASSERT_TRUE(helper.index(flatdoc("id", "b", "note", "x"), UpdateMessage::COMMIT).success);

  std::latch release(1);
  std::promise<void> started;
  auto cleanup = scope_guard([&] {
    if (!release.try_wait()) release.count_down();
    iw->updateGraph->wait_for_all();
    Signal::unlisten("mergeLiveDocsDiff");
  });
  Signal::listen("mergeLiveDocsDiff", [&](void*, void*, void*) -> void* {
    started.set_value();
    release.wait();
    return nullptr;
  });
  JsonUpdate merge(R"({"commit":{"max_segments":1}})");
  ASSERT_TRUE(iw->submitUpdate(&merge));
  ASSERT_EQ(std::future_status::ready, started.get_future().wait_for(5s));

  auto out = patch(*iw, R"({"patches":[{"id":"a","ops":[{"op":"set","path":"note","value":"y"}]}]})");
  EXPECT_EQ(Status::UPDATED, out[0].status);
  EXPECT_EQ(0u, sourceOnly(*iw) - counted);
  release.count_down();
  ASSERT_TRUE(merge.waitFor(10s));
  iw->updateGraph->wait_for_all();
  helper.commit();
  auto after = docs(helper);
  ASSERT_EQ(2u, after.size());
  EXPECT_EQ(R"({"id":"a","note":"y"})", after[0].source);
  EXPECT_EQ((int64_t)out[0].version, after[0].version);
}

// The manifest lists the patched map and records that it holds versions, so a
// reopened writer reads both.
TEST_F(SourceOnlyPatchTest, survivesAReopen) {
  LuxirNode node;
  CollectionHelper helper(node);
  schema(helper);
  auto iw = helper.getIndexWriter();
  ASSERT_TRUE(helper.index(flatdoc("id", "a", "note", "x"), UpdateMessage::COMMIT).success);
  auto out = patch(*iw, R"({"patches":[{"id":"a","ops":[{"op":"set","path":"note","value":"y"}]}]})");
  ASSERT_EQ(Status::UPDATED, out[0].status);
  iw->close();

  IndexWriter reopened(iw->snapshots);
  auto again = patch(reopened, R"({"patches":[{"id":"a","if_version":)" + std::to_string(out[0].version) +
                                   R"(,"when":[{"path":"note","op":"eq","value":"y"}],)"
                                   R"("ops":[{"op":"set","path":"note","value":"z"}]}]})");
  EXPECT_EQ(Status::UPDATED, again[0].status);
  EXPECT_EQ(1u, maxDocs(reopened));
  EXPECT_EQ(1u, sourceOnly(reopened));
}

// Each commit that patches a segment adds a file to it; past twice max_files
// the records of the files it uses least move into the commit's file too, with
// their versions, and nothing is reindexed.
TEST_F(SourceOnlyPatchTest, patchFilesPerSegmentAreFolded) {
  CollectionHelper helper;
  schema(helper, 1);
  auto iw = helper.getIndexWriter();
  uint64_t counted = sourceOnly(*iw);  // the node's counter, shared with other tests
  // c keeps the flushed file referenced.
  ASSERT_TRUE(helper.indexAll({flatdoc("id", "a", "note", "x"), flatdoc("id", "b", "note", "x"),
                               flatdoc("id", "c", "note", "x")}, UpdateMessage::COMMIT).success);
  auto first = patch(*iw, R"({"patches":[{"id":"a","ops":[{"op":"set","path":"note","value":"y"}]}]})");
  // The flushed file, the first patch file and this one would pass the cap: a fold.
  auto second = patch(*iw, R"({"patches":[{"id":"b","ops":[{"op":"set","path":"note","value":"y"}]}]})");
  auto folded = docs(helper);
  EXPECT_EQ(R"({"id":"a","note":"y"})", folded[0].source);
  EXPECT_EQ((int64_t)first[0].version, folded[0].version);  // a folded record keeps its version
  patch(*iw, R"({"patches":[{"id":"a","ops":[{"op":"set","path":"note","value":"z"}]}]})");
  EXPECT_EQ(3u, sourceOnly(*iw) - counted);
  EXPECT_EQ(3u, maxDocs(*iw));
  auto all = docs(helper);
  ASSERT_EQ(3u, all.size());
  EXPECT_EQ(R"({"id":"a","note":"z"})", all[0].source);
  EXPECT_EQ(R"({"id":"b","note":"y"})", all[1].source);
  EXPECT_EQ((int64_t)second[0].version, all[1].version);
  EXPECT_EQ(R"({"id":"c","note":"x"})", all[2].source);
  EXPECT_LT(all[2].version, (int64_t)first[0].version);
}

// Replaced maps and patch files whose records were all replaced again are
// deleted once no snapshot lists them: repeated patches of one document leave
// the flushed file, one patch file and the segment's current map.
TEST_F(SourceOnlyPatchTest, replacedFilesAreDeleted) {
  CollectionHelper helper;
  schema(helper);
  auto iw = helper.getIndexWriter();
  uint64_t counted = sourceOnly(*iw);  // the node's counter, shared with other tests
  ASSERT_TRUE(helper.indexAll({flatdoc("id", "a", "note", "x"), flatdoc("id", "b", "note", "x")},
                              UpdateMessage::COMMIT).success);
  for (int i = 0; i < 5; i++) {
    patch(*iw, R"({"patches":[{"id":"a","ops":[{"op":"set","path":"note","value":")" + std::to_string(i) + R"("}]}]})");
  }
  ASSERT_EQ(5u, sourceOnly(*iw) - counted);
  helper.commit();
  auto onDisk = [&] {
    std::vector<Directory::FileInfo> files;
    iw->dir.listFiles(files);
    size_t data = 0, maps = 0;
    for (const auto& file : files) {
      if (isSourceMapFile(file.name)) maps++;
      else if (isSourceFile(file.name)) data++;
    }
    return std::pair(data, maps);
  };
  // The flushed segment's first map stays with its immutable file list.
  auto expected = std::pair<size_t, size_t>(2, 2);
  for (int i = 0; i < 200 && onDisk() != expected; i++) std::this_thread::sleep_for(10ms);
  EXPECT_EQ(expected, onDisk());
  EXPECT_EQ(R"({"id":"a","note":"4"})", docs(helper)[0].source);
}

}  // namespace luxir::test
