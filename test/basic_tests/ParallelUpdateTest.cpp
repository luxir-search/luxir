// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

// A large update message is split into chunks indexed into separate inverters in
// parallel (ProtoUpdateMessage::handle).  The split must be invisible: duplicate
// ids, deletes, per-doc errors, return_ids and status read exactly as for a
// message indexed whole.  The chunk size is lowered so small inputs split.

#include <gtest/gtest.h>

#include "luxir/index/IndexWriter.h"
#include "test/LuxirTest.h"
#include "test/CollectionHelper.h"
#include "test/LocalReq.h"
#include "test/TestUtils.h"

using namespace luxir;
using namespace luxir::test;

using ResponseStatus = luxir::api::UpdateResponse_::Status;

static constexpr const char* BAD_FIELD = "no_such_field";
static constexpr int N_DOCS = 40;

class ParallelUpdateTest : public LuxirTest {
public:
  struct ChunkGuard {
    std::shared_ptr<IndexWriter> iw;
    size_t saved;
    explicit ChunkGuard(std::shared_ptr<IndexWriter> w)
      : iw(std::move(w)), saved(iw->parallelUpdateChunkDocs) {
      iw->parallelUpdateChunkDocs = 4;  // 40 docs -> up to 10 chunks
    }
    ~ChunkGuard() { iw->parallelUpdateChunkDocs = saved; }
  };

  // Docs "d0".."d39" with text_w "<word> n<i>".
  static std::vector<Doc> docs(std::string_view word) {
    std::vector<Doc> out;
    for (int i = 0; i < N_DOCS; i++) {
      out.push_back(flatdoc("id", "d" + std::to_string(i), "text_w",
                            std::string(word) + " n" + std::to_string(i)));
    }
    return out;
  }

  static void addAll(CollectionHelper::UpdateBuilder& b, const std::vector<Doc>& docs) {
    for (const auto& doc : docs) b.add(doc);
    b.overwrite(true);
  }

  static int64_t count(CollectionHelper& helper, std::string_view word) {
    auto req = localReq(helper.getSearchEngine());
    req->collection("main").topDocs("q").matchQuery("text_w", word).withStats().limit(0);
    req->execute();
    EXPECT_OK(req);
    return req->getMatchCount("q");
  }

  static int64_t countAll(CollectionHelper& helper) {
    auto req = localReq(helper.getSearchEngine());
    req->collection("main").topDocs("q").allQuery().withStats().limit(0);
    req->execute();
    EXPECT_OK(req);
    return req->getMatchCount("q");
  }
};


// Copies of one id fall in different contiguous chunks; they are indexed in one
// inverter so only the last survives.  The request lands in several segments.
TEST_F(ParallelUpdateTest, duplicateIdsAcrossChunksLastWins) {
  CollectionHelper helper("main");
  ChunkGuard guard(helper.getIndexWriter());

  auto in = docs("plain");
  in[1] = flatdoc("id", "dup", "text_w", "vone");
  in[18] = flatdoc("id", "dup", "text_w", "vtwo");
  in[37] = flatdoc("id", "dup", "text_w", "vthree");
  CollectionHelper::UpdateBuilder b;
  addAll(b, in);
  b.commit();
  auto result = helper.submit(b);
  ASSERT_EQ(ResponseStatus::OK, result.status);

  auto reader = helper.getIndexWriter()->snapshots.readers.getReader();
  EXPECT_GT(reader->segments().size(), 1u) << "the request should have been split";
  EXPECT_EQ(N_DOCS - 2, countAll(helper));
  EXPECT_EQ(0, count(helper, "vone"));
  EXPECT_EQ(0, count(helper, "vtwo"));
  EXPECT_EQ(1, count(helper, "vthree"));
}


// A failed later copy leaves the earlier copy in place, as in an unsplit request.
TEST_F(ParallelUpdateTest, failedLaterDuplicateKeepsEarlierCopy) {
  CollectionHelper helper("main");
  ChunkGuard guard(helper.getIndexWriter());

  auto in = docs("plain");
  in[2] = flatdoc("id", "dup", "text_w", "vgood");
  in[30] = flatdoc("id", "dup", "text_w", "vbad", BAD_FIELD, "boom");
  CollectionHelper::UpdateBuilder b;
  addAll(b, in);
  b.commit();
  auto result = helper.submit(b);
  ASSERT_EQ(ResponseStatus::PARTIAL, result.status);
  ASSERT_EQ(1u, result.errors.size());
  EXPECT_EQ(30, result.errors[0].index);
  EXPECT_EQ(1, count(helper, "vgood"));
  EXPECT_EQ(0, count(helper, "vbad"));
  EXPECT_EQ(N_DOCS - 1, countAll(helper));
}


TEST_F(ParallelUpdateTest, overwritesCommittedDocs) {
  CollectionHelper helper("main");
  ChunkGuard guard(helper.getIndexWriter());

  CollectionHelper::UpdateBuilder first;
  addAll(first, docs("old"));
  first.commit();
  ASSERT_EQ(ResponseStatus::OK, helper.submit(first).status);
  CollectionHelper::UpdateBuilder second;
  addAll(second, docs("new"));
  second.commit();
  ASSERT_EQ(ResponseStatus::OK, helper.submit(second).status);

  EXPECT_EQ(N_DOCS, countAll(helper));
  EXPECT_EQ(0, count(helper, "old"));
  EXPECT_EQ(N_DOCS, count(helper, "new"));
}


// delete_ids apply to versions below the request's, so an id deleted and
// re-added in the same request survives whichever chunk re-adds it.
TEST_F(ParallelUpdateTest, deleteIdsAndReaddInSameRequest) {
  CollectionHelper helper("main");
  ChunkGuard guard(helper.getIndexWriter());

  helper.index(flatdoc("id", "a", "text_w", "olda"), UpdateMessage::NO_COMMIT, true);
  helper.index(flatdoc("id", "b", "text_w", "oldb"), UpdateMessage::COMMIT, true);

  auto in = docs("plain");
  in[35] = flatdoc("id", "a", "text_w", "newa");
  CollectionHelper::UpdateBuilder b;
  addAll(b, in);
  b.remove("a").remove("b");
  b.commit();
  ASSERT_EQ(ResponseStatus::OK, helper.submit(b).status);

  EXPECT_EQ(0, count(helper, "olda"));
  EXPECT_EQ(0, count(helper, "oldb"));
  EXPECT_EQ(1, count(helper, "newa"));
  EXPECT_EQ(N_DOCS, countAll(helper));
}


// Errors and returned ids are merged in request order across chunks.
TEST_F(ParallelUpdateTest, errorsAndReturnIdsInRequestOrder) {
  CollectionHelper helper("main");
  ChunkGuard guard(helper.getIndexWriter());

  auto in = docs("plain");
  in[5] = flatdoc("id", "d5", "text_w", "bad", BAD_FIELD, "boom");
  in[33] = flatdoc("id", "d33", "text_w", "bad", BAD_FIELD, "boom");
  CollectionHelper::UpdateBuilder b;
  addAll(b, in);
  b.returnIds(true);
  b.commit();
  auto result = helper.submit(b);
  ASSERT_EQ(ResponseStatus::PARTIAL, result.status);
  ASSERT_EQ(2u, result.errors.size());
  EXPECT_EQ(5, result.errors[0].index);
  EXPECT_EQ("d5", result.errors[0].id);
  EXPECT_EQ(33, result.errors[1].index);
  EXPECT_EQ("d33", result.errors[1].id);

  std::vector<std::string> expected;
  for (int i = 0; i < N_DOCS; i++) {
    if (i != 5 && i != 33) expected.push_back("d" + std::to_string(i));
  }
  EXPECT_EQ(expected, result.ids);
  EXPECT_EQ(N_DOCS - 2, countAll(helper));
}


// A commit after a split request without one flushes every chunk's inverter.
TEST_F(ParallelUpdateTest, commitAfterSplitRequestSeesAllDocs) {
  CollectionHelper helper("main");
  ChunkGuard guard(helper.getIndexWriter());

  CollectionHelper::UpdateBuilder b;
  addAll(b, docs("plain"));
  ASSERT_EQ(ResponseStatus::OK, helper.submit(b).status);
  helper.commit();
  EXPECT_EQ(N_DOCS, countAll(helper));
}
