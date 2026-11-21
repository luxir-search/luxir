// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

// Collection merge limits (MergePolicyDef max_merged_segment_bytes,
// max_merged_segment_docs, deletes_pct_allowed), the merge working-set gate,
// tiered's cap-fill merges and the writer's delete-reclaim merges.

#include <gtest/gtest.h>

#include <algorithm>
#include <format>
#include <numeric>

#include "luxir/index/MergePolicyRegistry.h"
#include "test/CollectionHelper.h"
#include "test/LuxirTest.h"
#include "test/TestUtils.h"

using namespace luxir;
using namespace luxir::test;

namespace {

class MergeLimitsTest : public LuxirTest {
protected:
  // Sets the collection's merge_policy (a MergePolicyDef in JSON).
  static void setMergePolicy(CollectionHelper& helper, std::string_view json) {
    std::pmr::monotonic_buffer_resource arena;
    api::SchemaDef def;
    std::string text = R"({"merge_policy":)" + std::string(json) + "}";
    ASSERT_TRUE(api::read_json(def, text, arena)) << text;
    helper.collection().updateSchema(def, api::SchemaRequest_::Mode::SET);
  }

  // One segment of `docs` documents with ids prefix0..; waits for the merges
  // it starts.
  static void addSegment(CollectionHelper& helper, std::string_view prefix, int docs = 2) {
    CollectionHelper::UpdateBuilder request;
    for (int i = 0; i < docs; i++) request.add(flatdoc("id", std::format("{}{}", prefix, i), "tag_s", "t"));
    request.commit(true);
    ASSERT_TRUE(helper.submit(request).success);
  }

  static void addSegments(CollectionHelper& helper, std::string_view prefix, int segments, int docs = 2) {
    for (int s = 0; s < segments; s++) addSegment(helper, std::format("{}{}_", prefix, s), docs);
  }

  static void deleteIds(CollectionHelper& helper, std::initializer_list<std::string> ids) {
    ASSERT_TRUE(helper.deleteByIds(ids, UpdateMessage::COMMIT).success);
  }

  static void forceMergeToOne(CollectionHelper& helper) {
    CollectionHelper::UpdateBuilder update;
    update.commit(true, 1);
    ASSERT_TRUE(helper.submit(update).success);
  }

  static std::vector<IndexWriter::SegmentStats> segments(CollectionHelper& helper) {
    auto stats = helper.getIndexWriter()->stats(true).segmentStats;
    std::ranges::sort(stats, {}, &IndexWriter::SegmentStats::minUpdateVersion);
    return stats;
  }

  // One terminal segment of 8 docs (ids a0_0..a3_1) under a doc cap of 8.
  static void buildTerminalSegment(CollectionHelper& helper, std::string_view extra) {
    setMergePolicy(helper, R"({"params":{"merge_factor":2},"max_merged_segment_docs":8)" + std::string(extra) + "}");
    addSegments(helper, "a", 4);
  }

  // Live docs of each segment, ascending.
  static std::vector<int32_t> liveDocs(CollectionHelper& helper) {
    std::vector<int32_t> live;
    for (const auto& segment : segments(helper)) live.push_back(segment.liveDocs);
    std::ranges::sort(live);
    return live;
  }
};

}  // namespace

TEST_F(MergeLimitsTest, defaultsFollowCollectionKind) {
  auto generic = resolveMergeLimits(nullptr, false, false);
  EXPECT_EQ(MergeLimits::DEFAULT_MAX_BYTES, generic.maxMergedSegmentBytes);
  EXPECT_EQ(MergeLimits::DEFAULT_MAX_DOCS, generic.maxMergedSegmentDocs);
  EXPECT_EQ(MergeLimits::DEFAULT_DELETES_PCT_ALLOWED, generic.deletesPctAllowed);
  auto vectors = resolveMergeLimits(nullptr, false, true);
  EXPECT_EQ(MergeLimits::VECTOR_MAX_BYTES, vectors.maxMergedSegmentBytes);
  EXPECT_EQ(MergeLimits::VECTOR_MAX_DOCS, vectors.maxMergedSegmentDocs);
  auto remote = resolveMergeLimits(nullptr, true, true);
  EXPECT_EQ(MergeLimits::REMOTE_MAX_BYTES, remote.maxMergedSegmentBytes);
  EXPECT_EQ(MergeLimits::REMOTE_MAX_DOCS, remote.maxMergedSegmentDocs);
  // Explicit fields win, 0 included.
  api::MergePolicyDef def;
  def.max_merged_segment_bytes = 0;
  def.deletes_pct_allowed = 50;
  auto explicitLimits = resolveMergeLimits(&def, true, false);
  EXPECT_EQ(0u, explicitLimits.maxMergedSegmentBytes);
  EXPECT_EQ(MergeLimits::REMOTE_MAX_DOCS, explicitLimits.maxMergedSegmentDocs);
  EXPECT_EQ(50, explicitLimits.deletesPctAllowed);
}

TEST_F(MergeLimitsTest, invalidLimitsFailTheSchema) {
  CollectionHelper helper("merge_limits_invalid");
  std::pmr::monotonic_buffer_resource arena;
  for (std::string_view json : {R"({"deletes_pct_allowed":0})", R"({"max_merged_segment_docs":4294967296})"}) {
    api::SchemaDef def;
    std::string text = R"({"merge_policy":)" + std::string(json) + "}";
    ASSERT_TRUE(api::read_json(def, text, arena)) << text;
    EXPECT_ANY_THROW(helper.collection().updateSchema(def, api::SchemaRequest_::Mode::SET)) << json;
  }
}

// Background merges stop short of the doc cap and segments over half of it
// stop merging; a force merge overrides the cap.
TEST_F(MergeLimitsTest, naturalMergesRespectMaxMergedSegmentDocs) {
  CollectionHelper helper("merge_limits_docs");
  setMergePolicy(helper, R"({"params":{"merge_factor":2},"max_merged_segment_docs":6})");
  addSegments(helper, "a", 8);
  EXPECT_EQ((std::vector<int32_t>{4, 4, 4, 4}), liveDocs(helper));
  for (const auto& segment : segments(helper)) EXPECT_TRUE(segment.terminal);
  forceMergeToOne(helper);
  EXPECT_EQ(std::vector<int32_t>{16}, liveDocs(helper));
}

TEST_F(MergeLimitsTest, naturalMergesRespectMaxMergedSegmentBytes) {
  CollectionHelper helper("merge_limits_bytes");
  // Docs large enough that their terms, not per-segment overhead, size a segment.
  auto addBulky = [&](std::string_view prefix) {
    CollectionHelper::UpdateBuilder request;
    for (int i = 0; i < 20; i++) {
      std::string body;
      for (int w = 0; w < 300; w++) body += std::format("{}d{}w{} ", prefix, i, w);
      request.add(flatdoc("id", std::format("{}{}", prefix, i), "body_t", body));
    }
    request.commit(true);
    ASSERT_TRUE(helper.submit(request).success);
  };
  setMergePolicy(helper, R"({"params":{"merge_factor":2},"max_merged_segment_docs":0,"max_merged_segment_bytes":0})");
  addBulky("p");
  uint64_t cap = helper.getIndexWriter()->segInfos.begin()->second->mergeBytes() * 5 / 2;
  setMergePolicy(helper, std::format(R"({{"params":{{"merge_factor":2}},"max_merged_segment_docs":0,)"
                                     R"("max_merged_segment_bytes":{}}})", cap));
  for (int s = 0; s < 8; s++) addBulky(std::format("a{}_", s));
  auto iw = helper.getIndexWriter();
  EXPECT_GE(iw->segInfos.size(), 3u);
  for (const auto& [segId, seg] : iw->segInfos) EXPECT_LE(seg->mergeBytes(), cap) << segId;
  // Both caps 0: unlimited, so merging resumes.
  setMergePolicy(helper, R"({"params":{"merge_factor":2},"max_merged_segment_docs":0,"max_merged_segment_bytes":0})");
  for (int s = 0; s < 4; s++) addBulky(std::format("b{}_", s));
  uint64_t largest = 0;
  for (const auto& [segId, seg] : iw->segInfos) largest = std::max(largest, seg->mergeBytes());
  EXPECT_GT(largest, cap);
}

// A segment's deletes bring it back under half of the cap, and lower its
// tiered level, so it merges with smaller segments again.
TEST_F(MergeLimitsTest, mostlyDeletedLargeSegmentRejoinsMerges) {
  CollectionHelper helper("merge_limits_rejoin");
  setMergePolicy(helper, R"({"params":{"merge_factor":2},"max_merged_segment_docs":6,"deletes_pct_allowed":100})");
  addSegments(helper, "a", 2);
  ASSERT_EQ(std::vector<int32_t>{4}, liveDocs(helper));
  deleteIds(helper, {"a0_0", "a0_1", "a1_0"});
  addSegment(helper, "b", 1);
  auto stats = segments(helper);
  ASSERT_EQ(1u, stats.size());
  EXPECT_EQ(2, stats[0].maxDoc);
  EXPECT_EQ(2, stats[0].liveDocs);
}

// A tiered level whose live size reaches a cap merges without waiting for
// merge_factor segments.
TEST_F(MergeLimitsTest, capFillMergesNearTerminalSegments) {
  CollectionHelper helper("merge_limits_fill");
  setMergePolicy(helper, R"({"params":{"merge_factor":10},"max_merged_segment_docs":6})");
  addSegments(helper, "a", 2);
  EXPECT_EQ((std::vector<int32_t>{2, 2}), liveDocs(helper));
  addSegment(helper, "b");
  EXPECT_EQ(std::vector<int32_t>{6}, liveDocs(helper));
}

TEST_F(MergeLimitsTest, reclaimRewritesTerminalSegmentPastDeletesPct) {
  CollectionHelper helper("merge_limits_reclaim");
  buildTerminalSegment(helper, "");
  ASSERT_EQ(std::vector<int32_t>{8}, liveDocs(helper));
  deleteIds(helper, {"a0_0", "a1_0"});  // 25%
  addSegment(helper, "b", 1);
  bool rewritten = false;
  for (const auto& segment : segments(helper)) rewritten |= segment.maxDoc == 6 && segment.liveDocs == 6;
  EXPECT_TRUE(rewritten);
  EXPECT_EQ(1u, helper.getIndexWriter()->stats(false).reclaimMerges);
}

TEST_F(MergeLimitsTest, reclaimLeavesSegmentUnderDeletesPct) {
  CollectionHelper helper("merge_limits_no_reclaim");
  buildTerminalSegment(helper, "");
  deleteIds(helper, {"a0_0"});  // 12.5%
  addSegment(helper, "b", 1);
  bool kept = false;
  for (const auto& segment : segments(helper)) kept |= segment.maxDoc == 8 && segment.liveDocs == 7;
  EXPECT_TRUE(kept);
  EXPECT_EQ(0u, helper.getIndexWriter()->stats(false).reclaimMerges);
}

TEST_F(MergeLimitsTest, reclaimDisabledAtOneHundredPct) {
  CollectionHelper helper("merge_limits_reclaim_off");
  buildTerminalSegment(helper, R"(,"deletes_pct_allowed":100)");
  deleteIds(helper, {"a0_0", "a1_0", "a2_0"});
  addSegment(helper, "b", 1);
  bool kept = false;
  for (const auto& segment : segments(helper)) kept |= segment.maxDoc == 8 && segment.liveDocs == 5;
  EXPECT_TRUE(kept);
}

// The node default (no caps) merges on; a collection override caps it, and a
// later change applies from the next merge decision.
TEST_F(MergeLimitsTest, collectionOverrideAppliesFromNextDecision) {
  CollectionHelper helper("merge_limits_override");
  setMergePolicy(helper, R"({"params":{"merge_factor":2},"max_merged_segment_docs":6})");
  addSegments(helper, "a", 6);
  EXPECT_EQ((std::vector<int32_t>{4, 4, 4}), liveDocs(helper));
  setMergePolicy(helper, R"({"params":{"merge_factor":2},"max_merged_segment_docs":0})");
  addSegments(helper, "b", 2);
  EXPECT_EQ(std::vector<int32_t>{16}, liveDocs(helper));
}

// Cap admission stops at the first source that does not fit: a later, smaller
// source never leapfrogs it, so append-only segments keep disjoint update
// version ranges.
TEST_F(MergeLimitsTest, appendOnlySegmentsKeepDisjointVersionRanges) {
  CollectionHelper helper("merge_limits_versions");
  setMergePolicy(helper, R"({"params":{"merge_factor":4},"max_merged_segment_docs":7})");
  int s = 0;
  for (int docs : {3, 3, 3, 1}) addSegment(helper, std::format("a{}_", s++), docs);
  EXPECT_EQ((std::vector<int32_t>{1, 3, 6}), liveDocs(helper));
  auto stats = segments(helper);
  for (size_t i = 1; i < stats.size(); i++) {
    EXPECT_LT(stats[i - 1].maxUpdateVersion, stats[i].minUpdateVersion);
  }
}

// A background merge admits no source that pushes its most expensive field's
// estimated working set past maxMergeWorkingSetBytes; a force merge still runs.
TEST_F(MergeLimitsTest, naturalMergesRespectMaxMergeWorkingSet) {
  CollectionHelper helper("merge_limits_working_set");
  auto iw = helper.getIndexWriter();
  setMergePolicy(helper, R"({"params":{"merge_factor":2}})");
  iw->maxMergeWorkingSetBytes = 1;  // any second source passes it
  addSegments(helper, "a", 4);
  EXPECT_EQ((std::vector<int32_t>{2, 2, 2, 2}), liveDocs(helper));
  forceMergeToOne(helper);
  EXPECT_EQ(std::vector<int32_t>{8}, liveDocs(helper));
}
