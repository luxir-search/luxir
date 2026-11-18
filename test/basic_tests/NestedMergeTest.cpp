// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include "test/CollectionHelper.h"
#include "test/LocalReq.h"
#include "test/DurableIndexInfo.h"
#include "test/LuxirTest.h"
#include "test/SchemaBuilder.h"
#include "test/QueryBuild.h"
#include "test/RowSpaceFixture.h"
#include "luxir/reader/IntColReader.h"
#include "luxir/reader/OwnershipShape.h"
#include "luxir/reader/Postings.h"

namespace luxir::test {
namespace {

void add(CollectionHelper::UpdateBuilder& input, std::string_view value) {
  input.addRaw([&](auto& doc, auto& arena) { ASSERT_TRUE(api::read_json(doc, value, arena)); });
}
void merge(CollectionHelper& helper, int32_t target = 1) {
  CollectionHelper::UpdateBuilder input;
  input.commit(true, target); ASSERT_TRUE(helper.submit(input).success);
}
void schema(CollectionHelper& helper) {
  SchemaBuilder b;
  for (std::string prefix : {"", "items_nested.", "items_nested.parts_nested."}) {
    auto& array = b.field(prefix + "array_s");
    array.type = api::FieldDef::FieldClass::STRING; array.multi = api::FieldDef::MultiMode::ARRAY;
    auto& n = b.field(prefix + "n_i");
    n.type = api::FieldDef::FieldClass::INT; n.index = api::FieldDef::IndexMode::RANGE;
    auto& geo = b.field(prefix + "geo_p");
    geo.type = api::FieldDef::FieldClass::GEO_POINT; geo.index = api::FieldDef::IndexMode::RANGE;
    auto& stored = b.field(prefix + "stored_s");
    stored.type = api::FieldDef::FieldClass::STRING;
    stored.index = api::FieldDef::IndexMode::NONE; stored.column = false; stored.stored = true;
  }
  b.set(helper.collection());
  helper.getIndexWriter()->mergePolicy->setMergeFactor(1000);
}
std::string values(int32_t row) {
  return "\"row_i\":" + std::to_string(row) + R"(,"text_w":"alpha beta alpha gamma","set_ss":["b","a","b"],"array_s":["b","a","b"],"vals_is":[3,1,3],"raw_sc":"bytes\u0000)"
      + std::to_string(row) + R"(","geo_p":[12.5,45.5],"vec_v":[0.25,1],"stored_s":"stored-)"
      + std::to_string(row) + "\",\"n_i\":" + std::to_string(row);
}
std::string document(int32_t row, bool outer, bool inner) {
  auto out = "{\"id\":\"r" + std::to_string(row) + "\"," + values(row);
  if (outer) {
    out += ",\"items_nested\":[{}, {" + values(row + 10);
    if (inner) out += ",\"parts_nested\":[{}, {" + values(row + 20) + "}]";
    out += "}]";
  }
  return out + '}';
}
std::vector<Doc> result(CollectionHelper& helper, std::string_view path, std::string_view query, bool geo = false) {
  auto req = localReq(helper.getSearchEngine());
  auto& op = req->collection(helper.collection().getId().name).topDocs().getScores().countLimit().limit(-1).fields({"row_i", "text_w", "set_ss", "array_s", "vals_is",
      "raw_sc", "geo_p", "vec_v", "stored_s", "n_i"});
  if (geo) op.rawQuery() = qb::geoBox(req->mr, "geo_p", 40, 50, 10, 20);
  else op.exprQuery(query);
  if (!path.empty()) op.rawOp().domain.emplace().to = path;
  req->execute(1); EXPECT_OK(req);
  auto docs = req->getDocs();
  if (auto* column = req->docList()->columns.find("_score_")) {
    auto scores = std::get<api::ColFloat>(column->kind).v;
    EXPECT_EQ(docs.size(), scores.size());
    for (size_t i = 0; i < docs.size(); i++) docs[i].push_back({"score", scores[i]});
  }
  auto key = [](const Doc& doc) { auto* row = find(doc, "row_i"); return row ? std::get<int64_t>(*row) : -1; };
  std::sort(docs.begin(), docs.end(), [&](const auto& a, const auto& b) { return key(a) < key(b); });
  EXPECT_EQ((int64_t)docs.size(), req->getMatchCount());
  return docs;
}

TEST(NestedMergeTest, largestSpaceSizesLevelsAndSurvivingOutput) {
  CollectionHelper helper("nested_levels");
  auto writer = helper.getIndexWriter();
  writer->mergePolicy->setMergeFactor(10);
  auto rows = [](int32_t count) {
    std::string out = "[";
    for (int32_t i = 0; i < count; i++) out += i ? ",{}" : "{}";
    return out + ']';
  };
  auto input = [&](std::initializer_list<std::string> docs) {
    CollectionHelper::UpdateBuilder batch;
    for (const auto& doc : docs) add(batch, doc);
    batch.commit(); ASSERT_TRUE(helper.submit(batch).success);
  };
  input({"{\"id\":\"dead\",\"left_nested\":" + rows(100) + ",\"right_nested\":[{}]}",
         "{\"id\":\"a\",\"left_nested\":[{}],\"right_nested\":" + rows(30) + "}"});
  input({"{\"id\":\"b\",\"right_nested\":" + rows(20) + "}"});
  input({R"({"id":"c"})"});
  auto sizes = [&] {
    std::vector<int32_t> counts;
    auto info = readDurableIndexInfo(writer->dir);
    auto stats = writer->stats(true, false);
    for (const auto& seg : info.info.segments) {
      auto count = seg.merge_rows.value_or(seg.max_doc);
      counts.push_back(count);
      auto found = std::ranges::find(stats.segmentStats, seg.seg_id, &IndexWriter::SegmentStats::segId);
      EXPECT_NE(stats.segmentStats.end(), found);
      if (found != stats.segmentStats.end()) {
        EXPECT_EQ(writer->mergePolicy->_mergeLevelForRows(count), found->mergeLevel);
      }
    }
    std::sort(counts.begin(), counts.end());
    return counts;
  };
  EXPECT_EQ((std::vector<int32_t>{1, 20, 101}), sizes());
  ASSERT_TRUE(helper.deleteById("dead", UpdateMessage::COMMIT).success);
  merge(helper, 2);
  EXPECT_EQ((std::vector<int32_t>{20, 101}), sizes());
  merge(helper);
  EXPECT_EQ((std::vector<int32_t>{50}), sizes());
  auto reader = writer->snapshots.readers.getReader();
  ASSERT_EQ(1u, reader->segments().size());
  EXPECT_EQ(3, reader->maxDoc());
  EXPECT_EQ(1, reader->view("left_nested")->maxDoc());
  EXPECT_EQ(50, reader->view("right_nested")->maxDoc());
}

TEST(NestedMergeTest, allRepresentationsAndOwnershipMatchFreshSurvivors) {
  CollectionHelper merged("nested_matrix"), fresh("nested_fresh");
  schema(merged); schema(fresh);
  std::vector<std::string> keep;
  for (int32_t source = 0; source < 4; source++) {
    CollectionHelper::UpdateBuilder input;
    if (source == 2) {
      SchemaBuilder later;
      for (auto name : {"late_nested", "never_nested"}) later.field(name).type = api::FieldDef::FieldClass::NESTED;
      later.set(merged.collection()); later.set(fresh.collection());
    }
    auto doc = document(source, source < 2, source == 0);
    if (source == 3) doc.insert(doc.size() - 1, R"(,"late_nested":{})");
    add(input, doc); keep.push_back(doc);
    if (source == 0) {
      auto dead = document(99, true, true);
      dead.insert(dead.size() - 1, R"(,"dead_nested":{"parts_nested":{}})");
      add(input, dead);
    }
    input.commit(); ASSERT_TRUE(merged.submit(input).success);
  }
  ASSERT_TRUE(merged.deleteById("r99", UpdateMessage::COMMIT).success);
  CollectionHelper::UpdateBuilder failed;
  add(failed, R"({"id":"failed","items_nested":[{"text_w":"discard","parts_nested":{"text_w":"discard"}},1]})");
  auto retained = document(4, false, false); add(failed, retained); keep.push_back(retained);
  failed.commit(); auto rejected = merged.submit(failed);
  ASSERT_EQ(1u, rejected.errors.size()); EXPECT_EQ("failed", rejected.errors[0].id);
  CollectionHelper::UpdateBuilder input;
  for (const auto& doc : keep) add(input, doc);
  input.commit(); ASSERT_TRUE(fresh.submit(input).success);
  auto old = merged.getIndexWriter()->snapshots.readers.getReader();
  merge(merged);
  auto reader = merged.getIndexWriter()->snapshots.readers.getReader();
  auto reference = fresh.getIndexWriter()->snapshots.readers.getReader();
  ASSERT_EQ(1u, reader->segments().size());
  const char* paths[]{"", "items_nested", "items_nested.parts_nested", "late_nested"};
  for (auto path : paths) {
    SCOPED_TRACE(path);
    auto& actual = reader->segments()[0].segmentCore();
    auto& expected = reference->segments()[0].segmentCore();
    ASSERT_NE(nullptr, actual.spaceReader(path));
    EXPECT_EQ(expected.spaceReader(path)->maxDoc(), actual.spaceReader(path)->maxDoc());
    if (*path) {
      auto* shape = actual.ownership(path); auto* other = expected.ownership(path);
      ASSERT_EQ(other->parentCount(), shape->parentCount());
      EXPECT_TRUE(shape->hasReverseMap());
      EXPECT_TRUE(other->hasReverseMap());
      for (int32_t row = 0; row < shape->childCount(); row++) EXPECT_EQ(other->owner(row), shape->owner(row));
      for (int32_t p = 0; p < shape->parentCount(); p++) EXPECT_EQ(other->range(p), shape->range(p));
    }
    for (auto query : {"*:*", "text_w:\"alpha beta\"", "set_ss:a", "array_s:b", "n_i:[2 TO 40]",
                       "text_w:gamma AND NOT set_ss:z", "text_w:alpha OR set_ss:z"}) {
      SCOPED_TRACE(query);
      auto a = result(merged, path, query), b = result(fresh, path, query);
      ASSERT_EQ(b.size(), a.size());
      for (size_t i = 0; i < a.size(); i++) EXPECT_TRUE(docEquals(b[i], a[i])) << docToString(a[i]);
    }
    auto a = result(merged, path, "", true), b = result(fresh, path, "", true);
    ASSERT_EQ(b.size(), a.size());
    for (size_t i = 0; i < a.size(); i++) EXPECT_TRUE(docEquals(b[i], a[i]));
  }
  EXPECT_GT(old->view("items_nested")->maxDoc(), reader->view("items_nested")->maxDoc());
  EXPECT_EQ(reader->maxDoc(), reader->liveDocs());
  EXPECT_EQ(nullptr, reader->segments()[0].segmentCore().spaceReader("never_nested"));
  EXPECT_EQ(nullptr, reader->segments()[0].segmentCore().spaceReader("dead_nested"));
  EXPECT_EQ(nullptr, reader->segments()[0].segmentCore().spaceReader("dead_nested.parts_nested"));
}

TEST(NestedMergeTest, childPartitionsAndStoredCopyAndReaddShareOneBundle) {
  CollectionHelper helper("nested_mechanisms");
  auto writer = helper.getIndexWriter();
  writer->mergePolicy->setMergeFactor(1000);
  writer->termPartitionMinBytes = 1; writer->termPartitionMinRangeBytes = 1; writer->termPartitionMaxRanges = 4;
  for (int32_t source = 0; source < 2; source++) {
    CollectionHelper::UpdateBuilder input;
    for (int root = 0; root < (source ? 2 : 1); root++) {
      auto doc = "{\"id\":\"" + std::to_string(source) + std::to_string(root) + "\",\"items_nested\":[";
      for (int i = 0; i < (source ? 64 : 128); i++) {
        if (i) doc += ',';
        doc += "{\"text_w\":\"alpha beta token" + std::to_string(i) + "\"}";
      }
      add(input, doc + "]}");
    }
    input.commit(); ASSERT_TRUE(helper.submit(input).success);
  }
  ASSERT_TRUE(helper.deleteById("10", UpdateMessage::COMMIT).success);
  std::atomic<int> ranges = 0, copies = 0, readds = 0;
  Signal::listen("termRangeStreamsCheckedOut", [&](void*, void*, void*) -> void* { ranges++; return nullptr; });
  Signal::listen("storedFieldsMergeSource", [&](void*, void*, void* value) -> void* {
    (*(bool*)value ? copies : readds)++; return nullptr;
  });
  auto unlisten = scope_guard([] { Signal::unlisten("termRangeStreamsCheckedOut"); Signal::unlisten("storedFieldsMergeSource"); });
  merge(helper);
  EXPECT_GT(ranges.load(), 0); EXPECT_GT(copies.load(), 0); EXPECT_GT(readds.load(), 0);
  auto req = localReq(helper.getSearchEngine());
  req->collection(helper.collection().getId().name).topDocs().exprQuery("text_w:\"alpha beta\"").countLimit().limit(0).rawOp().domain.emplace().to = "items_nested";
  req->execute(1); ASSERT_OK(req); EXPECT_EQ(192, req->getMatchCount());
}

TEST(NestedMergeTest, physicalChildRowsPastTheLimitSkipTheMerge) {
  CollectionHelper helper("nested_capacity");
  helper.clear();
  auto writer = helper.getIndexWriter(); writer->mergePolicy->setMergeFactor(1000);
  auto& inv = writer->obtainInverter();
  for (auto id : {"dead", "empty"}) { inv.startDoc(); inv.getIndexHandler("id").index(inv, std::string_view(id)); }
  auto& output = inv.getPostingsWriter();
  int64_t cap = PostingsReader::MAX_SEGMENT_DOCS;
  auto child = output.addSpace("items_nested");
  output.finishSpace(child, (int32_t)cap, RowSpaceFixture::shape(output, std::vector<int64_t>{cap, cap}));
  writer->releaseInverter(inv, true); helper.commit();
  CollectionHelper::UpdateBuilder input;
  add(input, R"({"id":"live","items_nested":{}})"); input.commit(); ASSERT_TRUE(helper.submit(input).success);
  // The deleted root owns every row of the large space, but admission counts
  // physical rows, so the two segments stay apart.
  ASSERT_TRUE(helper.deleteById("dead", UpdateMessage::COMMIT).success);
  merge(helper);
  auto reader = writer->snapshots.readers.getReader();
  EXPECT_EQ(2u, reader->segments().size());
  EXPECT_EQ(2, reader->liveDocs());
  auto req = localReq(helper.getSearchEngine());
  req->collection(helper.collection().getId().name).topDocs().exprQuery("items_nested:*").countLimit().limit(0);
  req->execute(1); ASSERT_OK(req); EXPECT_EQ(1, req->getMatchCount());
}

// Runs the same root and element-scope reads over a collection before and
// after merging it to one segment.
struct ScopedReads {
  CollectionHelper& helper;
  int64_t count(std::string_view query, std::string_view path = "") {
    auto req = localReq(helper.getSearchEngine());
    auto& op = req->collection(helper.collection().getId().name).topDocs().exprQuery(query).countLimit().limit(0);
    if (!path.empty()) op.rawOp().domain.emplace().to = path;
    req->execute(1); EXPECT_OK(req);
    return req->getMatchCount();
  }
  std::vector<std::string> ids(std::string_view query) {
    auto req = localReq(helper.getSearchEngine());
    req->collection(helper.collection().getId().name).topDocs().exprQuery(query).fields({"id"}).limit(-1);
    req->execute(1); EXPECT_OK(req);
    std::vector<std::string> out;
    for (const auto& doc : req->getDocs()) out.push_back(std::get<std::string>(*find(doc, "id")));
    std::ranges::sort(out);
    return out;
  }
  int64_t scalar(std::string_view expr) {
    auto req = localReq(helper.getSearchEngine());
    req->collection(helper.collection().getId().name).expr("e", expr);
    req->execute(1); EXPECT_OK(req);
    return req->scalar<int64_t>("e");
  }
};

// A declared multi-valued int column (the _is suffix) where every document
// holds one value reads as single-valued. Merged with a segment holding
// arrays, the column is multi-valued again, and every read stays exact.
TEST(NestedMergeTest, oneValueEachIntColumnsStayExactThroughMerges) {
  CollectionHelper helper("one_value_each");
  helper.getIndexWriter()->mergePolicy->setMergeFactor(1000);
  for (auto batch : {std::vector<std::string_view>{
           R"({"id":"a","tag_s":"x","n_is":5,"items_nested":[{"n_is":5},{"n_is":[7]}]})",
           R"({"id":"b","tag_s":"x","items_nested":[{}]})"},
       std::vector<std::string_view>{
           R"({"id":"c","tag_s":"y","n_is":[1,9],"items_nested":[{"n_is":[1,9]}]})",
           R"({"id":"d","tag_s":"x","n_is":6,"items_nested":[{"n_is":6}]})"}}) {
    CollectionHelper::UpdateBuilder input;
    for (auto doc : batch) add(input, doc);
    input.commit(); ASSERT_TRUE(helper.submit(input).success);
  }
  // Per segment, whether the root and the element n_is columns read multi-valued.
  auto multiValued = [&] {
    std::vector<bool> out;
    for (auto path : {"", "items_nested"}) {
      auto root = helper.getIndexWriter()->snapshots.readers.getReader();
      auto reader = *path ? root->view(path) : root;
      auto infos = reader->field(*path ? "items_nested.n_is" : "n_is");
      for (const auto& segment : reader->segments()) {
        auto* info = infos.at(segment);
        if (info == nullptr) { ADD_FAILURE() << "no n_is column in " << path; continue; }
        IntColReader column(segment.postingsReader(), *info);
        out.push_back(column.multiValued());
        EXPECT_NE(nullptr, column.getEndValueRankReader());
      }
    }
    return out;
  };
  EXPECT_EQ((std::vector<bool>{false, true, false, true}), multiValued());
  ScopedReads reads{helper};
  auto check = [&] {
    EXPECT_EQ((std::vector<std::string>{"a", "d"}), reads.ids("n_is:[5 TO 8]"));
    EXPECT_EQ((std::vector<std::string>{"c"}), reads.ids("n_is:[* TO 1]"));
    EXPECT_EQ((std::vector<std::string>{"a", "d"}), reads.ids("tag_s:x AND items_nested.n_is:[5 TO 8]"));
    EXPECT_EQ(2, reads.count("tag_s:x AND items_nested.n_is:[5 TO 8]"));
    EXPECT_EQ(3, reads.count("n_is:[5 TO 8]", "items_nested"));
    EXPECT_EQ(22, reads.scalar("sum(max(items_nested.n_is))"));
    EXPECT_EQ(5, reads.scalar("sum(count(items_nested.n_is))"));
  };
  check();
  merge(helper);
  EXPECT_EQ((std::vector<bool>{true, true}), multiValued());
  check();
}

// Segments without elements for a path serve element-scope counts,
// reductions, joins and projections, alone and merged with one that has them.
TEST(NestedMergeTest, segmentsWithoutElementsServeEveryScopedOperation) {
  CollectionHelper helper("elementless_segments");
  helper.getIndexWriter()->mergePolicy->setMergeFactor(1000);
  for (auto doc : {R"({"id":"E","title_s":"cap"})",
                   R"({"id":"A","title_s":"shirt","variants_nested":[{"color_s":"red","price_i":10},{"color_s":"blue","price_i":12}]})"}) {
    CollectionHelper::UpdateBuilder input;
    add(input, doc);
    input.commit(); ASSERT_TRUE(helper.submit(input).success);
  }
  ASSERT_EQ(2u, helper.getIndexWriter()->snapshots.readers.getReader()->segments().size());
  ScopedReads reads{helper};
  auto check = [&] {
    EXPECT_EQ(2, reads.count("*:*", "variants_nested"));
    EXPECT_EQ(1, reads.count("color_s:blue", "variants_nested"));
    EXPECT_EQ((std::vector<std::string>{"A"}), reads.ids("variants_nested.color_s:red"));
    EXPECT_EQ((std::vector<std::string>{"E"}), reads.ids("NOT variants_nested.color_s:*"));
    EXPECT_EQ(22, reads.scalar("sum(sum(variants_nested.price_i))"));
    auto req = localReq(helper.getSearchEngine());
    auto& rows = req->collection("elementless_segments").topDocs().allQuery().countLimit().limit(0);
    rows.rawOp().domain.emplace().to = "variants_nested";
    rows.topDocs("owners").allQuery().fields({"id"}).countLimit().limit(-1).rawOp().domain.emplace().to = "..";
    req->execute(1); ASSERT_OK(req);
    EXPECT_EQ(2, *req->docList()->found);
    auto* owners = (*req->docList()->ops.find("owners"))->docList();
    ASSERT_NE(nullptr, owners);
    EXPECT_EQ(1, *owners->found);
    EXPECT_EQ("A", std::get<api::ColStr>(owners->columns.at("id").kind).v.at(0));
    auto sorted = localReq(helper.getSearchEngine());
    sorted->collection("elementless_segments").topDocs().getScores().fields({"id"}).limit(-1).rawQuery() =
        qb::rescore(sorted->mr, qb::all(), "def(max(variants_nested.price_i),0)");
    sorted->execute(1); ASSERT_OK(sorted);
    ASSERT_EQ(2u, sorted->getDocs().size());
    EXPECT_EQ("A", std::get<std::string>(*find(sorted->getDocs()[0], "id")));
    EXPECT_EQ(12, std::get<api::ColFloat>(sorted->docList()->columns.at("_score_").kind).v.at(0));
  };
  check();
  merge(helper);
  ASSERT_EQ(1u, helper.getIndexWriter()->snapshots.readers.getReader()->segments().size());
  check();
}

} // namespace
} // namespace luxir::test
