// Tests for update edge cases around the unique id field.

#include <gtest/gtest.h>

#include "test/SoluxTest.h"
#include "test/CollectionHelper.h"
#include "test/LocalReq.h"
#include "test/TestUtils.h"

using namespace std;
using namespace solux;
using namespace solux::test;

class UpdateErrorTest : public SoluxTest {
public:
  // ids of all docs in the index
  static std::vector<Doc> allDocs(CollectionHelper& helper) {
    auto* req = LocalReq::create(helper.getSearchEngine());
    auto docs = req->collection("main").allQuery().fields({"id"}).limit(-1).execute().getDocs();
    req->done();
    return docs;
  }

  // ids of docs whose text_w contains the (lowercase, single-token) word
  static std::vector<Doc> matchDocs(CollectionHelper& helper, std::string_view word) {
    auto* req = LocalReq::create(helper.getSearchEngine());
    auto docs = req->collection("main").matchQuery("text_w", word).fields({"id"}).limit(-1).execute().getDocs();
    req->done();
    return docs;
  }
};


// Same id twice in one batch with overwrite: the earlier in-segment doc is
// superseded and must be deleted (applyDeletes can't reach it - the id postings
// only list the latest doc per id).
TEST_F(UpdateErrorTest, inSegmentDuplicateOverwrite) {
  CollectionHelper helper("main");
  helper.clear();

  std::vector<Doc> docs = {
    flatdoc("id", "dup", "text_w", "first version"),
    flatdoc("id", "dup", "text_w", "second version"),
  };

  auto result = helper.indexAll(docs, UpdateMessage::COMMIT, true);
  EXPECT_TRUE(result.success);

  EXPECT_EQ(1u, allDocs(helper).size());
  EXPECT_EQ(1u, matchDocs(helper, "second").size());
  EXPECT_EQ(0u, matchDocs(helper, "first").size());
}


// Same as above but split across two uncommitted requests (typically reusing the
// same inverter): the cross-request overwrite must also leave a single live doc.
TEST_F(UpdateErrorTest, crossRequestDuplicateOverwrite) {
  CollectionHelper helper("main");
  helper.clear();

  helper.index(flatdoc("id", "dup", "text_w", "first version"), UpdateMessage::NO_COMMIT, true);
  helper.index(flatdoc("id", "dup", "text_w", "second version"), UpdateMessage::COMMIT, true);

  EXPECT_EQ(1u, allDocs(helper).size());
  EXPECT_EQ(1u, matchDocs(helper, "second").size());
  EXPECT_EQ(0u, matchDocs(helper, "first").size());
}
