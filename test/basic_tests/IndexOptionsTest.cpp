// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include "luxir/query/StoredPhraseQuery.h"
#include "luxir/util/Signal.h"
#include "test/CollectionHelper.h"
#include "test/LocalReq.h"
#include "test/LuxirTest.h"
#include "test/QueryBuild.h"
#include "test/TestUtils.h"

#include <atomic>
#include <cstdio>
#include <format>
#include <map>
#include <set>

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

  struct FieldSummary {
    int32_t flags;
    bool norms;  // the segment wrote a norms part
    int32_t docsWithField;
  };

  std::vector<FieldSummary> fieldInfos(std::string_view field) {
    auto reader = helper.getIndexWriter()->snapshots.readers.getReader();
    std::vector<FieldSummary> infos;
    for (auto& segment : reader->segments()) {
      if (auto* info = segment.postingsReader().fieldInfo(field)) {
        infos.push_back({info->flags, info->norms() != nullptr, info->docsWithField});
      }
    }
    return infos;
  }

  static void mergeAll(CollectionHelper& target) {
    CollectionHelper::UpdateBuilder merge;
    merge.commit(true, 1);
    ASSERT_TRUE(target.submit(merge).success);
  }

  void mergeAll() { mergeAll(helper); }

  // id -> score for an API query.
  std::map<std::string, float> scores(const api::Query& query) {
    auto req = localReq(helper.getSearchEngine());
    auto& cursor = req->collection("main").topDocs("q");
    cursor.rawQuery() = query;
    cursor.fields({"id"}).limit(-1).getScores();
    req->execute();
    EXPECT_TRUE(req->ok()) << req->errorMsg();
    std::map<std::string, float> result;
    const auto* docs = req->docList();
    if (!req->ok() || docs == nullptr || docs->row_count == 0) return result;
    const auto& ids = std::get<api::ColStr>(docs->columns.at("id").kind).v;
    const auto& col = std::get<api::ColFloat>(docs->columns.at("_score_").kind).v;
    for (size_t i = 0; i < ids.size(); i++) result[std::string(ids[i])] = col[i];
    return result;
  }
};

TEST_F(IndexOptionsTest, docsWithoutNormsScoresMatchesAlike) {
  put(R"({"fields":{"msg":{"type":"text","index_options":"docs","norms":false}}})");
  ASSERT_TRUE(helper.indexAll({flatdoc("id", "a", "msg", "disk full"),
                               flatdoc("id", "b", "msg", "disk disk disk on the long path of a long message"),
                               flatdoc("id", "c", "msg", "all good")}, UpdateMessage::COMMIT).success);
  auto info = fieldInfos("msg").at(0);
  EXPECT_FALSE(FieldType::hasFreqs(info.flags));
  EXPECT_FALSE(info.norms);
  EXPECT_EQ(3, info.docsWithField);

  auto hits = scores("msg:disk");
  ASSERT_EQ(2u, hits.size());
  EXPECT_GT(hits["a"], 0.0f);
  EXPECT_EQ(hits["a"], hits["b"]);
  EXPECT_EQ(1u, scores("msg:good").size());
  EXPECT_EQ(3u, scores("exists(msg)").size());
}

TEST_F(IndexOptionsTest, positionlessPhrasesVerifyAgainstStoredSource) {
  put(R"({"fields":{"msg":{"type":"text","index_options":"docs","norms":false},
                    "lines":{"type":"text","index_options":"freqs","multi":true},
                    "kind":"string"}})");
  ASSERT_TRUE(helper.indexAll({flatdoc("id", "a", "msg", "disk full on host", "kind", "rare"),
                               flatdoc("id", "b", "msg", "full disk on host", "kind", "common"),
                               flatdoc("id", "c", "msg", "disk is nearly full", "kind", "common"),
                               flatdoc("id", "d", "msg", "disk disk", "lines", vecs("the disk", "full now"))},
                              UpdateMessage::COMMIT).success);
  auto ids = [&](std::string_view expr) {
    std::set<std::string> result;
    for (auto& [id, score] : scores(expr)) result.insert(id);
    return result;
  };
  using Ids = std::set<std::string>;
  EXPECT_EQ(Ids({"a"}), ids("msg:\"disk full\""));
  EXPECT_EQ(Ids({"a"}), ids("msg:\"disk full\"~1"));
  EXPECT_EQ(Ids({"a", "b", "c"}), ids("msg:\"disk full\"~2"));
  EXPECT_EQ(Ids({"d"}), ids("msg:\"disk disk\""));
  EXPECT_EQ(Ids({"a", "b"}), ids("msg:\"on host\""));
  // Values sit 100 positions apart, as in an indexed phrase.
  EXPECT_EQ(Ids(), ids("lines:\"disk full\"~99"));
  EXPECT_EQ(Ids({"d"}), ids("lines:\"disk full\"~100"));

  // Verification runs only on candidates the cheaper clause accepts: a, not
  // all three docs holding both terms.
  StoredPhraseQuery::Verifier::matchCallsForTests = 0;
  EXPECT_EQ(Ids({"a"}), ids("kind:rare AND msg:\"disk full\"~2"));
  EXPECT_LT(StoredPhraseQuery::Verifier::matchCallsForTests.load(), 3);

  auto req = localReq(helper.getSearchEngine());
  req->collection("main").topDocs("q").simpleQuery("\"full disk\"", {"msg"}).fields({"id"}).limit(-1);
  req->execute();
  ASSERT_TRUE(req->ok()) << req->errorMsg();
  ASSERT_EQ(1u, req->getDocs().size());
  EXPECT_EQ("b", std::get<std::string>(*find(req->getDocs()[0], "id")));
}

TEST_F(IndexOptionsTest, positionlessPhraseWithoutStoredSourceFails) {
  put(R"({"fields":{"msg":{"type":"text","index_options":"docs","norms":false,"stored":false}}})");
  ASSERT_TRUE(helper.index(flatdoc("id", "a", "msg", "disk full"), UpdateMessage::COMMIT).success);
  EXPECT_EQ(1u, scores("msg:disk").size());
  std::string error;
  EXPECT_TRUE(scores("msg:\"disk full\"", &error).empty());
  EXPECT_NE(std::string::npos, error.find("requires indexed positions or a stored source")) << error;
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
  EXPECT_FALSE(fieldInfos("pos").at(0).norms);
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
  EXPECT_FALSE(after[0].norms);
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
  EXPECT_TRUE(info.norms);
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
  EXPECT_TRUE(info.norms);
  auto phrase = scores("msg:\"disk full\"");
  ASSERT_EQ(2u, phrase.size());
  EXPECT_GT(phrase["new1"], phrase["new2"]);
}

TEST_F(IndexOptionsTest, mergesWhenNoSourceIsIndexed) {
  put(R"({"fields":{"msg":{"type":"text","index":"none"}}})");
  for (auto id : {"a", "b", "c"}) {
    ASSERT_TRUE(helper.index(flatdoc("id", id, "msg", "disk full"), UpdateMessage::COMMIT).success);
    mergeAll();
  }
  auto infos = fieldInfos("msg");
  ASSERT_EQ(1u, infos.size());
  EXPECT_EQ(0, infos[0].flags & FieldType::INDEX_DOCS_FREQS_POSITIONS);
  EXPECT_EQ(3, infos[0].docsWithField);
}

TEST_F(IndexOptionsTest, partitionedMergeDowngrades) {
  auto writer = helper.getIndexWriter();
  writer->termPartitionMinBytes = 1;
  writer->termPartitionMinRangeBytes = 1;
  writer->termPartitionMaxRanges = 4;
  put(R"({"fields":{"msg":"text"}})");
  std::vector<Doc> docs;
  for (int i = 0; i < 300; i++) {
    docs.push_back(flatdoc("id", std::format("old{}", i), "msg",
                           std::format("disk w{} x{} w{} full", i, i, i % 7)));
  }
  ASSERT_TRUE(helper.indexAll(docs, UpdateMessage::COMMIT).success);
  put(R"({"fields":{"msg":{"type":"text","index_options":"docs","norms":false}}})");
  ASSERT_TRUE(helper.index(flatdoc("id", "new", "msg", "disk full after a long write"),
                           UpdateMessage::COMMIT).success);
  std::atomic<int> ranges = 0;
  Signal::listen("termRangeStreamsCheckedOut", [&](void*, void*, void*) -> void* { ranges++; return nullptr; });
  auto unlisten = scope_guard([] { Signal::unlisten("termRangeStreamsCheckedOut"); });
  mergeAll();
  EXPECT_GT(ranges.load(), 1);
  auto info = fieldInfos("msg").at(0);
  EXPECT_FALSE(FieldType::hasFreqs(info.flags));
  EXPECT_FALSE(info.norms);
  auto hits = scores("msg:disk AND (id:old0 OR id:new)");
  ASSERT_EQ(2u, hits.size());
  EXPECT_EQ(hits["old0"], hits["new"]);
  EXPECT_EQ(1u, scores("msg:x33").size());
  EXPECT_EQ(1u, scores("msg:\"x5 w5 full\"").size());
}

TEST_F(IndexOptionsTest, segmentsWithoutNormsScoreAtAverageLength) {
  // Every value has 4 tokens, so the average length is 4 and a segment
  // without norms must score like a segment with norms.
  put(R"({"fields":{"msg":"text"}})");
  ASSERT_TRUE(helper.indexAll({flatdoc("id", "a", "msg", "disk full now go"),
                               flatdoc("id", "c", "msg", "rain cold wind snow")},
                              UpdateMessage::COMMIT).success);
  put(R"({"fields":{"msg":{"type":"text","norms":false}}})");
  ASSERT_TRUE(helper.index(flatdoc("id", "b", "msg", "disk full now go"), UpdateMessage::COMMIT).success);
  ASSERT_EQ(2u, fieldInfos("msg").size());

  auto term = scores("msg:disk");
  ASSERT_EQ(2u, term.size());
  EXPECT_FLOAT_EQ(term["a"], term["b"]);
  google::protobuf::Arena arena;
  ArenaResource mr{&arena};
  // A weight other than 1 builds a combined-field query even for one field.
  auto combined = scores(qb::multiMatch(mr, "disk", {{"msg", 2.0f}}));
  ASSERT_EQ(2u, combined.size());
  EXPECT_FLOAT_EQ(combined["a"], combined["b"]);
}

TEST_F(IndexOptionsTest, freqsScoresLikePositions) {
  put(R"({"fields":{"pos":"text","freq":{"type":"text","index_options":"freqs"}}})");
  std::vector<Doc> docs;
  for (auto [id, text] : {std::pair{"a", "disk full"}, {"b", "disk disk full on a long running host"},
                          {"c", "the host is fine"}, {"d", "disk"}}) {
    docs.push_back(flatdoc("id", id, "pos", text, "freq", text));
  }
  ASSERT_TRUE(helper.indexAll(docs, UpdateMessage::COMMIT).success);
  for (auto term : {"disk", "host", "full"}) {
    auto pos = scores(std::string("pos:") + term);
    EXPECT_FALSE(pos.empty()) << term;
    EXPECT_EQ(pos, scores(std::string("freq:") + term)) << term;
  }
}

TEST_F(IndexOptionsTest, lowerIndexOptionsAreSmaller) {
  // Whole-segment bytes after a merge to one segment, for one text field.
  auto segmentBytes = [](std::string_view name, std::string_view options, bool prose) {
    CollectionHelper target(*LuxirTest::luxirNode, name);
    put(target, std::format(R"({{"fields":{{"msg":{{"type":"text","stored":false{}}}}}}})", options));
    static constexpr std::string_view words[] = {
        "the", "disk", "of", "a", "server", "was", "full", "and", "every", "write",
        "failed", "until", "an", "operator", "cleared", "old", "logs", "from", "it", "today"};
    std::vector<Doc> docs;
    for (int i = 0; i < 2000; i++) {
      std::string msg;
      if (prose) {
        for (int w = 0; w < 30; w++) {
          if (w) msg += ' ';
          msg += words[(size_t) ((i * 7 + w * w * 3 + w) % 20)];
        }
      } else {
        msg = std::format("{} host-{} GET /api/items/{} status {} took {} ms", i % 7 == 0 ? "ERROR" : "INFO",
                          i % 13, i % 101, 200 + i % 5, i % 997);
      }
      docs.push_back(flatdoc("id", std::to_string(i), "msg", msg));
    }
    EXPECT_TRUE(target.indexAll(docs, UpdateMessage::COMMIT).success);
    mergeAll(target);
    auto reader = target.getIndexWriter()->snapshots.readers.getReader();
    EXPECT_EQ(1u, reader->segments().size());
    return (int64_t) reader->segments()[0].postingsReader().core().sizeInBytes();
  };
  for (bool prose : {false, true}) {
    std::string tag = prose ? "prose" : "log";
    int64_t positions = segmentBytes(tag + "_positions", "", prose);
    int64_t freqs = segmentBytes(tag + "_freqs", R"(,"index_options":"freqs")", prose);
    int64_t docsOnly = segmentBytes(tag + "_docs", R"(,"index_options":"docs","norms":false)", prose);
    EXPECT_LT(freqs, positions) << tag;
    EXPECT_LT(docsOnly, freqs) << tag;
    std::printf("%s corpus segment bytes: positions %lld, freqs %lld, docs without norms %lld\n",
                tag.c_str(), (long long) positions, (long long) freqs, (long long) docsOnly);
  }
}

} // namespace luxir::test
