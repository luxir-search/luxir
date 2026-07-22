#include "gtest/gtest.h"
#include "solux/index/IndexWriter.h"
#include "solux/index/PostingsWriter.h"
#include "solux/reader/FieldReader.h"
#include "solux/reader/PostingsReader.h"
#include "solux/reader/PosEnum.h"
#include "solux/schema/Schema.h"
#include "test/SoluxTest.h"
#include "test/TestIndex.h"

#include <array>
#include <algorithm>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

using namespace solux;
using namespace solux::test;

class TextMergeFuzzTest : public SoluxTest {
protected:
  struct Posting {
    int32_t doc;
    std::vector<int32_t> positions;
  };

  struct DocSpec {
    std::string body = "base";
    bool deleted = false;
  };

  using Oracle = std::map<std::string, std::vector<Posting>>;

  static void append(std::string& body, std::string_view term, int32_t count = 1) {
    for (int32_t i = 0; i < count; i++) {
      if (!body.empty()) body.push_back(' ');
      body.append(term);
    }
  }

  static void modelDoc(Oracle& oracle, int32_t doc, std::string_view body) {
    std::map<std::string, std::vector<int32_t>> docTerms;
    int32_t position = 0;
    size_t start = 0;
    while (start < body.size()) {
      while (start < body.size() && body[start] == ' ') start++;
      if (start == body.size()) break;
      size_t end = body.find(' ', start);
      if (end == std::string_view::npos) end = body.size();
      docTerms[std::string(body.substr(start, end - start))].push_back(position++);
      start = end;
    }
    for (auto& [term, positions] : docTerms) {
      oracle[term].push_back({doc, std::move(positions)});
    }
  }

  static std::shared_ptr<Schema> mixedSchema() {
    auto schema = std::make_shared<Schema>();
    schema->fieldTypeMap["pos"] = std::make_shared<TextFieldType>(
        "pos", FieldType::INDEX_DOCS_FREQS_POSITIONS);
    schema->fieldTypeMap["freq"] = std::make_shared<TextFieldType>(
        "freq", FieldType::INDEX_DOCS_FREQS);
    schema->fieldTypeMap["docs"] = std::make_shared<TextFieldType>(
        "docs", FieldType::INDEX_DOCS);
    return schema;
  }

  static int64_t expectedTtf(const std::vector<Posting>& postings, bool hasFreqs) {
    if (!hasFreqs) return (int64_t) postings.size();
    int64_t total = 0;
    for (const auto& posting : postings) total += (int64_t) posting.positions.size();
    return total;
  }

  void verifyField(TestIndex& index, std::string_view field, const Oracle& oracle,
                   bool hasFreqs, bool hasPositions) {
    ASSERT_EQ(1u, index.reader->segments().size());
    Segment& segment = index.reader->segments()[0];
    FieldReader fieldReader(index.pool, segment.postingsReader());
    ASSERT_TRUE(fieldReader.seek(field));
    SegFieldInfo fieldInfo;
    fieldReader.readFieldInfo(fieldInfo);
    ASSERT_EQ(hasFreqs, FieldType::hasFreqs(fieldInfo.flags));
    ASSERT_EQ(hasPositions, FieldType::hasPositions(fieldInfo.flags));
    ASSERT_FALSE(fieldInfo.rangeTableLoc.isNull());

    int64_t sumDf = 0;
    int64_t sumTtf = 0;
    TermsEnum terms(index.pool, segment.postingsReader(), fieldInfo);
    for (const auto& [term, postings] : oracle) {
      ASSERT_TRUE(terms.nextTerm()) << field << " missing " << term;
      ASSERT_EQ(term, std::string_view(terms.term())) << field;
      DocsEnum docs(terms);
      std::unique_ptr<PosEnum> posEnum;
      if (hasPositions) {
        posEnum = std::make_unique<PosEnum>(docs);
      }
      ASSERT_EQ((int32_t) postings.size(), docs.numDocs()) << field << " " << term;
      int64_t ttf = expectedTtf(postings, hasFreqs);
      ASSERT_EQ(ttf, docs.totalTermFreq()) << field << " " << term;
      if (hasPositions && ttf == 1) {
        const auto& state = terms.postingsState();
        ASSERT_EQ(state.docsStart, state.docsEnd) << term;
      }

      for (const auto& posting : postings) {
        ASSERT_EQ(posting.doc, docs.nextDoc()) << field << " " << term;
        int32_t tf = hasFreqs ? (int32_t) posting.positions.size() : 1;
        ASSERT_EQ(tf, docs.termFreq()) << field << " " << term << " doc " << posting.doc;
        if (hasPositions) {
          posEnum->startPositions();
          for (int32_t position : posting.positions) {
            ASSERT_EQ(position, posEnum->nextPosition())
                << term << " doc " << posting.doc;
          }
          ASSERT_EQ(PosEnum::END, posEnum->nextPosition());
        }
      }
      ASSERT_EQ(DocsEnum::END, docs.nextDoc()) << field << " " << term;
      sumDf += (int64_t) postings.size();
      sumTtf += ttf;
    }
    ASSERT_FALSE(terms.nextTerm()) << field;
    ASSERT_EQ(sumDf, fieldInfo.sumDocFreq) << field;
    ASSERT_EQ(sumTtf, fieldInfo.sumTotalTermFreq) << field;
  }

  void buildOracle(TestIndex& index, const std::vector<std::string>& bodies,
                   Oracle& oracle, int32_t& nextDoc) {
    nextDoc = 0;
    std::vector<Segment*> sources;
    for (Segment& segment : index.reader->segments()) sources.push_back(&segment);
    std::sort(sources.begin(), sources.end(), [](const Segment* a, const Segment* b) {
      return a->segInfo.seg_id < b->segInfo.seg_id;
    });
    for (Segment* source : sources) {
      Segment& segment = *source;
      std::vector<int32_t> sourceIds((size_t) segment.maxDoc(), -1);
      FieldReader fieldReader(index.pool, segment.postingsReader());
      ASSERT_TRUE(fieldReader.seek("pos"));
      SegFieldInfo fieldInfo;
      fieldReader.readFieldInfo(fieldInfo);
      TermsEnum terms(index.pool, segment.postingsReader(), fieldInfo);
      while (terms.nextTerm()) {
        std::string_view term(terms.term());
        if (!term.starts_with("uid")) continue;
        int32_t sourceId = (int32_t) std::stoi(std::string(term.substr(3)));
        DocsEnum docs(terms);
        int32_t localDoc = docs.nextDoc();
        ASSERT_NE(DocsEnum::END, localDoc);
        ASSERT_EQ(-1, sourceIds[(size_t) localDoc]);
        sourceIds[(size_t) localDoc] = sourceId;
        ASSERT_EQ(DocsEnum::END, docs.nextDoc());
      }

      LiveDocs* liveDocs = segment.liveDocs();
      for (int32_t localDoc = 0; localDoc < segment.maxDoc(); localDoc++) {
        ASSERT_NE(-1, sourceIds[(size_t) localDoc]);
        if (liveDocs && !liveDocs->bitset().get(localDoc)) continue;
        int32_t sourceId = sourceIds[(size_t) localDoc];
        ASSERT_LT(sourceId, (int32_t) bodies.size());
        modelDoc(oracle, nextDoc++, bodies[(size_t) sourceId]);
      }
    }
  }

public:
  void addSegment(TestIndex& index, TestField& pos, TestField& freq, TestField& docs,
                  std::vector<DocSpec> segment, std::vector<std::string>& bodies) {
    for (auto& doc : segment) {
      append(doc.body, "uid" + std::to_string(bodies.size()));
      bodies.push_back(doc.body);
    }
    pos.startIndexing();
    freq.startIndexing();
    docs.startIndexing();
    for (int32_t doc = 0; doc < (int32_t) segment.size(); doc++) {
      pos.add(doc, segment[(size_t) doc].body);
      freq.add(doc, segment[(size_t) doc].body);
      docs.add(doc, segment[(size_t) doc].body);
    }
    for (int32_t doc = 0; doc < (int32_t) segment.size(); doc++) {
      if (segment[(size_t) doc].deleted) index.deleteDoc(doc);
    }
    index.flush();
  }
};

TEST_F(TextMergeFuzzTest, bulkPositionDeltaShapes) {
  auto schema = mixedSchema();
  TestIndex index;
  index.iw = std::make_unique<IndexWriter>(index.dir, [schema] { return schema; });
  index.iw->termPartitionMinBytes = 1;
  index.iw->termPartitionMinRangeBytes = 1;
  index.iw->termPartitionMaxRanges = 4;
  TestField pos(index, "pos");
  TestField freq(index, "freq");
  TestField docs(index, "docs");
  Oracle oracle;
  std::vector<std::string> bodies;

  const std::array<int32_t, 7> boundaries = {1, 2, 127, 128, 129, 255, 256};
  std::vector<DocSpec> boundaryDocs(260);
  for (int32_t n : boundaries) {
    std::string dfTerm = "df" + std::to_string(n);
    for (int32_t doc = 0; doc < n; doc++) append(boundaryDocs[(size_t) doc].body, dfTerm);
    append(boundaryDocs[0].body, "ttf" + std::to_string(n), n);
  }
  append(boundaryDocs[0].body, "wide");
  append(boundaryDocs[0].body, "gap", 140);
  append(boundaryDocs[0].body, "wide");
  addSegment(index, pos, freq, docs, std::move(boundaryDocs), bodies);

  const std::array<int32_t, 3> phase1 = {1, 126, 2};
  const std::array<int32_t, 3> phase127 = {127, 2, 1};
  const std::array<int32_t, 3> smallSources = {63, 64, 2};
  for (int32_t source = 0; source < 3; source++) {
    std::vector<DocSpec> segment(1);
    append(segment[0].body, "phase1", phase1[(size_t) source]);
    append(segment[0].body, "phase127", phase127[(size_t) source]);
    append(segment[0].body, "small_sources", smallSources[(size_t) source]);
    addSegment(index, pos, freq, docs, std::move(segment), bodies);
  }

  {
    std::vector<DocSpec> segment(2);
    append(segment[0].body, "pulse_2_to_1");
    append(segment[1].body, "pulse_2_to_1");
    segment[0].deleted = true;
    addSegment(index, pos, freq, docs, std::move(segment), bodies);
  }
  {
    std::vector<DocSpec> segment(2);
    append(segment[0].body, "pulse_1_to_0");
    segment[0].deleted = true;
    addSegment(index, pos, freq, docs, std::move(segment), bodies);
  }
  for (int32_t source = 0; source < 3; source++) {
    std::vector<DocSpec> segment(2);
    append(segment[0].body, "multi_source_pulse");
    segment[0].deleted = source != 2;
    addSegment(index, pos, freq, docs, std::move(segment), bodies);
  }
  {
    std::vector<DocSpec> segment(1);
    append(segment[0].body, "single_doc_nonpulse", 2);
    addSegment(index, pos, freq, docs, std::move(segment), bodies);
  }

  {
    std::vector<DocSpec> segment(129);
    for (auto& doc : segment) append(doc.body, "block_to_tail");
    for (int32_t doc = 0; doc < 10; doc++) segment[(size_t) doc].deleted = true;
    addSegment(index, pos, freq, docs, std::move(segment), bodies);
  }

  {
    // Seed output phase 1 so the following source blocks split 127/1 at
    // output doc-block boundaries.
    std::vector<DocSpec> segment(1);
    append(segment[0].body, "dense_batch", 5);
    addSegment(index, pos, freq, docs, std::move(segment), bodies);
  }
  {
    std::vector<DocSpec> segment(300);
    for (auto& doc : segment) append(doc.body, "dense_batch", 5);
    addSegment(index, pos, freq, docs, std::move(segment), bodies);
  }
  {
    // Only the middle source postings block is dirty; the clean blocks on
    // either side use the batch path in the same term merge.
    std::vector<DocSpec> segment(384);
    for (auto& doc : segment) append(doc.body, "mixed_clean_dirty_blocks", 5);
    segment[150].deleted = true;
    addSegment(index, pos, freq, docs, std::move(segment), bodies);
  }

  const std::array<std::string_view, 7> patterns = {
      "leading", "middle", "trailing", "alternating", "contiguous", "all", "all_but_one"};
  for (std::string_view pattern : patterns) {
    std::vector<DocSpec> segment(260);
    std::string term = "delete_" + std::string(pattern);
    for (int32_t doc = 0; doc < (int32_t) segment.size(); doc++) {
      append(segment[(size_t) doc].body, term);
      if (pattern == "leading") segment[(size_t) doc].deleted = doc < 20;
      if (pattern == "middle") segment[(size_t) doc].deleted = doc >= 120 && doc < 140;
      if (pattern == "trailing") segment[(size_t) doc].deleted = doc >= 240;
      if (pattern == "alternating") segment[(size_t) doc].deleted = (doc & 1) == 0;
      if (pattern == "contiguous") {
        segment[(size_t) doc].deleted = doc >= 126 && doc <= 130;
      }
      if (pattern == "all") segment[(size_t) doc].deleted = true;
      if (pattern == "all_but_one") segment[(size_t) doc].deleted = doc != 127;
    }
    addSegment(index, pos, freq, docs, std::move(segment), bodies);
  }

  {
    std::vector<DocSpec> segment(260);
    for (int32_t doc = 0; doc < (int32_t) segment.size(); doc++) {
      if ((doc & 1) == 0) append(segment[(size_t) doc].body, "delete_nonmatch");
      segment[(size_t) doc].deleted = (doc & 1) != 0;
    }
    addSegment(index, pos, freq, docs, std::move(segment), bodies);
  }

  for (int32_t source = 0; source < 5; source++) {
    int32_t count = rng.rint(20, 181);
    std::vector<DocSpec> segment((size_t) count);
    for (int32_t doc = 0; doc < count; doc++) {
      int32_t terms = rng.rint(1, 5);
      for (int32_t i = 0; i < terms; i++) {
        std::string term = "fuzz" + std::to_string(rng.rint(0, 8));
        append(segment[(size_t) doc].body, term, rng.rint(1, 6));
      }
      segment[(size_t) doc].deleted = rng.rint(0, 5) == 0;
    }
    if (source == 0) {
      append(segment[0].body, "fuzz_wide");
      append(segment[0].body, "fuzz_gap", 130);
      append(segment[0].body, "fuzz_wide");
    }
    addSegment(index, pos, freq, docs, std::move(segment), bodies);
  }

  index.initReader();
  ASSERT_GT(index.reader->segments().size(), 1u);
  index.iw->mergeSegments();
  index.initReader();
  int32_t nextDoc;
  buildOracle(index, bodies, oracle, nextDoc);
  ASSERT_EQ(nextDoc, index.reader->maxDoc());
  ASSERT_EQ(oracle.end(), oracle.find("pulse_1_to_0"));
  ASSERT_EQ(oracle.end(), oracle.find("delete_all"));
  verifyField(index, "pos", oracle, true, true);
  verifyField(index, "freq", oracle, true, false);
  verifyField(index, "docs", oracle, false, false);
}

TEST_F(TextMergeFuzzTest, rawZeroAndWideDeltas) {
  RAMDir sourceDir;
  MemPool pool;
  PostingsWriter sourcePostings(sourceDir, 0, 1);
  {
    TextWriter writer(sourcePostings);
    auto& fieldInfo = sourcePostings.addField("pos");
    fieldInfo.type = FieldType::TEXT;
    fieldInfo.flags = FieldType::INDEX_DOCS_FREQS_POSITIONS;
    writer.startField(&fieldInfo);
    TermRef term(pool, "term", 4);
    writer.startTerm(term);
    writer.startDoc(0);
    writer.addPositionDelta(1);
    writer.addPositionDelta(0);
    writer.addPositionDelta(300);
    writer.endDoc(0);
    writer.endTerm(term);
    writer.endField();
  }
  sourcePostings.finish();

  PostingsReader sourceReader(sourceDir, 0);
  FieldReader sourceFields(pool, sourceReader);
  ASSERT_TRUE(sourceFields.readNextField());
  SegFieldInfo sourceInfo;
  sourceFields.readFieldInfo(sourceInfo);
  TermsEnum sourceTerms(pool, sourceReader, sourceInfo);
  ASSERT_TRUE(sourceTerms.nextTerm());
  DocsEnum sourceDocs(sourceTerms);
  PosEnum sourcePositions(sourceDocs);
  ASSERT_EQ(0, sourceDocs.nextDoc());
  ASSERT_EQ(3, sourceDocs.termFreq());
  sourcePositions.startPositions();

  RAMDir targetDir;
  PostingsWriter targetPostings(targetDir, 1, 1);
  {
    TextWriter writer(targetPostings);
    auto& fieldInfo = targetPostings.addField("pos");
    fieldInfo.type = FieldType::TEXT;
    fieldInfo.flags = FieldType::INDEX_DOCS_FREQS_POSITIONS;
    writer.startField(&fieldInfo);
    TermRef term(pool, "term", 4);
    writer.startTerm(term);
    writer.startDoc(0);
    std::vector<int32_t> copied;
    for (;;) {
      auto deltas = sourcePositions.nextPositionDeltaSpan();
      if (deltas.empty()) break;
      copied.insert(copied.end(), deltas.begin(), deltas.end());
      writer.appendPositionDeltas(deltas);
    }
    ASSERT_EQ((std::vector<int32_t>{1, 0, 300}), copied);
    writer.endDoc(0, sourceDocs.termFreq());
    writer.endTerm(term);
    writer.endField();
  }
  targetPostings.finish();

  PostingsReader targetReader(targetDir, 1);
  FieldReader targetFields(pool, targetReader);
  ASSERT_TRUE(targetFields.readNextField());
  SegFieldInfo targetInfo;
  targetFields.readFieldInfo(targetInfo);
  TermsEnum targetTerms(pool, targetReader, targetInfo);
  ASSERT_TRUE(targetTerms.nextTerm());
  DocsEnum targetDocs(targetTerms);
  PosEnum targetPositions(targetDocs);
  ASSERT_EQ(0, targetDocs.nextDoc());
  ASSERT_EQ(3, targetDocs.termFreq());
  targetPositions.startPositions();
  ASSERT_EQ(0, targetPositions.nextPosition());
  ASSERT_EQ(0, targetPositions.nextPosition());
  ASSERT_EQ(300, targetPositions.nextPosition());
  ASSERT_EQ(PosEnum::END, targetPositions.nextPosition());
}
