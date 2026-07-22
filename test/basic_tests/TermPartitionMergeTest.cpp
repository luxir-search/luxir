#include <gtest/gtest.h>

#include "solux/index/SegmentMerger.h"
#include "solux/reader/DocsEnum.h"
#include "solux/reader/PosEnum.h"
#include "solux/reader/FieldReader.h"
#include "solux/reader/TermsEnum.h"
#include "test/SegmentTest.h"
#include "test/TestIndex.h"

#include <array>
#include <string>
#include <vector>

using namespace solux;
using namespace solux::test;

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

void buildSources(TestIndex& index, const std::shared_ptr<Schema>& schema,
                  bool partitioned, int32_t termBase = 0,
                  IndexRamBudget* budget = nullptr) {
  index.iw = std::make_unique<IndexWriter>(
      index.dir, [schema] { return schema; }, budget);
  index.iw->mergePolicy->setMergeFactor(1000);
  if (partitioned) {
    index.iw->termPartitionMinBytes = 1;
    index.iw->termPartitionMinRangeBytes = 1;
    index.iw->termPartitionMaxRanges = 4;
  } else {
    // The unpartitioned oracle must stay serial even under the debug-build
    // tiny default thresholds.
    index.iw->termPartitionMinBytes = INT64_MAX;
  }

  TestField body(index, "body");
  for (int32_t segment = 0; segment < 4; segment++) {
    body.startIndexing();
    for (int32_t doc = 0; doc < 24; doc++) {
      std::string value = "common common";
      for (int32_t i = 0; i < 8; i++) {
        std::string term = termName(termBase + doc * 8 + i);
        value += " " + term + " " + term;
      }
      body.add(doc, value);
    }
    if ((segment & 1) == 0) index.deleteDoc(segment);
    index.flush();
  }
  index.iw->mergeSegments();
  index.initReader();
  ASSERT_EQ(1u, index.reader->segments().size());
}

SegFieldInfo readBodyInfo(MemPool& pool, Segment& segment) {
  FieldReader fields(pool, segment.postingsReader());
  EXPECT_TRUE(fields.seek("body"));
  SegFieldInfo info;
  fields.readFieldInfo(info);
  return info;
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
  FieldReader fields(pool, outputReader);
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
    snapshot.terms.push_back(std::move(term));
  }
  if (partitioned) verifySeeks(pool, outputReader, info, snapshot);
  return snapshot;
}

SegFieldInfo buildSparseRanges(TestIndex& index, const std::shared_ptr<Schema>& schema,
                               bool keepOneRange) {
  index.iw = std::make_unique<IndexWriter>(index.dir, [schema] { return schema; });
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
  buildSources(partitioned, schema, true);

  Segment& serialSegment = serial.reader->segments()[0];
  Segment& partitionedSegment = partitioned.reader->segments()[0];
  SegFieldInfo serialInfo = readBodyInfo(serial.pool, serialSegment);
  SegFieldInfo partitionedInfo = readBodyInfo(partitioned.pool, partitionedSegment);
  ASSERT_TRUE(serialInfo.rangeTableLoc.isNull());
  ASSERT_FALSE(partitionedInfo.rangeTableLoc.isNull());

  FieldSnapshot oracle = snapshotField(serial.pool, serialSegment);
  FieldSnapshot actual = snapshotField(partitioned.pool, partitionedSegment);
  EXPECT_EQ(oracle, actual);
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
  SegFieldInfo oneInfo = buildSparseRanges(oneRange, schema, true);
  ASSERT_FALSE(oneInfo.rangeTableLoc.isNull());
  InputStream table = oneRange.reader->segments()[0].postingsReader()
      .getInputStreamSeek(oneInfo.rangeTableLoc);
  const auto* header = reinterpret_cast<const TermRangeTableHeader*>(table.ptr());
  EXPECT_EQ(1u, header->nRanges);

  TestIndex allEmpty;
  SegFieldInfo emptyInfo = buildSparseRanges(allEmpty, schema, false);
  EXPECT_EQ(0, emptyInfo.nTerms);
  EXPECT_TRUE(emptyInfo.rangeTableLoc.isNull());
  EXPECT_TRUE(emptyInfo.termBlockIndexLoc.isNull());
}

TEST(TermPartitionMergeTest, BudgetFallbackStaysSerial) {
  auto schema = textSchema();
  IndexRamBudget budget(64);
  TestIndex index;
  buildSources(index, schema, true, 0, &budget);
  SegFieldInfo info = readBodyInfo(index.pool, index.reader->segments()[0]);
  EXPECT_GT(info.nTerms, 0);
  EXPECT_TRUE(info.rangeTableLoc.isNull());
}
