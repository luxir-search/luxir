// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include "test/CollectionHelper.h"
#include "test/LocalReq.h"
#include "test/LuxirTest.h"
#include "test/QueryBuild.h"
#include "test/SchemaBuilder.h"
#include "test/KnnTestUtils.h"
#include "luxir/index/NestedInversion.h"
#include "luxir/reader/VectorAuxReader.h"
#include "luxir/reader/OwnershipShape.h"
#include "luxir/util/Uuid.h"

namespace luxir::test {
namespace {

void addJson(CollectionHelper::UpdateBuilder& input, std::string_view json) {
  input.addRaw([&](auto& doc, auto& arena) { ASSERT_TRUE(api::read_json(doc, json, arena)); });
}

IndexResult ingest(CollectionHelper& helper, std::initializer_list<std::string_view> docs, bool commit = true) {
  CollectionHelper::UpdateBuilder input;
  for (auto doc : docs) addJson(input, doc);
  input.overwrite();
  if (commit) input.commit();
  return helper.submit(input);
}

std::shared_ptr<IndexReader> reader(CollectionHelper& helper) {
  return helper.getIndexWriter()->snapshots.readers.getReader();
}

std::vector<Doc> joined(CollectionHelper& helper, std::string_view value) {
  auto req = localReq(helper.getSearchEngine());
  auto& op = req->topDocs().countLimit().fields({"id"});
  op.rawQuery() = qb::toParent(req->mr, "items_nested", qb::match(req->mr, "label_s", value), "none");
  req->execute();
  EXPECT_OK(req);
  return req->getDocs();
}

class NestedIngestTest : public LuxirTest {};

TEST_F(NestedIngestTest, shapesDepthsAndLocalStoredValuesUseOrdinaryViews) {
  CollectionHelper helper;
  ASSERT_TRUE(ingest(helper, {
      R"({"id":"a"})", R"({"id":"b","items_nested":null})", R"({"id":"c","items_nested":[]})",
      R"({"id":"d","items_nested":{}})",
      R"({"id":"e","items_nested":{"label_s":"Red","deep_nested":[{},{"text_w":"deep phrase"}]}})",
      R"({"id":"f","items_nested":[{"label_s":"Blue"},{"label_s":"Green","deep_nested":{}}]})"}).success);
  auto root = reader(helper);
  auto child = root->view("items_nested");
  EXPECT_EQ(6, root->maxDoc()); EXPECT_EQ(4, child->maxDoc());
  EXPECT_EQ(3, root->view("items_nested.deep_nested")->maxDoc());
  EXPECT_TRUE(containsDoc(joined(helper, "Red"), flatdoc("id", "e")));
  auto req = localReq(helper.getSearchEngine());
  api::Query phrase;
  auto& phraseBody = phrase.kind.emplace<api::PhraseQuery>();
  phraseBody.field = "text_w";
  phraseBody.text = "deep phrase";
  auto inner = qb::toParent(req->mr, "deep_nested", phrase, "none");
  req->topDocs().countLimit().fields({"id"}).rawQuery() = qb::toParent(req->mr, "items_nested", inner, "none");
  auto& rows = req->topDocs("rows").allQuery().countLimit().fields({"*"});
  rows.rawOp().domain.emplace().to = "items_nested";
  req->execute();
  ASSERT_OK(req);
  EXPECT_TRUE(containsDoc(req->getDocs(), flatdoc("id", "e")));
  EXPECT_EQ(4, req->getMatchCount("rows"));
  auto docs = req->getDocs("rows");
  ASSERT_EQ(4u, docs.size());
  EXPECT_TRUE(docEquals(Doc{}, docs[0]));
  EXPECT_TRUE(containsDoc(docs, flatdoc("label_s", "Red")));
}

TEST_F(NestedIngestTest, emptyPathsAreOmittedAndMappingStopsAtTheRoot) {
  CollectionHelper helper;
  CollectionHelper::UpdateBuilder input;
  input.fieldMap({{"key", "id"}, {"external", "items_nested"}}).dropUnmapped().commit();
  addJson(input, R"({"key":"a","external":[{"label_s":"kept"}],"unknown":"dropped"})");
  addJson(input, R"({"key":"b","external":[]})");
  ASSERT_TRUE(helper.submit(input).success);
  EXPECT_EQ(1, reader(helper)->view("items_nested")->maxDoc());
  EXPECT_EQ(1u, joined(helper, "kept").size());
  ASSERT_TRUE(ingest(helper, {R"({"id":"c","empty_nested":null})", R"({"id":"d","empty_nested":[]})"}).success);
  for (const auto& seg : reader(helper)->segments()) EXPECT_EQ(nullptr, seg.segmentCore().spaceReader("empty_nested"));
}

TEST_F(NestedIngestTest, failedReplacementRetainsOldFamilyForBufferedAndFlushedRoots) {
  for (bool flushed : {false, true}) {
    CollectionHelper helper;
    ASSERT_TRUE(ingest(helper, {R"({"id":"x","items_nested":{"label_s":"old"}})"}, flushed).success);
    auto bad = ingest(helper, {R"({"id":"x","items_nested":[{"label_s":"prefix"},{"number_i":"bad"}]})"});
    EXPECT_FALSE(bad.success);
    ASSERT_EQ(1u, bad.errors.size());
    EXPECT_NE(std::string::npos, bad.errors[0].error_message.find("items_nested.number_i"));
    EXPECT_EQ(1u, joined(helper, "old").size());
    EXPECT_TRUE(joined(helper, "prefix").empty());
    ASSERT_TRUE(ingest(helper, {R"({"id":"x","items_nested":{"label_s":"new"}})"}).success);
    EXPECT_TRUE(joined(helper, "old").empty());
    EXPECT_EQ(1u, joined(helper, "new").size());
    ASSERT_TRUE(helper.deleteById("x", UpdateMessage::COMMIT).success);
    EXPECT_TRUE(joined(helper, "new").empty());
    helper.clear();
  }
}

TEST_F(NestedIngestTest, bufferedDeletesAndLateRootFailuresHideTheirChildRows) {
  CollectionHelper helper;
  ASSERT_TRUE(ingest(helper, {R"({"id":"a","items_nested":{"label_s":"deleted"}})"}, false).success);
  ASSERT_TRUE(helper.deleteById("a", UpdateMessage::COMMIT).success);
  EXPECT_TRUE(joined(helper, "deleted").empty());
  auto result = ingest(helper, {R"({"id":"b","items_nested":{"label_s":"failed"},"number_i":"bad"})",
                                R"({"id":"c","items_nested":{"label_s":"kept"}})"});
  EXPECT_EQ(IndexResult::Status::PARTIAL, result.status);
  EXPECT_TRUE(joined(helper, "failed").empty());
  EXPECT_EQ(1u, joined(helper, "kept").size());
}

TEST_F(NestedIngestTest, duplicateIdsAndAllOrNoneKeepRootUndoAndOwnership) {
  CollectionHelper helper;
  ASSERT_TRUE(ingest(helper, {R"({"id":"x","items_nested":{"label_s":"first"}})",
                             R"({"id":"x","items_nested":{"label_s":"last"}})"}, false).success);
  CollectionHelper::UpdateBuilder input;
  input.remove("x").overwrite().allOrNone().returnIds().commit();
  addJson(input, R"({"id":"x","items_nested":{"label_s":"replacement"}})");
  addJson(input, R"({"id":"bad","items_nested":[{}, {"unknown":"bad"}]})");
  auto result = helper.submit(input);
  EXPECT_FALSE(result.success); EXPECT_TRUE(result.ids.empty());
  EXPECT_TRUE(joined(helper, "first").empty());
  EXPECT_TRUE(joined(helper, "replacement").empty());
  EXPECT_EQ(1u, joined(helper, "last").size());
  EXPECT_EQ(1, reader(helper)->view("items_nested")->liveDocs());
}

TEST_F(NestedIngestTest, malformedChildrenRestoreCursorsAndDoNotConsumeDroppedFamilies) {
  CollectionHelper helper;
  auto result = ingest(helper, {
      R"({"id":"a","items_nested":[{}, 1]})", R"({"id":"b","items_nested":{"bad.key":1}})",
      R"({"id":"c","items_nested":{"deep_nested":[{},false]}})",
      R"({"id":"d","items_nested":[{"label_s":"discard"}],"items_nested":{"label_s":"good"}})"});
  EXPECT_EQ(IndexResult::Status::PARTIAL, result.status);
  ASSERT_EQ(3u, result.errors.size());
  EXPECT_EQ(3, result.errors[2].index + 1);
  EXPECT_TRUE(joined(helper, "discard").empty());
  EXPECT_EQ(1u, joined(helper, "good").size());
  EXPECT_EQ(1, reader(helper)->view("items_nested")->liveDocs());
  EXPECT_EQ(0, reader(helper)->view("items_nested.deep_nested")->liveDocs());
}

TEST_F(NestedIngestTest, cachedChildHandlersCannotBeUsedByDottedRootKeys) {
  CollectionHelper helper;
  auto result = ingest(helper, {
      R"({"id":"a","items_nested":{"label_s":"good"}})",
      R"({"id":"b","items_nested.label_s":"bad"})"});
  EXPECT_EQ(IndexResult::Status::PARTIAL, result.status);
  ASSERT_EQ(1u, result.errors.size());
  EXPECT_EQ(1u, joined(helper, "good").size());
  EXPECT_TRUE(joined(helper, "bad").empty());
}

TEST_F(NestedIngestTest, childRowsTriggerFlushWithoutManyRoots) {
  CollectionHelper helper;
  auto writer = helper.getIndexWriter();
  SearchOverridesGuard cap(writer->perInverterMaxDocs);
  writer->perInverterMaxDocs = 2;
  ASSERT_TRUE(ingest(helper, {R"({"id":"a","items_nested":[{},{},{}]})"}, false).success);
  writer->updateGraph->wait_for_all();
  EXPECT_EQ(1, writer->segInfos.size());
  helper.commit();
  EXPECT_EQ(1, reader(helper)->maxDoc());
  EXPECT_EQ(3, reader(helper)->view("items_nested")->maxDoc());
}

TEST_F(NestedIngestTest, sparseFamiliesCoverEarlierAndMissingParentsAcrossBatches) {
  CollectionHelper helper;
  ASSERT_TRUE(ingest(helper, {R"({"id":"a","items_nested":[{}]})"}, false).success);
  ASSERT_TRUE(ingest(helper, {R"({"id":"b"})"}, false).success);
  ASSERT_TRUE(ingest(helper, {R"({"id":"c","items_nested":[{},{"deep_nested":{"number_i":7}}]})"}, false).success);
  ASSERT_TRUE(ingest(helper, {R"({"id":"d","items_nested":[{}]})"}).success);
  auto root = reader(helper);
  ASSERT_EQ(1u, root->segments().size());
  auto& core = root->segments()[0].segmentCore();
  auto* shape = core.ownership("items_nested.deep_nested");
  ASSERT_NE(nullptr, shape);
  EXPECT_EQ(4, shape->parentCount());
  const int32_t ends[]{0, 0, 1, 1};
  for (int32_t row = 0; row < 4; row++) EXPECT_EQ(ends[row], shape->range(row).second);
  auto req = localReq(helper.getSearchEngine());
  req->topDocs().allQuery().countLimit().fields({"number_i"});
  req->execute(root->view("items_nested.deep_nested"));
  ASSERT_OK(req);
  EXPECT_TRUE(containsDoc(req->getDocs(), flatdoc("number_i", (int64_t)7)));
}

TEST_F(NestedIngestTest, childStoredResourcesAreChargedOnceAtCreation) {
  for (auto codec : {api::StoredResourceDef::Codec::LZ4, api::StoredResourceDef::Codec::ZSTD}) {
    CollectionHelper helper;
    SchemaBuilder schema;
    schema.storedResource("_stored_").codec = codec;
    schema.set(helper.collection());
    ASSERT_TRUE(ingest(helper, {R"({"id":"a","items_nested":{}})"}, false).success);
    auto writer = helper.getIndexWriter();
    auto& inverter = writer->obtainInverter();
    auto* config = dynamic_cast<const StoredFieldType*>(inverter.schema->getFieldTypePtr("_stored_"));
    ASSERT_NE(nullptr, config);
    auto before = inverter.extraRamBytes;
    inverter.getOrCreateStoredFields(SpaceOrdinal::ROOT, "_unused_", config);
    EXPECT_EQ(before, inverter.extraRamBytes);
    auto space = (SpaceOrdinal)1; // The first child space is items_nested.
    auto& stored = inverter.getOrCreateStoredFields(space, "_stored_", config);
    auto charged = inverter.extraRamBytes;
    EXPECT_GE(charged - before, (size_t)config->format_.chunkTarget
        + StoredChunkCompressor::contextBytes(config->format_, config->format_.chunkTarget));
    EXPECT_EQ(&stored, &inverter.getOrCreateStoredFields(space, "_stored_", config));
    EXPECT_EQ(charged, inverter.extraRamBytes);
    stored.addValue(0, "items_nested.payload_w", "word");
    auto bytes = inverter.memSize();
    EXPECT_FALSE(inverter.shouldFlush(SIZE_MAX, SIZE_MAX));
    EXPECT_EQ(bytes, inverter.memSize());
    writer->releaseInverter(inverter, false);
    auto& reused = writer->obtainInverter();
    EXPECT_EQ(&inverter, &reused);
    EXPECT_EQ(charged, reused.extraRamBytes);
    EXPECT_EQ(bytes, reused.memSize());
    writer->releaseInverter(reused, false);
    helper.commit();
    helper.clear();
  }
}

TEST_F(NestedIngestTest, rowBoundArithmeticCannotOverflow) {
  auto hard = (uint64_t)PostingsReader::HARD_MAX_DOC;
  EXPECT_TRUE(Inverter::rowsFit(hard - 2, 1));
  EXPECT_FALSE(Inverter::rowsFit(hard - 1, 1));
  EXPECT_FALSE(Inverter::rowsFit(hard, 0));
  EXPECT_FALSE(Inverter::rowsFit(1, UINT64_MAX));
  EXPECT_TRUE(Inverter::rowsFit(PostingsReader::MAX_SEGMENT_DOCS, 1));
}

TEST_F(NestedIngestTest, mergesCompactNestedAndRootOnlyBundles) {
  CollectionHelper helper;
  auto writer = helper.getIndexWriter();
  writer->mergePolicy->setMergeFactor(2);
  ASSERT_TRUE(ingest(helper, {R"({"id":"nested","items_nested":[{},{}]})"}).success);
  ASSERT_TRUE(ingest(helper, {R"({"id":"a"})"}).success);
  ASSERT_TRUE(ingest(helper, {R"({"id":"b"})"}).success);
  writer->updateGraph->wait_for_all();
  helper.commit();
  EXPECT_EQ(2u, helper.durableSegmentCount());
  CollectionHelper::UpdateBuilder force;
  force.commit(true, 1);
  auto result = helper.submit(force);
  EXPECT_TRUE(result.success);
  EXPECT_EQ(1u, helper.durableSegmentCount());
  EXPECT_EQ(2, reader(helper)->view("items_nested")->maxDoc());
}

TEST_F(NestedIngestTest, nestedDepthIsBoundedIndependentlyOfTheInputParser) {
  CollectionHelper helper;
  SchemaBuilder schema;
  std::string path;
  std::string json = R"({"id":"deep",)";
  for (uint32_t depth = 0; depth <= NestedState::MAX_DEPTH; depth++) {
    if (!path.empty()) path += ".";
    path += "a";
    schema.field(path).type = api::FieldDef::FieldClass::NESTED;
    json += R"("a":{)";
  }
  json += "}";
  json.append(NestedState::MAX_DEPTH + 1, '}');
  schema.set(helper.collection());
  CollectionHelper::UpdateBuilder input;
  addJson(input, json);
  input.commit();
  auto result = helper.submit(input);
  EXPECT_FALSE(result.success);
  ASSERT_EQ(1u, result.errors.size());
  EXPECT_NE(std::string::npos, result.errors[0].error_message.find("depth"));
}

TEST_F(NestedIngestTest, childOverlayBuildsOnItsOwnReaderAndServesAJoinedKnn) {
  IvfTestGuard small;
  ForceIvfGuard force;
  CollectionHelper helper;
  SchemaBuilder schema;
  auto& vector = schema.field("items_nested.embedding_v");
  vector.type = api::FieldDef::FieldClass::VECTOR;
  vector.dims = 2;
  vector.metric = api::VectorMetric::L2;
  vector.ann = rabitqAnn(1);
  schema.set(helper.collection());
  ASSERT_TRUE(ingest(helper, {
      R"({"id":"a","items_nested":[{"embedding_v":[1,0]},{"embedding_v":[2,0]}]})",
      R"({"id":"b","items_nested":[{"embedding_v":[10,0]},{"embedding_v":[11,0]}]})"}).success);
  CollectionHelper::UpdateBuilder build;
  build.commitWithAux({"*"});
  ASSERT_TRUE(helper.submit(build).success);
  auto root = reader(helper);
  auto child = root->view("items_nested");
  auto aux = child->segments()[0].getAuxReader("vec.items_nested.embedding_v");
  ASSERT_NE(nullptr, dynamic_cast<const VectorAuxReader*>(aux.get()));
  ASSERT_TRUE(ingest(helper, {R"({"id":"c","items_nested":{"embedding_v":[20,0]}})"}).success);
  ASSERT_TRUE(helper.deleteById("b", UpdateMessage::COMMIT).success);
  auto builds = VectorIndexBuilder::ivfMergeBuildCountForTests.load();
  CollectionHelper::UpdateBuilder compact;
  compact.commit(true, 1); ASSERT_TRUE(helper.submit(compact).success);
  auto merged = reader(helper)->view("items_nested");
  ASSERT_EQ(1u, merged->segments().size()); EXPECT_EQ(3, merged->maxDoc());
  EXPECT_GT(VectorIndexBuilder::ivfMergeBuildCountForTests.load(), builds);
  EXPECT_NE(nullptr, merged->segments()[0].getAuxReader("vec.items_nested.embedding_v"));
  auto req = localReq(helper.getSearchEngine());
  auto knn = qb::knn(req->mr, "embedding_v", {1.0f, 0.0f}, 1, 1);
  req->topDocs().countLimit().fields({"id"}).rawQuery() = qb::toParent(req->mr, "items_nested", knn);
  req->execute();
  ASSERT_OK(req);
  auto docs = req->getDocs();
  ASSERT_EQ(1u, docs.size());
  EXPECT_TRUE(containsDoc(docs, flatdoc("id", "a")));
}

TEST_F(NestedIngestTest, reopenedBundlesKeepShapesThroughAutomaticMerges) {
  auto path = std::filesystem::temp_directory_path() / ("luxir-nested-" + newUuid());
  auto cleanup = scope_guard([&] { std::filesystem::remove_all(path); });
  LuxirConfig config;
  config.store.backend = "fs";
  config.store.data_dir = path;
  {
    LuxirNode node(config);
    CollectionHelper helper(node);
    ASSERT_TRUE(ingest(helper, {R"({"id":"old","items_nested":{"label_s":"old"}})"}).success);
  }
  LuxirNode node(config);
  CollectionHelper helper(node);
  auto writer = helper.getIndexWriter();
  writer->mergePolicy->setMergeFactor(2);
  EXPECT_EQ(1u, joined(helper, "old").size());
  ASSERT_TRUE(ingest(helper, {R"({"id":"a"})"}).success);
  ASSERT_TRUE(ingest(helper, {R"({"id":"b"})"}).success);
  writer->updateGraph->wait_for_all();
  helper.commit();
  EXPECT_EQ(2u, helper.durableSegmentCount());
  CollectionHelper::UpdateBuilder compact;
  compact.commit(true, 1); ASSERT_TRUE(helper.submit(compact).success);
  EXPECT_EQ(1u, helper.durableSegmentCount());
  EXPECT_EQ(1u, joined(helper, "old").size());
  ASSERT_TRUE(helper.deleteById("old", UpdateMessage::COMMIT).success);
  EXPECT_TRUE(joined(helper, "old").empty());
  writer->mergeSegments();
  EXPECT_EQ(nullptr, reader(helper)->segments()[0].segmentCore().spaceReader("items_nested"));
}

} // namespace
} // namespace luxir::test
