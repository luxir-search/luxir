// Tests for update failure handling: failed docs are marked deleted (a partially
// indexed doc can't be backed out), errors are reported per doc, id-map mutations
// are rolled back so previous versions survive, and all_or_none makes a request
// atomic.

#include <gtest/gtest.h>

#include "test/LuxirTest.h"
#include "test/CollectionHelper.h"
#include "test/LocalReq.h"
#include "test/TestUtils.h"

using namespace std;
using namespace luxir;
using namespace luxir::test;

using ResponseStatus = luxir::api::UpdateResponse_::Status;

// No schema entry and no default suffix match, so getIndexHandler throws.
static constexpr const char* BAD_FIELD = "no_such_field";

class UpdateErrorTest : public LuxirTest {
public:
  struct SubmitOpts {
    bool allOrNone = false;
    bool returnIds = false;
    bool overwrite = true;
    std::vector<std::string> deleteIds;
  };

  static IndexResult submitDocs(CollectionHelper& helper, std::span<const Doc> docs) {
    return submitDocs(helper, docs, SubmitOpts());
  }

  static IndexResult submitDocs(CollectionHelper& helper, std::span<const Doc> docs,
                                const SubmitOpts& opts) {
    CollectionHelper::UpdateBuilder b;
    for (const auto& doc : docs) {
      b.add(doc);
    }
    for (const auto& id : opts.deleteIds) {
      b.remove(id);
    }
    b.overwrite(opts.overwrite);
    b.allOrNone(opts.allOrNone);
    b.returnIds(opts.returnIds);
    b.commit();
    return helper.submit(b);
  }

  // ids of all docs in the index
  static std::vector<Doc> allDocs(CollectionHelper& helper) {
    auto req = localReq(helper.getSearchEngine());
    req->collection("main").topDocs("q").allQuery().fields({"id"}).limit(-1);
    req->execute();
    return req->getDocs();
  }

  // ids of docs whose text_w contains the (lowercase, single-token) word
  static std::vector<Doc> matchDocs(CollectionHelper& helper, std::string_view word) {
    auto req = localReq(helper.getSearchEngine());
    req->collection("main").topDocs("q").matchQuery("text_w", word).fields({"id"}).limit(-1);
    req->execute();
    return req->getDocs();
  }
};


TEST_F(UpdateErrorTest, partialFailureMarksDocDeleted) {
  CollectionHelper helper("main");

  std::vector<Doc> docs = {
    flatdoc("id", "g1", "text_w", "alpha bravo"),
    flatdoc("id", "b1", "text_w", "alpha zzzbadcontent", BAD_FIELD, "boom"),
    flatdoc("id", "g2", "text_w", "alpha charlie"),
  };

  auto result = submitDocs(helper, docs);
  EXPECT_TRUE(result.success);  // partial success
  ASSERT_EQ(ResponseStatus::PARTIAL, result.status);
  ASSERT_EQ(1u, result.errors.size());
  EXPECT_EQ("b1", result.errors[0].id);
  EXPECT_EQ(1, result.errors[0].index);
  EXPECT_NE(std::string::npos, result.errors[0].error_message.find(BAD_FIELD));
  EXPECT_EQ("unknown_field", result.errors[0].code);

  auto all = allDocs(helper);
  ASSERT_EQ(2u, all.size());
  EXPECT_TRUE(containsDoc(all, flatdoc("id", "g1")));
  EXPECT_TRUE(containsDoc(all, flatdoc("id", "g2")));

  // Whatever was indexed of the failed doc before the throw must not be visible.
  EXPECT_EQ(0u, matchDocs(helper, "zzzbadcontent").size());
}


TEST_F(UpdateErrorTest, failedOverwriteKeepsOldVersion) {
  CollectionHelper helper("main");

  helper.index(flatdoc("id", "x1", "text_w", "original content"), UpdateMessage::COMMIT, true);

  Doc badUpdate = flatdoc("id", "x1", "text_w", "updated content", BAD_FIELD, "boom");
  auto result = submitDocs(helper, {&badUpdate, 1});
  EXPECT_FALSE(result.success);
  ASSERT_EQ(ResponseStatus::ERROR, result.status);  // everything failed
  ASSERT_EQ(1u, result.errors.size());
  EXPECT_EQ("x1", result.errors[0].id);

  // The failed update must not have deleted (or replaced) the old version.
  EXPECT_EQ(1u, allDocs(helper).size());
  EXPECT_EQ(1u, matchDocs(helper, "original").size());
  EXPECT_EQ(0u, matchDocs(helper, "updated").size());
}


TEST_F(UpdateErrorTest, allOrNoneRollsBackBatch) {
  CollectionHelper helper("main");

  helper.index(flatdoc("id", "a", "text_w", "aye one"), UpdateMessage::COMMIT, true);
  helper.index(flatdoc("id", "b", "text_w", "bee one"), UpdateMessage::COMMIT, true);

  std::vector<Doc> docs = {
    flatdoc("id", "a", "text_w", "aye two"),                       // indexed, then rolled back
    flatdoc("id", "z", "text_w", "zee one", BAD_FIELD, "boom"),    // fails
    flatdoc("id", "c", "text_w", "cee one"),                       // never attempted
  };

  SubmitOpts opts;
  opts.allOrNone = true;
  auto result = submitDocs(helper, docs, opts);
  EXPECT_FALSE(result.success);
  ASSERT_EQ(ResponseStatus::ERROR, result.status);
  ASSERT_EQ(1u, result.errors.size());  // docs after the failure are not listed
  EXPECT_EQ("z", result.errors[0].id);
  EXPECT_EQ(1, result.errors[0].index);

  // The index is unchanged: a still has its old content, c never made it in.
  auto all = allDocs(helper);
  ASSERT_EQ(2u, all.size());
  EXPECT_TRUE(containsDoc(all, flatdoc("id", "a")));
  EXPECT_TRUE(containsDoc(all, flatdoc("id", "b")));
  EXPECT_EQ(1u, matchDocs(helper, "aye").size());
  EXPECT_EQ(0u, matchDocs(helper, "two").size());
  EXPECT_EQ(0u, matchDocs(helper, "cee").size());
}


TEST_F(UpdateErrorTest, allOrNoneRollsBackDeletes) {
  CollectionHelper helper("main");

  helper.index(flatdoc("id", "y1", "text_w", "keep me"), UpdateMessage::COMMIT, true);

  Doc bad = flatdoc("id", "z", "text_w", "zee", BAD_FIELD, "boom");

  // all_or_none: the queued delete must be rolled back along with the docs.
  SubmitOpts opts;
  opts.allOrNone = true;
  opts.deleteIds = {"y1"};
  auto result = submitDocs(helper, {&bad, 1}, opts);
  ASSERT_EQ(ResponseStatus::ERROR, result.status);
  EXPECT_EQ(1u, matchDocs(helper, "keep").size());

  // Without all_or_none the delete applies even though the doc failed.
  opts.allOrNone = false;
  result = submitDocs(helper, {&bad, 1}, opts);
  ASSERT_EQ(ResponseStatus::PARTIAL, result.status);
  EXPECT_EQ(0u, matchDocs(helper, "keep").size());
}


TEST_F(UpdateErrorTest, allFailedIsError) {
  CollectionHelper helper("main");

  std::vector<Doc> docs = {
    flatdoc("id", "b1", "text_w", "one", BAD_FIELD, "boom"),
    flatdoc("id", "b2", "text_w", "two", BAD_FIELD, "boom"),
  };

  auto result = submitDocs(helper, docs);
  EXPECT_FALSE(result.success);
  ASSERT_EQ(ResponseStatus::ERROR, result.status);
  ASSERT_EQ(2u, result.errors.size());
  EXPECT_EQ(0, result.errors[0].index);
  EXPECT_EQ(1, result.errors[1].index);
  EXPECT_EQ(0u, allDocs(helper).size());
}


TEST_F(UpdateErrorTest, returnIdsListsOnlySuccesses) {
  CollectionHelper helper("main");

  std::vector<Doc> docs = {
    flatdoc("id", "g1", "text_w", "alpha"),
    flatdoc("id", "b1", "text_w", "bravo", BAD_FIELD, "boom"),
    flatdoc("id", "g2", "text_w", "charlie"),
  };

  SubmitOpts opts;
  opts.returnIds = true;
  auto result = submitDocs(helper, docs, opts);
  ASSERT_EQ(ResponseStatus::PARTIAL, result.status);
  ASSERT_EQ(2u, result.ids.size());
  EXPECT_EQ("g1", result.ids[0]);
  EXPECT_EQ("g2", result.ids[1]);
  EXPECT_GT(result.updateVersion, 0u);
}


TEST_F(UpdateErrorTest, failedDocThenSameIdSucceeds) {
  CollectionHelper helper("main");

  std::vector<Doc> docs = {
    flatdoc("id", "r1", "text_w", "bad attempt", BAD_FIELD, "boom"),
    flatdoc("id", "r1", "text_w", "good attempt"),
  };

  auto result = submitDocs(helper, docs);
  ASSERT_EQ(ResponseStatus::PARTIAL, result.status);

  EXPECT_EQ(1u, allDocs(helper).size());
  EXPECT_EQ(1u, matchDocs(helper, "good").size());
  EXPECT_EQ(0u, matchDocs(helper, "bad").size());
  EXPECT_EQ(1u, matchDocs(helper, "attempt").size());
}


// Same id twice in one batch with overwrite: the earlier in-segment doc is
// superseded and must be deleted (applyDeletes can't reach it - the id postings
// only list the latest doc per id).
TEST_F(UpdateErrorTest, inSegmentDuplicateOverwrite) {
  CollectionHelper helper("main");

  std::vector<Doc> docs = {
    flatdoc("id", "dup", "text_w", "first version"),
    flatdoc("id", "dup", "text_w", "second version"),
  };

  auto result = helper.indexAll(docs, UpdateMessage::COMMIT, true);
  EXPECT_TRUE(result.success);
  EXPECT_EQ(ResponseStatus::OK, result.status);

  EXPECT_EQ(1u, allDocs(helper).size());
  EXPECT_EQ(1u, matchDocs(helper, "second").size());
  EXPECT_EQ(0u, matchDocs(helper, "first").size());
}


// A failed doc must not constrain later docs via learned vector dims: the first
// vector of the bad doc tentatively learns dims=3 during validation, the second
// throws (mismatch) before anything is appended, so the segment's dims are set
// by the first doc whose value is actually written (v2, dims=2).  Once a value
// is appended the constraint is real: v3 (dims=4) fails against it.
TEST_F(UpdateErrorTest, failedDocDoesNotLockVectorDims) {
  CollectionHelper helper("main");

  std::vector<Doc> docs = {
    flatdoc("id", "v1", "vec_vs", std::vector<std::vector<float>>{{1.0f, 2.0f, 3.0f}, {1.0f, 2.0f}}),
    flatdoc("id", "v2", "vec_vs", std::vector<std::vector<float>>{{5.0f, 6.0f}}),
    flatdoc("id", "v3", "vec_vs", std::vector<std::vector<float>>{{1.0f, 2.0f, 3.0f, 4.0f}}),
  };

  auto result = submitDocs(helper, docs);
  ASSERT_EQ(ResponseStatus::PARTIAL, result.status);
  ASSERT_EQ(2u, result.errors.size());
  EXPECT_EQ("v1", result.errors[0].id);
  EXPECT_NE(std::string::npos, result.errors[0].error_message.find("dims"));
  EXPECT_EQ("invalid_value", result.errors[0].code);
  EXPECT_EQ("v3", result.errors[1].id);
  EXPECT_NE(std::string::npos, result.errors[1].error_message.find("dims=2"));

  auto all = allDocs(helper);
  ASSERT_EQ(1u, all.size());
  EXPECT_TRUE(containsDoc(all, flatdoc("id", "v2")));
}


// Same as above but split across two uncommitted requests (typically reusing the
// same inverter): the cross-request overwrite must also leave a single live doc.
TEST_F(UpdateErrorTest, crossRequestDuplicateOverwrite) {
  CollectionHelper helper("main");

  helper.index(flatdoc("id", "dup", "text_w", "first version"), UpdateMessage::NO_COMMIT, true);
  helper.index(flatdoc("id", "dup", "text_w", "second version"), UpdateMessage::COMMIT, true);

  EXPECT_EQ(1u, allDocs(helper).size());
  EXPECT_EQ(1u, matchDocs(helper, "second").size());
  EXPECT_EQ(0u, matchDocs(helper, "first").size());
}
