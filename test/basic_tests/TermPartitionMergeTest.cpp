// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include "luxir/reader/SegmentCore.h"
#include "test/FieldRecordTest.h"
#include "test/StorageTestUtils.h"
#include <gtest/gtest.h>

#include "luxir/index/SegmentMerger.h"
#include "luxir/reader/DocsEnum.h"
#include "luxir/reader/PosEnum.h"
#include "luxir/reader/PostingsReader.h"
#include "luxir/reader/TermsEnum.h"
#include "luxir/util/Signal.h"
#include "luxir/util/luxir_util.h"
#include "test/SegmentTest.h"
#include "test/TestIndex.h"

#include <oneapi/tbb/task_arena.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <format>
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
  test::reserveIncarnation(index.dir);
  index.iw = std::make_unique<IndexWriter>(
      index.snapshots, schema, budget);
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
      if ((segment & 1) == 0 && doc % 3 == 0) index.getInverter().updateVersions((uint64_t)segment * 40 + doc + 1);
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
                                const std::shared_ptr<Schema>& schema,
                                int32_t termCount = 1600) {
  test::reserveIncarnation(index.dir);
  index.iw = std::make_unique<IndexWriter>(
      index.snapshots, schema);
  index.iw->mergePolicy->setMergeFactor(1000);
  TestField body(index, "body");

  std::string value;
  value.reserve((size_t)termCount * (PackedTerm::MAX_LEN + 1));
  for (int32_t term = 0; term < termCount; term++) {
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

const DecodedFieldRecord& readBodyInfo(Segment& segment) {
  auto* field = segment.postingsReader().fieldInfo("body");
  if (!field) throw std::runtime_error("body field missing");
  return *field;
}

size_t baseFileCount(Directory& directory, uint64_t segId) {
  std::vector<Directory::FileInfo> files;
  directory.listFiles(files);
  std::string prefix = "s" + Postings::segmentKey(segId, 1);
  return (size_t) std::count_if(files.begin(), files.end(), [&](const auto& file) {
    return file.name.starts_with(prefix + "_")
        && !file.name.starts_with(prefix + "__");
  });
}

FieldSnapshot snapshotField(MemPool& pool, Segment& segment) {
  const auto& info = readBodyInfo(segment);
  FieldSnapshot snapshot;
  snapshot.docsWithField = info.docsWithField;
  TermsEnum terms(pool, segment.postingsReader(), info);
  snapshot.sumDocFreq = terms.sumDocFreq();
  snapshot.sumTotalTermFreq = terms.sumTotalTermFreq();
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

void verifySeeks(MemPool& pool, PostingsReader& reader, const DecodedFieldRecord& info,
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

  auto runs = info.terms()->runs;
  for (size_t i = 1; i < runs.size(); ++i) {
    int64_t nextOrd = runs[i].firstTermOrd;
    ASSERT_GT(nextOrd, 0);
    TermsEnum cross(pool, reader, info);
    ASSERT_TRUE(cross.seek(expected.terms[(size_t)nextOrd - 1].term));
    std::string gap = expected.terms[(size_t)nextOrd - 1].term;
    gap.push_back('\0');
    EXPECT_FALSE(cross.seekForward(gap));
    ASSERT_TRUE(cross.seekForward(expected.terms[(size_t)nextOrd].term));
    EXPECT_EQ(expected.terms[(size_t)nextOrd].term, (std::string_view)cross.term());
  }

  // Block stats step in both directions without crossing a run boundary.
  int64_t nTerms = (int64_t) expected.terms.size();
  for (bool descending : {false, true}) {
    TermsEnum blocks(pool, reader, info);
    int64_t next = descending ? nTerms - 1 : 0;
    while (next >= 0 && next < nTerms) {
      auto [firstOrd, docFreqs] = blocks.docFreqBlock(next);
      int64_t endOrd = firstOrd + (int64_t) docFreqs.size();
      ASSERT_LE(firstOrd, next);
      ASSERT_LT(next, endOrd);
      ASSERT_LE(endOrd, nTerms);
      ASSERT_LE(docFreqs.size(), (size_t) Postings::TERMS_BLOCK_SIZE);
      for (const auto& run : runs) {
        auto start = run.firstTermOrd;
        EXPECT_FALSE(start > firstOrd && start < endOrd) << start;
      }
      for (int64_t i = firstOrd; i < endOrd; i++) {
        EXPECT_EQ(expected.terms[(size_t) i].docFreq,
                  (int32_t) docFreqs[(size_t) (i - firstOrd)]) << i;
      }
      blocks.seekOrd(endOrd - 1);
      EXPECT_EQ(expected.terms[(size_t) endOrd - 1].term, (std::string_view) blocks.term());
      EXPECT_EQ(firstOrd, blocks.docFreqBlock(firstOrd).firstOrd);
      EXPECT_EQ(endOrd - 1, blocks.ord());
      next = descending ? firstOrd - 1 : endOrd;
    }
  }
}

struct CapturedTermRouting {
  std::optional<field_parts::Terms> terms;
  std::vector<field_parts::TermRun> runs;
  int64_t termCount = 0;

  CapturedTermRouting() {
    Signal::listen("termRoutingWritten", [&](void* descriptor, void* count, void*) -> void* {
      terms = *(const field_parts::Terms*)descriptor;
      runs.assign(terms->runs.begin(), terms->runs.end());
      terms->runs = runs;
      termCount = *(const int64_t*)count;
      return nullptr;
    });
  }

  ~CapturedTermRouting() { Signal::unlisten("termRoutingWritten"); }
};

void expectRouting(const CapturedTermRouting& captured, const DecodedFieldRecord& info) {
  ASSERT_TRUE(captured.terms && info.terms() && (info.flags & FieldType::INDEX_DOCS));
  EXPECT_EQ(*captured.terms, *info.terms());
  EXPECT_EQ(captured.termCount, info.indexedTotals().numTerms);
}

FieldSnapshot mergePair(TestIndex& left, TestIndex& right, bool partitioned,
                        int32_t& runCount, bool& hasMidFieldTail) {
  std::array<std::shared_ptr<SegmentCore>, 2> readers = {
      left.reader->segments()[0].segmentCoreShared(),
      right.reader->segments()[0].segmentCoreShared()};
  std::array<LiveDocs*, 2> liveDocs = {nullptr, nullptr};
  RAMDir outputDir;
  PostingsWriter writer(outputDir, 900, 1, -1);
  IndexRamBudget budget;
  Schema schema;
  StoredDictionaryRegistry dictionaries;
  dictionaries.setSchema(StoredDictionaryRegistry::prepareSchema(schema));
  SegmentMerger merger(readers, liveDocs, writer, budget,
                       partitioned ? 1 : INT64_MAX, 1, 4, schema, dictionaries);
  CapturedTermRouting captured;
  merger.merge();
  writer.finish();

  SegmentCore outputReaderCore(outputDir, 900, 1);
  auto& outputReader = outputReaderCore.rootReader();
  VersionRuns::Cursor outputBirths(outputReader.core().versionRuns());
  int32_t base = 0;
  for (const auto& core : readers) {
    VersionRuns::Cursor inputBirths(core->versionRuns());
    int32_t count = core->rootReader().maxDoc();
    for (int32_t doc = 0; doc < count; doc++) EXPECT_EQ(inputBirths.birth(doc), outputBirths.birth(base + doc));
    base += count;
  }
  MemPool pool;
  auto* fields = outputReader.fieldInfo("body");
  EXPECT_NE(nullptr, fields);
  const auto& info = *fields;
  auto runs = info.terms()->runs;
  runCount = (int32_t)runs.size();
  hasMidFieldTail = false;
  if (runCount > 1) {
    if (partitioned) expectRouting(captured, info);
    for (size_t i = 1; i < runs.size(); ++i) {
      uint64_t rangeTerms = runs[i].firstTermOrd - runs[i - 1].firstTermOrd;
      hasMidFieldTail |= (rangeTerms % Postings::TERMS_BLOCK_SIZE) != 0;
    }
  }

  FieldSnapshot snapshot;
  snapshot.docsWithField = info.docsWithField;
  TermsEnum terms(pool, outputReader, info);
  snapshot.sumDocFreq = terms.sumDocFreq();
  snapshot.sumTotalTermFreq = terms.sumTotalTermFreq();
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
  if (runCount > 1) verifySeeks(pool, outputReader, info, snapshot);
  return snapshot;
}

const DecodedFieldRecord& buildSparseRanges(TestIndex& index, const std::shared_ptr<Schema>& schema,
                               std::initializer_list<int32_t> survivingGroups) {
  test::reserveIncarnation(index.dir);
  index.iw = std::make_unique<IndexWriter>(index.snapshots, schema);
  index.iw->mergePolicy->setMergeFactor(1000);
  index.iw->termPartitionMinBytes = 1;
  index.iw->termPartitionMinRangeBytes = 1;
  index.iw->termPartitionMaxRanges = 4;
  TestField body(index, "body");
  for (int32_t group = 0; group < 4; group++) {
    body.startIndexing();
    std::string value;
    int32_t termCount = survivingGroups.size() != 0 && group == 0 ? 1 : 64;
    for (int32_t i = 0; i < termCount; i++) {
      // Keep each group's terms together after analysis so deleting a
      // group can empty an entire planned partition.
      std::string term = std::format("t{:03}", group * 64 + i);
      value += term + " " + term + " ";
    }
    body.add(0, value);
    body.add(1, "");
    if (!std::ranges::contains(survivingGroups, group)) index.deleteDoc(0);
    index.flush();
  }
  index.iw->mergeSegments();
  index.initReader();
  EXPECT_EQ(1u, index.reader->segments().size());
  return readBodyInfo(index.reader->segments()[0]);
}

// The checkout barriers hold all four range streams at once; with fewer
// concurrent merge workers they would spin forever.
bool canHoldFourRanges() {
  return oneapi::tbb::this_task_arena::max_concurrency() >= 4;
}

} // namespace

TEST(TermPartitionMergeTest, RoundTripSeeksAndCascade) {
  auto schema = textSchema();
  TestIndex serial;
  TestIndex partitioned;
  buildSources(serial, schema, false);

  buildSources(partitioned, schema, true);

  Segment& serialSegment = serial.reader->segments()[0];
  Segment& partitionedSegment = partitioned.reader->segments()[0];
  const auto& serialInfo = readBodyInfo(serialSegment);
  const auto& partitionedInfo = readBodyInfo(partitionedSegment);
  ASSERT_EQ(1u, serialInfo.terms()->runs.size());
  auto runs = partitionedInfo.terms()->runs;
  ASSERT_EQ(4u, runs.size());
  EXPECT_FALSE(partitionedInfo.segFlags & FieldSegmentFlags::MAY_HAVE_FITTED);
  for (size_t i = 0; i < runs.size(); ++i) {
    auto& run = runs[i];
    ASSERT_TRUE(run.docs && run.positions);
    EXPECT_EQ(0u, run.docs.location.filenum());
    EXPECT_EQ(0u, run.positions.location.filenum());
    if (i > 0) {
      EXPECT_LT(runs[i - 1].dictionaryStart, run.dictionaryStart);
    }
  }

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
  int32_t serialRuns;
  bool serialMidFieldTail;
  int32_t partitionedRuns;
  bool partitionedMidFieldTail;
  FieldSnapshot cascadeOracle = mergePair(
      partitioned, right, false, serialRuns, serialMidFieldTail);
  FieldSnapshot cascade = mergePair(
      partitioned, right, true, partitionedRuns, partitionedMidFieldTail);
  EXPECT_EQ(1, serialRuns);
  EXPECT_FALSE(serialMidFieldTail);
  EXPECT_GT(partitionedRuns, 1);
  EXPECT_TRUE(partitionedMidFieldTail);
  EXPECT_EQ(cascadeOracle, cascade);
}

TEST(TermPartitionMergeTest, SizeCutsInSerialAndPartitionedMerges) {
  auto schema = textSchema();
  TestIndex left, right;
  buildSources(left, schema, false);
  buildSources(right, schema, false, 20);

  int32_t runs;
  bool partialTail;
  auto expected = mergePair(left, right, false, runs, partialTail);
  ASSERT_EQ(1, runs);

  auto saved = TextWriter::setRunSplitBytesForTests(0);
  auto restore = scope_guard([&] { TextWriter::setRunSplitBytesForTests(saved); });
  std::atomic<int32_t> partitions = 0;
  Signal::listen("termRangeStreamsCheckedOut", [&](void*, void*, void*) -> void* {
    partitions.fetch_add(1, std::memory_order_relaxed);
    return nullptr;
  });
  auto cleanup = scope_guard([] { Signal::unlisten("termRangeStreamsCheckedOut"); });

  EXPECT_EQ(expected, mergePair(left, right, false, runs, partialTail));
  EXPECT_EQ(0, partitions.load());
  EXPECT_EQ(Postings::termBlockCount(expected.terms.size()), runs);
  EXPECT_FALSE(partialTail);
  EXPECT_EQ(expected, mergePair(left, right, true, runs, partialTail));
  EXPECT_EQ(4, partitions.load());
  EXPECT_GT(runs, partitions.load());
}

TEST(TermPartitionMergeTest, SerialFlushWithMoreThan64Runs) {
  constexpr int32_t count = 70 * Postings::TERMS_BLOCK_SIZE;
  TestIndex index;
  auto schema = textSchema();
  test::reserveIncarnation(index.dir);
  index.iw = std::make_unique<IndexWriter>(index.snapshots, schema);
  auto saved = TextWriter::setRunSplitBytesForTests(0);
  auto restore = scope_guard([&] { TextWriter::setRunSplitBytesForTests(saved); });
  TestField body(index, "body");
  body.startIndexing();
  std::string value;
  for (int32_t i = 0; i < count; ++i) {
    auto term = std::format("t{:04}", i);
    value += term + " " + term + " ";
  }
  body.add(0, value);
  index.flush();
  index.initReader();

  auto& segment = index.reader->segments()[0];
  const auto& info = readBodyInfo(segment);
  ASSERT_EQ(70u, info.terms()->runs.size());
  auto snapshot = snapshotField(index.pool, segment);
  ASSERT_EQ((size_t)count, snapshot.terms.size());
  verifySeeks(index.pool, segment.postingsReader(), info, snapshot);
  TermsEnum terms(index.pool, segment.postingsReader(), info);
  EXPECT_EQ(70u, terms.estimateTermBlocks(7).size());
}

// Planning may sample every stride-th block: a sample keeps that block's
// leading term and the bytes up to the next sample, so cuts land on the same
// terms with the weights of the blocks they cover.
TEST(TermPartitionMergeTest, SampledBlockEstimatesCoverTheirBlocks) {
  auto schema = textSchema();
  TestIndex index;
  buildWideDictionarySources(index, schema);
  MemPool pool;
  Segment& segment = index.reader->segments()[0];
  const auto& info = readBodyInfo(segment);
  TermsEnum terms(pool, segment.postingsReader(), info);
  auto every = terms.estimateTermBlocks();
  ASSERT_GT(every.size(), 8u);
  for (size_t stride : {(size_t) 2, (size_t) 3, (size_t) 7, every.size()}) {
    auto sampled = terms.estimateTermBlocks((int32_t) stride);
    ASSERT_EQ((every.size() + stride - 1) / stride, sampled.size()) << stride;
    for (size_t i = 0; i < sampled.size(); i++) {
      EXPECT_EQ(every[i * stride].firstTerm, sampled[i].firstTerm) << stride;
      uint64_t covered = 0;
      for (size_t block = i * stride; block < std::min(every.size(), (i + 1) * stride); block++) {
        covered += every[block].bytes;
      }
      EXPECT_EQ(covered, sampled[i].bytes) << stride << " sample " << i;
    }
  }
}

TEST(TermPartitionMergeTest, CompactRoutingSkipsEmptyRuns) {
  TestIndex index;
  CapturedTermRouting captured;
  std::atomic<int32_t> plannedPartitions = 0;
  Signal::listen("termRangeStreamsCheckedOut", [&](void*, void*, void*) -> void* {
    plannedPartitions++;
    return nullptr;
  });
  auto cleanup = scope_guard([] { Signal::unlisten("termRangeStreamsCheckedOut"); });
  auto schema = textSchema();
  const auto& info = buildSparseRanges(index, schema, {0, 3});
  auto& reader = index.reader->segments()[0].postingsReader();
  auto files = segmentFileSizes(index.dir, reader.core().id(), reader.core().epoch());
  SegmentCore reopened(index.dir, reader.core().id(), reader.core().epoch());
  EncodedFieldRecord(*reopened.rootReader().fieldInfo(std::string_view(info.name)), reader.maxDoc()).check(reader.maxDoc(), files, true);
  expectRouting(captured, info);
  ASSERT_TRUE(captured.terms);
  EXPECT_GT(captured.terms->runs.size(), 1u);
  EXPECT_LT(captured.terms->runs.size(), (size_t)plannedPartitions.load());
}

TEST(TermPartitionMergeTest, RoutingExpansionValidatesTotals) {
  using namespace field_parts;
  // The first run ends in a partial block, and the second has a docs stream.
  const auto start = IndexFormat::HEADER.size();
  const std::array runs{TermRun{0, 0, 0, {}, {}}, TermRun{33, 2, 5, {{1, start}, 1}, {}}};
  Terms terms{{0, start}, 9, 4, 0, runs};
  MemPool pool;
  FieldRecordHeader header{.name = PackedTerm(pool, "f"), .type = FieldType::TEXT,
                           .flags = FieldType::INDEX_DOCS, .parts = {},
                           .docsWithField = 1, .indexedTotals = {34, 34, 34}, .bounds = {}};
  RAMFile file("");
  OutputStream out(&file);
  IndexFieldInfo fieldOnWrite;
  fieldOnWrite.header = header;
  fieldOnWrite.setPostings({header.indexedTotals, terms, 0, {}});
  FieldRecordCodec::encode(out, fieldOnWrite, 1);
  out.close();
  std::vector<char> bytes(file.size());
  file.copyTo(bytes.data());
  const std::array<uint64_t, 2> sizes{field_parts::termTrie(field_parts::termBlockOffsets(terms, 34), terms.trieBytes).end().offset(), start + 1};
  auto record = FieldRecordCodec::decode(bytes, 1, sizes, -1);
  ASSERT_NE(nullptr, record->terms());
  EXPECT_EQ(terms, *record->terms());

  // First run's count and length, dictionary location, then the second run's docs slice.
  const std::array tail{char{33}, char{5}, char{0}, (char)start, char{1}, (char)start, char{1}};
  ASSERT_TRUE(std::ranges::equal(std::span(bytes).last(tail.size()), tail));
  auto tailStart = bytes.size() - tail.size();
  for (auto [offset, value] : {std::pair{0, 34}, {1, 9}, {6, 0}}) {
    auto corrupt = bytes;
    corrupt[tailStart + offset] = value;
    EXPECT_THROW(FieldRecordCodec::decode(corrupt, 1, sizes, -1), std::runtime_error);
  }
}

TEST(TermPartitionMergeTest, EmptyRangesAreOmitted) {
  if (!canHoldFourRanges()) GTEST_SKIP() << "needs >= 4 TBB workers";
  auto schema = textSchema();
  TestIndex oneRange;
  std::atomic<int32_t> checkedOut = 0;
  const DecodedFieldRecord* oneInfo = nullptr;
  CapturedTermRouting captured;
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
    oneInfo = &buildSparseRanges(oneRange, schema, {0});
  }
  ASSERT_EQ(4, checkedOut.load(std::memory_order_relaxed));
  expectRouting(captured, *oneInfo);
  ASSERT_TRUE(oneInfo->terms());
  ASSERT_EQ(1u, oneInfo->terms()->runs.size());
  EXPECT_EQ(1u, baseFileCount(oneRange.dir,
                              oneRange.reader->segments()[0].segInfo.seg_id));
  FieldSnapshot snapshot = snapshotField(oneRange.pool,
                                         oneRange.reader->segments()[0]);
  ASSERT_EQ(1u, snapshot.terms.size());
  ASSERT_EQ(1u, snapshot.terms[0].postings.size());
  const auto& term = snapshot.terms.front().term;
  auto& core = oneRange.reader->segments()[0].segmentCore();
  SegmentCore reopened(oneRange.dir, core.id(), core.epoch());
  const auto* bounds = reopened.rootReader().fieldInfo("body")->dictionaryBounds();
  ASSERT_TRUE(bounds);
  EXPECT_EQ(term, std::string_view(bounds->lower));
  EXPECT_EQ(term, std::string_view(bounds->upper));
  EXPECT_EQ(field_parts::DictionaryBounds::Lower::EXACT, bounds->lowerMode);
  EXPECT_EQ(field_parts::DictionaryBounds::Upper::INCLUSIVE, bounds->upperMode);

  TestIndex allEmpty;
  const auto& emptyInfo = buildSparseRanges(allEmpty, schema, {});
  ASSERT_TRUE(emptyInfo.flags & FieldType::INDEX_DOCS);
  EXPECT_EQ((field_parts::IndexedTotals{0, 0, 0}), emptyInfo.indexedTotals());
  EXPECT_EQ(nullptr, emptyInfo.terms());
  TermsEnum emptyTerms(allEmpty.pool, allEmpty.reader->segments()[0].postingsReader(), emptyInfo);
  EXPECT_EQ(0, emptyTerms.numBlocks());
  EXPECT_FALSE(emptyTerms.nextTerm());
}

TEST(TermPartitionMergeTest, TinyBudgetStillPartitions) {
  auto schema = textSchema();
  IndexRamBudget budget(64);
  TestIndex index;
  buildSources(index, schema, true, 0, &budget);
  const auto& info = readBodyInfo(index.reader->segments()[0]);
  ASSERT_TRUE(info.terms());
  ASSERT_TRUE(info.flags & FieldType::INDEX_DOCS);
  EXPECT_GT(info.indexedTotals().numTerms, 0);
  EXPECT_GT(info.terms()->runs.size(), 1u);
}

// Every range and its term-block charge must be forced through a full budget.
// Completed ranges keep their term blocks charged until stitch, and everything
// is released by the time the merge returns.
TEST(TermPartitionMergeTest, PartitionsUnderOccupiedBudget) {
  auto schema = textSchema();
  TestIndex sources;
  constexpr int32_t termCount = 18'000;
  buildWideDictionarySources(sources, schema, termCount);

  std::vector<std::shared_ptr<SegmentCore>> readers;
  std::vector<LiveDocs*> liveDocs;
  for (auto& segment : sources.reader->segments()) {
    readers.push_back(segment.segmentCoreShared());
    liveDocs.push_back(nullptr);
    EXPECT_TRUE(segment.postingsReader().fieldInfo("body")->segFlags & FieldSegmentFlags::MAY_HAVE_FITTED);
  }

  int64_t cap = 2 * MergeCostModel::LIGHT_BYTES;
  IndexRamBudget budget(cap);
  [[maybe_unused]] auto blocker = budget.forceAcquire(cap);
  ASSERT_EQ(cap, budget.reservedBytes());
  RAMDir outputDir;
  PostingsWriter writer(outputDir, 901, 1, -1);
  StoredDictionaryRegistry dictionaries;
  dictionaries.setSchema(StoredDictionaryRegistry::prepareSchema(*schema));
  std::atomic<int64_t> maxCharged = 0;
  Signal::listen("termRangeStreamsCheckedOut", [&](void*, void*, void*) -> void* {
    int64_t charged = budget.reservedBytes() - cap;
    int64_t seen = maxCharged.load();
    while (charged > seen && !maxCharged.compare_exchange_weak(seen, charged)) {}
    return nullptr;
  });
  auto signalCleanup = luxir::scope_guard([]() {
    Signal::unlisten("termRangeStreamsCheckedOut");
  });
  SegmentMerger merger(readers, liveDocs, writer, budget, 1, 1, 4, *schema, dictionaries);
  merger.merge();
  writer.finish();
  EXPECT_EQ(cap, budget.reservedBytes());

  SegmentCore outputReaderCore(outputDir, 901, 1);
  auto& outputReader = outputReaderCore.rootReader();
  auto* fields = outputReader.fieldInfo("body");
  ASSERT_NE(nullptr, fields);
  const auto& info = *fields;
  ASSERT_GT(info.terms()->runs.size(), 1u);
  EXPECT_NE(0u, info.terms()->location.filenum());
  MemPool pool;
  TermsEnum terms(pool, outputReader, info);
  ASSERT_TRUE(terms.seek(wideTermName(termCount - 1)));
  EXPECT_EQ(8, terms.docFreq());
  // The last range to check out its streams sees every range's term blocks.
  int64_t termBytes = info.terms()->dictionaryBytes;
  EXPECT_GE(maxCharged.load() + (int64_t) MAX_ALIGN, termBytes);
  // Stitching keeps the marker that a range writer saw on its full-width terms.
  EXPECT_TRUE(fields->segFlags & FieldSegmentFlags::MAY_HAVE_FITTED);
}

TEST(TermPartitionMergeTest, DerivedRangeLimitExceedsEight) {
  auto schema = textSchema();
  TestIndex index;
  std::atomic<int32_t> partitions = 0;
  Signal::listen("termRangeStreamsCheckedOut", [&](void*, void*, void*) -> void* {
    partitions.fetch_add(1, std::memory_order_relaxed);
    return nullptr;
  });
  auto cleanup = scope_guard([] { Signal::unlisten("termRangeStreamsCheckedOut"); });
  buildSources(index, schema, true, 0, nullptr,
               MergeCostModel::DERIVED_TERM_RANGES);

  EXPECT_GT(partitions.load(), 8);
  EXPECT_LE(partitions.load(), MergeCostModel::maxTermRangesForStreams(
                                   MergeCostModel::STREAM_BUDGET_FLOOR));
}
