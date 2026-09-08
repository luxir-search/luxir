// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <iterator>
#include <string>
#include <vector>

#include "gtest/gtest.h"
#include "test/LuxirTest.h"
#include "test/TestIndex.h"
#include "luxir/query/TermQuery.h"
#include "luxir/query/BooleanQuery.h"
#include "luxir/search/PostingsIntersection.h"
#include "luxir/search/Similarity.h"
#include "luxir/reader/PosEnum.h"

using namespace luxir;
using namespace luxir::test;

// Fuzz for postings-enum advance() and friends.
// Builds randomized indexes and replays random nextDoc()/advance()/position-read sequences against an
// independent model, checking doc id, term freq, and positions every step.
class DocsEnumAdvanceTest : public LuxirTest {
protected:
  struct Posting { int32_t docid; int32_t firstPos; int32_t tf; };

  static constexpr int VOCAB_SIZE = 40;  // t%9 sets density 1, 1/2, ... 1/256

  static int32_t impactTfForDoc(int32_t docid) {
    int32_t block = docid / Postings::DOCS_BLOCK_SIZE;
    return 1 + ((block * 7 + (docid % 3)) % 23);
  }

  static int32_t impactTokenCountForDoc(int32_t docid) {
    return impactTfForDoc(docid) + 3 + ((docid * 11) % 67);
  }

  static std::vector<int32_t> expectedBlockMaxTf(int32_t numDocs, bool hasFreqs) {
    int32_t numBlocks = (numDocs + Postings::DOCS_BLOCK_SIZE - 1) / Postings::DOCS_BLOCK_SIZE;
    std::vector<int32_t> expected(numBlocks, hasFreqs ? 0 : 1);
    if (!hasFreqs) {
      return expected;
    }
    for (int32_t doc = 0; doc < numDocs; doc++) {
      int32_t block = doc / Postings::DOCS_BLOCK_SIZE;
      expected[block] = std::max(expected[block], impactTfForDoc(doc));
    }
    return expected;
  }

  static std::vector<int32_t> expectedBlockMinNorm(int32_t numDocs, bool hasNorms) {
    int32_t numBlocks = (numDocs + Postings::DOCS_BLOCK_SIZE - 1) / Postings::DOCS_BLOCK_SIZE;
    std::vector<int32_t> expected(numBlocks, hasNorms ? 255 : 0);
    if (!hasNorms) {
      return expected;
    }
    for (int32_t doc = 0; doc < numDocs; doc++) {
      int32_t block = doc / Postings::DOCS_BLOCK_SIZE;
      int32_t norm = SmallFloat::intToByte4(impactTokenCountForDoc(doc));
      expected[block] = std::min(expected[block], norm);
    }
    return expected;
  }

  // Model frontiers over any span size: DOCS_BLOCK_SIZE for L0 block headers,
  // DocsEnumMeta::L1_DOCS for L1 group headers.
  static DocsEnumMeta::ImpactFrontiers expectedSpanFrontiers(int32_t numDocs,
                                                         bool hasFreqs,
                                                         bool hasNorms,
                                                         int32_t spanDocs) {
    int32_t numSpans = (numDocs + spanDocs - 1) / spanDocs;
    DocsEnumMeta::ImpactFrontiers expected;
    expected.offsets.reserve((size_t) numSpans + 1);
    std::array<int32_t, 256> maxTfPerNorm;
    for (int32_t span = 0; span < numSpans; span++) {
      expected.offsets.push_back((int32_t) expected.tfs.size());
      if (!(hasFreqs && hasNorms)) {
        continue;
      }
      maxTfPerNorm.fill(0);
      int32_t start = span * spanDocs;
      int32_t end = std::min(start + spanDocs, numDocs);
      for (int32_t doc = start; doc < end; doc++) {
        int32_t norm = SmallFloat::intToByte4(impactTokenCountForDoc(doc));
        maxTfPerNorm[(size_t) norm] = std::max(maxTfPerNorm[(size_t) norm], impactTfForDoc(doc));
      }
      int32_t runningMaxTf = 0;
      for (int32_t norm = 0; norm < 256; norm++) {
        int32_t tf = maxTfPerNorm[(size_t) norm];
        if (tf > runningMaxTf) {
          expected.norms.push_back(norm);
          expected.tfs.push_back(tf);
          runningMaxTf = tf;
        }
      }
    }
    expected.offsets.push_back((int32_t) expected.tfs.size());
    return expected;
  }

  static DocsEnumMeta::ImpactFrontiers expectedRawSpanFrontiers(const std::vector<uint8_t>& norms,
                                                            const std::vector<int32_t>& tfs,
                                                            int32_t spanDocs) {
    assert(norms.size() == tfs.size());
    int32_t numDocs = (int32_t) tfs.size();
    int32_t numSpans = (numDocs + spanDocs - 1) / spanDocs;
    DocsEnumMeta::ImpactFrontiers expected;
    expected.offsets.reserve((size_t) numSpans + 1);
    std::array<int32_t, 256> maxTfPerNorm;
    for (int32_t span = 0; span < numSpans; span++) {
      expected.offsets.push_back((int32_t) expected.tfs.size());
      maxTfPerNorm.fill(0);
      int32_t start = span * spanDocs;
      int32_t end = std::min(start + spanDocs, numDocs);
      for (int32_t doc = start; doc < end; doc++) {
        int32_t norm = (int32_t) norms[(size_t) doc];
        maxTfPerNorm[(size_t) norm] = std::max(maxTfPerNorm[(size_t) norm], tfs[(size_t) doc]);
      }
      int32_t runningMaxTf = 0;
      for (int32_t norm = 0; norm < 256; norm++) {
        int32_t tf = maxTfPerNorm[(size_t) norm];
        if (tf > runningMaxTf) {
          expected.norms.push_back(norm);
          expected.tfs.push_back(tf);
          runningMaxTf = tf;
        }
      }
    }
    expected.offsets.push_back((int32_t) expected.tfs.size());
    return expected;
  }

  static std::vector<int32_t> expectedRawBlockMaxTf(const std::vector<int32_t>& tfs) {
    int32_t numBlocks = ((int32_t) tfs.size() + Postings::DOCS_BLOCK_SIZE - 1)
                        / Postings::DOCS_BLOCK_SIZE;
    std::vector<int32_t> expected(numBlocks, 0);
    for (int32_t doc = 0; doc < (int32_t) tfs.size(); doc++) {
      int32_t block = doc / Postings::DOCS_BLOCK_SIZE;
      expected[(size_t) block] = std::max(expected[(size_t) block], tfs[(size_t) doc]);
    }
    return expected;
  }

  static std::vector<int32_t> expectedRawBlockMinNorm(const std::vector<uint8_t>& norms) {
    int32_t numBlocks = ((int32_t) norms.size() + Postings::DOCS_BLOCK_SIZE - 1)
                        / Postings::DOCS_BLOCK_SIZE;
    std::vector<int32_t> expected(numBlocks, 255);
    for (int32_t doc = 0; doc < (int32_t) norms.size(); doc++) {
      int32_t block = doc / Postings::DOCS_BLOCK_SIZE;
      expected[(size_t) block] = std::min(expected[(size_t) block],
                                          (int32_t) norms[(size_t) doc]);
    }
    return expected;
  }

  void writeRawPositionsImpactTerm(RAMDir& dir, MemPool& pool,
                                   const std::vector<uint8_t>& norms,
                                   const std::vector<int32_t>& tfs,
                                   std::string_view term = "hot") {
    ASSERT_FALSE(tfs.empty());
    ASSERT_EQ(norms.size(), tfs.size());
    PostingsWriter postingsWriter(dir, 0, (int32_t) tfs.size() + Postings::DOCS_BLOCK_SIZE + 100);
    {
      TextWriter writer(postingsWriter);
      auto& finfo = postingsWriter.addField("f");
      finfo.type = FieldType::TEXT;
      finfo.flags = FieldType::INDEX_DOCS_FREQS_POSITIONS;
      writer.startField(&finfo);
      writer.setNorms(TextNormsView(norms, nullptr));

      TermRef tref(pool, term.data(), (uint32_t) term.size());
      writer.startTerm(tref);
      for (int32_t doc = 0; doc < (int32_t) tfs.size(); doc++) {
        writer.addDoc(doc, tfs[(size_t) doc]);
      }
      writer.endTerm(tref);
      writer.endField();
    }
    postingsWriter.finish();
  }

  static std::vector<char> encodeTfBytes(const std::vector<uint32_t>& tfs, uint32_t width) {
    std::vector<char> bytes;
    bytes.reserve(tfs.size() * width);
    for (uint32_t tf : tfs) {
      if (width == 2) {
        bytes.push_back((char) tf);
        bytes.push_back((char) (tf >> 8));
      } else {
        bytes.push_back((char) tf);
        bytes.push_back((char) (tf >> 8));
        bytes.push_back((char) (tf >> 16));
        bytes.push_back((char) (tf >> 24));
      }
    }
    return bytes;
  }

  static std::vector<int32_t> expectedGroupSpanImpacts(const std::vector<int32_t>& blockMaxTf) {
    int32_t numGroups = ((int32_t) blockMaxTf.size() + 31) / 32;
    std::vector<int32_t> expected;
    expected.reserve(numGroups);
    for (int32_t group = 0; group < numGroups; group++) {
      int32_t start = group * 32;
      int32_t end = std::min(start + 32, (int32_t) blockMaxTf.size());
      int32_t spanImpact = 0;
      for (int32_t block = start; block < end; block++) {
        spanImpact = std::max(spanImpact, blockMaxTf[block]);
      }
      expected.push_back(spanImpact);
    }
    return expected;
  }

  static std::vector<int32_t> expectedGroupSpanMinNorms(const std::vector<int32_t>& blockMinNorm) {
    int32_t numGroups = ((int32_t) blockMinNorm.size() + 31) / 32;
    std::vector<int32_t> expected;
    expected.reserve(numGroups);
    for (int32_t group = 0; group < numGroups; group++) {
      int32_t start = group * 32;
      int32_t end = std::min(start + 32, (int32_t) blockMinNorm.size());
      int32_t spanMinNorm = 255;
      for (int32_t block = start; block < end; block++) {
        spanMinNorm = std::min(spanMinNorm, blockMinNorm[block]);
      }
      expected.push_back(blockMinNorm.empty() ? 0 : spanMinNorm);
    }
    return expected;
  }

  static std::vector<int32_t> makeWordProbeDocs(int32_t fullBlocks) {
    std::vector<int32_t> docs;
    docs.reserve((size_t) fullBlocks * Postings::DOCS_BLOCK_SIZE);
    for (int32_t block = 0; block < fullBlocks; block++) {
      const int32_t docBase = block * 192;
      for (int32_t bit = 0; bit < 192; bit++) {
        if ((bit % 3) != 1) {
          docs.push_back(docBase + bit);
        }
      }
    }
    return docs;
  }

  static std::vector<int32_t> makeContiguousDocs(int32_t count) {
    std::vector<int32_t> docs;
    docs.reserve((size_t) count);
    for (int32_t doc = 0; doc < count; doc++) {
      docs.push_back(doc);
    }
    return docs;
  }

  static int32_t nextBlockBase(const std::vector<int32_t>& docs) {
    return docs.empty() ? 0 : docs.back() + 1;
  }

  static void appendWordBlock(std::vector<int32_t>& docs) {
    const int32_t base = nextBlockBase(docs);
    for (int32_t bit = 0; bit < 192; bit++) {
      if ((bit % 3) != 1) {
        docs.push_back(base + bit);
      }
    }
  }

  static void appendContiguousBlock(std::vector<int32_t>& docs) {
    const int32_t base = nextBlockBase(docs);
    for (int32_t i = 0; i < Postings::DOCS_BLOCK_SIZE; i++) {
      docs.push_back(base + i);
    }
  }

  static void appendPackedBlock(std::vector<int32_t>& docs) {
    const int32_t base = nextBlockBase(docs);
    for (int32_t i = 0; i < Postings::DOCS_BLOCK_SIZE; i++) {
      docs.push_back(base + i * 33);
    }
  }

  static int32_t modelCeil(const std::vector<int32_t>& docs, int32_t target) {
    auto it = std::lower_bound(docs.begin(), docs.end(), target);
    return it == docs.end() ? DocsEnumMeta::END : *it;
  }

  struct CheckpointTermModel {
    std::string term;
    std::vector<Posting> postings;
  };

  class CheckpointToggleGuard {
    bool savedCheckpoints;
    bool savedStats;

  public:
    CheckpointToggleGuard()
        : savedCheckpoints(DocsEnumMeta::disableL0CheckpointsForTests),
          savedStats(SkipStats::enabled) {
      DocsEnumMeta::disableL0CheckpointsForTests = false;
      SkipStats::enabled = false;
      SkipStats::reset();
    }

    ~CheckpointToggleGuard() {
      DocsEnumMeta::disableL0CheckpointsForTests = savedCheckpoints;
      SkipStats::enabled = savedStats;
      SkipStats::reset();
    }
  };

  class SkipStatsGuard {
    bool saved;

  public:
    SkipStatsGuard() : saved(SkipStats::enabled) {
      SkipStats::reset();
      SkipStats::enabled = true;
    }

    ~SkipStatsGuard() {
      SkipStats::enabled = saved;
      SkipStats::reset();
    }
  };

  static std::vector<int32_t> collectDocSet(DocSet& set, int32_t maxDoc) {
    std::vector<int32_t> docs;
    for (int32_t doc = 0; doc < maxDoc; doc++) {
      if (set.get(doc)) {
        docs.push_back(doc);
      }
    }
    return docs;
  }

  static std::string checkpointTermName(int32_t blocks) {
    std::string digits = std::to_string(blocks);
    return "checkpoint" + std::string(3 - digits.size(), '0') + digits;
  }

  static void writeCheckpointTerms(
      RAMDir& dir, MemPool& pool,
      std::vector<CheckpointTermModel>& models) {
    models.resize(0);
    int32_t maxDoc = 0;
    for (int32_t blocks : {5, 8, 9, 32, 40, 100}) {
      CheckpointTermModel model;
      model.term = checkpointTermName(blocks);
      model.postings.reserve(
          (size_t) blocks * Postings::DOCS_BLOCK_SIZE);
      for (int32_t block = 0; block < blocks; block++) {
        int32_t blockBase =
            block * (Postings::DOCS_BLOCK_SIZE + 1000)
            + (block / 11) * 50000;
        for (int32_t i = 0; i < Postings::DOCS_BLOCK_SIZE; i++) {
          int32_t ord = block * Postings::DOCS_BLOCK_SIZE + i;
          int32_t doc = blockBase + i;
          model.postings.push_back({doc, 1, 1 + (ord % 3)});
        }
      }
      maxDoc = std::max(maxDoc, model.postings.back().docid + 1);
      models.push_back(std::move(model));
    }

    std::vector<uint8_t> norms((size_t) maxDoc, 1);
    PostingsWriter postingsWriter(dir, 0, maxDoc);
    {
      TextWriter writer(postingsWriter);
      auto& finfo = postingsWriter.addField("f");
      finfo.type = FieldType::TEXT;
      finfo.flags = FieldType::INDEX_DOCS_FREQS_POSITIONS;
      writer.startField(&finfo);
      writer.setNorms(TextNormsView(norms, nullptr));
      for (const CheckpointTermModel& model : models) {
        TermRef term(pool, model.term.data(), (uint32_t) model.term.size());
        writer.startTerm(term);
        for (const Posting& posting : model.postings) {
          writer.startDoc(posting.docid);
          for (int32_t pos = 0; pos < posting.tf; pos++) {
            writer.addPositionDelta(pos == 0 ? 2 : 1);
          }
          writer.endDoc(posting.docid, posting.tf);
        }
        writer.endTerm(term);
      }
      writer.endField();
    }
    postingsWriter.finish();
  }

  static const Posting* checkpointPostingAtOrAfter(
      const CheckpointTermModel& model, int32_t target) {
    auto found = std::lower_bound(
        model.postings.begin(), model.postings.end(), target,
        [](const Posting& posting, int32_t value) {
          return posting.docid < value;
        });
    return found == model.postings.end() ? nullptr : &*found;
  }

  static std::vector<int32_t> checkpointTargets(
      const CheckpointTermModel& model) {
    int32_t blocks =
        (int32_t) model.postings.size() / Postings::DOCS_BLOCK_SIZE;
    std::vector<int32_t> targets;
    for (int32_t ord = 0;
         ord < std::min(
             (int32_t) model.postings.size(),
             2 * Postings::DOCS_BLOCK_SIZE);
         ord += 19) {
      targets.push_back(model.postings[(size_t) ord].docid);
    }
    for (int32_t block = 2; block < blocks; block += 7) {
      targets.push_back(
          model.postings[
              (size_t) block * Postings::DOCS_BLOCK_SIZE + 13].docid + 5);
    }
    for (int32_t block = TextWriter::kCheckpointStride;
         block < blocks; block += TextWriter::kCheckpointStride) {
      targets.push_back(
          model.postings[
              (size_t) block * Postings::DOCS_BLOCK_SIZE - 1].docid);
      targets.push_back(
          model.postings[
              (size_t) block * Postings::DOCS_BLOCK_SIZE].docid);
    }
    for (int32_t groupEnd = DocsEnumMeta::L1_PERIOD;
         groupEnd < blocks; groupEnd += DocsEnumMeta::L1_PERIOD) {
      targets.push_back(
          model.postings[
              (size_t) groupEnd * Postings::DOCS_BLOCK_SIZE - 1].docid + 1);
    }
    targets.push_back(model.postings.back().docid + 1);
    std::ranges::sort(targets);
    auto duplicates = std::ranges::unique(targets);
    targets.erase(duplicates.begin(), duplicates.end());
    return targets;
  }

  void writeRawSingleTerm(RAMDir& dir, MemPool& pool, std::string_view term,
                          const std::vector<int32_t>& docs) {
    ASSERT_FALSE(docs.empty());
    PostingsWriter postingsWriter(dir, 0, docs.back() + Postings::DOCS_BLOCK_SIZE + 100);
    {
      TextWriter writer(postingsWriter);
      auto& finfo = postingsWriter.addField("f");
      finfo.type = FieldType::TEXT;
      finfo.flags = FieldType::INDEX_DOCS_FREQS;
      writer.startField(&finfo);

      TermRef tref(pool, term.data(), (uint32_t) term.size());
      writer.startTerm(tref);
      for (int32_t doc : docs) {
        writer.addDoc(doc, 1 + (doc % 5));
      }
      writer.endTerm(tref);
      writer.endField();
    }
    postingsWriter.finish();
  }

  std::vector<int32_t> docsFromBits(const std::vector<uint64_t>& bits,
                                    int32_t bitsBase, int32_t upTo) {
    std::vector<int32_t> got;
    for (int32_t doc = bitsBase; doc < upTo; doc++) {
      int32_t index = doc - bitsBase;
      if ((bits[(size_t) index >> 6] & (1ULL << (index & 63))) != 0) {
        got.push_back(doc);
      }
    }
    return got;
  }

  std::vector<int32_t> modelWindow(const std::vector<int32_t>& docs,
                                   int32_t from, int32_t to) {
    std::vector<int32_t> want;
    auto it = std::lower_bound(docs.begin(), docs.end(), from);
    while (it != docs.end() && *it < to) {
      want.push_back(*it);
      ++it;
    }
    return want;
  }

  void appendIntoBitSetWindow(DocsOnlyEnum& denum, const std::vector<int32_t>& docs,
                              int32_t from, int32_t to,
                              std::vector<int32_t>& got) {
    ASSERT_LT(from, to);
    std::vector<uint64_t> bits((size_t) (to - from + 63) / 64, 0);
    denum.intoBitSet(bits, from, to);
    std::vector<int32_t> window = docsFromBits(bits, from, to);
    EXPECT_EQ(window, modelWindow(docs, from, to)) << "window [" << from << "," << to << ")";
    got.insert(got.end(), window.begin(), window.end());
  }

  void assertImpactHeaders(DocsOnlyEnum& denum, const std::vector<int32_t>& expectedBlockMaxTf,
                           const std::vector<int32_t>& expectedBlockMinNorm,
                           std::string_view label,
                           const DocsEnumMeta::ImpactFrontiers* expectedFrontiers = nullptr,
                           const DocsEnumMeta::ImpactFrontiers* expectedGroupFrontiers = nullptr) {
    std::vector<int32_t> blockMaxTf;
    std::vector<int32_t> groupSpanImpacts;
    std::vector<int32_t> blockLastDocs;
    std::vector<int32_t> blockMinNorm;
    std::vector<int32_t> groupSpanMinNorms;
    DocsEnumMeta::ImpactFrontiers frontiers;
    denum.readBlockMaxTf(blockMaxTf, &groupSpanImpacts, &blockLastDocs, &blockMinNorm,
                         &groupSpanMinNorms,
                         expectedFrontiers == nullptr ? nullptr : &frontiers);
    ASSERT_EQ(blockMaxTf, expectedBlockMaxTf) << label;
    ASSERT_EQ(groupSpanImpacts, expectedGroupSpanImpacts(expectedBlockMaxTf)) << label;
    ASSERT_EQ(blockMinNorm, expectedBlockMinNorm) << label;
    ASSERT_EQ(groupSpanMinNorms, expectedGroupSpanMinNorms(expectedBlockMinNorm)) << label;
    if (expectedFrontiers != nullptr) {
      ASSERT_EQ(frontiers.offsets, expectedFrontiers->offsets) << label;
      ASSERT_EQ(frontiers.tfs, expectedFrontiers->tfs) << label;
      ASSERT_EQ(frontiers.norms, expectedFrontiers->norms) << label;
    }

    // The group-header-only scan must agree with the full walk.
    DocsEnumMeta::GroupImpacts groups;
    denum.readGroupImpacts(groups);
    ASSERT_EQ(groups.spanMaxTfs, groupSpanImpacts) << label;
    ASSERT_EQ(groups.spanMinNorms, groupSpanMinNorms) << label;
    std::vector<int32_t> expectedGroupLastDocs;
    for (size_t block = 0; block < blockLastDocs.size(); block += DocsEnumMeta::L1_PERIOD) {
      size_t last = std::min(block + DocsEnumMeta::L1_PERIOD, blockLastDocs.size()) - 1;
      expectedGroupLastDocs.push_back(blockLastDocs[last]);
    }
    ASSERT_EQ(groups.lastDocs, expectedGroupLastDocs) << label;
    if (expectedGroupFrontiers != nullptr) {
      ASSERT_EQ(groups.frontiers.offsets, expectedGroupFrontiers->offsets) << label;
      ASSERT_EQ(groups.frontiers.tfs, expectedGroupFrontiers->tfs) << label;
      ASSERT_EQ(groups.frontiers.norms, expectedGroupFrontiers->norms) << label;
    }
  }

  void checkRawImpactHeaders(FieldType::flag_type flags, const std::vector<int32_t>& expectedBlockMaxTf,
                             const std::vector<int32_t>& expectedBlockMinNorm, int32_t numDocs,
                             std::string_view label,
                             const DocsEnumMeta::ImpactFrontiers* expectedFrontiers = nullptr,
                             const DocsEnumMeta::ImpactFrontiers* expectedGroupFrontiers = nullptr) {
    RAMDir dir;
    MemPool pool;
    PostingsWriter postingsWriter(dir, 0, numDocs + Postings::DOCS_BLOCK_SIZE + 100);
    {
      TextWriter writer(postingsWriter);
      auto& finfo = postingsWriter.addField("f");
      finfo.type = FieldType::TEXT;
      finfo.flags = flags;
      writer.startField(&finfo);

      TermRef term(pool, "hot", 3);
      writer.startTerm(term);
      for (int32_t doc = 0; doc < numDocs; doc++) {
        writer.addDoc(doc, impactTfForDoc(doc));
      }
      writer.endTerm(term);

      TermRef nextTerm(pool, "zzz", 3);
      writer.startTerm(nextTerm);
      for (int32_t doc = 0; doc < Postings::DOCS_BLOCK_SIZE + 3; doc++) {
        writer.addDoc(numDocs + 10 + doc, 31);
      }
      writer.endTerm(nextTerm);
      writer.endField();
    }
    postingsWriter.finish();

    PostingsReader reader(dir, 0);
    FieldReader fieldReader(reader);
    ASSERT_TRUE(fieldReader.readNextField()) << label;
    SegFieldInfo fieldInfo;
    fieldReader.readFieldInfo(fieldInfo);
    TermsEnum tenum(pool, reader, fieldInfo);
    ASSERT_TRUE(tenum.seek("hot")) << label;
    DocsOnlyEnum denum(tenum);
    assertImpactHeaders(denum, expectedBlockMaxTf, expectedBlockMinNorm, label, expectedFrontiers,
                        expectedGroupFrontiers);
  }

  void addImpactDocs(TestField& f, int32_t firstDoc, int32_t numDocs, int32_t globalBase) {
    std::string text;
    for (int32_t i = 0; i < numDocs; i++) {
      int32_t globalDoc = globalBase + i;
      int32_t tf = impactTfForDoc(globalDoc);
      int32_t tokenCount = impactTokenCountForDoc(globalDoc);
      text.clear();
      for (int32_t j = 0; j < tf; j++) {
        text += "hot ";
      }
      for (int32_t j = tf; j < tokenCount; j++) {
        text += "pad ";
      }
      f.add(firstDoc + i, text);
    }
  }

  void assertImpactHeadersForField(TestField& f, const std::vector<int32_t>& expectedBlockMaxTf,
                                   const std::vector<int32_t>& expectedBlockMinNorm,
                                   std::string_view label,
                                   const DocsEnumMeta::ImpactFrontiers* expectedFrontiers = nullptr,
                                   const DocsEnumMeta::ImpactFrontiers* expectedGroupFrontiers = nullptr) {
    TermsEnum tenum = f.createTermsEnum();
    ASSERT_TRUE(tenum.seek("hot")) << label;
    DocsOnlyEnum denum(tenum);
    assertImpactHeaders(denum, expectedBlockMaxTf, expectedBlockMinNorm, label, expectedFrontiers,
                        expectedGroupFrontiers);
  }

  void checkAdvanceWalk(TestIndex& testIndex, PostingsReader& reader, TermsEnum& tenum,
                        const std::vector<Posting>& model, const std::string& term,
                        int maxDoc, int indexIter, int walk) {
    SCOPED_TRACE(::testing::Message() << "indexIter=" << indexIter << " term=" << term << " walk=" << walk);

    DocsPosEnum denum(tenum);
    PosEnum posEnum(denum);
    ASSERT_EQ(denum.numDocs(), (int) model.size()) << term;

    auto byDoc = [](const Posting& p, int32_t v) { return p.docid < v; };
    size_t i = 0;       // model index of the next doc the enum will return
    int32_t cur = -1;
    for (;;) {
      size_t j;         // model index this op must land on
      if (rng.rbool()) {
        cur = denum.nextDoc();
        j = i;
      } else {
        int32_t span = rng.rbool() ? 200 : maxDoc;  // small step or far jump
        int32_t target = cur + 1 + (int32_t) rng.rint(0, span + 1);
        cur = denum.advance(target);
        j = (size_t) (std::lower_bound(model.begin(), model.end(), target, byDoc) - model.begin());
      }
      int32_t expected = j < model.size() ? model[j].docid : DocsEnumMeta::END;
      ASSERT_EQ(cur, expected) << term;
      if (cur == DocsEnumMeta::END) break;
      ASSERT_EQ(denum.termFreq(), model[j].tf) << term << " tf at doc " << cur;

      if (rng.rbool()) {
        posEnum.startPositions();
        int toRead = (int) rng.rint(0, model[j].tf + 1);
        for (int k = 0; k < toRead; k++) {
          ASSERT_EQ(posEnum.nextPosition(), model[j].firstPos + k) << term << " pos " << k << " doc " << cur;
        }
        if (toRead == model[j].tf) {
          ASSERT_EQ(posEnum.nextPosition(), PosEnum::END) << term << " pos end doc " << cur;
        }
      }
      i = j + 1;
    }
  }

  void runAdvanceFuzzIndex(int indexIter, int walksPerTerm) {
    // indexIter 0 ends exactly on a block boundary and is sized to span multiple L1
    // skip groups (L1 period is 32 blocks): 72 blocks => t0 (every doc) covers 2 full
    // groups + a partial third, so far advances exercise the L1 group step-over and
    // mid-density terms land their tails in later groups.  Later indexes use arbitrary sizes.
    const int N = indexIter == 0 ? 72 * Postings::DOCS_BLOCK_SIZE : 500 + (int) rng.rint(0, 2000);

    std::vector<std::vector<Posting>> model(VOCAB_SIZE);
    TestIndex testIndex;
    TestField f(testIndex, "body_w");
    f.startIndexing();
    std::string text;
    for (int d = 0; d < N; d++) {
      text.clear();
      int32_t posn = 0;
      for (int t = 0; t < VOCAB_SIZE; t++) {
        if (rng.rint(0, 1 << (t % 9)) != 0) continue;
        int tf = 1 + (int) rng.rint(0, 3);
        model[t].push_back({d, posn, tf});
        for (int k = 0; k < tf; k++) { text += 't'; text += std::to_string(t); text += ' '; }
        posn += tf;
      }
      f.add(d, text);  // t0 is present in every doc, so maxDoc == N
    }
    testIndex.flush();
    f.startReading();

    auto& reader = f.currentSegment()->postingsReader();
    for (int t = 0; t < VOCAB_SIZE; t++) {
      const auto& m = model[t];
      if (m.empty()) continue;
      std::string term = "t" + std::to_string(t);
      TermsEnum tenum = f.createTermsEnum();
      ASSERT_TRUE(tenum.seek(term)) << term;

      for (int walk = 0; walk < walksPerTerm; walk++) {
        checkAdvanceWalk(testIndex, reader, tenum, m, term, N, indexIter, walk);
      }
    }
  }
};


TEST_F(DocsEnumAdvanceTest, advanceFuzz) {
  int32_t indexIterations =
      1 + (int32_t)scaleTestDimension(1, 2);
  int32_t walksPerTerm =
      (int32_t)scaleTestDimension(3, 2);
  for (int32_t iter = 0; iter < indexIterations; iter++) {
    runAdvanceFuzzIndex(iter, walksPerTerm);
  }
}

// DOCS-only fields use the shorter skip payload (no cumTf) and an implicit term
// frequency of 1 -- a layout the positions-field fuzz above never exercises.
TEST_F(DocsEnumAdvanceTest, advanceDocsOnly) {
  const int N = 400;  // dense ids 0..399 => 3 full blocks + tail
  TestIndex testIndex;
  TestField f(testIndex, "tag_s");
  f.startIndexing();
  for (int d = 0; d < N; d++) f.add(d, std::string_view("x"));  // term "x" in every doc
  testIndex.flush();
  f.startReading();

  TermsEnum tenum = f.createTermsEnum();
  ASSERT_TRUE(tenum.seek("x"));
  ASSERT_EQ(DocsFreqEnum(tenum).numDocs(), N);

  // Random forward walk on one enum (resumes the skip cursor, never restarts at 0).
  DocsFreqEnum denum(tenum);
  int32_t cur = -1;
  while (cur != DocsEnumMeta::END) {
    int32_t target = cur + 1 + (int32_t) rng.rint(0, rng.rbool() ? 5 : N);
    cur = denum.advance(target);
    ASSERT_EQ(cur, target < N ? target : DocsEnumMeta::END) << "advance(" << target << ")";  // dense -> exact
    if (cur != DocsEnumMeta::END) { ASSERT_EQ(denum.termFreq(), 1); }
  }
}

TEST_F(DocsEnumAdvanceTest, advanceCrossesL1AndTailOnTrailerFreeSlice) {
  const int32_t N = DocsEnumMeta::L1_DOCS + Postings::DOCS_BLOCK_SIZE + 13;
  RAMDir dir;
  MemPool pool;
  PostingsWriter postingsWriter(dir, 0, N + Postings::DOCS_BLOCK_SIZE + 100);
  {
    TextWriter writer(postingsWriter);
    auto& finfo = postingsWriter.addField("f");
    finfo.type = FieldType::TEXT;
    finfo.flags = FieldType::INDEX_DOCS;
    writer.startField(&finfo);

    TermRef hot(pool, "hot", 3);
    writer.startTerm(hot);
    for (int32_t doc = 0; doc < N; doc++) {
      writer.addDoc(doc, 1);
    }
    writer.endTerm(hot);

    TermRef zzz(pool, "zzz", 3);
    writer.startTerm(zzz);
    for (int32_t doc = 0; doc < Postings::DOCS_BLOCK_SIZE + 7; doc++) {
      writer.addDoc(N + 10 + doc, 1);
    }
    writer.endTerm(zzz);
    writer.endField();
  }
  postingsWriter.finish();

  PostingsReader reader(dir, 0);
  FieldReader fieldReader(reader);
  ASSERT_TRUE(fieldReader.readNextField());
  SegFieldInfo fieldInfo;
  fieldReader.readFieldInfo(fieldInfo);
  TermsEnum tenum(pool, reader, fieldInfo);
  ASSERT_TRUE(tenum.seek("hot"));
  DocsFreqEnum denum(tenum);

  for (int32_t target : {0, 1, DocsEnumMeta::L1_DOCS - 1, DocsEnumMeta::L1_DOCS,
                         DocsEnumMeta::L1_DOCS + 1, N - 2, N - 1}) {
    ASSERT_EQ(denum.advance(target), target) << target;
    ASSERT_EQ(denum.termFreq(), 1) << target;
  }
  ASSERT_EQ(denum.advance(N), DocsEnumMeta::END);
}

TEST_F(DocsEnumAdvanceTest, trackedAndUntrackedSkipParityCrossesL1) {
  const int32_t nDocs = 2 * DocsEnumMeta::L1_DOCS + 257;
  TestIndex testIndex;
  TestField field(testIndex, "body_w");
  field.startIndexing();
  for (int32_t doc = 0; doc < nDocs; doc++) {
    const int32_t tf = 1 + doc % 3;
    field.add(doc, tf == 1 ? "hot" : tf == 2 ? "hot hot" : "hot hot hot");
  }
  testIndex.flush();
  field.startReading();

  const std::vector<int32_t> targets = {
    DocsEnumMeta::L1_DOCS + 17,
    DocsEnumMeta::L1_DOCS + 9 * Postings::DOCS_BLOCK_SIZE + 23,
    2 * DocsEnumMeta::L1_DOCS + 11,
    nDocs + 7
  };
  const bool saved = DocsEnumImpl::disableSlimL0WalkForTests;
  const bool savedWrapperTrims =
      DocsEnumImpl::disableProbeWrapperTrimsForTests;
  auto restore = scope_guard([&] {
    DocsEnumImpl::disableSlimL0WalkForTests = saved;
    DocsEnumImpl::disableProbeWrapperTrimsForTests = savedWrapperTrims;
  });

  auto untracked = [&](bool disableSlim) {
    DocsEnumImpl::disableSlimL0WalkForTests = disableSlim;
    TermsEnum terms = field.createTermsEnum();
    EXPECT_TRUE(terms.seek("hot"));
    DocsFreqEnum docs(terms);
    std::vector<int32_t> result;
    for (int32_t target : targets) {
      const int32_t doc = docs.advance(target);
      result.push_back(doc);
      if (doc != DocsEnumMeta::END) {
        result.push_back(docs.termFreq());
      }
    }
    return result;
  };

  auto tracked = [&](bool disableSlim) {
    DocsEnumImpl::disableSlimL0WalkForTests = disableSlim;
    TermsEnum terms = field.createTermsEnum();
    EXPECT_TRUE(terms.seek("hot"));
    DocsPosEnum docs(terms);
    PosEnum positions(docs);
    std::vector<int32_t> result;
    for (int32_t target : targets) {
      const int32_t doc = docs.advance(target);
      result.push_back(doc);
      if (doc == DocsEnumMeta::END) {
        continue;
      }
      const int32_t tf = docs.termFreq();
      result.push_back(tf);
      positions.startPositions();
      for (int32_t i = 0; i < tf; i++) {
        result.push_back(positions.nextPosition());
      }
      result.push_back(positions.nextPosition());
    }
    return result;
  };

  EXPECT_EQ(untracked(false), untracked(true));
  EXPECT_EQ(tracked(false), tracked(true));

  auto scoredProbe = [&](bool disableTrims) {
    DocsEnumImpl::disableProbeWrapperTrimsForTests = disableTrims;
    TermsEnum terms = field.createTermsEnum();
    EXPECT_TRUE(terms.seek("hot"));
    DocsFreqEnum docs(terms);
    std::vector<int32_t> result;
    for (int32_t target : targets) {
      const int32_t doc = docs.advanceScoredProbe(target);
      result.push_back(doc);
      if (doc != DocsEnumMeta::END) {
        result.push_back(docs.termFreq());
      }
    }
    return result;
  };

  EXPECT_EQ(scoredProbe(false), scoredProbe(true));
}

TEST_F(DocsEnumAdvanceTest, docsTierAdvanceProbesWordBlocksWithoutDecoding) {
  const std::vector<int32_t> docs = makeWordProbeDocs(3);
  RAMDir dir;
  MemPool pool;
  writeRawSingleTerm(dir, pool, "wordprobe", docs);

  PostingsReader reader(dir, 0);
  FieldReader fieldReader(reader);
  ASSERT_TRUE(fieldReader.readNextField());
  SegFieldInfo fieldInfo;
  fieldReader.readFieldInfo(fieldInfo);
  TermsEnum tenum(pool, reader, fieldInfo);
  ASSERT_TRUE(tenum.seek("wordprobe"));

  DocsOnlyEnum denum(tenum);

  bool savedStats = SkipStats::enabled;
  SkipStats::enabled = true;
  SkipStats::reset();

  int32_t cur = -1;
  for (int32_t target : {1, 63, 64, 127, 191, 192, 256, 319, 383, 384, 576}) {
    ASSERT_GT(target, cur);
    cur = denum.advance(target);
    ASSERT_EQ(cur, modelCeil(docs, target)) << "target=" << target;
    if (cur == DocsEnumMeta::END) {
      break;
    }
  }

  EXPECT_EQ(SkipStats::docBlocksDecoded, 0);
  EXPECT_GT(SkipStats::docsOnlyFreqBlocksSkipped, 0);
  EXPECT_GT(SkipStats::docsOnlyWordProbeAdvances, 6);
  SkipStats::enabled = savedStats;
  SkipStats::reset();
}

TEST_F(DocsEnumAdvanceTest, docsTierNextDocWalksResidentWordBlock) {
  const std::vector<int32_t> docs = makeWordProbeDocs(2);
  RAMDir dir;
  MemPool pool;
  writeRawSingleTerm(dir, pool, "wordprobe", docs);

  PostingsReader reader(dir, 0);
  FieldReader fieldReader(reader);
  ASSERT_TRUE(fieldReader.readNextField());
  SegFieldInfo fieldInfo;
  fieldReader.readFieldInfo(fieldInfo);
  TermsEnum tenum(pool, reader, fieldInfo);
  ASSERT_TRUE(tenum.seek("wordprobe"));

  DocsOnlyEnum denum(tenum);
  ASSERT_EQ(denum.advance(1), 2);
  ASSERT_EQ(denum.nextDoc(), 3);
  ASSERT_EQ(denum.advance(64), 65);
  ASSERT_EQ(denum.nextDoc(), 66);
  ASSERT_EQ(denum.advance(127), 128);
  ASSERT_EQ(denum.nextDoc(), 129);
  ASSERT_EQ(denum.advance(191), 191);
  ASSERT_EQ(denum.advance(192), 192);
}

// Regression for a stack-buffer-overflow in expandDocWords: it writes a
// branchless 8-wide row per bitset byte and only advances by the byte's
// popcount, so db[] needs padding for the LAST byte processed landing
// entirely past the true end. That happens whenever the block's final doc
// isn't within that byte's 8-bit range - here docs {0..126, 192} force a
// bitset block (docBase=0, lastDoc=192, numWords=4) whose last word (word 3)
// has only bit 0 set, so word 3's top byte (bits 56-63, the very last byte
// expandDocWords touches) has popcount 0.
TEST_F(DocsEnumAdvanceTest, docsTierBitsetBlockWithZeroPopcountTailByteDoesNotOverflow) {
  std::vector<int32_t> docs;
  for (int32_t d = 0; d <= 126; d++) docs.push_back(d);
  docs.push_back(192);
  ASSERT_EQ(docs.size(), (size_t) Postings::DOCS_BLOCK_SIZE);

  RAMDir dir;
  MemPool pool;
  writeRawSingleTerm(dir, pool, "tailzero", docs);

  PostingsReader reader(dir, 0);
  FieldReader fieldReader(reader);
  ASSERT_TRUE(fieldReader.readNextField());
  SegFieldInfo fieldInfo;
  fieldReader.readFieldInfo(fieldInfo);
  TermsEnum tenum(pool, reader, fieldInfo);
  ASSERT_TRUE(tenum.seek("tailzero"));

  DocsOnlyEnum denum(tenum);
  int32_t doc = -1;
  int32_t count = 0;
  while ((doc = denum.nextDoc()) != DocsEnumMeta::END) {
    ASSERT_EQ(doc, modelCeil(docs, doc));
    count++;
  }
  ASSERT_EQ(count, (int32_t) docs.size());
}

TEST_F(DocsEnumAdvanceTest, intoBitSetUsesResidentWordBlockAndStopsInsideWindow) {
  const std::vector<int32_t> docs = makeWordProbeDocs(2);
  RAMDir dir;
  MemPool pool;
  writeRawSingleTerm(dir, pool, "wordprobe", docs);

  PostingsReader reader(dir, 0);
  FieldReader fieldReader(reader);
  ASSERT_TRUE(fieldReader.readNextField());
  SegFieldInfo fieldInfo;
  fieldReader.readFieldInfo(fieldInfo);
  TermsEnum tenum(pool, reader, fieldInfo);
  ASSERT_TRUE(tenum.seek("wordprobe"));

  DocsOnlyEnum denum(tenum);

  bool savedStats = SkipStats::enabled;
  SkipStats::enabled = true;
  SkipStats::reset();

  ASSERT_EQ(denum.advance(64), 65);
  const int32_t from = 60;
  const int32_t to = 100;
  std::vector<uint64_t> bits((size_t) (to - from + 63) / 64, 0);
  denum.intoBitSet(bits, from, to);
  EXPECT_EQ(docsFromBits(bits, from, to), modelWindow(docs, 65, to));
  EXPECT_EQ(denum.docId(), 99);
  EXPECT_EQ(denum.nextDoc(), 101);

  EXPECT_GT(SkipStats::countBulkFillWordBlocks, 0);
  EXPECT_GT(SkipStats::docsOnlyWordProbeAdvances, 0);
  SkipStats::enabled = savedStats;
  SkipStats::reset();
}

TEST_F(DocsEnumAdvanceTest, intoBitSetPackedScatterClipsAndCrossesWords) {
  std::vector<int32_t> docs;
  docs.reserve((size_t) Postings::DOCS_BLOCK_SIZE);
  for (int32_t i = 0; i < Postings::DOCS_BLOCK_SIZE; i++) {
    docs.push_back(i * 33);
  }

  RAMDir dir;
  MemPool pool;
  writeRawSingleTerm(dir, pool, "packedscatter", docs);

  PostingsReader reader(dir, 0);
  FieldReader fieldReader(reader);
  ASSERT_TRUE(fieldReader.readNextField());
  SegFieldInfo fieldInfo;
  fieldReader.readFieldInfo(fieldInfo);
  TermsEnum tenum(pool, reader, fieldInfo);
  ASSERT_TRUE(tenum.seek("packedscatter"));

  bool savedStats = SkipStats::enabled;
  SkipStats::enabled = true;
  SkipStats::reset();

  DocsOnlyEnum denum(tenum);
  const int32_t from = 50;
  const int32_t to = 260;
  std::vector<uint64_t> bits((size_t) (to - from + 63) / 64, 0);
  denum.intoBitSet(bits, from, to);
  EXPECT_EQ(docsFromBits(bits, from, to), modelWindow(docs, from, to));
  EXPECT_EQ(denum.docId(), 231);
  EXPECT_EQ(denum.nextDoc(), 264);

  DocsOnlyEnum single(tenum);
  const int32_t singleFrom = 132;
  const int32_t singleTo = 133;
  std::vector<uint64_t> singleBits((size_t) (singleTo - singleFrom + 63) / 64, 0);
  single.intoBitSet(singleBits, singleFrom, singleTo);
  EXPECT_EQ(docsFromBits(singleBits, singleFrom, singleTo),
            modelWindow(docs, singleFrom, singleTo));
  EXPECT_EQ(single.docId(), 132);
  EXPECT_EQ(single.nextDoc(), 165);

  EXPECT_GT(SkipStats::docBlocksDecoded, 0);
  EXPECT_GT(SkipStats::countBulkFillBlocks, 1);
  EXPECT_EQ(SkipStats::countBulkFillWordBlocks, 0);
  SkipStats::enabled = savedStats;
  SkipStats::reset();
}

TEST_F(DocsEnumAdvanceTest, intoBitSetStraddleWordBlockStaysResidentAcrossWindows) {
  const std::vector<int32_t> docs = makeWordProbeDocs(1);
  RAMDir dir;
  MemPool pool;
  writeRawSingleTerm(dir, pool, "wordprobe", docs);

  PostingsReader reader(dir, 0);
  FieldReader fieldReader(reader);
  ASSERT_TRUE(fieldReader.readNextField());
  SegFieldInfo fieldInfo;
  fieldReader.readFieldInfo(fieldInfo);
  TermsEnum tenum(pool, reader, fieldInfo);
  ASSERT_TRUE(tenum.seek("wordprobe"));

  DocsOnlyEnum denum(tenum);

  bool savedStats = SkipStats::enabled;
  SkipStats::enabled = true;
  SkipStats::reset();

  std::vector<int32_t> got;
  appendIntoBitSetWindow(denum, docs, 0, 64, got);
  appendIntoBitSetWindow(denum, docs, 64, 95, got);
  appendIntoBitSetWindow(denum, docs, 95, 130, got);
  appendIntoBitSetWindow(denum, docs, 130, 160, got);
  appendIntoBitSetWindow(denum, docs, 160, 192, got);

  EXPECT_EQ(got, docs);
  EXPECT_EQ(denum.docId(), DocsEnumMeta::END);
  EXPECT_EQ(SkipStats::docBlocksDecoded, 0);
  EXPECT_EQ(SkipStats::docsOnlyFreqBlocksSkipped, 1);
  EXPECT_GE(SkipStats::countBulkFillWordBlocks, 5);
  SkipStats::enabled = savedStats;
  SkipStats::reset();
}

TEST_F(DocsEnumAdvanceTest, intoBitSetFirstStraddleWithNoEmitDoesNotConsume) {
  std::vector<int32_t> docs;
  docs.reserve((size_t) Postings::DOCS_BLOCK_SIZE);
  for (int32_t doc = 65; doc < 65 + Postings::DOCS_BLOCK_SIZE; doc++) {
    docs.push_back(doc);
  }

  RAMDir dir;
  MemPool pool;
  writeRawSingleTerm(dir, pool, "gapword", docs);

  PostingsReader reader(dir, 0);
  FieldReader fieldReader(reader);
  ASSERT_TRUE(fieldReader.readNextField());
  SegFieldInfo fieldInfo;
  fieldReader.readFieldInfo(fieldInfo);
  TermsEnum tenum(pool, reader, fieldInfo);
  ASSERT_TRUE(tenum.seek("gapword"));

  DocsOnlyEnum denum(tenum);

  bool savedStats = SkipStats::enabled;
  SkipStats::enabled = true;
  SkipStats::reset();

  std::vector<uint64_t> bits(1, 0);
  denum.intoBitSet(bits, 0, 64);
  EXPECT_TRUE(docsFromBits(bits, 0, 64).empty());
  // Nothing was reported, so the cursor must not rest on a live doc: a
  // docs-only successor scans from docid + 1 and would skip it.
  EXPECT_LT(denum.docId(), 0);
  EXPECT_EQ(denum.nextDoc(), 65);

  std::vector<int32_t> got;
  appendIntoBitSetWindow(denum, docs, 64, 100, got);
  EXPECT_EQ(got, modelWindow(docs, 64, 100));
  EXPECT_EQ(SkipStats::docBlocksDecoded, 0);
  EXPECT_EQ(SkipStats::docsOnlyFreqBlocksSkipped, 1);
  SkipStats::enabled = savedStats;
  SkipStats::reset();
}

// The packed-block twin of the word-block straddle test above: a no-emit
// window over a bit-packed landing block must also leave the cursor off live
// docs (the block-boundary peek decodes without publishing), and a jumped
// window base must reach later blocks through skip data without losing the
// landing block's first doc.
TEST_F(DocsEnumAdvanceTest, intoBitSetPackedStraddleWithNoEmitDoesNotConsume) {
  // Stride keeps the block far from contiguous/word-encodable: packed codec.
  std::vector<int32_t> docs;
  docs.reserve((size_t) (2 * Postings::DOCS_BLOCK_SIZE));
  for (int32_t i = 0; i < 2 * Postings::DOCS_BLOCK_SIZE; i++) {
    docs.push_back(65 + i * 997);
  }

  RAMDir dir;
  MemPool pool;
  writeRawSingleTerm(dir, pool, "packedgap", docs);

  PostingsReader reader(dir, 0);
  FieldReader fieldReader(reader);
  ASSERT_TRUE(fieldReader.readNextField());
  SegFieldInfo fieldInfo;
  fieldReader.readFieldInfo(fieldInfo);
  TermsEnum tenum(pool, reader, fieldInfo);
  ASSERT_TRUE(tenum.seek("packedgap"));

  DocsOnlyEnum denum(tenum);

  std::vector<uint64_t> bits(1, 0);
  denum.intoBitSet(bits, 0, 64);
  EXPECT_TRUE(docsFromBits(bits, 0, 64).empty());
  // The landing block was decoded but nothing was published or consumed.
  EXPECT_LT(denum.docId(), 0);

  // A jumped window base past the whole first block leaps via skip data and
  // must present the second block's docs exactly.
  int32_t secondBlockFirst = docs[(size_t) Postings::DOCS_BLOCK_SIZE];
  std::vector<int32_t> got;
  appendIntoBitSetWindow(denum, docs, secondBlockFirst - 1,
                         secondBlockFirst + 4000, got);
  EXPECT_EQ(got, modelWindow(docs, secondBlockFirst - 1,
                             secondBlockFirst + 4000));

  // The straddle-and-abandon in between must not have lost the first
  // block's docs for successor consumers on a fresh enum.
  DocsOnlyEnum denum2(tenum);
  std::vector<uint64_t> bits2(1, 0);
  denum2.intoBitSet(bits2, 0, 64);
  EXPECT_LT(denum2.docId(), 0);
  EXPECT_EQ(denum2.nextDoc(), 65);
}

TEST_F(DocsEnumAdvanceTest, intoBitSetStraddleContiguousBlockReachesBlockBoundary) {
  const std::vector<int32_t> docs = makeContiguousDocs(Postings::DOCS_BLOCK_SIZE);
  RAMDir dir;
  MemPool pool;
  writeRawSingleTerm(dir, pool, "runprobe", docs);

  PostingsReader reader(dir, 0);
  FieldReader fieldReader(reader);
  ASSERT_TRUE(fieldReader.readNextField());
  SegFieldInfo fieldInfo;
  fieldReader.readFieldInfo(fieldInfo);
  TermsEnum tenum(pool, reader, fieldInfo);
  ASSERT_TRUE(tenum.seek("runprobe"));

  DocsOnlyEnum denum(tenum);

  bool savedStats = SkipStats::enabled;
  SkipStats::enabled = true;
  SkipStats::reset();

  std::vector<int32_t> got;
  appendIntoBitSetWindow(denum, docs, 0, 17, got);
  appendIntoBitSetWindow(denum, docs, 17, 64, got);
  appendIntoBitSetWindow(denum, docs, 64, 127, got);
  appendIntoBitSetWindow(denum, docs, 127, 128, got);

  EXPECT_EQ(got, docs);
  EXPECT_EQ(denum.docId(), DocsEnumMeta::END);
  EXPECT_EQ(SkipStats::docBlocksDecoded, 0);
  EXPECT_EQ(SkipStats::docsOnlyFreqBlocksSkipped, 1);
  EXPECT_GE(SkipStats::countBulkFillWordBlocks, 4);
  SkipStats::enabled = savedStats;
  SkipStats::reset();
}

TEST_F(DocsEnumAdvanceTest, intoBitSetStraddleKeepsFreqStreamAlignedForFollowingBlocks) {
  std::vector<int32_t> docs = makeWordProbeDocs(2);
  appendPackedBlock(docs);
  RAMDir dir;
  MemPool pool;
  writeRawSingleTerm(dir, pool, "mixedprobe", docs);

  PostingsReader reader(dir, 0);
  FieldReader fieldReader(reader);
  ASSERT_TRUE(fieldReader.readNextField());
  SegFieldInfo fieldInfo;
  fieldReader.readFieldInfo(fieldInfo);
  TermsEnum tenum(pool, reader, fieldInfo);
  ASSERT_TRUE(tenum.seek("mixedprobe"));

  DocsOnlyEnum denum(tenum);

  bool savedStats = SkipStats::enabled;
  SkipStats::enabled = true;
  SkipStats::reset();

  std::vector<int32_t> got;
  appendIntoBitSetWindow(denum, docs, 0, 64, got);
  appendIntoBitSetWindow(denum, docs, 64, 150, got);
  appendIntoBitSetWindow(denum, docs, 150, 260, got);
  appendIntoBitSetWindow(denum, docs, 260, docs.back() + 1, got);

  EXPECT_EQ(got, docs);
  EXPECT_EQ(denum.docId(), DocsEnumMeta::END);
  EXPECT_EQ(SkipStats::docsOnlyFreqBlocksSkipped, 3);
  EXPECT_EQ(SkipStats::docBlocksDecoded, 1);
  SkipStats::enabled = savedStats;
  SkipStats::reset();
}

TEST_F(DocsEnumAdvanceTest, intoBitSetRandomPartitionsMatchNextDocOracle) {
  std::vector<int32_t> docs;
  for (int32_t block = 0; block < 12; block++) {
    if (block % 3 == 0) {
      appendContiguousBlock(docs);
    } else if (block % 3 == 1) {
      appendWordBlock(docs);
    } else {
      appendPackedBlock(docs);
    }
  }
  const int32_t tailBase = nextBlockBase(docs) + 5;
  for (int32_t i = 0; i < 73; i++) {
    docs.push_back(tailBase + i * 7);
  }

  RAMDir dir;
  MemPool pool;
  writeRawSingleTerm(dir, pool, "mixedprobe", docs);

  PostingsReader reader(dir, 0);
  FieldReader fieldReader(reader);
  ASSERT_TRUE(fieldReader.readNextField());
  SegFieldInfo fieldInfo;
  fieldReader.readFieldInfo(fieldInfo);
  TermsEnum tenum(pool, reader, fieldInfo);
  ASSERT_TRUE(tenum.seek("mixedprobe"));

  const int32_t maxDoc = docs.back() + 97;
  std::vector<uint64_t> oracleBits((size_t) (maxDoc + 63) / 64, 0);
  DocsOnlyEnum oracle(tenum);
  for (int32_t doc = oracle.nextDoc(); doc != DocsEnumMeta::END; doc = oracle.nextDoc()) {
    ASSERT_LT(doc, maxDoc);
    oracleBits[(size_t) doc >> 6] |= 1ULL << (doc & 63);
  }

  for (int32_t iter = 0; iter < 12; iter++) {
    DocsOnlyEnum denum(tenum);
    std::vector<uint64_t> gotBits(oracleBits.size(), 0);
    int32_t from = 0;
    while (from < maxDoc) {
      int32_t step;
      if (rng.rint(0, 5) == 0) {
        step = 64;
      } else if (rng.rint(0, 5) == 0) {
        step = Postings::DOCS_BLOCK_SIZE;
      } else {
        step = 1 + (int32_t) rng.rint(0, 311);
      }
      const int32_t to = std::min(maxDoc, from + step);
      std::vector<uint64_t> bits((size_t) (to - from + 63) / 64, 0);
      denum.intoBitSet(bits, from, to);
      for (int32_t bit = 0; bit < to - from; bit++) {
        if ((bits[(size_t) bit >> 6] & (1ULL << (bit & 63))) != 0) {
          const int32_t doc = from + bit;
          gotBits[(size_t) doc >> 6] |= 1ULL << (doc & 63);
        }
      }
      from = to;
    }
    EXPECT_EQ(gotBits, oracleBits) << "iter=" << iter;
  }
}

TEST_F(DocsEnumAdvanceTest, advanceAndIntoBitSetProbeContiguousRuns) {
  const std::vector<int32_t> docs = makeContiguousDocs(3 * Postings::DOCS_BLOCK_SIZE);
  RAMDir dir;
  MemPool pool;
  writeRawSingleTerm(dir, pool, "runprobe", docs);

  PostingsReader reader(dir, 0);
  FieldReader fieldReader(reader);
  ASSERT_TRUE(fieldReader.readNextField());
  SegFieldInfo fieldInfo;
  fieldReader.readFieldInfo(fieldInfo);
  TermsEnum tenum(pool, reader, fieldInfo);
  ASSERT_TRUE(tenum.seek("runprobe"));

  DocsOnlyEnum denum(tenum);

  bool savedStats = SkipStats::enabled;
  SkipStats::enabled = true;
  SkipStats::reset();

  ASSERT_EQ(denum.advance(10), 10);
  ASSERT_EQ(denum.nextDoc(), 11);
  ASSERT_EQ(denum.advance(64), 64);
  ASSERT_EQ(denum.advance(127), 127);
  ASSERT_EQ(denum.advance(128), 128);
  ASSERT_EQ(denum.advance(130), 130);

  const int32_t from = 120;
  const int32_t to = 140;
  std::vector<uint64_t> bits((size_t) (to - from + 63) / 64, 0);
  denum.intoBitSet(bits, from, to);
  EXPECT_EQ(docsFromBits(bits, from, to), modelWindow(docs, 130, to));
  EXPECT_EQ(denum.docId(), 139);
  EXPECT_EQ(denum.nextDoc(), 140);
  EXPECT_EQ(SkipStats::docBlocksDecoded, 0);
  EXPECT_GT(SkipStats::docsOnlyWordProbeAdvances, 3);
  SkipStats::enabled = savedStats;
  SkipStats::reset();
}

TEST_F(DocsEnumAdvanceTest, residentDocsBlockPeekAndConsumeMaterializeSpan) {
  const std::vector<int32_t> docs = makeWordProbeDocs(1);
  RAMDir dir;
  MemPool pool;
  writeRawSingleTerm(dir, pool, "wordprobe", docs);

  PostingsReader reader(dir, 0);
  FieldReader fieldReader(reader);
  ASSERT_TRUE(fieldReader.readNextField());
  SegFieldInfo fieldInfo;
  fieldReader.readFieldInfo(fieldInfo);
  TermsEnum tenum(pool, reader, fieldInfo);
  ASSERT_TRUE(tenum.seek("wordprobe"));

  DocsOnlyEnum denum(tenum);
  ASSERT_EQ(denum.advance(64), 65);

  auto expectedStart = std::lower_bound(docs.begin(), docs.end(), 65);
  std::vector<int32_t> expected(expectedStart, docs.end());
  auto span = denum.peekDocBlock();
  ASSERT_EQ((int32_t) span.size(), (int32_t) expected.size());
  EXPECT_TRUE(std::equal(span.begin(), span.end(), expected.begin()));

  denum.consumeDocBlock(5);
  ASSERT_EQ(denum.docId(), expected[4]);
  auto span2 = denum.peekDocBlock();
  ASSERT_EQ((int32_t) span2.size(), (int32_t) expected.size() - 5);
  EXPECT_TRUE(std::equal(span2.begin(), span2.end(), expected.begin() + 5));
}

TEST_F(DocsEnumAdvanceTest,
       materializePostingsIntersectionUsesBitWindowsAcrossBlockEncodings) {
  std::vector<int32_t> docs;
  appendWordBlock(docs);
  appendContiguousBlock(docs);
  appendPackedBlock(docs);
  int32_t tailBase = nextBlockBase(docs);
  for (int32_t i = 0; i < 37; i++) {
    docs.push_back(tailBase + i * 5);
  }
  docs.push_back(3 * DocsEnumMeta::L1_DOCS + 17);

  RAMDir dir;
  MemPool pool;
  writeRawSingleTerm(dir, pool, "mixed", docs);
  PostingsReader reader(dir, 0);
  int32_t maxDoc = reader.maxDoc();

  RAMBitDocSet domain(maxDoc);
  for (int32_t doc = 0; doc < maxDoc; doc++) {
    if ((doc % 3) != 1) {
      domain.mutableBits().set(doc);
    }
  }
  std::vector<int32_t> expected;
  for (int32_t doc : docs) {
    if (domain.get(doc)) {
      expected.push_back(doc);
    }
  }

  FieldReader fieldReader(reader);
  ASSERT_TRUE(fieldReader.readNextField());
  SegFieldInfo fieldInfo;
  fieldReader.readFieldInfo(fieldInfo);
  TermsEnum terms(pool, reader, fieldInfo);
  ASSERT_TRUE(terms.seek("mixed"));
  int32_t docFreq = terms.docFreq();
  EXPECT_EQ(domain.cachedCard(), -1);
  DocsOnlyEnum postings(terms);

  SkipStatsGuard stats;
  auto result = materializePostingsIntersection(
      postings, docFreq, &domain, maxDoc);

  EXPECT_EQ(collectDocSet(*result, maxDoc), expected);
  EXPECT_EQ(domain.cachedCard(), -1);
  EXPECT_GT(SkipStats::countBulkFillWordBlocks, 0);
  EXPECT_GT(SkipStats::advanceCalls, 1);
}

TEST_F(DocsEnumAdvanceTest,
       materializePostingsIntersectionRoutesArrayByRelativeSize) {
  std::vector<int32_t> docs;
  appendWordBlock(docs);
  appendContiguousBlock(docs);
  appendPackedBlock(docs);
  int32_t tailBase = nextBlockBase(docs);
  for (int32_t i = 0; i < 37; i++) {
    docs.push_back(tailBase + i * 5);
  }

  RAMDir dir;
  MemPool pool;
  writeRawSingleTerm(dir, pool, "mixed", docs);
  PostingsReader reader(dir, 0);
  int32_t maxDoc = reader.maxDoc();

  auto materialize = [&](DocSet* domain) {
    FieldReader fieldReader(reader);
    EXPECT_TRUE(fieldReader.readNextField());
    SegFieldInfo fieldInfo;
    fieldReader.readFieldInfo(fieldInfo);
    TermsEnum terms(pool, reader, fieldInfo);
    EXPECT_TRUE(terms.seek("mixed"));
    int32_t docFreq = terms.docFreq();
    DocsOnlyEnum postings(terms);
    return materializePostingsIntersection(
        postings, docFreq, domain, maxDoc);
  };

  std::vector<int32_t> mergeDocs;
  for (int32_t doc = 0; doc < maxDoc; doc += 11) {
    mergeDocs.push_back(doc);
  }
  ArrDocSet mergeDomain(std::move(mergeDocs));
  std::vector<int32_t> mergeExpected;
  for (int32_t doc : docs) {
    if (mergeDomain.get(doc)) {
      mergeExpected.push_back(doc);
    }
  }

  SkipStatsGuard stats;
  auto mergeResult = materialize(&mergeDomain);
  EXPECT_EQ(collectDocSet(*mergeResult, maxDoc), mergeExpected);
  EXPECT_EQ(SkipStats::advanceCalls, 0);

  std::vector<int32_t> sparseDocs{docs[5], docs[300]};
  ArrDocSet sparseDomain(std::move(sparseDocs));
  ASSERT_TRUE(shouldDrivePostingsFromArray(
      (int32_t) docs.size(), sparseDomain.card()));
  SkipStats::reset();
  auto sparseResult = materialize(&sparseDomain);
  EXPECT_EQ(collectDocSet(*sparseResult, maxDoc),
            (std::vector<int32_t>{docs[5], docs[300]}));
  EXPECT_GT(SkipStats::advanceCalls, 0);
}

// The whole-term impact frontier stored in the term dictionary must equal the
// staircase over the term's global (norm -> maxTf) surface - on a fresh
// segment and after a merge (the merger regenerates it by replay).
TEST_F(DocsEnumAdvanceTest, termImpactFrontierRoundTrip) {
  const int32_t N = 72 * Postings::DOCS_BLOCK_SIZE + 17;
  std::array<int32_t, 256> surface{};
  for (int32_t doc = 0; doc < N; doc++) {
    int32_t norm = SmallFloat::intToByte4(impactTokenCountForDoc(doc));
    surface[(size_t) norm] = std::max(surface[(size_t) norm], impactTfForDoc(doc));
  }
  std::vector<int32_t> expNorms;
  std::vector<int32_t> expTfs;
  int32_t running = 0;
  for (int32_t norm = 0; norm < 256; norm++) {
    if (surface[(size_t) norm] > running) {
      expNorms.push_back(norm);
      expTfs.push_back(surface[(size_t) norm]);
      running = surface[(size_t) norm];
    }
  }

  auto check = [&](TestField& f, std::string_view label) {
    TermsEnum tenum = f.createTermsEnum();
    ASSERT_TRUE(tenum.seek("hot")) << label;
    std::vector<int32_t> norms;
    std::vector<int32_t> tfs;
    tenum.readTermImpactFrontier(norms, tfs);
    EXPECT_EQ(norms, expNorms) << label;
    EXPECT_EQ(tfs, expTfs) << label;
  };

  {
    TestIndex testIndex;
    TestField f(testIndex, "body_w");
    f.startIndexing();
    addImpactDocs(f, 0, N, 0);
    // a pulsed (single-doc) term shares the block: exact one-point frontier
    f.add(N, "solo x y z");
    testIndex.flush();
    f.startReading();
    check(f, "fresh");

    TermsEnum tenum = f.createTermsEnum();
    ASSERT_TRUE(tenum.seek("solo"));
    std::vector<int32_t> norms;
    std::vector<int32_t> tfs;
    ASSERT_EQ(tenum.readTermImpactFrontier(norms, tfs), 1);
    EXPECT_EQ(tfs[0], 1);
    EXPECT_EQ(norms[0], SmallFloat::intToByte4(4));
  }

  {
    const int32_t split = 40 * Postings::DOCS_BLOCK_SIZE + 9;
    TestIndex testIndex;
    TestField f(testIndex, "body_w");
    f.startIndexing();
    addImpactDocs(f, 0, split, 0);
    testIndex.flush();
    f.startIndexing();
    addImpactDocs(f, 0, N - split, split);
    testIndex.flush();
    testIndex.iw->mergeSegments();
    f.startReading();
    check(f, "merged");
  }
}

TEST_F(DocsEnumAdvanceTest, blockImpactHeadersRoundTrip) {
  const int32_t N = 72 * Postings::DOCS_BLOCK_SIZE + 17;
  std::vector<int32_t> expectedFreqImpacts = expectedBlockMaxTf(N, true);
  std::vector<int32_t> expectedDocsOnlyImpacts = expectedBlockMaxTf(N, false);
  std::vector<int32_t> expectedNorms = expectedBlockMinNorm(N, true);
  std::vector<int32_t> expectedNoNorms = expectedBlockMinNorm(N, false);
  DocsEnumMeta::ImpactFrontiers expectedFrontiers =
      expectedSpanFrontiers(N, true, true, Postings::DOCS_BLOCK_SIZE);
  DocsEnumMeta::ImpactFrontiers expectedNoFrontiers =
      expectedSpanFrontiers(N, true, false, Postings::DOCS_BLOCK_SIZE);
  DocsEnumMeta::ImpactFrontiers expectedGroupFrontiers =
      expectedSpanFrontiers(N, true, true, DocsEnumMeta::L1_DOCS);
  DocsEnumMeta::ImpactFrontiers expectedNoGroupFrontiers =
      expectedSpanFrontiers(N, true, false, DocsEnumMeta::L1_DOCS);

  {
    TestIndex testIndex;
    TestField f(testIndex, "body_w");
    f.startIndexing();
    addImpactDocs(f, 0, N, 0);
    testIndex.flush();
    f.startReading();

    assertImpactHeadersForField(f, expectedFreqImpacts, expectedNorms, "positions",
                                &expectedFrontiers, &expectedGroupFrontiers);
  }

  {
    const int32_t split = 40 * Postings::DOCS_BLOCK_SIZE + 9;
    TestIndex testIndex;
    TestField f(testIndex, "body_w");
    f.startIndexing();
    addImpactDocs(f, 0, split, 0);
    testIndex.flush();
    f.startIndexing();
    addImpactDocs(f, 0, N - split, split);
    testIndex.flush();

    testIndex.iw->mergeSegments();
    f.startReading();
    ASSERT_EQ(testIndex.reader->segments().size(), 1u);

    assertImpactHeadersForField(f, expectedFreqImpacts, expectedNorms, "merged positions",
                                &expectedFrontiers, &expectedGroupFrontiers);
  }

  checkRawImpactHeaders(FieldType::INDEX_DOCS_FREQS, expectedFreqImpacts, expectedNoNorms, N,
                        "docs+freqs", &expectedNoFrontiers, &expectedNoGroupFrontiers);
  checkRawImpactHeaders(FieldType::INDEX_DOCS, expectedDocsOnlyImpacts, expectedNoNorms, N,
                        "docs-only", &expectedNoFrontiers, &expectedNoGroupFrontiers);
}

TEST_F(DocsEnumAdvanceTest, packedL1GroupFrontierRoundTripShapesAndWidths) {
  auto run = [&](std::string_view label, const std::vector<uint8_t>& norms,
                 const std::vector<int32_t>& tfs, int32_t expectedCount,
                 uint32_t expectedWidth, int32_t expectedMaxTf) {
    SCOPED_TRACE(std::string(label));
    RAMDir dir;
    MemPool pool;
    writeRawPositionsImpactTerm(dir, pool, norms, tfs);

    PostingsReader reader(dir, 0);
    FieldReader fieldReader(reader);
    ASSERT_TRUE(fieldReader.readNextField());
    SegFieldInfo fieldInfo;
    fieldReader.readFieldInfo(fieldInfo);
    TermsEnum tenum(pool, reader, fieldInfo);
    ASSERT_TRUE(tenum.seek("hot"));
    DocsOnlyEnum denum(tenum);

    DocsEnumMeta::GroupImpactCursor cursor;
    DocsEnumMeta::GroupImpactHeader firstHeader;
    ASSERT_EQ(denum.readGroupImpactHeadersThrough(
                  cursor, 0, false,
                  [&](const DocsEnumMeta::GroupImpactHeader& header) {
                    firstHeader = header;
                  }),
              1);
    ASSERT_EQ((int32_t) firstHeader.frontierNorms.size(), expectedCount);
    ASSERT_EQ(firstHeader.frontierTfWidth, expectedWidth);
    ASSERT_EQ(firstHeader.spanMaxTf, expectedMaxTf);

    DocsEnumMeta::ImpactFrontiers expectedGroups =
        expectedRawSpanFrontiers(norms, tfs, DocsEnumMeta::L1_DOCS);
    DocsEnumMeta::GroupImpacts groups;
    denum.readGroupImpacts(groups);
    ASSERT_EQ(groups.frontiers.offsets, expectedGroups.offsets);
    ASSERT_EQ(groups.frontiers.norms, expectedGroups.norms);
    ASSERT_EQ(groups.frontiers.tfs, expectedGroups.tfs);

    std::vector<int32_t> blockMaxTf;
    std::vector<int32_t> groupSpanImpacts;
    std::vector<int32_t> blockMinNorms;
    std::vector<int32_t> groupSpanMinNorms;
    denum.readBlockMaxTf(blockMaxTf, &groupSpanImpacts, nullptr, &blockMinNorms,
                         &groupSpanMinNorms);
    ASSERT_EQ(blockMaxTf, expectedRawBlockMaxTf(tfs));
    ASSERT_EQ(blockMinNorms, expectedRawBlockMinNorm(norms));
    ASSERT_EQ(groupSpanImpacts, expectedGroupSpanImpacts(blockMaxTf));
    ASSERT_EQ(groupSpanMinNorms, expectedGroupSpanMinNorms(blockMinNorms));
  };

  std::vector<uint8_t> normsOne((size_t) DocsEnumMeta::L1_DOCS, 7);
  std::vector<int32_t> tfsOne((size_t) DocsEnumMeta::L1_DOCS, 5);
  run("N=1 u16", normsOne, tfsOne, 1, 2, 5);

  std::vector<uint8_t> normsFull((size_t) DocsEnumMeta::L1_DOCS);
  std::vector<int32_t> tfsFull((size_t) DocsEnumMeta::L1_DOCS);
  for (int32_t doc = 0; doc < DocsEnumMeta::L1_DOCS; doc++) {
    uint8_t norm = (uint8_t) (doc & 255);
    normsFull[(size_t) doc] = norm;
    tfsFull[(size_t) doc] = (int32_t) norm + 1;
  }
  run("N=256 absolute tfs", normsFull, tfsFull, 256, 2, 256);

  std::vector<uint8_t> normsU16((size_t) DocsEnumMeta::L1_DOCS, 3);
  std::vector<int32_t> tfsU16((size_t) DocsEnumMeta::L1_DOCS, 1);
  tfsU16[17] = 65535;
  run("u16 boundary", normsU16, tfsU16, 1, 2, 65535);

  std::vector<uint8_t> normsU32((size_t) DocsEnumMeta::L1_DOCS, 3);
  std::vector<int32_t> tfsU32((size_t) DocsEnumMeta::L1_DOCS, 1);
  tfsU32[19] = 65536;
  run("u32 escape", normsU32, tfsU32, 1, 4, 65536);
}

TEST_F(DocsEnumAdvanceTest, packedL1SkipToBlockAcrossManyGroups) {
  const int32_t N = 10 * DocsEnumMeta::L1_DOCS + 77;
  std::vector<uint8_t> norms((size_t) N);
  std::vector<int32_t> tfs((size_t) N);
  for (int32_t doc = 0; doc < N; doc++) {
    norms[(size_t) doc] = (uint8_t) ((doc * 29) & 255);
    tfs[(size_t) doc] = 1 + ((doc * 17) % 97);
  }

  RAMDir dir;
  MemPool pool;
  writeRawPositionsImpactTerm(dir, pool, norms, tfs);

  PostingsReader reader(dir, 0);
  FieldReader fieldReader(reader);
  ASSERT_TRUE(fieldReader.readNextField());
  SegFieldInfo fieldInfo;
  fieldReader.readFieldInfo(fieldInfo);
  TermsEnum tenum(pool, reader, fieldInfo);
  ASSERT_TRUE(tenum.seek("hot"));
  DocsFreqEnum denum(tenum);

  bool savedStats = SkipStats::enabled;
  SkipStats::enabled = true;
  SkipStats::reset();
  for (int32_t target : {17, DocsEnumMeta::L1_DOCS + 9, 3 * DocsEnumMeta::L1_DOCS + 123,
                         6 * DocsEnumMeta::L1_DOCS + 7, 9 * DocsEnumMeta::L1_DOCS + 31,
                         N - 1}) {
    ASSERT_EQ(denum.advance(target), target);
    ASSERT_EQ(denum.termFreq(), tfs[(size_t) target]);
  }
  ASSERT_GT(SkipStats::l1GroupSteps, 0);
  SkipStats::enabled = savedStats;
  SkipStats::reset();
}

TEST_F(DocsEnumAdvanceTest, l0CheckpointParityAndPositionTracking) {
  RAMDir dir;
  MemPool pool;
  std::vector<CheckpointTermModel> models;
  writeCheckpointTerms(dir, pool, models);

  PostingsReader reader(dir, 0);
  FieldReader fieldReader(reader);
  ASSERT_TRUE(fieldReader.readNextField());
  SegFieldInfo fieldInfo;
  fieldReader.readFieldInfo(fieldInfo);
  CheckpointToggleGuard guard;

  auto runFreqs = [&](const CheckpointTermModel& model,
                      const std::vector<int32_t>& targets,
                      bool disableCheckpoints, bool docsOnly) {
    DocsEnumMeta::disableL0CheckpointsForTests = disableCheckpoints;
    TermsEnum terms(pool, reader, fieldInfo);
    std::vector<int32_t> sequence;
    if (!terms.seek(model.term)) {
      ADD_FAILURE() << model.term;
      return sequence;
    }
    DocsFreqEnum docs(terms);
    int32_t current = -1;
    for (int32_t target : targets) {
      if (target <= current) {
        continue;
      }
      current = docsOnly
          ? docs.advanceDocOnly(target) : docs.advance(target);
      const Posting* expected =
          checkpointPostingAtOrAfter(model, target);
      int32_t expectedDoc =
          expected == nullptr ? DocsEnumMeta::END : expected->docid;
      EXPECT_EQ(current, expectedDoc)
          << model.term << " target=" << target
          << " docsOnly=" << docsOnly
          << " disabled=" << disableCheckpoints;
      sequence.push_back(current);
      if (current == DocsEnumMeta::END) {
        break;
      }
      if (!docsOnly) {
        EXPECT_EQ(docs.termFreq(), expected->tf)
            << model.term << " doc=" << current;
      }
    }
    return sequence;
  };

  auto runDocsTier = [&](const CheckpointTermModel& model,
                         const std::vector<int32_t>& targets,
                         bool disableCheckpoints) {
    DocsEnumMeta::disableL0CheckpointsForTests = disableCheckpoints;
    TermsEnum terms(pool, reader, fieldInfo);
    std::vector<int32_t> sequence;
    if (!terms.seek(model.term)) {
      ADD_FAILURE() << model.term;
      return sequence;
    }
    DocsOnlyEnum docs(terms);
    int32_t current = -1;
    for (int32_t target : targets) {
      if (target <= current) {
        continue;
      }
      current = docs.advance(target);
      const Posting* expected =
          checkpointPostingAtOrAfter(model, target);
      EXPECT_EQ(current, expected == nullptr
                             ? DocsEnumMeta::END : expected->docid)
          << model.term << " target=" << target
          << " disabled=" << disableCheckpoints;
      sequence.push_back(current);
      if (current == DocsEnumMeta::END) {
        break;
      }
    }
    return sequence;
  };

  for (const CheckpointTermModel& model : models) {
    SCOPED_TRACE(model.term);
    std::vector<int32_t> targets = checkpointTargets(model);
    EXPECT_EQ(runFreqs(model, targets, false, false),
              runFreqs(model, targets, true, false));
    EXPECT_EQ(runFreqs(model, targets, false, true),
              runFreqs(model, targets, true, true));
    EXPECT_EQ(runDocsTier(model, targets, false),
              runDocsTier(model, targets, true));
  }

  const CheckpointTermModel& positional = models.back();
  std::vector<int32_t> positionalTargets = checkpointTargets(positional);
  auto runPositions = [&](bool disableCheckpoints) {
    DocsEnumMeta::disableL0CheckpointsForTests = disableCheckpoints;
    TermsEnum terms(pool, reader, fieldInfo);
    std::vector<int32_t> sequence;
    if (!terms.seek(positional.term)) {
      ADD_FAILURE() << positional.term;
      return sequence;
    }
    DocsPosEnum docs(terms);
    PosEnum positions(docs);
    int32_t current = -1;
    for (int32_t target : positionalTargets) {
      if (target <= current) {
        continue;
      }
      current = docs.advance(target);
      const Posting* expected =
          checkpointPostingAtOrAfter(positional, target);
      EXPECT_EQ(current, expected == nullptr
                             ? DocsEnumMeta::END : expected->docid)
          << "target=" << target
          << " disabled=" << disableCheckpoints;
      sequence.push_back(current);
      if (current == DocsEnumMeta::END) {
        break;
      }
      if (expected == nullptr) {
        ADD_FAILURE() << "missing model posting for doc=" << current;
        break;
      }
      EXPECT_EQ(docs.termFreq(), expected->tf);
      positions.startPositions();
      for (int32_t i = 0; i < expected->tf; i++) {
        EXPECT_EQ(positions.nextPosition(), expected->firstPos + i)
            << "doc=" << current << " posOrd=" << i;
      }
      EXPECT_EQ(positions.nextPosition(), PosEnum::END)
          << "doc=" << current;
    }
    return sequence;
  };
  EXPECT_EQ(runPositions(false), runPositions(true));
}

TEST_F(DocsEnumAdvanceTest, l0CheckpointEngagementAndKillSwitch) {
  RAMDir dir;
  MemPool pool;
  std::vector<CheckpointTermModel> models;
  writeCheckpointTerms(dir, pool, models);

  PostingsReader reader(dir, 0);
  FieldReader fieldReader(reader);
  ASSERT_TRUE(fieldReader.readNextField());
  SegFieldInfo fieldInfo;
  fieldReader.readFieldInfo(fieldInfo);
  const CheckpointTermModel& dense = models.back();
  CheckpointToggleGuard guard;
  SkipStats::enabled = true;

  auto run = [&](bool disableCheckpoints) {
    DocsEnumMeta::disableL0CheckpointsForTests = disableCheckpoints;
    SkipStats::reset();
    TermsEnum terms(pool, reader, fieldInfo);
    if (!terms.seek(dense.term)) {
      ADD_FAILURE() << dense.term;
      return std::pair<int64_t, int64_t>{-1, -1};
    }
    DocsFreqEnum docs(terms);
    for (int32_t block : {20, 29, 55, 90}) {
      const Posting& expected = dense.postings[
          (size_t) block * Postings::DOCS_BLOCK_SIZE];
      EXPECT_EQ(docs.advance(expected.docid), expected.docid)
          << "block=" << block;
      EXPECT_EQ(docs.termFreq(), expected.tf)
          << "block=" << block;
    }
    return std::pair{
        SkipStats::l0CheckpointJumps,
        SkipStats::l0HeaderSteps};
  };

  auto enabled = run(false);
  auto disabled = run(true);
  EXPECT_GT(enabled.first, 0);
  EXPECT_EQ(disabled.first, 0);
  EXPECT_LT(enabled.second, disabled.second);
}

TEST_F(DocsEnumAdvanceTest, packedFrontierScoreMatchesScalarBitExact) {
  Similarity sim;
  Similarity::FieldStats fieldStats;
  fieldStats.maxDoc = 10000;
  fieldStats.docsWithField = 10000;
  fieldStats.sumTotalTermFreq = 70000;
  Similarity::TermStats termStats;
  termStats.docFreq = 137;
  termStats.totalTermFreq = 2000;
  auto scorer = sim.getScorer(1.0f, fieldStats, termStats);
  const float boost = 1.75f;

  for (int32_t iter = 0; iter < 200; iter++) {
    uint32_t width = (iter & 1) == 0 ? 2u : 4u;
    int32_t count = 1 + rng.rint(256);
    std::vector<uint8_t> norms((size_t) count);
    std::vector<uint32_t> tfs((size_t) count);
    for (int32_t i = 0; i < count; i++) {
      norms[(size_t) i] = (uint8_t) rng.rint(256);
      if (width == 2) {
        tfs[(size_t) i] = 1u + (uint32_t) rng.rint(65535);
      } else {
        tfs[(size_t) i] = 65536u + (uint32_t) rng.rint(1000000);
      }
    }
    std::vector<char> bytes = encodeTfBytes(tfs, width);
    float expected = 0.0f;
    for (int32_t i = 0; i < count; i++) {
      float score = boost * scorer.score((float) tfs[(size_t) i],
                                         (int64_t) norms[(size_t) i]);
      ASSERT_TRUE(std::isfinite(score));
      expected = std::max(expected, score);
    }
    float actual = scorer.scoreFrontier(norms, bytes, width, boost);
    EXPECT_EQ(std::bit_cast<uint32_t>(actual), std::bit_cast<uint32_t>(expected))
        << "iter=" << iter << " width=" << width << " count=" << count;
  }
}

TEST_F(DocsEnumAdvanceTest, wrongSegmentMagicIsRejected) {
  RAMDir dir;
  MemPool pool;
  writeRawSingleTerm(dir, pool, "hot", {0, 7, 19, 43});

  std::string segName = Postings::getIndexFileName(Postings::getSortableString(0), 0);
  auto input = dir.openFile(segName);
  ASSERT_NE(input, nullptr);
  std::string bytes(input->read());
  ASSERT_GE(bytes.size(), Postings::LUXIR_HEADER.size());
  memcpy(bytes.data(), "LUXIR000", Postings::LUXIR_HEADER.size());

  ASSERT_TRUE(dir.deleteFile(segName));
  auto outFile = dir.createFile(segName);
  OutputStream out(outFile.get());
  out.write(bytes.data(), bytes.size());
  out.close();
  dir.finishFile(*outFile);

  EXPECT_THROW({ PostingsReader reader(dir, 0); }, std::runtime_error);
}

// Position reads after far advances must land exactly where sequential decoding
// would, now that skipToBlock repairs the position stream through the L0
// posByteOff anchors.  Every doc holds all three terms, so the per-term
// cumulative tf pins each doc block's anchor alignment deterministically:
//   one (tf=1)    - anchors on exact position-block boundaries, and the final
//                   doc block's anchor at the vint-tail start (N % 128 != 0).
//   cst (tf=128)  - each doc is exactly one position block; anchors aligned.
//   var (tf=1+((d*7+3)%250)) - anchors rotate through in-block ords, docs
//                   straddle position blocks.
// Sized to span multiple L1 groups so far advances take the group step-over
// into a direct seek.  Runs on a single segment and again after a merge (the
// merger regenerates the anchors by replaying positions through TextWriter).
class PositionSeekTest : public DocsEnumAdvanceTest {
protected:
  static int32_t docCount() {
    return (int32_t)scaleTestWork(1) * DocsEnumMeta::L1_DOCS + 300;
  }

  static int32_t tfFor(std::string_view term, int32_t globalDoc) {
    if (term == "one") return 1;
    if (term == "cst") return Postings::POSITIONS_BLOCK_SIZE;
    return 1 + ((globalDoc * 7 + 3) % 250);  // var
  }

  static int32_t firstPosFor(std::string_view term, int32_t globalDoc) {
    if (term == "one") return 0;
    if (term == "var") return 1;
    return 1 + tfFor("var", globalDoc);  // cst
  }

  static void addSeekDocs(TestField& f, int32_t firstDoc, int32_t numDocs, int32_t globalBase) {
    std::string text;
    for (int32_t i = 0; i < numDocs; i++) {
      int32_t globalDoc = globalBase + i;
      text = "one ";
      for (int32_t k = 0; k < tfFor("var", globalDoc); k++) text += "var ";
      for (int32_t k = 0; k < tfFor("cst", globalDoc); k++) text += "cst ";
      f.add(firstDoc + i, text);
    }
  }

  // readMode: 0 = all positions + END, 1 = half, 2 = none except one position
  // every 4th landing (repeated seeks over stale buffer states).
  void checkSeekWalk(TestField& f, std::string_view term,
                     const std::vector<int32_t>& targets, int readMode) {
    SCOPED_TRACE(::testing::Message() << "term=" << term << " readMode=" << readMode);
    TermsEnum tenum = f.createTermsEnum();
    ASSERT_TRUE(tenum.seek(term));
    DocsPosEnum denum(tenum);
    PosEnum posEnum(denum);

    int32_t landings = 0;
    for (int32_t target : targets) {
      if (target <= denum.docId()) continue;
      int32_t doc = denum.advance(target);
      ASSERT_EQ(doc, target < docCount() ? target : DocsEnumMeta::END)
          << "advance(" << target << ")";
      if (doc == DocsEnumMeta::END) break;
      int32_t tf = tfFor(term, doc);
      ASSERT_EQ(denum.termFreq(), tf) << "doc " << doc;
      int32_t toRead = readMode == 0 ? tf
                     : readMode == 1 ? tf / 2
                     : (landings % 4 == 0 ? 1 : 0);
      landings++;
      if (readMode == 2 && toRead == 0) continue;
      posEnum.startPositions();
      int32_t firstPos = firstPosFor(term, doc);
      for (int32_t k = 0; k < toRead; k++) {
        ASSERT_EQ(posEnum.nextPosition(), firstPos + k) << "doc " << doc << " pos " << k;
      }
      if (toRead == tf) {
        ASSERT_EQ(posEnum.nextPosition(), PosEnum::END) << "doc " << doc << " pos end";
      }
    }
  }

  void checkAllWalks(TestField& f) {
    int32_t nDocs = docCount();
    int32_t tailBlockStart =
        (nDocs / Postings::DOCS_BLOCK_SIZE) * Postings::DOCS_BLOCK_SIZE;
    // Codec block boundaries, every L1 boundary, and the final partial block.
    std::vector<int32_t> boundaries = {
      1, 2, 127, 128, 130, 258
    };
    int32_t fullGroups = (nDocs - 300) / DocsEnumMeta::L1_DOCS;
    for (int32_t group = 1; group <= fullGroups; group++) {
      int32_t boundary = group * DocsEnumMeta::L1_DOCS;
      boundaries.push_back(boundary - 1);
      boundaries.push_back(boundary);
      boundaries.push_back(boundary + 1);
    }
    boundaries.insert(boundaries.end(), {
      tailBlockStart - 1, tailBlockStart, tailBlockStart + 1,
      tailBlockStart + 12, nDocs - 1, nDocs
    });
    for (std::string_view term : {"one", "cst", "var"}) {
      checkSeekWalk(f, term, boundaries, 0);
      int readMode = 0;
      for (int32_t stride : {997, 313, 129}) {
        std::vector<int32_t> targets;
        for (int32_t t = stride / 2; t <= nDocs; t += stride) targets.push_back(t);
        checkSeekWalk(f, term, targets, readMode++);
      }
      // Sequential reads resuming after a far seek, then another far seek.
      std::vector<int32_t> mixed;
      mixed.push_back(3000);
      for (int32_t d = 3001; d < 3200; d++) mixed.push_back(d);
      mixed.push_back(nDocs - 92);
      checkSeekWalk(f, term, mixed, 0);
    }
  }
};

TEST_F(PositionSeekTest, positionSeekAlignments) {
  bool savedStats = SkipStats::enabled;
  SkipStats::enabled = true;
  SkipStats::reset();

  {
    TestIndex testIndex;
    TestField f(testIndex, "body_w");
    f.startIndexing();
    addSeekDocs(f, 0, docCount(), 0);
    testIndex.flush();
    f.startReading();
    checkAllWalks(f);
    ASSERT_GT(SkipStats::posSeeks, 0);  // the new anchor-seek path engaged
  }

  {
    const int32_t split = 20 * Postings::DOCS_BLOCK_SIZE + 9;
    TestIndex testIndex;
    TestField f(testIndex, "body_w");
    f.startIndexing();
    addSeekDocs(f, 0, split, 0);
    testIndex.flush();
    f.startIndexing();
    addSeekDocs(f, 0, docCount() - split, split);
    testIndex.flush();

    testIndex.iw->mergeSegments();
    f.startReading();
    ASSERT_EQ(testIndex.reader->segments().size(), 1u);
    checkAllWalks(f);
  }

  SkipStats::enabled = savedStats;
  SkipStats::reset();
}

// A conjunction's advance() must route through the skip list (TermQuery::Scorer
// forwards advance() to DocsEnumImpl::advance()).  The sparse term leads and advance()s
// the dense one across many blocks; some sparse docs are not dense, exercising the
// advance-overshoot/re-advance path.  The result must equal the true intersection.
TEST_F(DocsEnumAdvanceTest, conjunctionLeapfrog) {
  const int N = 1500;
  std::vector<int32_t> dense, sparse;
  TestIndex testIndex;
  TestField f(testIndex, "body_w");
  f.startIndexing();
  for (int d = 0; d < N; d++) {
    std::string text;
    if (rng.rint(0, 3) != 0) { text += "dense "; dense.push_back(d); }    // ~2/3 of docs, spans many blocks
    if (rng.rint(0, 20) == 0) { text += "sparse"; sparse.push_back(d); }  // ~1/20, some not in dense
    if (!text.empty()) f.add(d, text);                                    // skip docs with neither term
  }
  testIndex.flush();
  f.startReading();

  std::vector<int32_t> expected;  // dense INTERSECT sparse
  std::set_intersection(dense.begin(), dense.end(), sparse.begin(), sparse.end(),
                        std::back_inserter(expected));

  TermQuery denseQ("body_w", "dense");
  TermQuery sparseQ("body_w", "sparse");
  std::vector<Query*> mand = {&denseQ, &sparseQ};
  BooleanQuery q(mand, {}, {}, {});  // pure conjunction

  auto poolFree = testIndex.pool.rewindScopeGuard();
  Query::Context qContext(testIndex.pool, *testIndex.reader);
  auto* weight = q.createWeight(qContext, Query::NEED_SCORES);
  Query::Scorer* scorer = weight->createScorer(testIndex.pool, qContext.topReader.segments()[0]);
  ASSERT_NE(scorer, nullptr);

  std::vector<int32_t> got;
  for (int32_t doc = scorer->next(); doc != PostingsReader::END; doc = scorer->next()) {
    got.push_back(doc);
  }
  ASSERT_EQ(got, expected);
}
