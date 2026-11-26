// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include <gtest/gtest.h>

#include <atomic>
#include <format>

#include "luxir/index/IndexWriter.h"
#include "luxir/reader/IdFilter.h"
#include "luxir/reader/ReaderErrors.h"
#include "luxir/reader/SegmentCore.h"
#include "luxir/reader/TermsEnum.h"
#include "luxir/search/IndexReader.h"
#include "luxir/util/Signal.h"
#include "luxir/util/luxir_util.h"
#include "luxir/store/Directory.h"
#include "test/CollectionHelper.h"
#include "test/DurableIndexInfo.h"
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

IdFilter::Options bloom(uint32_t bitsPerKey = 16, uint32_t minBytes = 0) {
  return {.kind = IdFilter::Kind::BLOOM, .bitsPerKey = bitsPerKey, .minBytes = minBytes};
}

IdFilter::Options fuse(uint32_t fingerprintBits) {
  return {.kind = IdFilter::Kind::FUSE, .fingerprintBits = fingerprintBits};
}

std::string name(const IdFilter::Options& options) {
  if (options.kind == IdFilter::Kind::BLOOM) return "bloom" + std::to_string(options.bitsPerKey);
  return "fuse" + std::to_string(options.fingerprintBits);
}

std::vector<char> buildFilter(const IdFilter::Options& options, int keys, std::string_view prefix = "present") {
  IdFilter::Builder builder(options, keys);
  for (int i = 0; i < keys; i++) builder.add(IdFilter::hash(std::format("{}{}", prefix, i)));
  EXPECT_EQ((uint64_t)keys, builder.numKeys());
  return builder.finish();
}

// Every term in every segment's id dictionary is reported as maybe present, and
// each segment's filter is the configured kind. Returns the terms seen.
size_t expectNoFalseNegatives(CollectionHelper& helper) {
  const auto& options = helper.getIndexWriter()->idFilterOptions;
  size_t seen = 0;
  auto reader = helper.getIndexWriter()->snapshots.readers.getReader();
  for (const auto& segment : reader->segments()) {
    auto core = segment.segmentCoreShared();
    auto& postings = core->rootReader();
    const auto* info = postings.fieldInfo("id");
    auto filter = core->idFilter();
    EXPECT_TRUE(filter && filter->filterKind() == options.kind);
    if (!filter) continue;
    if (options.kind == IdFilter::Kind::BLOOM) {
      EXPECT_EQ(IdFilter::HEADER_BYTES + IdFilter::bloomBlocksFor(options, postings.maxDoc()) * IdFilter::BLOCK_BYTES,
                info->idFilter().bytes);
    }
    MemPool pool;
    TermsEnum terms(pool, postings, *info);
    while (terms.nextTerm()) {
      EXPECT_TRUE(filter->mayContain(std::string_view(terms.term()))) << std::string_view(terms.term());
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

// Collection tests run once per filter kind a writer can build.
class IdFilterKindTest : public LuxirTest, public ::testing::WithParamInterface<IdFilter::Options> {};

INSTANTIATE_TEST_SUITE_P(Kinds, IdFilterKindTest, ::testing::Values(bloom(16, 16 * 1024), fuse(8), fuse(16)),
                         [](const auto& info) { return name(info.param); });

TEST_F(IdFilterTest, eachKindHasNoFalseNegativesAndItsFalsePositiveRate) {
  constexpr int keys = 20000, probes = 200000;
  // Expected rates: about 0.13%, 0.39% and 0.0015%.
  for (auto [options, maxFalsePositives] : {std::pair{bloom(16), 520}, {fuse(8), 1000}, {fuse(16), 15}}) {
    auto bytes = buildFilter(options, keys);
    auto filter = IdFilter::parse(bytes);
    ASSERT_TRUE(filter) << name(options);
    EXPECT_EQ(options.kind, filter->filterKind());
    for (int i = 0; i < keys; i++) ASSERT_TRUE(filter->mayContain(std::format("present{}", i))) << name(options);
    int falsePositives = 0;
    for (int i = 0; i < probes; i++) falsePositives += filter->mayContain(std::format("absent{}", i));
    EXPECT_LT(falsePositives, maxFalsePositives) << name(options);
  }
  // A fuse filter over 20,000 keys costs about 1.23 fingerprints per key (1.13 at millions).
  EXPECT_LT(buildFilter(fuse(16), keys).size(), keys * 2 * 1.3);
}

TEST_F(IdFilterTest, fuseBuildsOverAnyKeyCount) {
  for (int keys : {1, 2, 3, 7, 100, 1000}) {
    for (auto bits : {8u, 16u}) {
      auto bytes = buildFilter(fuse(bits), keys);
      auto filter = IdFilter::parse(bytes);
      ASSERT_TRUE(filter && filter->filterKind() == IdFilter::Kind::FUSE) << keys;
      for (int i = 0; i < keys; i++) EXPECT_TRUE(filter->mayContain(std::format("present{}", i))) << keys;
    }
  }
}

TEST_F(IdFilterTest, bloomHasAFloorAndInvalidBytesAreNoFilter) {
  // A small segment's filter is the floor, so it is near-certain to exclude.
  EXPECT_EQ(IdFilter::HEADER_BYTES + 16 * 1024, buildFilter(bloom(16, 16 * 1024), 10).size());
  EXPECT_EQ(IdFilter::HEADER_BYTES + IdFilter::BLOCK_BYTES, buildFilter(bloom(16), 10).size());

  auto bytes = buildFilter(fuse(16), 50);
  EXPECT_FALSE(IdFilter::parse(std::span(bytes).first(IdFilter::HEADER_BYTES - 1)));
  EXPECT_FALSE(IdFilter::parse(std::span(bytes).first(bytes.size() - 2)));  // Body size differs from the shape.
  auto corrupt = [&](std::vector<char> copy, size_t at, char value) {
    copy[at] = value;
    return IdFilter::parse(copy).has_value();
  };
  EXPECT_FALSE(corrupt(bytes, 0, 9));  // Unknown kind.
  EXPECT_FALSE(corrupt(bytes, 1, 12));  // Fingerprint bits.
  EXPECT_FALSE(corrupt(bytes, 2, 1));  // Reserved.
  bytes = buildFilter(bloom(), 50);
  EXPECT_FALSE(IdFilter::parse(std::span(bytes).first(bytes.size() - 4)));  // Partial block.
  EXPECT_FALSE(corrupt(bytes, 1, 0));  // Bits per key.
}

TEST_P(IdFilterKindTest, everyIdIsMaybePresentAfterFlushAndMerge) {
  const auto& options = GetParam();
  LuxirNode node;
  CollectionHelper helper(node);
  helper.getIndexWriter()->idFilterOptions = options;
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
  EXPECT_TRUE(core->idFilter()->mayContain(fitted(longId(20))));
}

TEST_P(IdFilterKindTest, partitionedMergeFeedsTheIdFilter) {
  const auto& options = GetParam();
  LuxirNode node;
  CollectionHelper helper(node);
  auto iw = helper.getIndexWriter();
  iw->idFilterOptions = options;
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

TEST_P(IdFilterKindTest, deletesLookUpOnlyCandidateSegments) {
  const auto& options = GetParam();
  LuxirNode node;
  CollectionHelper helper(node);
  auto iw = helper.getIndexWriter();
  iw->idFilterOptions = options;
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
  // Segment 3 holds all ten; few of the other probes pass a filter.
  EXPECT_GE(lookups.load(), 1);
  EXPECT_LE(lookups.load(), 3);

  // An update by id is a delete of the earlier document.
  lookups = 0;
  ASSERT_TRUE(helper.index(flatdoc("id", "id00006"), UpdateMessage::COMMIT, true).success);
  EXPECT_EQ(800 - 10, iw->snapshots.readers.getReader()->liveDocs());
  EXPECT_GE(lookups.load(), 1);
  EXPECT_LE(lookups.load(), 3);
}

// Writers build no filter by default.
TEST_F(IdFilterTest, noFilterStillAppliesDeletes) {
  LuxirNode node;
  CollectionHelper helper(node);
  auto iw = helper.getIndexWriter();
  ASSERT_TRUE(helper.indexAll(docs(0, 100), UpdateMessage::COMMIT).success);
  auto core = iw->snapshots.readers.getReader()->segments()[0].segmentCoreShared();
  EXPECT_FALSE(core->rootReader().fieldInfo("id")->idFilter());
  EXPECT_FALSE(core->idFilter());
  ASSERT_TRUE(helper.deleteByIds({"id00001", longId(10)}, UpdateMessage::COMMIT).success);
  EXPECT_EQ(98, iw->snapshots.readers.getReader()->liveDocs());
}

// Writers only write filters that parse, so a reader takes one that does not as damage.
TEST_F(IdFilterTest, unparseableFilterIsCorruption) {
  LuxirNode node;
  CollectionHelper helper(node);
  auto iw = helper.getIndexWriter();
  iw->idFilterOptions = bloom();
  ASSERT_TRUE(helper.indexAll(docs(0, 10), UpdateMessage::COMMIT).success);
  auto durable = readDurableIndexInfo(iw->dir);
  const auto& segment = durable.info.segments.front();
  auto part = SegmentCore::create(iw->dir, segment)->rootReader().fieldInfo("id")->idFilter();
  auto damaged = Postings::getIndexFileName(Postings::segmentKey(segment.seg_id, segment.creator_epoch),
                                            part.location.filenum());
  RAMDir copy;
  for (const auto& file : segment.files) {
    auto input = iw->dir.openFile(file.name);
    auto hold = input->acquire({0, input->size()});
    std::string bytes(hold.getData(), hold.getLength());
    if (file.name == damaged) bytes[part.location.offset()] = 9;  // No such kind.
    auto out = copy.createFile(file.name);
    OutputStream stream(out.get());
    stream.write(bytes.data(), bytes.size());
    stream.close();
    copy.finishFile(*out);
  }
  EXPECT_THROW(SegmentCore::create(copy, segment)->idFilter(), SegmentCorruptError);
}

} // namespace luxir::test
