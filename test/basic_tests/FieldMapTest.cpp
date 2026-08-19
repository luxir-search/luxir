// Tests for UpdateRequest.field_map / drop_unmapped: per-request renaming of input
// doc keys onto schema fields at ingest, "" targets dropping a key, last-wins dedup
// on the post-mapping name, and request-level validation of the map itself.

#include <gtest/gtest.h>

#include "test/LuxirTest.h"
#include "test/CollectionHelper.h"
#include "test/LocalReq.h"
#include "test/TestUtils.h"

using namespace std;
using namespace luxir;
using namespace luxir::test;

using ResponseStatus = luxir::api::UpdateResponse_::Status;

class FieldMapTest : public LuxirTest {
public:
  struct Opts {
    std::initializer_list<std::pair<std::string_view, std::string_view>> fieldMap = {};
    bool dropUnmapped = false;
    bool returnIds = false;
  };

  static IndexResult submitDocs(CollectionHelper& helper, std::span<const Doc> docs,
                                const Opts& opts) {
    CollectionHelper::UpdateBuilder b;
    for (const auto& doc : docs) {
      b.add(doc);
    }
    b.fieldMap(opts.fieldMap);
    b.dropUnmapped(opts.dropUnmapped);
    b.returnIds(opts.returnIds);
    b.commit();
    return helper.submit(b);
  }

  // ids of docs whose text_w contains the (lowercase, single-token) word
  static std::vector<Doc> matchDocs(CollectionHelper& helper, std::string_view word) {
    auto req = localReq(helper.getSearchEngine());
    req->collection("main").topDocs("q").matchQuery("text_w", word).fields({"id"}).limit(-1);
    req->execute();
    return req->getDocs();
  }
};


TEST_F(FieldMapTest, renameIndexesUnderSchemaField) {
  CollectionHelper helper("main");

  // "headline" matches no schema field or template; only the mapping makes it indexable.
  std::vector<Doc> docs = {
    flatdoc("id", "a1", "headline", "alpha bravo", "tag_s", "keep"),
  };
  auto result = submitDocs(helper, docs, {.fieldMap = {{"headline", "text_w"}}});
  ASSERT_EQ(ResponseStatus::OK, result.status);

  EXPECT_EQ(1u, matchDocs(helper, "alpha").size());

  // The unmapped key indexed under its own name.
  auto req = localReq(helper.getSearchEngine());
  req->collection("main").topDocs("q").matchQuery("tag_s", "keep").fields({"id"}).limit(-1);
  req->execute();
  EXPECT_EQ(1u, req->getDocs().size());
}


TEST_F(FieldMapTest, emptyTargetDropsKey) {
  CollectionHelper helper("main");

  // "junk" would fail schema resolution; mapping it to "" drops it instead.
  std::vector<Doc> docs = {
    flatdoc("id", "a1", "text_w", "alpha", "junk", "boom"),
  };
  auto result = submitDocs(helper, docs, {.fieldMap = {{"junk", ""}}});
  ASSERT_EQ(ResponseStatus::OK, result.status);
  EXPECT_EQ(1u, matchDocs(helper, "alpha").size());
}


TEST_F(FieldMapTest, dropUnmappedKeepsOnlyMappedKeys) {
  CollectionHelper helper("main");

  // Foreign shape: pick out two keys, everything else (including a key that would
  // fail schema resolution) is dropped.  "id" must be mapped explicitly too.
  std::vector<Doc> docs = {
    flatdoc("doc_id", "a1", "headline", "alpha bravo", "noise", "boom"),
  };
  auto result = submitDocs(helper, docs,
                           {.fieldMap = {{"doc_id", "id"}, {"headline", "text_w"}},
                            .dropUnmapped = true,
                            .returnIds = true});
  ASSERT_EQ(ResponseStatus::OK, result.status);
  ASSERT_EQ(1u, result.ids.size());
  EXPECT_EQ("a1", result.ids[0]);  // docId resolves through the mapping
  EXPECT_EQ(1u, matchDocs(helper, "alpha").size());
}


TEST_F(FieldMapTest, mappedIdOverwrites) {
  CollectionHelper helper("main");

  Opts opts{.fieldMap = {{"doc_id", "id"}, {"headline", "text_w"}}, .dropUnmapped = true};
  Doc v1 = flatdoc("doc_id", "dup", "headline", "first version");
  Doc v2 = flatdoc("doc_id", "dup", "headline", "second version");
  ASSERT_EQ(ResponseStatus::OK, submitDocs(helper, {&v1, 1}, opts).status);
  ASSERT_EQ(ResponseStatus::OK, submitDocs(helper, {&v2, 1}, opts).status);

  EXPECT_EQ(0u, matchDocs(helper, "first").size());
  EXPECT_EQ(1u, matchDocs(helper, "second").size());
}


TEST_F(FieldMapTest, postMappingCollisionLastWins) {
  CollectionHelper helper("main");

  // "headline" maps onto a key the doc also carries directly; the later occurrence
  // (in doc order) wins, same as a duplicated key.
  std::vector<Doc> docs = {
    flatdoc("id", "a1", "text_w", "first stuff", "headline", "second stuff"),
  };
  auto result = submitDocs(helper, docs, {.fieldMap = {{"headline", "text_w"}}});
  ASSERT_EQ(ResponseStatus::OK, result.status);
  EXPECT_EQ(0u, matchDocs(helper, "first").size());
  EXPECT_EQ(1u, matchDocs(helper, "second").size());
}


TEST_F(FieldMapTest, unmappedUnresolvableKeyStillErrors) {
  CollectionHelper helper("main");

  // Without drop_unmapped, a key the map does not cover still fails resolution.
  std::vector<Doc> docs = {
    flatdoc("id", "b1", "headline", "alpha", "no_such_field", "boom"),
  };
  auto result = submitDocs(helper, docs, {.fieldMap = {{"headline", "text_w"}}});
  ASSERT_EQ(ResponseStatus::ERROR, result.status);
  ASSERT_EQ(1u, result.errors.size());
  EXPECT_EQ("b1", result.errors[0].id);
  EXPECT_NE(std::string::npos, result.errors[0].error_message.find("no_such_field"));
}


TEST_F(FieldMapTest, dropUnmappedRequiresMap) {
  CollectionHelper helper("main");

  std::vector<Doc> docs = { flatdoc("id", "a1", "text_w", "alpha") };
  auto result = submitDocs(helper, docs, {.dropUnmapped = true});
  EXPECT_FALSE(result.success);
  EXPECT_NE(std::string::npos, result.error_message.find("field_map"));
  EXPECT_EQ(0u, matchDocs(helper, "alpha").size());
}


TEST_F(FieldMapTest, invalidTargetRejectsRequest) {
  CollectionHelper helper("main");

  std::vector<Doc> docs = { flatdoc("id", "a1", "text_w", "alpha") };
  auto result = submitDocs(helper, docs, {.fieldMap = {{"headline", "9bad name"}}});
  EXPECT_FALSE(result.success);
  EXPECT_NE(std::string::npos, result.error_message.find("field_map target"));
  EXPECT_EQ(0u, matchDocs(helper, "alpha").size());
}
