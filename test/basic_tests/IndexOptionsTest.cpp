// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include "test/CollectionHelper.h"
#include "test/LocalReq.h"
#include "test/LuxirTest.h"
#include "test/TestUtils.h"

#include <format>
#include <map>

namespace luxir::test {

// index_options and norms: what TEXT postings record, and how scoring and
// merges handle segments written under different definitions.
class IndexOptionsTest : public LuxirTest {
protected:
  CollectionHelper helper;

  static void put(CollectionHelper& target, std::string_view json) {
    std::pmr::monotonic_buffer_resource arena;
    api::SchemaDef def;
    std::string error;
    if (!api::read_json(def, json, arena, &error)) throw std::runtime_error(error);
    target.collection().updateSchema(def, api::SchemaRequest_::Mode::SET);
  }

  void put(std::string_view json) { put(helper, json); }

  // id -> score; empty when the request fails.
  std::map<std::string, float> scores(std::string_view expr, std::string* error = nullptr) {
    auto req = localReq(helper.getSearchEngine());
    req->collection("main").topDocs("q").exprQuery(expr).withStats().fields({"id"}).limit(-1);
    req->execute();
    std::map<std::string, float> result;
    if (!req->ok()) {
      if (error) *error = req->errorMsg();
      return result;
    }
    const auto& col = std::get<api::ColFloat>(req->docList()->columns.at("_score_").kind).v;
    auto docs = req->getDocs();
    for (size_t i = 0; i < docs.size(); i++) {
      result[std::get<std::string>(*find(docs[i], "id"))] = col[i];
    }
    return result;
  }

  std::vector<SegFieldInfo> fieldInfos(std::string_view field) {
    auto reader = helper.getIndexWriter()->snapshots.readers.getReader();
    std::vector<SegFieldInfo> infos;
    for (auto& segment : reader->segments()) {
      FieldReader fields(segment.postingsReader());
      if (fields.seek(field)) fields.readFieldInfo(infos.emplace_back());
    }
    return infos;
  }

  static void mergeAll(CollectionHelper& target) {
    CollectionHelper::UpdateBuilder merge;
    merge.commit(true, 1);
    ASSERT_TRUE(target.submit(merge).success);
  }

  void mergeAll() { mergeAll(helper); }
};

TEST_F(IndexOptionsTest, docsWithoutNormsScoresMatchesAlikeAndRejectsPhrases) {
  put(R"({"fields":{"msg":{"type":"text","index_options":"docs","norms":false}}})");
  ASSERT_TRUE(helper.indexAll({flatdoc("id", "a", "msg", "disk full"),
                               flatdoc("id", "b", "msg", "disk disk disk on the long path of a long message"),
                               flatdoc("id", "c", "msg", "all good")}, UpdateMessage::COMMIT).success);
  auto info = fieldInfos("msg").at(0);
  EXPECT_FALSE(FieldType::hasFreqs(info.flags));
  EXPECT_EQ(SegFieldInfo::NORMS_NONE, info.normsFormat);
  EXPECT_EQ(3, info.docsWithField);

  auto hits = scores("msg:disk");
  ASSERT_EQ(2u, hits.size());
  EXPECT_GT(hits["a"], 0.0f);
  EXPECT_EQ(hits["a"], hits["b"]);
  EXPECT_EQ(1u, scores("msg:good").size());
  EXPECT_EQ(3u, scores("exists(msg)").size());

  std::string error;
  EXPECT_TRUE(scores("msg:\"disk full\"", &error).empty());
  EXPECT_NE(std::string::npos, error.find("requires indexed positions")) << error;
}

TEST_F(IndexOptionsTest, normsFalseIgnoresLengthButKeepsFreqsAndPhrases) {
  put(R"({"fields":{"pos":{"type":"text","norms":false},
                    "freq":{"type":"text","index_options":"freqs","norms":false}}})");
  for (auto field : {"pos", "freq"}) {
    ASSERT_TRUE(helper.indexAll({flatdoc("id", "short", field, "disk full"),
                                 flatdoc("id", "long", field, "disk full after a very long write of many words"),
                                 flatdoc("id", "twice", field, "disk disk full")}, UpdateMessage::COMMIT).success);
    auto hits = scores(std::string(field) + ":disk");
    ASSERT_EQ(3u, hits.size()) << field;
    EXPECT_EQ(hits["short"], hits["long"]) << field;
    EXPECT_GT(hits["twice"], hits["short"]) << field;
  }
  auto phrase = scores("pos:\"disk full\"");
  ASSERT_EQ(3u, phrase.size());
  EXPECT_EQ(phrase["short"], phrase["long"]);
  EXPECT_EQ(SegFieldInfo::NORMS_NONE, fieldInfos("pos").at(0).normsFormat);
}

TEST_F(IndexOptionsTest, mixedSegmentsQueryAndMergeDowngrades) {
  put(R"({"fields":{"msg":"text"}})");
  ASSERT_TRUE(helper.indexAll({flatdoc("id", "old1", "msg", "disk full"),
                               flatdoc("id", "old2", "msg", "disk full after a long write")},
                              UpdateMessage::COMMIT).success);
  put(R"({"fields":{"msg":{"type":"text","index_options":"docs","norms":false}}})");
  ASSERT_TRUE(helper.indexAll({flatdoc("id", "new1", "msg", "disk full"),
                               flatdoc("id", "new2", "msg", "disk full after a long write")},
                              UpdateMessage::COMMIT).success);
  auto before = fieldInfos("msg");
  ASSERT_EQ(2u, before.size());

  // The old segment still scores with norms, the new one without.
  auto hits = scores("msg:disk");
  ASSERT_EQ(4u, hits.size());
  EXPECT_GT(hits["old1"], hits["old2"]);
  EXPECT_EQ(hits["new1"], hits["new2"]);

  mergeAll();
  auto after = fieldInfos("msg");
  ASSERT_EQ(1u, after.size());
  EXPECT_FALSE(FieldType::hasFreqs(after[0].flags));
  EXPECT_EQ(SegFieldInfo::NORMS_NONE, after[0].normsFormat);
  EXPECT_EQ(4, after[0].docsWithField);
  EXPECT_EQ(4u, scores("exists(msg)").size());
  hits = scores("msg:disk");
  ASSERT_EQ(4u, hits.size());
  EXPECT_EQ(hits["old1"], hits["old2"]);
  EXPECT_EQ(hits["old1"], hits["new1"]);
}

TEST_F(IndexOptionsTest, upgradeSkipsPositionlessSegmentsForPhrases) {
  put(R"({"fields":{"msg":{"type":"text","index_options":"freqs"}}})");
  ASSERT_TRUE(helper.index(flatdoc("id", "old", "msg", "disk full"), UpdateMessage::COMMIT).success);
  put(R"({"fields":{"msg":"text"}})");
  ASSERT_TRUE(helper.index(flatdoc("id", "new", "msg", "disk full"), UpdateMessage::COMMIT).success);
  EXPECT_EQ(2u, scores("msg:disk").size());
  auto phrase = scores("msg:\"disk full\"");
  ASSERT_EQ(1u, phrase.size());
  EXPECT_TRUE(phrase.contains("new"));

  // A merge keeps only what every source has: positions are gone, norms stay.
  mergeAll();
  auto info = fieldInfos("msg").at(0);
  EXPECT_TRUE(FieldType::hasFreqs(info.flags));
  EXPECT_FALSE(FieldType::hasPositions(info.flags));
  EXPECT_NE(SegFieldInfo::NORMS_NONE, info.normsFormat);
  EXPECT_EQ(2u, scores("msg:disk").size());
  EXPECT_TRUE(scores("msg:\"disk full\"").empty());
}

TEST_F(IndexOptionsTest, unindexedSourceKeepsMergedPositionsAndNorms) {
  put(R"({"fields":{"msg":{"type":"text","index":"none"}}})");
  ASSERT_TRUE(helper.index(flatdoc("id", "old", "msg", "disk full"), UpdateMessage::COMMIT).success);
  put(R"({"fields":{"msg":"text"}})");
  ASSERT_TRUE(helper.indexAll({flatdoc("id", "new1", "msg", "disk full"),
                               flatdoc("id", "new2", "msg", "disk full after a long write")},
                              UpdateMessage::COMMIT).success);
  mergeAll();
  auto info = fieldInfos("msg").at(0);
  EXPECT_TRUE(FieldType::hasPositions(info.flags));
  EXPECT_NE(SegFieldInfo::NORMS_NONE, info.normsFormat);
  auto phrase = scores("msg:\"disk full\"");
  ASSERT_EQ(2u, phrase.size());
  EXPECT_GT(phrase["new1"], phrase["new2"]);
}

TEST_F(IndexOptionsTest, matchOnlyTextIsSmaller) {
  auto segmentBytes = [](std::string_view name, std::string_view schema) {
    CollectionHelper target(*LuxirTest::luxirNode, name);
    put(target, schema);
    std::vector<Doc> docs;
    for (int i = 0; i < 2000; i++) {
      docs.push_back(flatdoc("id", std::to_string(i), "msg", std::format(
          "{} host-{} GET /api/items/{} status {} took {} ms", i % 7 == 0 ? "ERROR" : "INFO",
          i % 13, i % 101, 200 + i % 5, i % 997)));
    }
    EXPECT_TRUE(target.indexAll(docs, UpdateMessage::COMMIT).success);
    mergeAll(target);
    auto reader = target.getIndexWriter()->snapshots.readers.getReader();
    EXPECT_EQ(1u, reader->segments().size());
    return (int64_t) reader->segments()[0].postingsReader().sizeInBytes();
  };
  int64_t full = segmentBytes("full", R"({"fields":{"msg":{"type":"text","stored":false}}})");
  int64_t lean = segmentBytes("lean", R"({"fields":{"msg":{"type":"text","stored":false,
      "index_options":"docs","norms":false}}})");
  EXPECT_LT(lean, full);
}

} // namespace luxir::test
