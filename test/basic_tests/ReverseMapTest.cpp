// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include "test/CollectionHelper.h"
#include "test/LocalReq.h"
#include "test/LuxirTest.h"
#include "test/QueryBuild.h"
#include "test/SchemaBuilder.h"
#include "luxir/reader/OwnershipMapper.h"
#include "luxir/search/AncestorRows.h"
#include "luxir/server/Stats.h"

namespace luxir::test {
namespace {
class ReverseMapTest : public LuxirTest {
protected:
  CollectionHelper helper;

  void setting(bool outer, bool inner) {
    SchemaBuilder schema;
    auto& first = schema.templ("_nested"); first.type = api::FieldDef::FieldClass::NESTED; first.reverse_map = outer;
    auto& second = schema.field("items_nested.parts_nested"); second.type = api::FieldDef::FieldClass::NESTED; second.reverse_map = inner;
    schema.set(helper.collection());
  }

  void index(std::initializer_list<const char*> docs) {
    CollectionHelper::UpdateBuilder input;
    for (auto json : docs) input.addRaw([&](auto& doc, auto& mr) { ASSERT_TRUE(api::read_json(doc,json,mr)); });
    input.commit(); ASSERT_TRUE(helper.submit(input).success);
  }

  void check(const SegmentCore& core, bool outer, bool inner) {
    for (auto path : {"items_nested","items_nested.parts_nested"}) {
      const auto& shape = *core.ownership(path);
      EXPECT_EQ(path == std::string_view("items_nested") ? outer : inner, shape.hasReverseMap());
      EXPECT_EQ(shape.hasReverseMap(), shape.ownersBytes() > 0);
      EXPECT_GT(shape.endsBytes(),0);
      for (int owner=0; owner<shape.parentCount(); owner++) {
        auto [first,end] = shape.range(owner);
        for (int row=first; row<end; row++) EXPECT_EQ(owner,shape.owner(row));
      }
    }
  }
};

TEST_F(ReverseMapTest, mixedSourcesAreRewrittenUsingTheCurrentSettingAtEveryLevel) {
  for (bool output : {false,true}) {
    helper.clear(); helper.getIndexWriter()->mergePolicy->setMergeFactor(1000);
    setting(true,false);
    index({R"({"id":"dead","items_nested":[{},{}]})",
        R"({"id":"a","items_nested":{"n_i":1,"parts_nested":[{},{}]}})"});
    setting(false,true);
    index({R"({"id":"b","items_nested":[{"n_i":2,"parts_nested":[{},{}]},{}]})"});
    auto before = helper.getIndexWriter()->snapshots.readers.getReader();
    ASSERT_EQ(2,before->segments().size());
    check(before->segments()[0].segmentCore(),true,false);
    check(before->segments()[1].segmentCore(),false,true);
    ASSERT_TRUE(helper.deleteById("dead",UpdateMessage::COMMIT).success);
    setting(output,output);
    CollectionHelper::UpdateBuilder merge; merge.commit(true,1); ASSERT_TRUE(helper.submit(merge).success);
    auto after = helper.getIndexWriter()->snapshots.readers.getReader();
    ASSERT_EQ(1,after->segments().size()); check(after->segments()[0].segmentCore(),output,output);
    EXPECT_EQ(3,after->view("items_nested")->maxDoc());
    EXPECT_EQ(4,after->view("items_nested.parts_nested")->maxDoc());
    auto req = localReq(helper.getSearchEngine());
    req->topDocs().exprQuery("items_nested:*").fields({"id","items_nested"}).countLimit();
    req->execute(1); ASSERT_OK(req); EXPECT_EQ(2,req->getMatchCount());
    auto writer = helper.getIndexWriter()->stats(true,false);
    auto reader = helper.getIndexWriter()->snapshots.readers.stats(true,false);
    ASSERT_EQ(1,writer.segmentStats.size()); ASSERT_EQ(1,reader.segmentStats.size());
    ASSERT_EQ(2,writer.segmentStats[0].spaces.size());
    for (size_t i=0; i<2; i++) {
      const auto& space = writer.segmentStats[0].spaces[i];
      EXPECT_EQ(space.path,reader.segmentStats[0].spaces[i].path);
      EXPECT_EQ(space.ownersBytes,reader.segmentStats[0].spaces[i].ownersBytes);
      EXPECT_EQ(output,space.ownersBytes>0);
    }
    check(before->segments()[0].segmentCore(),true,false);
  }
}

TEST_F(ReverseMapTest, cursorsStepToTheNextFamilyAndReadOwnersForAnyOtherLookup) {
  index({R"({"id":"a","items_nested":[{"parts_nested":[{},{}]},{}]})",
      R"({"id":"b","items_nested":{"parts_nested":[{},{}]}})",
      R"({"id":"c","items_nested":[{"parts_nested":[{},{},{}]},{"parts_nested":[{}]}]})"});
  auto root = helper.getIndexWriter()->snapshots.readers.getReader();
  auto many = root->view("items_nested.parts_nested");
  auto& core = root->segments()[0].segmentCore();
  OwnershipMapper mapper(core,"","items_nested.parts_nested");
  SkipStatsScope stats;
  EXPECT_EQ(1,mapper.owner(3)); EXPECT_EQ(0,mapper.owner(0));
  EXPECT_EQ(4,SkipStats::ownershipReverseReads);
  MemPool pool; OwnershipMapper::Cursor cursor(mapper,pool);
  EXPECT_EQ(0,cursor.owner(0).owner);
  auto reads = SkipStats::ownershipReverseReads;
  for (int row=0; row<4; row++) EXPECT_EQ(row/2,cursor.owner(row).owner);
  EXPECT_EQ(reads,SkipStats::ownershipReverseReads);
  EXPECT_EQ(0,cursor.owner(0).owner); EXPECT_GT(SkipStats::ownershipReverseReads,reads);
  reads = SkipStats::ownershipReverseReads;
  EXPECT_EQ(2,cursor.owner(7).owner); EXPECT_EQ(2,SkipStats::ownershipReverseReads-reads);
  AncestorRows ancestors(*many,*root);
  segdoc input[]{segdoc(0,3),segdoc(0,3)}, output[2]; uint32_t indices[]{0,1};
  reads = SkipStats::ownershipReverseReads;
  ancestors.map(input,indices,output);
  EXPECT_EQ(2,SkipStats::ownershipReverseReads-reads);
  EXPECT_EQ(1,output[0].docId()); EXPECT_EQ(output[0],output[1]);
}

TEST_F(ReverseMapTest, segmentStatsExposeEncodedOwnershipBytesWithoutReadingRows) {
  LuxirNode node; CollectionHelper data(node);
  CollectionHelper::UpdateBuilder input;
  for (int root=0; root<2048; root++) {
    std::string json = "{\"id\":\""+std::to_string(root)+"\",\"items_nested\":[";
    for (int row=0; row<root%7+1; row++) json += row ? ",{}" : "{}";
    json += "]}";
    input.addRaw([&](auto& doc,auto& mr) { ASSERT_TRUE(api::read_json(doc,json,mr)); });
  }
  input.commit(); ASSERT_TRUE(data.submit(input).success);
  std::pmr::monotonic_buffer_resource mr;
  api::StatsRequest request; request.collection="main"; request.segments=true;
  api::StatsResponse response; gatherStats(node,request,response,mr);
  ASSERT_EQ(1,response.collections.size());
  auto segments = response.collections[0].shards[0].index.segments;
  ASSERT_EQ(1,segments.size()); ASSERT_EQ(1,segments[0].spaces.size());
  const auto& space = segments[0].spaces[0];
  auto reader = data.getIndexWriter()->snapshots.readers.getReader();
  const auto& shape = *reader->segments()[0].segmentCore().ownership("items_nested");
  EXPECT_EQ(shape.childCount(),space.max_doc);
  EXPECT_EQ(shape.endsBytes(),space.ends_bytes); EXPECT_EQ(shape.ownersBytes(),space.owners_bytes);
  EXPECT_GT(space.owners_bytes,0);
}
} // namespace
} // namespace luxir::test
