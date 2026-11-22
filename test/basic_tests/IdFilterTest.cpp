// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include <gtest/gtest.h>

#include <atomic>
#include <format>

#include "luxir/index/IndexWriter.h"
#include "luxir/reader/IdFilter.h"
#include "luxir/reader/SegmentCore.h"
#include "luxir/reader/TermsEnum.h"
#include "luxir/search/IndexReader.h"
#include "luxir/util/Signal.h"
#include "luxir/util/luxir_util.h"
#include "test/CollectionHelper.h"
#include "test/LuxirTest.h"
#include "test/TestUtils.h"

namespace luxir::test {

class IdFilterTest : public LuxirTest {};

namespace {

// Over the 255-byte term limit, so the dictionary keeps its hash-tail form.
std::string longId(int i) { return std::string(300, 'x') + std::to_string(i); }

std::string fitted(std::string_view id) {
  PackedTerm::TermBuffer scratch;
  return std::string(*PackedTerm::fitTerm(TermPolicy::HASH128, id, scratch));
}

// Every term in every segment's id dictionary is reported as maybe present, and
// the filter holds one key per term. Returns the terms seen.
size_t expectNoFalseNegatives(CollectionHelper& helper) {
  size_t seen = 0;
  auto reader = helper.getIndexWriter()->snapshots.readers.getReader();
  for (const auto& segment : reader->segments()) {
    auto core = segment.segmentCoreShared();
    auto& postings = core->rootReader();
    const auto* info = postings.fieldInfo("id");
    EXPECT_TRUE(info && info->idFilter());
    if (!info || !info->idFilter()) continue;
    EXPECT_EQ(IdFilter::blocksFor(postings.maxDoc()) * IdFilter::BLOCK_BYTES, info->idFilter().bytes);
    MemPool pool;
    TermsEnum terms(pool, postings, *info);
    while (terms.nextTerm()) {
      EXPECT_TRUE(core->mayContainId(std::string_view(terms.term()))) << std::string_view(terms.term());
      seen++;
    }
  }
  return seen;
}

std::vector<Doc> docs(int first, int count) {
  std::vector<Doc> out;
  for (int i = first; i < first + count; i++) {
    out.push_back(flatdoc("id", i % 10 ? std::format("id{:05}", i) : longId(i), "body_t",
                          std::format("word{} common", i % 7)));
  }
  return out;
}

} // namespace

TEST_F(IdFilterTest, builderHasNoFalseNegativesAndAboutOnePercentFalsePositives) {
  constexpr int keys = 20000;
  IdFilter::Builder builder(keys);
  for (int i = 0; i < keys; i++) builder.add(IdFilter::hash(std::format("present{}", i)));
  EXPECT_EQ((uint64_t)keys, builder.numKeys());
  ASSERT_TRUE(IdFilter::validSize(builder.sizeInBytes()));
  IdFilter filter(builder.encoded());
  for (int i = 0; i < keys; i++) EXPECT_TRUE(filter.mayContain(std::format("present{}", i)));
  int falsePositives = 0;
  for (int i = 0; i < 100000; i++) falsePositives += filter.mayContain(std::format("absent{}", i));
  EXPECT_LT(falsePositives, 2000);  // Under 2%.
  EXPECT_FALSE(IdFilter::validSize(0));
  EXPECT_FALSE(IdFilter::validSize(IdFilter::BLOCK_BYTES + 4));
}

TEST_F(IdFilterTest, everyIdIsMaybePresentAfterFlushAndMerge) {
  LuxirNode node;
  CollectionHelper helper(node);
  helper.getIndexWriter()->mergePolicy->setMergeFactor(1000);
  for (int segment = 0; segment < 3; segment++) {
    ASSERT_TRUE(helper.indexAll(docs(segment * 200, 200), UpdateMessage::COMMIT).success);
  }
  EXPECT_EQ(600u, expectNoFalseNegatives(helper));

  // A merge rebuilds the filter from the surviving terms only.
  ASSERT_TRUE(helper.deleteByIds({"id00001", longId(10)}, UpdateMessage::COMMIT).success);
  CollectionHelper::UpdateBuilder merge;
  merge.commit(true, 1);
  ASSERT_TRUE(helper.submit(merge).success);
  ASSERT_EQ(1u, helper.getIndexWriter()->snapshots.readers.getReader()->segments().size());
  EXPECT_EQ(598u, expectNoFalseNegatives(helper));
  auto core = helper.getIndexWriter()->snapshots.readers.getReader()->segments()[0].segmentCoreShared();
  EXPECT_TRUE(core->mayContainId(fitted(longId(20))));
  EXPECT_FALSE(core->mayContainId("zzz"));  // Above the exact bounds.
}

TEST_F(IdFilterTest, partitionedMergeFeedsTheIdFilter) {
  LuxirNode node;
  CollectionHelper helper(node);
  auto iw = helper.getIndexWriter();
  iw->mergePolicy->setMergeFactor(1000);
  // Text fields split into parallel term ranges; the id dictionary stays one writer.
  iw->termPartitionMinBytes = 1;
  iw->termPartitionMinRangeBytes = 1;
  iw->termPartitionMaxRanges = 4;
  for (int segment = 0; segment < 4; segment++) {
    ASSERT_TRUE(helper.indexAll(docs(segment * 150, 150), UpdateMessage::COMMIT).success);
  }
  CollectionHelper::UpdateBuilder merge;
  merge.commit(true, 1);
  ASSERT_TRUE(helper.submit(merge).success);
  EXPECT_EQ(600u, expectNoFalseNegatives(helper));
  ASSERT_TRUE(helper.deleteById(longId(30), UpdateMessage::COMMIT).success);
  EXPECT_EQ(599, iw->snapshots.readers.getReader()->liveDocs());
}

TEST_F(IdFilterTest, deletesLookUpOnlyCandidateSegments) {
  LuxirNode node;
  CollectionHelper helper(node);
  auto iw = helper.getIndexWriter();
  iw->mergePolicy->setMergeFactor(1000);
  // Interleaved ids give every segment the same id range, so only the filter can skip.
  constexpr int segments = 8;
  for (int segment = 0; segment < segments; segment++) {
    std::vector<Doc> batch;
    for (int i = 0; i < 100; i++) batch.push_back(flatdoc("id", std::format("id{:05}", i * segments + segment)));
    ASSERT_TRUE(helper.indexAll(batch, UpdateMessage::COMMIT).success);
  }
  std::atomic<int> lookups = 0;
  Signal::listen("deleteLookupInSegment", [&](void*, void*, void*) -> void* { lookups++; return nullptr; });
  auto unlisten = scope_guard([] { Signal::unlisten("deleteLookupInSegment"); });
  std::vector<std::string> ids;
  for (int i = 0; i < 10; i++) ids.push_back(std::format("id{:05}", i * segments + 3));
  ASSERT_TRUE(helper.deleteByIds(ids, UpdateMessage::COMMIT).success);
  EXPECT_EQ(800 - 10, iw->snapshots.readers.getReader()->liveDocs());
  // Segment 3 holds all ten; about 1% of the other probes pass a filter.
  EXPECT_GE(lookups.load(), 1);
  EXPECT_LE(lookups.load(), 3);

  // An update by id is a delete of the earlier document.
  lookups = 0;
  ASSERT_TRUE(helper.index(flatdoc("id", "id00006"), UpdateMessage::COMMIT, true).success);
  EXPECT_EQ(800 - 10, iw->snapshots.readers.getReader()->liveDocs());
  EXPECT_GE(lookups.load(), 1);
  EXPECT_LE(lookups.load(), 3);
}

} // namespace luxir::test
