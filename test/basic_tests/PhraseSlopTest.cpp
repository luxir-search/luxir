// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <functional>
#include <limits>
#include <random>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <gtest/gtest.h>

#include "luxir/query/PhraseQuery.h"
#include "luxir/search/Collector.h"
#include "luxir/search/Similarity.h"
#include "test/LuxirTest.h"
#include "test/TestIndex.h"

using namespace luxir;
using namespace luxir::test;

namespace {

struct PhraseHit {
  int32_t doc;
  uint32_t scoreBits;
  float freq;

  bool operator==(const PhraseHit&) const = default;
};

std::vector<PhraseHit> runPhrase(TestIndex& index, Query::Context& context,
                                 std::span<std::string_view> terms,
                                 std::span<const int32_t> positions, int32_t slop,
                                 bool twoPhase = false,
                                 std::string_view field = "body_w",
                                 float minCompetitiveScore = 0.0f) {
  PhraseQuery query(field, terms, positions, slop);
  auto* weight = query.createWeight(context, Query::NEED_SCORES);
  auto* scorer = weight->createScorer(index.pool, context.topReader.segments()[0]);
  std::vector<PhraseHit> hits;
  if (scorer == nullptr) return hits;
  auto phraseFreq = [&]() {
    if (slop > 0) {
      return dynamic_cast<PhraseQuery::SloppyScorer*>(scorer)->phraseFreqForTests();
    }
    return dynamic_cast<PhraseQuery::Scorer*>(scorer)->phraseFreqForTests();
  };
  scorer->setMinCompetitiveScore(minCompetitiveScore);
  if (!twoPhase) {
    for (int32_t doc = scorer->next(); doc != PostingsReader::END; doc = scorer->next()) {
      float score = scorer->score();
      hits.push_back({doc, std::bit_cast<uint32_t>(score), phraseFreq()});
    }
  } else {
    for (int32_t doc = scorer->approximationNext(); doc != PostingsReader::END;
         doc = scorer->approximationNext()) {
      if (scorer->matches()) {
        float score = scorer->score();
        hits.push_back({doc, std::bit_cast<uint32_t>(score), phraseFreq()});
      }
    }
  }
  return hits;
}

bool bruteExists(const std::vector<std::string>& doc,
                 const std::vector<std::string_view>& terms,
                 const std::vector<int32_t>& offsets, int32_t slop) {
  std::unordered_map<std::string_view, std::vector<int32_t>> occurrences;
  for (int32_t pos = 0; pos < (int32_t) doc.size(); pos++) {
    occurrences[doc[(size_t) pos]].push_back(pos);
  }
  std::unordered_map<std::string_view, std::vector<bool>> used;
  for (const auto& [term, positions] : occurrences) {
    used[term].resize(positions.size());
  }
  std::function<bool(size_t, int64_t, int64_t)> visit =
      [&](size_t slot, int64_t lo, int64_t hi) {
        if (slot == terms.size()) return hi - lo <= slop;
        auto found = occurrences.find(terms[slot]);
        if (found == occurrences.end()) return false;
        auto& termUsed = used[terms[slot]];
        for (size_t i = 0; i < found->second.size(); i++) {
          if (termUsed[i]) continue;
          int64_t value = (int64_t) found->second[i] - offsets[slot];
          int64_t nextLo = slot == 0 ? value : std::min(lo, value);
          int64_t nextHi = slot == 0 ? value : std::max(hi, value);
          if (nextHi - nextLo > slop) continue;
          termUsed[i] = true;
          if (visit(slot + 1, nextLo, nextHi)) return true;
          termUsed[i] = false;
        }
        return false;
      };
  return visit(0, 0, 0);
}

} // namespace

class PhraseSlopTest : public LuxirTest {};

TEST_F(PhraseSlopTest, handTracedFrequencyAndCapturedThreshold) {
  TestIndex index;
  TestField field(index, "body_w");
  field.startIndexing();
  field.add(0, "a b a");
  field.add(1, "a a a");
  field.add(2, "b a");
  field.add(3, "a x b");
  index.flush();
  field.startReading();

  auto scope = index.pool.rewindScopeGuard();
  Query::Context context(index.pool, *index.reader);
  std::vector<std::string_view> ab = {"a", "b"};
  std::vector<std::string_view> aa = {"a", "a"};
  std::vector<int32_t> adjacent = {0, 1};

  auto exact = runPhrase(index, context, ab, adjacent, 0);
  EXPECT_NE(exact.end(), std::find_if(exact.begin(), exact.end(),
                                      [](const auto& h) { return h.doc == 0; }));
  EXPECT_EQ(exact.end(), std::find_if(exact.begin(), exact.end(),
                                      [](const auto& h) { return h.doc == 2; }));

  std::vector<int32_t> hole = {0, 2};
  auto exactHole = runPhrase(index, context, ab, hole, 0);
  EXPECT_NE(exactHole.end(), std::find_if(exactHole.begin(), exactHole.end(),
                                          [](const auto& h) { return h.doc == 3; }));

  std::vector<int32_t> sameOffset = {0, 0};
  EXPECT_TRUE(runPhrase(index, context, ab, sameOffset, 0).empty());
  EXPECT_FALSE(runPhrase(index, context, ab, sameOffset, 1).empty());

  auto abHits = runPhrase(index, context, ab, adjacent, 2);
  auto doc0 = std::find_if(abHits.begin(), abHits.end(), [](const auto& h) { return h.doc == 0; });
  ASSERT_NE(doc0, abHits.end());
  EXPECT_FLOAT_EQ(1.0f + 1.0f / 3.0f, doc0->freq);

  auto aaHits = runPhrase(index, context, aa, adjacent, 1);
  auto doc1 = std::find_if(aaHits.begin(), aaHits.end(), [](const auto& h) { return h.doc == 1; });
  ASSERT_NE(doc1, aaHits.end());
  EXPECT_FLOAT_EQ(2.0f, doc1->freq);

  auto slop1 = runPhrase(index, context, ab, adjacent, 1);
  EXPECT_EQ(slop1.end(), std::find_if(slop1.begin(), slop1.end(),
                                      [](const auto& h) { return h.doc == 2; }));
  auto slop2 = runPhrase(index, context, ab, adjacent, 2);
  EXPECT_NE(slop2.end(), std::find_if(slop2.begin(), slop2.end(),
                                      [](const auto& h) { return h.doc == 2; }));
  EXPECT_NE(slop1.end(), std::find_if(slop1.begin(), slop1.end(),
                                      [](const auto& h) { return h.doc == 3; }));
}

TEST_F(PhraseSlopTest, greedyFrequencyRetainsQueryOrderArtifact) {
  TestIndex index;
  TestField field(index, "body_w");
  field.startIndexing();
  field.add(0, "a b c b a");
  index.flush();
  field.startReading();

  auto scope = index.pool.rewindScopeGuard();
  Query::Context context(index.pool, *index.reader);
  std::vector<std::string_view> forward = {"a", "b", "c"};
  std::vector<std::string_view> reverse = {"c", "b", "a"};
  std::vector<int32_t> positions = {0, 1, 2};
  auto forwardHits = runPhrase(index, context, forward, positions, 4);
  auto reverseHits = runPhrase(index, context, reverse, positions, 4);
  ASSERT_EQ(1u, forwardHits.size());
  ASSERT_EQ(1u, reverseHits.size());
  EXPECT_NE(forwardHits[0].freq, reverseHits[0].freq);
}

TEST(PhraseSlopNumericTest, rawEnvelopeSurvivesRoundedImpactCancellation) {
  Similarity::FieldStats stats;
  stats.maxDoc = 1;
  stats.docsWithField = 1;
  stats.sumTotalTermFreq = 17;
  stats.sumDocFreq = 17;
  auto scorer = Similarity().getScorer(1.0f, stats, 1.0f);

  EXPECT_EQ(0.0f, scorer.score(2.0f, 255));
  EXPECT_GT(scorer.score(129.0f, 255), 0.0f);
}

TEST_F(PhraseSlopTest, executionPlanAndDriverParity) {
  TestIndex index;
  TestField field(index, "body_w");
  field.startIndexing();
  for (int32_t doc = 0; doc < 300; doc++) {
    switch (doc % 5) {
      case 0: field.add(doc, "common rare common x"); break;
      case 1: field.add(doc, "rare x common common"); break;
      case 2: field.add(doc, "common x rare common"); break;
      default: field.add(doc, "common x common x"); break;
    }
  }
  index.flush();
  field.startReading();
  auto scope = index.pool.rewindScopeGuard();
  Query::Context context(index.pool, *index.reader);
  std::vector<std::string_view> terms = {"common", "rare", "common"};
  std::vector<int32_t> positions = {0, 1, 2};

  bool saved = PhraseQuery::ScorerControls::disableSortForTests;
  PhraseQuery::ScorerControls::disableSortForTests = false;
  auto sorted = runPhrase(index, context, terms, positions, 3);
  PhraseQuery::ScorerControls::disableSortForTests = true;
  auto textOrder = runPhrase(index, context, terms, positions, 3);
  PhraseQuery::ScorerControls::disableSortForTests = saved;
  EXPECT_EQ(sorted, textOrder);

  auto single = runPhrase(index, context, terms, positions, 3, false);
  auto twoPhase = runPhrase(index, context, terms, positions, 3, true);
  EXPECT_EQ(single, twoPhase);

  float minimum = std::numeric_limits<float>::infinity();
  for (const PhraseHit& hit : single) {
    minimum = std::min(minimum, std::bit_cast<float>(hit.scoreBits));
  }
  float threshold = std::nextafter(minimum, 0.0f);
  bool savedGuard = PhraseQuery::ScorerControls::disableDocBoundForTests;
  PhraseQuery::ScorerControls::disableDocBoundForTests = false;
  auto guarded = runPhrase(index, context, terms, positions, 3, false, "body_w", threshold);
  PhraseQuery::ScorerControls::disableDocBoundForTests = true;
  auto unguarded = runPhrase(index, context, terms, positions, 3, false, "body_w", threshold);
  PhraseQuery::ScorerControls::disableDocBoundForTests = savedGuard;
  EXPECT_EQ(guarded, unguarded);

  auto collectTop = [&](bool disableGuard) {
    PhraseQuery query("body_w", terms, positions, 3);
    auto* weight = query.createWeight(context, Query::NEED_SCORES);
    auto* scorer = weight->createScorer(index.pool, context.topReader.segments()[0]);
    EXPECT_NE(scorer, nullptr);
    TopDocsCollector collector(5);
    bool prior = PhraseQuery::ScorerControls::disableDocBoundForTests;
    PhraseQuery::ScorerControls::disableDocBoundForTests = disableGuard;
    collectTopK(0, scorer, nullptr, nullptr, collector);
    PhraseQuery::ScorerControls::disableDocBoundForTests = prior;
    auto sorted = collector.sort();
    return std::vector<TopDocsCollector::ScoreDoc>(sorted.begin(), sorted.end());
  };
  auto boundedTop = collectTop(false);
  auto unboundedTop = collectTop(true);
  ASSERT_EQ(boundedTop.size(), unboundedTop.size());
  for (size_t i = 0; i < boundedTop.size(); i++) {
    EXPECT_EQ(boundedTop[i].doc, unboundedTop[i].doc);
    EXPECT_EQ(std::bit_cast<uint32_t>(boundedTop[i].score),
              std::bit_cast<uint32_t>(unboundedTop[i].score));
  }
}

TEST_F(PhraseSlopTest, repeatPhraseResultsMatchAcrossAllPlanToggles) {
  TestIndex index;
  TestField field(index, "body_w");
  field.startIndexing();
  field.add(0, "a b a");
  field.add(1, "a x b a");
  field.add(2, "a b x a");
  field.add(3, "a b a a b a");
  field.add(4, "b a b a");
  field.add(5, "a x x b x a");
  index.flush();
  field.startReading();

  auto scope = index.pool.rewindScopeGuard();
  Query::Context context(index.pool, *index.reader);
  std::vector<std::string_view> terms = {"a", "b", "a"};
  std::vector<int32_t> positions = {0, 1, 2};
  std::array<std::array<std::vector<PhraseHit>, 4>, 2> runs;

  bool savedSort = PhraseQuery::ScorerControls::disableSortForTests;
  bool savedDedup =
      PhraseQuery::ScorerControls::disableRepeatDedupForTests;
  for (size_t slopIndex = 0; slopIndex < runs.size(); slopIndex++) {
    int32_t slop = slopIndex == 0 ? 0 : 3;
    for (bool disableSort : {false, true}) {
      for (bool disableDedup : {false, true}) {
        PhraseQuery::ScorerControls::disableSortForTests = disableSort;
        PhraseQuery::ScorerControls::disableRepeatDedupForTests =
            disableDedup;
        size_t key = (disableSort ? 2u : 0u) | (disableDedup ? 1u : 0u);
        runs[slopIndex][key] = runPhrase(
            index, context, terms, positions, slop);
      }
    }
  }
  PhraseQuery::ScorerControls::disableSortForTests = savedSort;
  PhraseQuery::ScorerControls::disableRepeatDedupForTests = savedDedup;

  for (size_t slopIndex = 0; slopIndex < runs.size(); slopIndex++) {
    ASSERT_FALSE(runs[slopIndex][0].empty());
    for (size_t key = 1; key < runs[slopIndex].size(); key++) {
      EXPECT_EQ(runs[slopIndex][0], runs[slopIndex][key])
          << "slop=" << (slopIndex == 0 ? 0 : 3) << " key=" << key;
    }
  }
}

TEST_F(PhraseSlopTest, bruteForceExistenceParity) {
  constexpr std::string_view vocab[] = {"a", "b", "c"};
  std::mt19937 rng(0x51A9u);
  TestIndex index;
  TestField field(index, "body_w");
  field.startIndexing();
  std::vector<std::vector<std::string>> docs;
  for (int32_t doc = 0; doc < 180; doc++) {
    int32_t len = 2 + (int32_t) (rng() % 8);
    std::string text;
    std::vector<std::string> tokens;
    for (int32_t i = 0; i < len; i++) {
      std::string token(vocab[rng() % std::size(vocab)]);
      if (!text.empty()) text.push_back(' ');
      text += token;
      tokens.push_back(std::move(token));
    }
    docs.push_back(tokens);
    field.add(doc, text);
  }
  index.flush();
  field.startReading();
  auto scope = index.pool.rewindScopeGuard();
  Query::Context context(index.pool, *index.reader);

  for (int32_t iteration = 0; iteration < 100; iteration++) {
    int32_t slots = 2 + (int32_t) (rng() % 4);
    std::vector<std::string_view> terms;
    std::vector<int32_t> offsets;
    int32_t offset = 0;
    for (int32_t slot = 0; slot < slots; slot++) {
      terms.push_back(vocab[rng() % std::size(vocab)]);
      if (slot > 0) offset += 1 + (int32_t) (rng() % 2);
      offsets.push_back(offset);
    }
    int32_t slop = 1 + (int32_t) (rng() % 6);
    auto hits = runPhrase(index, context, terms, offsets, slop);
    std::vector<bool> actual(docs.size());
    for (const PhraseHit& hit : hits) actual[(size_t) hit.doc] = true;
    for (size_t doc = 0; doc < docs.size(); doc++) {
      EXPECT_EQ(bruteExists(docs[doc], terms, offsets, slop), actual[doc])
          << "iteration=" << iteration << " doc=" << doc;
    }
  }
}

TEST_F(PhraseSlopTest, rawBoundsCoverScoresAndPulsedFallsBack) {
  const int32_t count = 4 * Postings::DOCS_BLOCK_SIZE + 19;
  TestIndex index;
  TestField field(index, "body_w");
  field.startIndexing();
  for (int32_t doc = 0; doc < count; doc++) {
    std::string text = doc % 3 == 0 ? "a x b " : "a b ";
    for (int32_t i = 0; i < doc % 23; i++) text += "pad ";
    field.add(doc, text);
  }
  index.flush();
  field.startReading();
  auto scope = index.pool.rewindScopeGuard();
  Query::Context context(index.pool, *index.reader);
  auto& segment = context.topReader.segments()[0];
  std::vector<std::string_view> terms = {"a", "b"};
  std::vector<int32_t> positions = {0, 1};
  PhraseQuery query("body_w", terms, positions, 2);
  auto* weight = query.createWeight(context, Query::NEED_SCORES);

  auto* bounds = dynamic_cast<PhraseQuery::SloppyScorer*>(
      weight->createScorer(index.pool, segment));
  auto* scores = dynamic_cast<PhraseQuery::SloppyScorer*>(
      weight->createScorer(index.pool, segment));
  ASSERT_NE(bounds, nullptr);
  ASSERT_NE(scores, nullptr);
  std::vector<float> actual((size_t) count);
  for (int32_t doc = scores->next(); doc != PostingsReader::END; doc = scores->next()) {
    actual[(size_t) doc] = scores->score();
  }
  for (int32_t target = 0; target < count;) {
    int32_t upTo = bounds->advanceShallow(target);
    ASSERT_NE(upTo, PostingsReader::END);
    float bound = bounds->getMaxScore(upTo);
    float maximum = 0.0f;
    for (int32_t doc = target; doc <= upTo; doc++) {
      maximum = std::max(maximum, actual[(size_t) doc]);
    }
    EXPECT_GE(bound, maximum) << "target=" << target << " upTo=" << upTo;
    target = upTo + 1;
  }

  bool saved = PhraseQuery::ScorerControls::disableRawBoundsForTests;
  PhraseQuery::ScorerControls::disableRawBoundsForTests = true;
  auto* tier1 = dynamic_cast<PhraseQuery::SloppyScorer*>(
      weight->createScorer(index.pool, segment));
  ASSERT_NE(tier1, nullptr);
  float tier1Bound = tier1->getMaxScore(PostingsReader::END);
  EXPECT_TRUE(std::isfinite(tier1Bound));
  EXPECT_GE(tier1Bound, *std::max_element(actual.begin(), actual.end()));
  PhraseQuery::ScorerControls::disableRawBoundsForTests = saved;

  TestIndex pulsedIndex;
  TestField pulsedField(pulsedIndex, "body_w");
  pulsedField.startIndexing();
  pulsedField.add(0, "pulse a b");
  pulsedIndex.flush();
  pulsedField.startReading();
  auto pulsedScope = pulsedIndex.pool.rewindScopeGuard();
  Query::Context pulsedContext(pulsedIndex.pool, *pulsedIndex.reader);
  PhraseQuery pulsedQuery("body_w", terms, positions, 1);
  auto* pulsedWeight = pulsedQuery.createWeight(pulsedContext, Query::NEED_SCORES);
  auto* pulsed = dynamic_cast<PhraseQuery::SloppyScorer*>(
      pulsedWeight->createScorer(pulsedIndex.pool, pulsedContext.topReader.segments()[0]));
  ASSERT_NE(pulsed, nullptr);
  EXPECT_EQ(PostingsReader::END, pulsed->advanceShallow(0));
  float fallback = pulsed->getMaxScore(PostingsReader::END);
  EXPECT_TRUE(std::isfinite(fallback));
  ASSERT_EQ(0, pulsed->next());
  EXPECT_GE(fallback, pulsed->score());
}

TEST_F(PhraseSlopTest, multiValueGapCrossesAtOneHundred) {
  TestIndex index;
  index.initWriter();
  index.inverter->schema->fieldTypeMap["body_mv"] = std::make_shared<TextFieldType>(
      "body_mv", FieldType::INDEX_DOCS_FREQS_POSITIONS | FieldType::MULTI_VALUED);
  TestField field(index, "body_mv");
  field.startIndexing();
  field.addStrings(0, {"a", "b"});
  index.flush();
  field.startReading();
  auto scope = index.pool.rewindScopeGuard();
  Query::Context context(index.pool, *index.reader);
  std::vector<std::string_view> terms = {"a", "b"};
  std::vector<int32_t> positions = {0, 1};

  EXPECT_TRUE(runPhrase(index, context, terms, positions, 99, false, "body_mv").empty());
  EXPECT_EQ(1u, runPhrase(index, context, terms, positions, 100, false, "body_mv").size());
  EXPECT_EQ(1u, runPhrase(index, context, terms, positions, 101, false, "body_mv").size());
}

// The far-position gallop is an internal permutation of the sloppy walk:
// hits, score bits, and freqs must be identical with it disabled. Long docs
// with a dense term throughout and sparse anchors exercise the skip on every
// candidate; the repeated-term case covers mixed group/non-group slots.
TEST_F(PhraseSlopTest, gallopMatchesStepwiseWalk) {
  std::mt19937 rng(0x6A110Fu);
  TestIndex index;
  TestField field(index, "body_w");
  field.startIndexing();
  for (int32_t doc = 0; doc < 120; doc++) {
    int32_t len = 200 + (int32_t) (rng() % 300);
    std::string text;
    for (int32_t i = 0; i < len; i++) {
      if (!text.empty()) text.push_back(' ');
      uint32_t r = rng() % 100;
      if (r < 45) text += "the";
      else if (r < 48) text += "rare";
      else if (r < 60) text += "mid";
      else text += "pad";
    }
    field.add(doc, text);
  }
  index.flush();
  field.startReading();
  auto scope = index.pool.rewindScopeGuard();
  Query::Context context(index.pool, *index.reader);

  std::vector<std::string_view> cases[] = {
      {"rare", "the"},
      {"the", "rare"},
      {"rare", "mid", "the"},
      {"the", "rare", "the"},
  };
  for (auto& terms : cases) {
    std::vector<int32_t> offsets;
    for (int32_t i = 0; i < (int32_t) terms.size(); i++) offsets.push_back(i);
    for (int32_t slop : {1, 3, 9}) {
      auto gallop = runPhrase(index, context, terms, offsets, slop);
      PhraseQuery::SloppyScorer::disableSloppyGallopForTests = true;
      auto stepwise = runPhrase(index, context, terms, offsets, slop);
      PhraseQuery::SloppyScorer::disableSloppyGallopForTests = false;
      EXPECT_FALSE(gallop.empty()) << terms[0] << " slop=" << slop;
      EXPECT_EQ(stepwise, gallop) << terms[0] << " slop=" << slop;
    }
  }
}
