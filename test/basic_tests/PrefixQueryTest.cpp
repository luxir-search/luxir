#include <gtest/gtest.h>

#include <array>
#include <bit>
#include <cmath>
#include <limits>

#include "test/LuxirTest.h"
#include "test/TestIndex.h"
#include "test/TestUtils.h"
#include "test/CollectionHelper.h"
#include "test/LocalReq.h"
#include "test/QueryBuild.h"
#include "luxir/query/BooleanQuery.h"
#include "luxir/query/ConstantScoreQuery.h"
#include "luxir/query/PrefixQuery.h"
#include "luxir/query/QueryBuilder.h"
#include "luxir/query/TermQuery.h"
#include "luxir/reader/SkipStats.h"
#include "luxir/schema/Schema.h"
#include "luxir/search/Collector.h"

using namespace luxir;
using namespace luxir::test;

// Drive PrefixQuery::Weight::createScorer directly to assert per-segment doc ids.
class PrefixQueryTest : public LuxirTest {
protected:
  // Collect the docs a prefix query matches in one segment, in iteration order.
  std::vector<int32_t> prefixDocs(TestIndex& ti, std::string_view field,
                                  std::string_view prefix, int segOrd, int32_t flags = 0) {
    auto g = ti.pool.rewindScopeGuard();
    PrefixQuery pq(field, prefix);
    Query::Context ctx(ti.pool, *ti.reader);
    auto* weight = pq.createWeight(ctx, flags);
    Query::Scorer* scorer = weight->createScorer(ti.pool, ctx.topReader.segments()[segOrd]);
    std::vector<int32_t> docs;
    if (scorer != nullptr) {
      for (int32_t d = scorer->next(); d != PostingsReader::END; d = scorer->next()) {
        docs.push_back(d);
      }
    }
    return docs;
  }
};

using ScorerMode = MultiTermQuery::Weight::ScorerMode;

class ScorerModeGuard {
  ScorerMode saved;

public:
  explicit ScorerModeGuard(ScorerMode mode)
    : saved(MultiTermQuery::Weight::scorerModeForTests) {
    MultiTermQuery::Weight::scorerModeForTests = mode;
  }

  ~ScorerModeGuard() {
    MultiTermQuery::Weight::scorerModeForTests = saved;
  }
};

class SkipStatsGuard {
  bool savedEnabled;

public:
  SkipStatsGuard() : savedEnabled(SkipStats::enabled) {
    SkipStats::reset();
    SkipStats::enabled = true;
  }

  ~SkipStatsGuard() {
    SkipStats::enabled = savedEnabled;
    SkipStats::reset();
  }
};

static void buildSparsePrefixIndex(TestIndex& ti) {
  constexpr std::array<int32_t, 15> docs = {
      0, 2, 5, 127, 4095, 4096, 4100, 5000,
      8191, 8192, 9000, 12000, 16000, 18000, 20000};
  TestField field(ti, "foo_w");
  field.startIndexing();
  for (size_t i = 0; i < docs.size(); i++) {
    std::string body = "pre";
    body.push_back((char) ('a' + i));
    if ((i & 1) == 0) body.append(" common");
    field.add(docs[i], body);
  }
  ti.flush();
  field.startReading();
}

static std::vector<TopDocsCollector::ScoreDoc> runPrefixTopK(
    TestIndex& ti, int32_t k, ScorerMode mode, bool conjunction = false,
    bool allowPruning = true) {
  ScorerModeGuard guard(mode);
  PrefixQuery prefix("foo_w", "pre");
  TermQuery common("foo_w", "common");
  Query* required[] = {&prefix, &common};
  BooleanQuery both(required, {}, {}, {});
  Query& query = conjunction ? (Query&) both : (Query&) prefix;

  MemPool pool;
  Query::Context context(pool, *ti.reader);
  int32_t flags = Query::NEED_SCORES
      | (allowPruning ? Query::ALLOW_PRUNING : 0);
  auto* weight = query.createWeight(context, flags);
  TopDocsCollector collector(k);
  for (auto& segment : context.topReader.segments()) {
    auto* scorer = weight->createScorer(pool, segment);
    if (scorer != nullptr) {
      collectTopK(segment.ord, scorer, nullptr, nullptr, collector, allowPruning);
    }
  }
  auto sorted = collector.sort();
  return {sorted.begin(), sorted.end()};
}

static void expectSamePrefixTopK(
    std::span<const TopDocsCollector::ScoreDoc> expected,
    std::span<const TopDocsCollector::ScoreDoc> actual) {
  ASSERT_EQ(expected.size(), actual.size());
  for (size_t i = 0; i < expected.size(); i++) {
    EXPECT_EQ(expected[i].doc, actual[i].doc) << "rank=" << i;
    EXPECT_EQ(std::bit_cast<uint32_t>(expected[i].score),
              std::bit_cast<uint32_t>(actual[i].score)) << "rank=" << i;
  }
}

struct PrefixDecodeRun {
  std::vector<TopDocsCollector::ScoreDoc> docs;
  int64_t hits;
  int64_t blocks;
};

static PrefixDecodeRun runPrefixDecode(
    TestIndex& ti, ScorerMode mode, bool allowPruning,
    bool conjunction = false, int64_t topCount = 10) {
  ScorerModeGuard modeGuard(mode);
  SkipStatsGuard statsGuard;
  PrefixQuery prefix("foo_w", "pre");
  TermQuery early("foo_w", "preearly");
  Query* required[] = {&prefix, &early};
  BooleanQuery both(required, {}, {}, {});
  Query& query = conjunction ? (Query&) both : (Query&) prefix;
  MemPool pool;
  Query::Context context(pool, *ti.reader);
  int32_t flags = Query::NEED_SCORES
      | (allowPruning ? Query::ALLOW_PRUNING : 0);
  auto* weight = query.createWeight(context, flags);
  TopDocsCollector collector(topCount);
  for (auto& segment : context.topReader.segments()) {
    auto* supplier = weight->scorerSupplier(pool, segment);
    auto* scorer = supplier == nullptr ? nullptr : buildScorerForTests(
        pool, *supplier, std::numeric_limits<int64_t>::max());
    if (scorer == nullptr) {
      ADD_FAILURE() << "prefix scorer missing";
      return {};
    }
    collectTopK(segment.ord, scorer, nullptr, nullptr, collector, allowPruning);
  }
  auto sorted = collector.sort();
  return {{sorted.begin(), sorted.end()}, collector.totalHits(),
          SkipStats::docBlocksDecoded};
}

TEST_F(PrefixQueryTest, singleSegment) {
  TestIndex ti;
  TestField f(ti, "foo_w");
  f.startIndexing();
  f.add(0, "apple apricot");
  f.add(1, "banana");
  f.add(2, "apricot avocado");
  f.add(3, "cherry");
  f.add(4, "apple");
  ti.flush();
  f.startReading();

  using V = std::vector<int32_t>;
  // Several terms share the prefix; a doc matching two of them appears once.
  EXPECT_EQ(prefixDocs(ti, "foo_w", "ap", 0), (V{0, 2, 4}));
  EXPECT_EQ(prefixDocs(ti, "foo_w", "apr", 0), (V{0, 2}));
  EXPECT_EQ(prefixDocs(ti, "foo_w", "a", 0), (V{0, 2, 4}));
  EXPECT_EQ(prefixDocs(ti, "foo_w", "av", 0), (V{2}));
  EXPECT_EQ(prefixDocs(ti, "foo_w", "b", 0), (V{1}));
  // An exact full term is just a one-term prefix.
  EXPECT_EQ(prefixDocs(ti, "foo_w", "apple", 0), (V{0, 4}));
  // Empty prefix matches every doc that has the field.
  EXPECT_EQ(prefixDocs(ti, "foo_w", "", 0), (V{0, 1, 2, 3, 4}));
  EXPECT_TRUE(prefixDocs(ti, "foo_w", "z", 0).empty());
  EXPECT_TRUE(prefixDocs(ti, "foo_w", "cherryX", 0).empty());
  EXPECT_TRUE(prefixDocs(ti, "foo_w", "bb", 0).empty());
  // No matches: field absent in the index.
  EXPECT_TRUE(prefixDocs(ti, "missing_w", "a", 0).empty());
}

TEST_F(PrefixQueryTest, advanceAndScore) {
  TestIndex ti;
  TestField f(ti, "foo_w");
  f.startIndexing();
  f.add(0, "apple");
  f.add(2, "apricot");
  f.add(4, "avocado");
  f.add(5, "banana");
  ti.flush();
  f.startReading();

  // advance() jumps to the first match >= target.
  {
    auto g = ti.pool.rewindScopeGuard();
    PrefixQuery pq("foo_w", "a");  // matches docs 0, 2, 4
    Query::Context ctx(ti.pool, *ti.reader);
    auto* weight = pq.createWeight(ctx, 0);
    auto* scorer = weight->createScorer(ti.pool, ctx.topReader.segments()[0]);
    ASSERT_NE(scorer, nullptr);
    EXPECT_EQ(scorer->advance(2), 2);    // lands on an exact match
    EXPECT_EQ(scorer->advance(3), 4);    // skips to the next match
    EXPECT_EQ(scorer->advance(5), PostingsReader::END);  // past the last match
  }

  // With NEED_SCORES, every hit gets the constant boost (1.0 by default).
  {
    auto g = ti.pool.rewindScopeGuard();
    PrefixQuery pq("foo_w", "a");
    Query::Context ctx(ti.pool, *ti.reader);
    auto* weight = pq.createWeight(ctx, Query::NEED_SCORES);
    EXPECT_TRUE(weight->isConstantScoring());
    auto* scorer = weight->createScorer(ti.pool, ctx.topReader.segments()[0]);
    ASSERT_NE(scorer, nullptr);
    EXPECT_EQ(scorer->next(), 0);
    EXPECT_FLOAT_EQ(scorer->score(), 1.0f);
  }
}

TEST_F(PrefixQueryTest, multiSegment) {
  TestIndex ti;
  TestField f(ti, "foo_w");
  f.startIndexing();
  f.add(0, "apple");
  f.add(1, "apricot");
  ti.flush();
  f.startIndexing();
  f.add(0, "avocado");
  f.add(1, "banana");
  ti.flush();
  f.startReading();

  using V = std::vector<int32_t>;
  // Each segment is scored independently with its own doc-id space / maxDoc.
  EXPECT_EQ(prefixDocs(ti, "foo_w", "a", 0), (V{0, 1}));  // apple, apricot
  EXPECT_EQ(prefixDocs(ti, "foo_w", "a", 1), (V{0}));     // avocado
  EXPECT_TRUE(prefixDocs(ti, "foo_w", "b", 0).empty());   // no b* in seg 0
  EXPECT_EQ(prefixDocs(ti, "foo_w", "b", 1), (V{1}));     // banana
}

TEST_F(PrefixQueryTest, lazyScoredPathMatchesMaterialized) {
  TestIndex ti;
  buildSparsePrefixIndex(ti);

  for (int32_t k : {5, 15, 30}) {
    auto materialized = runPrefixTopK(ti, k, ScorerMode::FORCE_EAGER);
    auto lazy = runPrefixTopK(ti, k, ScorerMode::AUTO);
    auto heap = runPrefixTopK(ti, k, ScorerMode::FORCE_HEAP);
    expectSamePrefixTopK(materialized, lazy);
    expectSamePrefixTopK(materialized, heap);
  }

  auto materializedConjunction =
      runPrefixTopK(ti, 4, ScorerMode::FORCE_EAGER, true);
  auto lazyConjunction = runPrefixTopK(ti, 4, ScorerMode::AUTO, true);
  expectSamePrefixTopK(materializedConjunction, lazyConjunction);
  expectSamePrefixTopK(runPrefixTopK(ti, 4, ScorerMode::FORCE_HEAP, true),
                       lazyConjunction);
  expectSamePrefixTopK(runPrefixTopK(ti, 5, ScorerMode::AUTO, false, false),
                       runPrefixTopK(ti, 5, ScorerMode::AUTO));
  expectSamePrefixTopK(runPrefixTopK(ti, 4, ScorerMode::AUTO, true, false),
                       lazyConjunction);

  auto first = runPrefixTopK(ti, 15, ScorerMode::AUTO);
  auto second = runPrefixTopK(ti, 15, ScorerMode::AUTO);
  expectSamePrefixTopK(first, second);

  auto top10 = runPrefixTopK(ti, 10, ScorerMode::AUTO);
  auto top1000 = runPrefixTopK(ti, 1000, ScorerMode::AUTO);
  ASSERT_EQ(10u, top10.size());
  ASSERT_GE(top1000.size(), top10.size());
  expectSamePrefixTopK(top10, std::span(top1000).first(top10.size()));
}

TEST_F(PrefixQueryTest, lazyRoutingKeepsCountMaterialized) {
  TestIndex ti;
  buildSparsePrefixIndex(ti);
  auto& segment = ti.reader->segments()[0];

  {
    ScorerModeGuard guard(ScorerMode::AUTO);
    MemPool pool;
    Query::Context context(pool, *ti.reader);
    PrefixQuery prefix("foo_w", "pre");
    auto* weight = prefix.createWeight(
        context, Query::NEED_SCORES | Query::ALLOW_PRUNING);
    EXPECT_TRUE(weight->allowsPruning());
    auto* supplier = weight->scorerSupplier(pool, segment);
    auto* scorer = buildScorerForTests(
        pool, *supplier, std::numeric_limits<int64_t>::max());
    ASSERT_NE(dynamic_cast<UnionLazyScorer*>(scorer), nullptr);
    EXPECT_EQ(0, scorer->next());
    scorer->setMinCompetitiveScore(
        std::nextafter(1.0f, std::numeric_limits<float>::infinity()));
    EXPECT_EQ(PostingsReader::END, scorer->next());
  }

  {
    ScorerModeGuard guard(ScorerMode::FORCE_HEAP);
    MemPool pool;
    Query::Context context(pool, *ti.reader);
    PrefixQuery prefix("foo_w", "pre");
    auto* weight = prefix.createWeight(
        context, Query::NEED_SCORES | Query::ALLOW_PRUNING);
    auto* supplier = weight->scorerSupplier(pool, segment);
    auto* scorer = buildScorerForTests(
        pool, *supplier, std::numeric_limits<int64_t>::max());
    ASSERT_NE(dynamic_cast<UnionHeapScorer*>(scorer), nullptr);
    EXPECT_EQ(0, scorer->next());
    scorer->setMinCompetitiveScore(
        std::nextafter(1.0f, std::numeric_limits<float>::infinity()));
    EXPECT_EQ(PostingsReader::END, scorer->next());
  }

  {
    ScorerModeGuard guard(ScorerMode::AUTO);
    MemPool pool;
    Query::Context context(pool, *ti.reader);
    PrefixQuery prefix("foo_w", "pre");
    auto* weight = prefix.createWeight(context, Query::NEED_SCORES);
    EXPECT_FALSE(weight->allowsPruning());
    auto* supplier = weight->scorerSupplier(pool, segment);
    auto* scorer = buildScorerForTests(
        pool, *supplier, std::numeric_limits<int64_t>::max());
    ASSERT_NE(dynamic_cast<MultiTermQuery::Scorer*>(scorer), nullptr);
    EXPECT_EQ(dynamic_cast<UnionLazyScorer*>(scorer), nullptr);
  }

  // Conjunction-driven consumption (finite leadCost) keeps the lazy union.
  {
    ScorerModeGuard guard(ScorerMode::AUTO);
    MemPool pool;
    Query::Context context(pool, *ti.reader);
    PrefixQuery prefix("foo_w", "pre");
    auto* weight = prefix.createWeight(
        context, Query::NEED_SCORES | Query::ALLOW_PRUNING);
    auto* supplier = weight->scorerSupplier(pool, segment);
    auto* scorer = buildScorerForTests(pool, *supplier, 1);
    ASSERT_NE(dynamic_cast<UnionLazyScorer*>(scorer), nullptr);
  }

  // Unscored contexts stay eager no matter which mode is forced.
  for (ScorerMode mode : {ScorerMode::AUTO, ScorerMode::FORCE_EAGER,
                          ScorerMode::FORCE_HEAP}) {
    ScorerModeGuard guard(mode);
    MemPool pool;
    Query::Context context(pool, *ti.reader);
    PrefixQuery prefix("foo_w", "pre");
    auto* scorer = prefix.createWeight(context, 0)->createScorer(pool, segment);
    ASSERT_NE(dynamic_cast<MultiTermQuery::Scorer*>(scorer), nullptr);
    EXPECT_EQ(dynamic_cast<UnionLazyScorer*>(scorer), nullptr);
    int32_t count = 0;
    for (int32_t doc = scorer->next(); doc != PostingsReader::END; doc = scorer->next()) {
      count++;
    }
    EXPECT_EQ(15, count);
  }

  for (ScorerMode mode : {ScorerMode::AUTO, ScorerMode::FORCE_EAGER,
                          ScorerMode::FORCE_HEAP}) {
    ScorerModeGuard guard(mode);
    PrefixQuery prefix("foo_w", "pre");
    ConstantScoreQuery constant(&prefix, 2.5f);
    MemPool pool;
    Query::Context context(pool, *ti.reader);
    auto* scorer = constant.createWeight(
        context, Query::NEED_SCORES | Query::ALLOW_PRUNING)
        ->createScorer(pool, segment);
    ASSERT_NE(scorer, nullptr);
    int32_t count = 0;
    for (int32_t doc = scorer->next(); doc != PostingsReader::END; doc = scorer->next()) {
      EXPECT_FLOAT_EQ(2.5f, scorer->score());
      count++;
    }
    EXPECT_EQ(15, count);
  }
}

TEST_F(PrefixQueryTest, lazyCrossoverRoutesHugeExpansionToHeap) {
  TestIndex ti;
  TestField field(ti, "foo_w");
  field.startIndexing();
  constexpr int32_t termCount =
      (int32_t) MultiTermQuery::Weight::MAX_LAZY_TERMS + 1;
  for (int32_t doc = 0; doc < termCount; doc++) {
    field.add(doc, "pre" + std::to_string(doc));
  }
  ti.flush();
  field.startReading();

  // Past the term cap AUTO routes to the heap scorer; every term here is a
  // pulsed single-doc term, so full iteration drives the pulsed-heavy path
  // to exhaustion.
  for (ScorerMode mode : {ScorerMode::AUTO, ScorerMode::FORCE_WINDOWED}) {
    ScorerModeGuard guard(mode);
    MemPool pool;
    Query::Context context(pool, *ti.reader);
    PrefixQuery prefix("foo_w", "pre");
    auto* scorer = prefix.createWeight(
        context, Query::NEED_SCORES | Query::ALLOW_PRUNING)
        ->createScorer(pool, context.topReader.segments()[0]);
    if (mode == ScorerMode::AUTO) {
      ASSERT_NE(dynamic_cast<UnionHeapScorer*>(scorer), nullptr);
    } else {
      // The forced windowed arm has no term cap.
      ASSERT_NE(dynamic_cast<UnionLazyScorer*>(scorer), nullptr);
    }
    int32_t count = 0;
    for (int32_t doc = scorer->next(); doc != PostingsReader::END; doc = scorer->next()) {
      count++;
    }
    EXPECT_EQ(termCount, count);
  }
}

TEST_F(PrefixQueryTest, lazyRoutingPinsPruningLeadAndDecodeVolume) {
  TestIndex ti;
  TestField field(ti, "foo_w");
  field.startIndexing();
  std::string lateBody = "preearly";
  for (int32_t term = 0; term < 16; term++) {
    lateBody.append(" prelate").append(std::to_string(term));
  }
  constexpr int32_t docCount = 20000;
  for (int32_t doc = 0; doc < docCount; doc++) {
    bool late = doc >= 13000 && doc <= 19350 && ((doc - 13000) % 50) == 0;
    field.add(doc, late ? lateBody : "preearly");
  }
  ti.flush();
  field.startReading();

  auto eager = runPrefixDecode(ti, ScorerMode::FORCE_EAGER, true);
  auto lazy = runPrefixDecode(ti, ScorerMode::AUTO, true);
  auto heap = runPrefixDecode(ti, ScorerMode::FORCE_HEAP, true);
  auto exactCount = runPrefixDecode(ti, ScorerMode::AUTO, false);
  auto exactCountKnob = runPrefixDecode(ti, ScorerMode::FORCE_EAGER, false);
  auto driven = runPrefixDecode(ti, ScorerMode::AUTO, true, true);
  auto drivenKnob = runPrefixDecode(ti, ScorerMode::FORCE_EAGER, true, true);
  auto lazyUnfilled =
      runPrefixDecode(ti, ScorerMode::AUTO, true, false, docCount + 1);
  auto eagerUnfilled =
      runPrefixDecode(ti, ScorerMode::FORCE_EAGER, true, false, docCount + 1);

  expectSamePrefixTopK(eager.docs, lazy.docs);
  expectSamePrefixTopK(eager.docs, heap.docs);
  EXPECT_EQ(10, heap.hits);
  EXPECT_LE(heap.blocks, eager.blocks);
  expectSamePrefixTopK(eager.docs, exactCount.docs);
  expectSamePrefixTopK(exactCount.docs, exactCountKnob.docs);
  expectSamePrefixTopK(driven.docs, drivenKnob.docs);
  expectSamePrefixTopK(lazyUnfilled.docs, eagerUnfilled.docs);
  EXPECT_EQ(10, eager.hits);
  EXPECT_EQ(10, lazy.hits);
  EXPECT_EQ(docCount, exactCount.hits);
  EXPECT_EQ(docCount, exactCountKnob.hits);
  EXPECT_EQ(10, driven.hits);
  EXPECT_EQ(docCount, lazyUnfilled.hits);
  EXPECT_GT(eager.blocks, 0);
  EXPECT_LT(lazy.blocks, eager.blocks);
  EXPECT_EQ(exactCount.blocks, exactCountKnob.blocks);
  EXPECT_EQ(exactCount.blocks, eager.blocks);
  EXPECT_EQ(driven.blocks, drivenKnob.blocks);
  EXPECT_LT(lazy.blocks, lazyUnfilled.blocks);
  EXPECT_LE(lazyUnfilled.blocks, eagerUnfilled.blocks);
}

// QueryBuilder validates that prefix queries only run on term-backed fields.
TEST_F(PrefixQueryTest, fieldTypeValidation) {
  auto schema = Schema::createDefaultSchema();
  MemPool pool;
  QueryBuilder builder(pool, *schema, CoerceContext{});

  EXPECT_NE(builder.createPrefixQuery("foo_s", "ab"), nullptr);   // STRING
  EXPECT_NE(builder.createPrefixQuery("foo_w", "ab"), nullptr);   // TEXT
  EXPECT_NE(builder.createPrefixQuery("id", "d"), nullptr);       // ID
  EXPECT_THROW(builder.createPrefixQuery("foo_sc", "ab"), std::runtime_error);  // STRING column only
  EXPECT_THROW(builder.createPrefixQuery("foo_i", "1"), std::runtime_error);  // INT: no terms
}

// End-to-end coverage for protobuf parsing, query building, and execution.
class PrefixQueryE2ETest : public LuxirTest {
public:
  CollectionHelper helper;

  PrefixQueryE2ETest() {
    helper.index(flatdoc("id", "d1", "body_w", "apple apricot", "color_s", "red"),
                 UpdateMessage::NO_COMMIT);
    helper.index(flatdoc("id", "d2", "body_w", "banana", "color_s", "reddish"),
                 UpdateMessage::NO_COMMIT);
    helper.index(flatdoc("id", "d3", "body_w", "apricot avocado", "color_s", "blue"),
                 UpdateMessage::NO_COMMIT);
    helper.index(flatdoc("id", "d4", "body_w", "cherry apple", "color_s", "red"),
                 UpdateMessage::COMMIT);
  }

  int64_t prefixCount(std::string_view field, std::string_view prefix) {
    auto req = localReq(helper.getSearchEngine());
    req->collection("main").topDocs("q").prefixQuery(field, prefix).withStats();
    req->execute();
    return req->getMatchCount();
  }

  std::vector<std::string> prefixIds(std::string_view field, std::string_view prefix) {
    auto req = localReq(helper.getSearchEngine());
    req->collection("main").topDocs("q").prefixQuery(field, prefix).fields({"id"}).limit(100);
    req->execute();
    std::vector<std::string> ids;
    for (auto& doc : req->getDocs()) {
      if (auto* v = find(doc, "id")) ids.push_back(std::get<std::string>(*v));
    }
    std::sort(ids.begin(), ids.end());
    return ids;
  }
};

TEST_F(PrefixQueryE2ETest, textField) {
  EXPECT_EQ(prefixCount("body_w", "ap"), 3);   // apple, apricot -> d1, d3, d4
  EXPECT_EQ(prefixCount("body_w", "a"), 3);    // + avocado, still d1, d3, d4
  EXPECT_EQ(prefixCount("body_w", "ban"), 1);  // banana -> d2
  EXPECT_EQ(prefixCount("body_w", ""), 4);     // every doc has an indexed term
  EXPECT_EQ(prefixCount("body_w", "z"), 0);    // no matches
}

TEST_F(PrefixQueryE2ETest, asBooleanFilter) {
  // Exercises prefix as a filter clause under conjunction planning.
  auto req = localReq(helper.getSearchEngine());
  req->collection("main");
  auto& cur = req->topDocs("q");
  cur.rawQuery() = qb::boolean(cur.mr(),
      /*required=*/{qb::match(cur.mr(), "body_w", "apple")},
      /*optional=*/{}, /*prohibited=*/{},
      /*filter=*/{qb::prefix(cur.mr(), "color_s", "re")});
  cur.fields({"id"}).limit(100);
  req->execute();

  std::vector<std::string> ids;
  for (auto& doc : req->getDocs()) {
    if (auto* v = find(doc, "id")) ids.push_back(std::get<std::string>(*v));
  }
  std::sort(ids.begin(), ids.end());
  EXPECT_EQ(ids, (std::vector<std::string>{"d1", "d4"}));
}

TEST_F(PrefixQueryE2ETest, textPrefixIsNormalized) {
  // multiterm input folds the way the field folds (never tokenized), so a
  // capitalized prefix finds lowercased indexed terms
  helper.index(flatdoc("id", "d5", "title_un", "Blade Runner"), UpdateMessage::COMMIT);
  EXPECT_EQ(prefixCount("title_un", "Runn"), 1);
  EXPECT_EQ(prefixCount("title_un", "runn"), 1);
  EXPECT_EQ(prefixCount("title_un", "BLADE"), 1);
  // STRING fields are unanalyzed: the prefix stays verbatim
  EXPECT_EQ(prefixCount("color_s", "RED"), 0);
}

TEST_F(PrefixQueryE2ETest, stringField) {
  EXPECT_EQ(prefixCount("color_s", "re"), 3);    // red (d1, d4), reddish (d2)
  EXPECT_EQ(prefixCount("color_s", "red"), 3);   // "red" still matches "reddish"
  EXPECT_EQ(prefixCount("color_s", "redd"), 1);  // only reddish -> d2
  EXPECT_EQ(prefixIds("color_s", "blue"), (std::vector<std::string>{"d3"}));
  EXPECT_EQ(prefixIds("color_s", "re"), (std::vector<std::string>{"d1", "d2", "d4"}));
}
