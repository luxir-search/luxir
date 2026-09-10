// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include <gtest/gtest.h>

#include "luxir/index/SegmentMerger.h"
#include "luxir/reader/DocsEnum.h"
#include "luxir/reader/PosEnum.h"
#include "luxir/reader/FieldReader.h"
#include "luxir/reader/TermsEnum.h"
#include "luxir/util/Signal.h"
#include "luxir/util/luxir_util.h"
#include "test/SegmentTest.h"
#include "test/TestIndex.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <string>
#include <thread>
#include <vector>

using namespace luxir;
using namespace luxir::test;

namespace {

struct PostingSnapshot {
  int32_t doc;
  int32_t tf;
  std::vector<int32_t> positions;
  bool operator==(const PostingSnapshot&) const = default;
};

struct TermSnapshot {
  std::string term;
  int32_t docFreq;
  int32_t packedBlockCount;
  int64_t totalTermFreq;
  std::vector<int32_t> impactNorms;
  std::vector<int32_t> impactTfs;
  std::vector<PostingSnapshot> postings;
  bool operator==(const TermSnapshot&) const = default;
};

struct FieldSnapshot {
  std::vector<TermSnapshot> terms;
  int32_t docsWithField;
  int64_t sumDocFreq;
  int64_t sumTotalTermFreq;
  bool operator==(const FieldSnapshot&) const = default;
};

std::shared_ptr<Schema> textSchema() {
  auto schema = std::make_shared<Schema>();
  schema->fieldTypeMap["body"] = std::make_shared<TextFieldType>(
      "body", FieldType::INDEX_DOCS_FREQS_POSITIONS);
  return schema;
}

std::string termName(int32_t ord) {
  std::string term;
  SegmentTest::makeTerm(ord, term);
  return term;
}

std::string wideTermName(int32_t ord) {
  std::string term(PackedTerm::MAX_LEN, 'a');
  term[0] = 't';
  uint32_t digits = (uint32_t) ord;
  for (int32_t i = 8; i > 0; i--) {
    term[(size_t) i] = (char) ('0' + digits % 10);
    digits /= 10;
  }
  term[9] = '_';
  uint64_t bits = (uint64_t) ord + 0x9e3779b97f4a7c15ULL;
  for (size_t i = 10; i < term.size(); i++) {
    bits ^= bits >> 12;
    bits ^= bits << 25;
    bits ^= bits >> 27;
    term[i] = (char) ('a' + bits % 26);
  }
  return term;
}

int32_t recomputePackedBlockCount(const std::vector<PostingSnapshot>& postings) {
  uint32_t base = 0;
  int32_t packed = 0;
  int32_t fullBlocks = (int32_t) postings.size() / Postings::DOCS_BLOCK_SIZE;
  for (int32_t block = 0; block < fullBlocks; block++) {
    int32_t start = block * Postings::DOCS_BLOCK_SIZE;
    int32_t end = start + Postings::DOCS_BLOCK_SIZE;
    uint32_t lastDoc = (uint32_t) postings[(size_t) end - 1].doc;
    uint32_t docBase = base + (block == 0 ? 0 : 1);
    uint32_t spanBits = lastDoc - docBase + 1;
    uint32_t deltaOr = (uint32_t) postings[(size_t) start].doc - base;
    for (int32_t i = start + 1; i < end; i++) {
      deltaOr |= (uint32_t) (postings[(size_t) i].doc
                             - postings[(size_t) i - 1].doc);
    }
    uint32_t bitsPerValue =
        32 - (uint32_t) std::countl_zero(deltaOr | 1);
    uint32_t numWords = (spanBits + 63) / 64;
    if (spanBits != (uint32_t) Postings::DOCS_BLOCK_SIZE
        && std::min(32u, bitsPerValue + 1)
               * (uint32_t) Postings::DOCS_BLOCK_SIZE
           <= numWords * 64) {
      packed++;
    }
    base = lastDoc;
  }
  return packed;
}

void buildSources(TestIndex& index, const std::shared_ptr<Schema>& schema,
                  bool partitioned, int32_t termBase = 0,
                  IndexRamBudget* budget = nullptr, int32_t maxRanges = 4) {
  index.iw = std::make_unique<IndexWriter>(
      index.dir, schema, budget);
  index.iw->mergePolicy->setMergeFactor(1000);
  if (partitioned) {
    index.iw->termPartitionMinBytes = 1;
    index.iw->termPartitionMinRangeBytes = 1;
    index.iw->termPartitionMaxRanges = maxRanges;
  } else {
    // The unpartitioned oracle must stay serial even under the debug-build
    // tiny default thresholds.
    index.iw->termPartitionMinBytes = INT64_MAX;
  }

  TestField body(index, "body");
  for (int32_t segment = 0; segment < 4; segment++) {
    body.startIndexing();
    for (int32_t doc = 0; doc < 40; doc++) {
      std::string value = "common common";
      for (int32_t i = 0; i < 8; i++) {
        std::string term = termName(termBase + doc * 8 + i);
        value += " " + term + " " + term;
      }
      body.add(doc * 16, value);
    }
    if ((segment & 1) == 0) index.deleteDoc(segment);
    index.flush();
  }
  index.iw->mergeSegments();
  index.initReader();
  ASSERT_EQ(1u, index.reader->segments().size());
}

void buildWideDictionarySources(TestIndex& index,
                                const std::shared_ptr<Schema>& schema) {
  index.iw = std::make_unique<IndexWriter>(
      index.dir, schema);
  index.iw->mergePolicy->setMergeFactor(1000);
  TestField body(index, "body");

  std::string value;
  value.reserve(1600 * (PackedTerm::MAX_LEN + 1));
  for (int32_t term = 0; term < 1600; term++) {
    value += wideTermName(term);
    value.push_back(' ');
  }
  for (int32_t segment = 0; segment < 4; segment++) {
    body.startIndexing();
    body.add(0, value);
    body.add(1, value);
    index.flush();
  }
  index.initReader();
  ASSERT_EQ(4u, index.reader->segments().size());
}

SegFieldInfo readBodyInfo(MemPool& pool, Segment& segment) {
  FieldReader fields(segment.postingsReader());
  EXPECT_TRUE(fields.seek("body"));
  SegFieldInfo info;
  fields.readFieldInfo(info);
  return info;
}

size_t baseFileCount(Directory& directory, uint64_t segId) {
  std::vector<Directory::FileInfo> files;
  directory.listFiles(files);
  std::string prefix = Postings::getIndexFileNamePrefix(segId);
  return (size_t) std::count_if(files.begin(), files.end(), [&](const auto& file) {
    return file.name.starts_with(prefix + "_")
        && !file.name.starts_with(prefix + "__");
  });
}

FieldSnapshot snapshotField(MemPool& pool, Segment& segment) {
  SegFieldInfo info = readBodyInfo(pool, segment);
  FieldSnapshot snapshot;
  snapshot.docsWithField = info.docsWithField;
  snapshot.sumDocFreq = info.sumDocFreq;
  snapshot.sumTotalTermFreq = info.sumTotalTermFreq;
  TermsEnum terms(pool, segment.postingsReader(), info);
  while (terms.nextTerm()) {
    TermSnapshot term;
    term.term = std::string((std::string_view) terms.term());
    term.docFreq = terms.docFreq();
    term.packedBlockCount = terms.packedBlockCount();
    term.totalTermFreq = terms.totalTermFreq();
    terms.readTermImpactFrontier(term.impactNorms, term.impactTfs);
    DocsPosEnum docs(terms);
    PosEnum positions(docs);
    for (int32_t doc = docs.nextDoc(); doc != DocsEnumMeta::END; doc = docs.nextDoc()) {
      PostingSnapshot posting{doc, docs.termFreq(), {}};
      positions.startPositions();
      for (int32_t i = 0; i < posting.tf; i++) {
        posting.positions.push_back(positions.nextPosition());
      }
      EXPECT_EQ(PosEnum::END, positions.nextPosition());
      term.postings.push_back(std::move(posting));
    }
    EXPECT_EQ(term.packedBlockCount, recomputePackedBlockCount(term.postings))
        << term.term;
    snapshot.terms.push_back(std::move(term));
  }
  return snapshot;
}

void verifySeeks(MemPool& pool, PostingsReader& reader, const SegFieldInfo& info,
                 const FieldSnapshot& expected) {
  TermsEnum exact(pool, reader, info);
  TermsEnum ceil(pool, reader, info);
  TermsEnum ord(pool, reader, info);
  TermsEnum forward(pool, reader, info);
  for (size_t i = 0; i < expected.terms.size(); i++) {
    const std::string& term = expected.terms[i].term;
    ASSERT_TRUE(exact.seek(term));
    EXPECT_EQ(term, (std::string_view) exact.term());
    ASSERT_TRUE(ceil.seekCeil(term));
    EXPECT_EQ(term, (std::string_view) ceil.term());
    ord.seekOrd((int32_t) i);
    EXPECT_EQ(term, (std::string_view) ord.term());
    ASSERT_TRUE(forward.seekForward(term));
    EXPECT_EQ(term, (std::string_view) forward.term());

    if (i + 1 < expected.terms.size()) {
      std::string gap = term;
      gap.push_back('\0');
      ASSERT_TRUE(ceil.seekCeil(gap));
      EXPECT_EQ(expected.terms[i + 1].term, (std::string_view) ceil.term());
    }
  }

  if (!info.rangeTableLoc.isNull()) {
    InputStream table = reader.getInputStreamSeek(info.rangeTableLoc);
    const auto* header = reinterpret_cast<const TermRangeTableHeader*>(table.ptr());
    const auto* rows = reinterpret_cast<const TermRangeRow*>(header + 1);
    for (uint32_t i = 1; i < header->nRanges; i++) {
      int64_t nextOrd = (int64_t) rows[i].firstTermOrd;
      ASSERT_GT(nextOrd, 0);
      TermsEnum cross(pool, reader, info);
      ASSERT_TRUE(cross.seek(expected.terms[(size_t) nextOrd - 1].term));
      std::string gap = expected.terms[(size_t) nextOrd - 1].term;
      gap.push_back('\0');
      EXPECT_FALSE(cross.seekForward(gap));
      ASSERT_TRUE(cross.seekForward(expected.terms[(size_t) nextOrd].term));
      EXPECT_EQ(expected.terms[(size_t) nextOrd].term, (std::string_view) cross.term());
    }
  }
}

FieldSnapshot mergePair(TestIndex& left, TestIndex& right, bool partitioned,
                        bool& hasRangeTable, bool& hasMidFieldTail) {
  std::array<PostingsReader*, 2> readers = {
      &left.reader->segments()[0].postingsReader(),
      &right.reader->segments()[0].postingsReader()};
  std::array<LiveDocs*, 2> liveDocs = {nullptr, nullptr};
  RAMDir outputDir;
  PostingsWriter writer(outputDir, 900, -1);
  IndexRamBudget budget;
  SegmentMerger merger(readers, liveDocs, writer, budget,
                       partitioned ? 1 : INT64_MAX, 1, 4);
  merger.merge();
  writer.finish();

  PostingsReader outputReader(outputDir, 900);
  MemPool pool;
  FieldReader fields(outputReader);
  EXPECT_TRUE(fields.seek("body"));
  SegFieldInfo info;
  fields.readFieldInfo(info);
  hasRangeTable = !info.rangeTableLoc.isNull();
  hasMidFieldTail = false;
  if (hasRangeTable) {
    InputStream table = outputReader.getInputStreamSeek(info.rangeTableLoc);
    const auto* header = reinterpret_cast<const TermRangeTableHeader*>(table.ptr());
    const auto* rows = reinterpret_cast<const TermRangeRow*>(header + 1);
    for (uint32_t i = 0; i + 1 < header->nRanges; i++) {
      uint64_t rangeTerms = rows[i + 1].firstTermOrd - rows[i].firstTermOrd;
      hasMidFieldTail |= (rangeTerms % Postings::TERMS_BLOCK_SIZE) != 0;
    }
  }

  FieldSnapshot snapshot;
  snapshot.docsWithField = info.docsWithField;
  snapshot.sumDocFreq = info.sumDocFreq;
  snapshot.sumTotalTermFreq = info.sumTotalTermFreq;
  TermsEnum terms(pool, outputReader, info);
  while (terms.nextTerm()) {
    TermSnapshot term;
    term.term = std::string((std::string_view) terms.term());
    term.docFreq = terms.docFreq();
    term.packedBlockCount = terms.packedBlockCount();
    term.totalTermFreq = terms.totalTermFreq();
    terms.readTermImpactFrontier(term.impactNorms, term.impactTfs);
    DocsPosEnum docs(terms);
    PosEnum positions(docs);
    for (int32_t doc = docs.nextDoc(); doc != DocsEnumMeta::END; doc = docs.nextDoc()) {
      PostingSnapshot posting{doc, docs.termFreq(), {}};
      positions.startPositions();
      for (int32_t i = 0; i < posting.tf; i++) posting.positions.push_back(positions.nextPosition());
      term.postings.push_back(std::move(posting));
    }
    EXPECT_EQ(term.packedBlockCount, recomputePackedBlockCount(term.postings))
        << term.term;
    snapshot.terms.push_back(std::move(term));
  }
  if (partitioned) verifySeeks(pool, outputReader, info, snapshot);
  return snapshot;
}

SegFieldInfo buildSparseRanges(TestIndex& index, const std::shared_ptr<Schema>& schema,
                               bool keepOneRange) {
  index.iw = std::make_unique<IndexWriter>(index.dir, schema);
  index.iw->mergePolicy->setMergeFactor(1000);
  index.iw->termPartitionMinBytes = 1;
  index.iw->termPartitionMinRangeBytes = 1;
  index.iw->termPartitionMaxRanges = 4;
  TestField body(index, "body");
  for (int32_t group = 0; group < 4; group++) {
    body.startIndexing();
    std::string value;
    int32_t termCount = keepOneRange && group == 0 ? 1 : 64;
    for (int32_t i = 0; i < termCount; i++) {
      std::string term = termName(group * 64 + i);
      value += term + " " + term + " ";
    }
    body.add(0, value);
    body.add(1, "");
    if (!keepOneRange || group != 0) index.deleteDoc(0);
    index.flush();
  }
  index.iw->mergeSegments();
  index.initReader();
  EXPECT_EQ(1u, index.reader->segments().size());
  return readBodyInfo(index.pool, index.reader->segments()[0]);
}

} // namespace

TEST(TermPartitionMergeTest, RoundTripSeeksAndCascade) {
  auto schema = textSchema();
  TestIndex serial;
  TestIndex partitioned;
  buildSources(serial, schema, false);

  // Hold all four ranges after writing but before returning their docs/pos
  // leases.  This makes file ownership deterministic: every range must have
  // checked out a distinct pair before any pair can be reused.
  std::atomic<int32_t> checkedOut = 0;
  {
    Signal::listen("termRangeStreamsCheckedOut", [&](void*, void*, void*) -> void* {
      checkedOut.fetch_add(1, std::memory_order_relaxed);
      while (checkedOut.load(std::memory_order_relaxed) < 4) {
        std::this_thread::yield();
      }
      return nullptr;
    });
    auto signalCleanup = luxir::scope_guard([]() {
      Signal::unlisten("termRangeStreamsCheckedOut");
    });
    buildSources(partitioned, schema, true);
  }

  Segment& serialSegment = serial.reader->segments()[0];
  Segment& partitionedSegment = partitioned.reader->segments()[0];
  SegFieldInfo serialInfo = readBodyInfo(serial.pool, serialSegment);
  SegFieldInfo partitionedInfo = readBodyInfo(partitioned.pool, partitionedSegment);
  ASSERT_TRUE(serialInfo.rangeTableLoc.isNull());
  ASSERT_FALSE(partitionedInfo.rangeTableLoc.isNull());
  ASSERT_EQ(4, checkedOut.load(std::memory_order_relaxed));

  InputStream table = partitionedSegment.postingsReader()
      .getInputStreamSeek(partitionedInfo.rangeTableLoc);
  const auto* header = reinterpret_cast<const TermRangeTableHeader*>(table.ptr());
  const auto* rows = reinterpret_cast<const TermRangeRow*>(header + 1);
  ASSERT_EQ(4u, header->nRanges);
  std::vector<uint32_t> postingFiles;
  postingFiles.reserve((size_t) header->nRanges * 2);
  for (uint32_t i = 0; i < header->nRanges; i++) {
    EXPECT_EQ(partitionedInfo.termBlockIndexLoc.filenum(), rows[i].termsBase.filenum());
    EXPECT_NE(rows[i].termsBase.filenum(), rows[i].docsBase.filenum());
    EXPECT_NE(rows[i].termsBase.filenum(), rows[i].posBase.filenum());
    postingFiles.push_back(rows[i].docsBase.filenum());
    postingFiles.push_back(rows[i].posBase.filenum());
  }
  std::sort(postingFiles.begin(), postingFiles.end());
  auto uniqueEnd = std::unique(postingFiles.begin(), postingFiles.end());
  EXPECT_EQ(postingFiles.size(), (size_t) (uniqueEnd - postingFiles.begin()));

  FieldSnapshot oracle = snapshotField(serial.pool, serialSegment);
  FieldSnapshot actual = snapshotField(partitioned.pool, partitionedSegment);
  EXPECT_EQ(oracle, actual);
  ASSERT_FALSE(actual.terms.empty());
  EXPECT_TRUE(std::any_of(actual.terms.begin(), actual.terms.end(),
                          [](const TermSnapshot& term) {
                            return term.packedBlockCount > 0;
                          }));
  verifySeeks(partitioned.pool, partitionedSegment.postingsReader(), partitionedInfo, actual);

  TestIndex right;
  buildSources(right, schema, true, 20);
  bool serialTable;
  bool serialMidFieldTail;
  bool partitionedTable;
  bool partitionedMidFieldTail;
  FieldSnapshot cascadeOracle = mergePair(
      partitioned, right, false, serialTable, serialMidFieldTail);
  FieldSnapshot cascade = mergePair(
      partitioned, right, true, partitionedTable, partitionedMidFieldTail);
  EXPECT_FALSE(serialTable);
  EXPECT_FALSE(serialMidFieldTail);
  EXPECT_TRUE(partitionedTable);
  EXPECT_TRUE(partitionedMidFieldTail);
  EXPECT_EQ(cascadeOracle, cascade);
}

TEST(TermPartitionMergeTest, EmptyRangesAreOmitted) {
  auto schema = textSchema();
  TestIndex oneRange;
  std::atomic<int32_t> checkedOut = 0;
  SegFieldInfo oneInfo;
  {
    Signal::listen("termRangeStreamsCheckedOut", [&](void*, void*, void*) -> void* {
      checkedOut.fetch_add(1, std::memory_order_relaxed);
      while (checkedOut.load(std::memory_order_relaxed) < 4) {
        std::this_thread::yield();
      }
      return nullptr;
    });
    auto signalCleanup = luxir::scope_guard([]() {
      Signal::unlisten("termRangeStreamsCheckedOut");
    });
    oneInfo = buildSparseRanges(oneRange, schema, true);
  }
  ASSERT_FALSE(oneInfo.rangeTableLoc.isNull());
  ASSERT_EQ(4, checkedOut.load(std::memory_order_relaxed));
  InputStream table = oneRange.reader->segments()[0].postingsReader()
      .getInputStreamSeek(oneInfo.rangeTableLoc);
  const auto* header = reinterpret_cast<const TermRangeTableHeader*>(table.ptr());
  EXPECT_EQ(1u, header->nRanges);
  const auto* rows = reinterpret_cast<const TermRangeRow*>(header + 1);
  EXPECT_NE(nullptr, oneRange.reader->segments()[0].postingsReader()
                         .getFile(rows[0].termsBase.filenum()));
  EXPECT_NE(nullptr, oneRange.reader->segments()[0].postingsReader()
                         .getFile(rows[0].docsBase.filenum()));
  EXPECT_NE(nullptr, oneRange.reader->segments()[0].postingsReader()
                         .getFile(rows[0].posBase.filenum()));
  EXPECT_EQ(3u, baseFileCount(oneRange.dir,
                              oneRange.reader->segments()[0].segInfo.seg_id));
  FieldSnapshot snapshot = snapshotField(oneRange.pool,
                                         oneRange.reader->segments()[0]);
  ASSERT_EQ(1u, snapshot.terms.size());
  ASSERT_EQ(1u, snapshot.terms[0].postings.size());

  TestIndex allEmpty;
  SegFieldInfo emptyInfo = buildSparseRanges(allEmpty, schema, false);
  EXPECT_EQ(0, emptyInfo.nTerms);
  EXPECT_TRUE(emptyInfo.rangeTableLoc.isNull());
  EXPECT_TRUE(emptyInfo.termBlockIndexLoc.isNull());
}

TEST(TermPartitionMergeTest, TinyBudgetStillPartitions) {
  auto schema = textSchema();
  IndexRamBudget budget(64);
  TestIndex index;
  buildSources(index, schema, true, 0, &budget);
  SegFieldInfo info = readBodyInfo(index.pool, index.reader->segments()[0]);
  EXPECT_GT(info.nTerms, 0);
  EXPECT_FALSE(info.rangeTableLoc.isNull());
}

TEST(TermPartitionMergeTest, CoordinatorGrowthIgnoresOccupiedBudget) {
  auto schema = textSchema();
  TestIndex sources;
  buildWideDictionarySources(sources, schema);

  std::vector<PostingsReader*> readers;
  std::vector<LiveDocs*> liveDocs;
  int64_t docsWithField = 0;
  int64_t dictionaryBytes = 0;
  for (auto& segment : sources.reader->segments()) {
    readers.push_back(&segment.postingsReader());
    liveDocs.push_back(nullptr);
    SegFieldInfo info = readBodyInfo(sources.pool, segment);
    docsWithField += info.docsWithField;
    dictionaryBytes += (int64_t) (info.termBlockIndexLoc.offset()
        - info.termsLoc.offset());
  }
  int64_t serialCost = MergeCostModel::LIGHT_BYTES + docsWithField * 2;
  int64_t coordinatorCost = dictionaryBytes + docsWithField * 2;
  ASSERT_GT(coordinatorCost, serialCost);

  int64_t cap = 2 * MergeCostModel::LIGHT_BYTES;
  IndexRamBudget budget(cap);
  [[maybe_unused]] auto blocker = budget.forceAcquire(cap);
  ASSERT_EQ(cap, budget.reservedBytes());
  RAMDir outputDir;
  PostingsWriter writer(outputDir, 901, -1);
  SegmentMerger merger(readers, liveDocs, writer, budget, 1, 1, 4);
  merger.merge();
  writer.finish();

  PostingsReader outputReader(outputDir, 901);
  FieldReader fields(outputReader);
  ASSERT_TRUE(fields.seek("body"));
  SegFieldInfo info;
  fields.readFieldInfo(info);
  EXPECT_FALSE(info.rangeTableLoc.isNull());
}

TEST(TermPartitionMergeTest, DerivedRangeLimitExceedsEight) {
  auto schema = textSchema();
  TestIndex index;
  buildSources(index, schema, true, 0, nullptr,
               MergeCostModel::DERIVED_TERM_RANGES);

  Segment& segment = index.reader->segments()[0];
  SegFieldInfo info = readBodyInfo(index.pool, segment);
  ASSERT_FALSE(info.rangeTableLoc.isNull());
  InputStream table = segment.postingsReader().getInputStreamSeek(info.rangeTableLoc);
  const auto* header = reinterpret_cast<const TermRangeTableHeader*>(table.ptr());
  EXPECT_GT(header->nRanges, 8u);
  EXPECT_LE(header->nRanges, (uint32_t) MergeCostModel::maxTermRangesForStreams(
                                 MergeCostModel::STREAM_BUDGET_FLOOR));
}
