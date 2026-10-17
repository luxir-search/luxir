// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include <gtest/gtest.h>

#include <array>
#include <limits>
#include <optional>
#include <random>
#include <vector>

#include "luxir/query/AllQuery.h"
#include "luxir/query/BooleanQuery.h"
#include "luxir/query/BoostQuery.h"
#include "luxir/query/ConstantScoreQuery.h"
#include "luxir/query/ExistsQuery.h"
#include "luxir/query/NumericPredicateQuery.h"
#include "luxir/query/PhraseQuery.h"
#include "luxir/query/PrefixQuery.h"
#include "luxir/query/QueryPrep.h"
#include "luxir/reader/PostingsReader.h"
#include "test/CollectionHelper.h"
#include "test/LuxirTest.h"
#include "test/SchemaBuilder.h"
#include "test/TestUtils.h"

using namespace luxir;
using namespace luxir::test;

namespace {

using Use = Query::ExecutionUse;
using Answer = Query::ScorerSupplier::BulkAnswer;

struct Products {
  MemPool pool;
  Query::Context context;
  Query::ScorerSupplier* supplier;

  Products(IndexReader& reader, Query& query, int32_t flags = 0)
      : context(pool, reader),
        supplier(query.createWeight(context, flags)->scorerSupplier(
            pool, reader.segments()[0], Query::SupplierExecutionMode::RAW_MEMBERSHIP)) {}

  Query::ScorerPlan* pullPlan(int64_t leadCost, Use use = Use::MATCH_WINDOWS) {
    return supplier->resolve(pool, supplier->makePlanContext(
        Query::Demand::fromLeadCost(leadCost, use)));
  }
};

std::vector<std::pair<int32_t, int32_t>> windows(int32_t maxDoc) {
  const int32_t l0 = Postings::DOCS_BLOCK_SIZE;
  const int32_t l1 = DocsEnumMeta::L1_DOCS;
  std::vector<std::pair<int32_t, int32_t>> result{
      {0, maxDoc}, {maxDoc, maxDoc}, {0, 1}, {maxDoc - 1, maxDoc},
      {0, maxDoc}, {l1, l1}, {l0 - 1, l0 + 1}, {l1 - 1, l1 + 1}, {3 * l1 - 1, 3 * l1 + 1},
      {0, l0}, {l0, l1}, {l1, maxDoc}, {1, 1}, {0, maxDoc}};
  std::mt19937 random(0x58a301);
  for (int32_t i = 0; i < 40; i++) {
    int32_t min = (int32_t) (random() % (uint32_t) (maxDoc + 1));
    int32_t length = (int32_t) (random() % (uint32_t) (maxDoc - min + 1));
    result.emplace_back(min, min + length);
  }
  return result;
}

std::vector<int32_t> pullDocs(Query::Scorer& scorer, int32_t min,
                              int32_t max, bool approximation) {
  std::vector<int32_t> docs;
  if (min >= max) return docs;
  int32_t doc = approximation ? scorer.approximationAdvance(min) : scorer.advance(min);
  while (doc < max) {
    if (!approximation || scorer.matches()) docs.push_back(doc);
    doc = approximation ? scorer.approximationNext() : scorer.next();
  }
  return docs;
}

// The same observer covers nested verifiers, including prohibited children
// and probes. Fresh-product parity alone would miss identical overproduction.
struct VerificationBounds {
#ifndef NDEBUG
  static inline thread_local int32_t min = 0, max = 0;
  static inline thread_local int64_t observations = 0;
  static void observe(int32_t doc) {
    observations++;
    EXPECT_GE(doc, min);
    EXPECT_LT(doc, max);
  }
  VerificationBounds(int32_t begin, int32_t end) {
    min = begin;
    max = end;
    NumericPredicateQuery::verificationObserver = observe;
  }
  ~VerificationBounds() { NumericPredicateQuery::verificationObserver = nullptr; }
#else
  VerificationBounds(int32_t, int32_t) {}
#endif
};

std::vector<int32_t> bulkDocs(BulkScorer& bulk, DocSet* domain,
                              int32_t min, int32_t max) {
  std::vector<int32_t> docs;
  // Call even for an empty range: it must leave no stale output behind.
  for (;;) {
    ScoreWindow window;
    VerificationBounds bounds(min, max);
    int32_t next = bulk.matchNextWindow(window, domain, min, max);
    for (int32_t i = 0; i < window.size; i++) {
      int32_t doc = window.docs[(size_t) i];
      EXPECT_GE(doc, min);
      EXPECT_LT(doc, max);
      docs.push_back(doc);
    }
    if (next == PostingsReader::END || next >= max) return docs;
    EXPECT_GT(next, min);
    if (next <= min) return docs;
    min = next;
  }
}

int64_t bulkCount(BulkScorer& bulk, DocSet* domain, int32_t min, int32_t max,
                  DocSetBuilder* output = nullptr) {
  int64_t count = 0;
  for (;;) {
    VerificationBounds bounds(min, max);
    int32_t next = bulk.countNextWindow(count, output, domain, min, max);
    if (next == PostingsReader::END || next >= max) return count;
    EXPECT_GT(next, min);
    if (next <= min) return count;
    min = next;
  }
}

void checkPull(IndexReader& reader, Query& query, int64_t leadCost) {
  Products reused(reader, query);
  ASSERT_NE(nullptr, reused.supplier);
  auto* plan = reused.pullPlan(leadCost);
  ASSERT_TRUE(plan->supportsReset());
  auto* scorer = plan->build(reused.pool);
  ASSERT_NE(nullptr, scorer);
  EXPECT_TRUE(plan->supportsReset());
  bool twoPhase = plan->shape().reportedTwoPhase == Query::ReportedTwoPhase::YES;
  int32_t epoch = 0;
  for (auto [min, max] : windows(reader.segments()[0].maxDoc())) {
    SCOPED_TRACE(::testing::Message() << "pull epoch=" << epoch << " range=" << min << ':' << max);
    Products fresh(reader, query);
    auto* expected = fresh.pullPlan(leadCost)->build(fresh.pool);
    ASSERT_NE(nullptr, expected);
    // Exhaustion hints belong to the old traversal, including the bounds
    // that constant scorers clamp in place.
    scorer->setMinCompetitiveScore(std::numeric_limits<float>::infinity());
    scorer->reset();
    EXPECT_EQ(-1, scorer->docId());
    EXPECT_EQ(-1, scorer->approximationDocId());
    bool approximation = twoPhase && (epoch++ & 1) != 0;
    EXPECT_EQ(pullDocs(*expected, min, max, approximation),
              pullDocs(*scorer, min, max, approximation));
    EXPECT_TRUE(plan->supportsReset());
  }
}

void checkBoundedPull(IndexReader& reader, Query& query, int64_t leadCost) {
  Products reused(reader, query);
  auto* scorer = reused.pullPlan(leadCost)->build(reused.pool);
  for (auto [min, max] : windows(reader.segments()[0].maxDoc())) {
    Products fresh(reader, query);
    auto expected = pullDocs(*fresh.pullPlan(leadCost)->build(fresh.pool), min, max, false);
    scorer->reset();
    std::vector<int32_t> actual;
    {
      VerificationBounds bounds(min, max);
      for (int32_t target = min; target < max;) {
        int32_t doc = scorer->advanceWindow(target, max);
        if (doc >= max) break;
        actual.push_back(doc);
        target = doc + 1;
      }
    }
    EXPECT_EQ(expected, actual);
    // A rejected point probe can leave an unverified cursor at the next
    // window. Filling must verify it, even when docId already equals min.
    scorer->reset();
    if (min > 0) {
      VerificationBounds bounds(min - 1, min);
      scorer->advanceWindow(min - 1, min);
    }
    std::vector<uint64_t> words((size_t)(max - min + 63) / 64);
    {
      VerificationBounds bounds(min, max);
      scorer->fillWindowBits(words, min, max);
    }
    actual.clear();
    for (int32_t doc = min; doc < max; doc++) {
      int32_t bit = doc - min;
      if ((words[(size_t)bit >> 6] >> (bit & 63)) & 1) actual.push_back(doc);
    }
    EXPECT_EQ(expected, actual);
    // A conjunction partner may re-enter below a match the scorer found past
    // empty navigation chunks. The answer is still that match.
    scorer->reset();
    int32_t first = scorer->advanceWindow(min, max);
    if (first < max && first > min) {
      EXPECT_EQ(first, scorer->advanceWindow(min + 1, max));
    }
  }
}

void checkBulk(IndexReader& reader, Query& query, std::optional<bool> expectedAvailable,
               DocSet* domain = nullptr, Query* attached = nullptr, bool probe = false,
               Query* enclosing = nullptr) {
  int32_t maxDoc = reader.segments()[0].maxDoc();
  for (Use use : {Use::MATCH_WINDOWS, Use::COUNT_WINDOWS}) {
    Products reused(reader, query);
    ASSERT_NE(nullptr, reused.supplier);
    auto planFor = [&](Products& product) {
      Query::ScorerSupplier::BulkScorerContext context;
      if (enclosing != nullptr) {
        auto filters = product.pool.make_span<Query::ScorerSupplier*>(1);
        filters[0] = enclosing->createWeight(product.context, 0)->scorerSupplier(
            product.pool, reader.segments()[0], Query::SupplierExecutionMode::RAW_MEMBERSHIP);
        context.filterSuppliers = filters;
        context.filterCost = filters[0]->cost();
        context.requireFilterConsumption = true;
      }
      return product.supplier->planBulk(use, context);
    };
    auto plan = planFor(reused);
    if (!expectedAvailable.value_or(plan.available == Answer::YES)) {
      EXPECT_EQ(Answer::NO, plan.supportsReposition());
      continue;
    }
    ASSERT_EQ(Answer::YES, plan.available);
    ASSERT_EQ(Answer::YES, plan.supportsReposition());
    auto* bulk = reused.supplier->buildBulk(reused.pool, plan);
    ASSERT_NE(nullptr, bulk);
    auto attach = [&](Products& product, BulkScorer* target) {
      auto* supplier = attached->createWeight(product.context, 0)->scorerSupplier(
          product.pool, reader.segments()[0], Query::SupplierExecutionMode::RAW_MEMBERSHIP);
      ASSERT_NE(nullptr, supplier);
      auto* childPlan = supplier->resolve(product.pool, supplier->makePlanContext(
          Query::Demand::fromLeadCost(maxDoc, use)));
      ASSERT_TRUE(childPlan->supportsReset());
      auto scorers = product.pool.make_span<Query::Scorer*>(1);
      scorers[0] = childPlan->build(product.pool);
      ASSERT_TRUE(target->attachWindowFilter(product.pool.make<WindowFilter>(
          product.pool, scorers, probe, supplier->cost())));
    };
    if (attached != nullptr) attach(reused, bulk);
    int32_t epoch = 0;
    for (auto [min, max] : windows(maxDoc)) {
      SCOPED_TRACE(::testing::Message() << "bulk epoch=" << epoch++ << " range=" << min << ':' << max);
      Products fresh(reader, query);
      auto freshPlan = planFor(fresh);
      auto* expected = fresh.supplier->buildBulk(fresh.pool, freshPlan);
      ASSERT_NE(nullptr, expected);
      if (attached != nullptr) attach(fresh, expected);
      bulk->reposition(min);
      if (use == Use::MATCH_WINDOWS) {
        EXPECT_EQ(bulkDocs(*expected, domain, min, max), bulkDocs(*bulk, domain, min, max));
        // Cover both rewinding partial state and continuing past bounded END.
        if (epoch & 1) {
          EXPECT_EQ(bulkDocs(*expected, domain, max, maxDoc), bulkDocs(*bulk, domain, max, maxDoc));
        }
      } else {
        EXPECT_EQ(bulkCount(*expected, domain, min, max), bulkCount(*bulk, domain, min, max));
        if (epoch & 1) {
          EXPECT_EQ(bulkCount(*expected, domain, max, maxDoc), bulkCount(*bulk, domain, max, maxDoc));
        }
      }
      EXPECT_EQ(Answer::YES, plan.supportsReposition());
    }
    // Leaf consumers continue at the old bound, even when a sparse scorer
    // returned an out-of-range lookahead doc or bounded END. Compare the
    // concatenation against an independent, unbounded traversal.
    Products fresh(reader, query);
    auto freshPlan = planFor(fresh);
    auto* expected = fresh.supplier->buildBulk(fresh.pool, freshPlan);
    ASSERT_NE(nullptr, expected);
    if (attached != nullptr) attach(fresh, expected);
    bulk->reposition(0);
    if (use == Use::MATCH_WINDOWS) {
      std::vector<int32_t> actual;
      for (int32_t min = 0; min < maxDoc; min += 512) {
        auto leaf = bulkDocs(*bulk, domain, min, std::min(min + 512, maxDoc));
        actual.insert(actual.end(), leaf.begin(), leaf.end());
      }
      EXPECT_EQ(bulkDocs(*expected, domain, 0, maxDoc), actual);
    } else {
      int64_t actual = 0;
      for (int32_t min = 0; min < maxDoc; min += 512) {
        actual += bulkCount(*bulk, domain, min, std::min(min + 512, maxDoc));
      }
      EXPECT_EQ(bulkCount(*expected, domain, 0, maxDoc), actual);
    }
  }
}

void checkChangingDomains(IndexReader& reader, Query& query, DocSet* array, DocSet* bits) {
  int32_t maxDoc = reader.segments()[0].maxDoc();
  for (Use use : {Use::MATCH_WINDOWS, Use::COUNT_WINDOWS}) {
    Products reused(reader, query);
    auto plan = reused.supplier->planBulk(use, {});
    ASSERT_EQ(Answer::YES, plan.supportsReposition());
    auto* bulk = reused.supplier->buildBulk(reused.pool, plan);
    int32_t epoch = 0;
    for (DocSet* domain : {array, (DocSet*) nullptr, bits, (DocSet*) nullptr,
                          array, bits, (DocSet*) nullptr, (DocSet*) nullptr}) {
      Products fresh(reader, query);
      auto freshPlan = fresh.supplier->planBulk(use, {});
      auto* expected = fresh.supplier->buildBulk(fresh.pool, freshPlan);
      int32_t min = epoch % 2 == 0 ? maxDoc / 2 : 0;
      int32_t max = maxDoc;
      epoch++;
      bulk->reposition(min);
      if (use == Use::MATCH_WINDOWS) {
        EXPECT_EQ(bulkDocs(*expected, domain, min, max), bulkDocs(*bulk, domain, min, max));
      } else if (epoch % 3 == 0) {
        DocSetBuilder want(maxDoc), actual(maxDoc);
        EXPECT_EQ(bulkCount(*expected, domain, min, max, &want),
                  bulkCount(*bulk, domain, min, max, &actual));
        auto wantDocs = want.build();
        auto actualDocs = actual.build();
        QueryPrep::DocSetScorer wantScorer(wantDocs.get(), maxDoc);
        QueryPrep::DocSetScorer actualScorer(actualDocs.get(), maxDoc);
        EXPECT_EQ(pullDocs(wantScorer, 0, maxDoc, false),
                  pullDocs(actualScorer, 0, maxDoc, false));
      } else {
        EXPECT_EQ(bulkCount(*expected, domain, min, max), bulkCount(*bulk, domain, min, max));
      }
    }
  }
}

template<class Enum>
void checkEnum(const TermsEnum::PostingsState& state, int32_t maxDoc) {
  Enum reused(state);
  for (auto [min, max] : windows(maxDoc)) {
    SCOPED_TRACE(::testing::Message() << "enum range=" << min << ':' << max);
    Enum fresh(state);
    reused.reset();
    ASSERT_EQ(-1, reused.docId());
    int32_t expected = fresh.advance(min);
    int32_t actual = reused.advance(min);
    for (;;) {
      ASSERT_EQ(expected, actual);
      if (actual == PostingsReader::END) break;
      if constexpr (Enum::TIER == DocsEnumTier::FREQS) {
        EXPECT_EQ(fresh.termFreq(), reused.termFreq());
      }
      if (actual >= max) break;
      expected = fresh.next();
      actual = reused.next();
    }
    // Crossing from bit production to frequency iteration in the next epoch
    // must clear docsOnlyConsumed as well as the ordinary doc cursor.
    reused.reset();
    Enum freshBits(state);
    std::vector<uint64_t> actualBits((size_t) (max - min + 63) / 64);
    auto expectedBits = actualBits;
    reused.intoBitSet(actualBits, min, max);
    freshBits.intoBitSet(expectedBits, min, max);
    EXPECT_EQ(expectedBits, actualBits);
  }
}

} // namespace

class ScorerResetTest : public LuxirTest {};

TEST_F(ScorerResetTest, freshProductsAcrossProtocolsAndEncodings) {
  constexpr int32_t N = 6 * DocsEnumMeta::L1_DOCS + 137;
  CollectionHelper helper;
  SchemaBuilder schema;
  for (std::string_view name : {"point", "column", "sparse", "scan", "multi", "rare"}) {
    auto& field = schema.field(name);
    field.type = api::FieldDef::FieldClass::INT;
    field.index = name == "point" ? api::FieldDef::IndexMode::RANGE : api::FieldDef::IndexMode::NONE;
    field.column = true;
    field.multi = name == "multi";
  }
  auto& tags = schema.field("tags");
  tags.type = api::FieldDef::FieldClass::STRING;
  tags.index = api::FieldDef::IndexMode::MATCH;
  tags.multi = true;
  schema.set(helper.collection());
  auto writer = helper.getIndexWriter();
  auto& inverter = writer->obtainInverter();
  auto& text = inverter.getIndexHandler("body_w");
  auto& tagValues = inverter.getIndexHandler("tags");
  auto& point = inverter.getIndexHandler("point");
  auto& column = inverter.getIndexHandler("column");
  auto& sparseValues = inverter.getIndexHandler("sparse");
  auto& scanValues = inverter.getIndexHandler("scan");
  auto& multiValues = inverter.getIndexHandler("multi");
  auto& rareValues = inverter.getIndexHandler("rare");
  std::mt19937 random(7919);
  for (int32_t doc = 0; doc < N; doc++) {
    inverter.startDoc();
    std::string body = "qdense";
    bool wordDoc = (random() & 3) != 0;
    if (wordDoc) body += " qword qword";
    if (doc % 3 == 0) body += " qpacked qpacked qpacked";
    if (doc < 43) body += " qshort";
    if (doc == 137) body += " qpulse";
    if (doc == 3 * DocsEnumMeta::L1_DOCS + 17
        || doc == 5 * DocsEnumMeta::L1_DOCS + 41) body += " qlate";
    text.index(inverter, std::string_view(body));
    std::vector<std::string_view> tagList{"qdense"};
    if (wordDoc) tagList.push_back("qword");
    if (doc % 3 == 0) tagList.push_back("qpacked");
    if (doc < 43) tagList.push_back("qshort");
    if (doc == 137) tagList.push_back("qpulse");
    tagValues.index(inverter, std::span<const std::string_view>(tagList));
    point.index(inverter, doc);
    column.index(inverter, doc);
    scanValues.index(inverter, doc % 73);
    if (doc % 5 != 0) {
      std::array<int64_t, 2> values{doc % 73, 100 + doc % 79};
      multiValues.index(inverter, std::span<const int64_t>(values));
    }
    if (doc % 7 == 0) sparseValues.index(inverter, doc);
    if (doc % 1000 == 0) rareValues.index(inverter, doc);
    inverter.finishDoc();
  }
  writer->releaseInverter(inverter);
  writer->commit();
  auto reader = writer->snapshots.readers.getReader();
  ASSERT_EQ(1, reader->segments().size());

  MemPool queries;
  auto term = [&](std::string_view name) { return queries.make<TermQuery>("body_w", name); };
  auto clauses = [&](std::initializer_list<Query*> values) {
    auto result = queries.make_span<Query*>(values.size());
    std::copy(values.begin(), values.end(), result.begin());
    return result;
  };
  auto boolean = [&](std::span<Query*> must, std::span<Query*> should,
                     std::span<Query*> prohibited = {}, std::span<Query*> filters = {}, int minMatch = 0) {
    return queries.make<BooleanQuery>(must, should, prohibited, filters, minMatch);
  };
  Query* dense = term("qdense");
  Query* word = term("qword");
  Query* packed = term("qpacked");
  Query* shortTerm = term("qshort");
  Query* pulse = term("qpulse");
  Query* conjunction = boolean(clauses({word, packed}), {});
  Query* lookahead = boolean(clauses({dense, term("qlate")}), {});
  Query* disjunction = boolean({}, clauses({shortTerm, pulse, packed}));
  Query* twoWay = boolean({}, clauses({shortTerm, packed}));
  Query* nested = boolean({}, clauses({conjunction, shortTerm}));
  Query* nestedFiltered = boolean(clauses({disjunction}), {}, {}, clauses({word}));
  Query* excluded = boolean(clauses({dense}), {}, clauses({disjunction}));
  Query* filtered = boolean({}, clauses({word, packed}), {}, clauses({dense}), 1);
  Query* filterOnly = boolean({}, {}, {}, clauses({word, packed}));
  Query* optional = boolean(clauses({packed}), clauses({word, shortTerm}));
  Query* all = queries.make<AllQuery>();
  Query* prefix = queries.make<PrefixQuery>("body_w", "q");
  Query* zone = queries.make<NumericPredicateQuery>("column", 100, N / 2);
  // Matches only past several empty zone-map iteration windows.
  Query* zoneLate = queries.make<NumericPredicateQuery>(
      "column", 3 * DocsEnumMeta::L1_DOCS + 5, 4 * DocsEnumMeta::L1_DOCS);
  Query* pointArray = queries.make<NumericPredicateQuery>("point", 125, 140);
  Query* pointBits = queries.make<NumericPredicateQuery>("point", 100, N / 2);
  Query* pointComplement = queries.make<NumericPredicateQuery>("point", 0, N - 100);
  Query* sparse = queries.make<NumericPredicateQuery>("sparse", 100, N / 2);
  Query* scan = queries.make<NumericPredicateQuery>("scan", 20, 40);
  Query* multi = queries.make<NumericPredicateQuery>("multi", 120, 140);
  Query* exists = queries.make<ExistsQuery>("sparse");
  Query* existsDense = queries.make<ExistsQuery>("column");
  Query* existsRare = queries.make<ExistsQuery>("rare");
  Query* constant = queries.make<ConstantScoreQuery>(disjunction);
  Query* boosted = queries.make<BoostQuery>(disjunction, 3.0f);
  Query* minMatch = boolean({}, clauses({word, packed, shortTerm}), {}, {}, 2);
  Query* numericConjunction = boolean(clauses({word, scan}), {});
  Query* numericUnion = boolean({}, clauses({packed, sparse}));
  Query* numericNot = boolean(clauses({word}), {}, clauses({multi}));
  for (Query* query : {dense, word, packed, shortTerm, pulse, conjunction, lookahead, disjunction,
                      nested, nestedFiltered, excluded, filtered, filterOnly, optional, all, prefix,
                      zone, zoneLate, pointArray, pointBits, pointComplement, sparse, scan,
                      multi, exists, existsDense, existsRare, constant, boosted, twoWay, minMatch,
                      numericConjunction, numericUnion, numericNot}) {
    SCOPED_TRACE(::testing::Message() << "query=" << (int) query->getKind());
    checkPull(*reader, *query, N);
    checkPull(*reader, *query, 0);
    checkBoundedPull(*reader, *query, N);
    checkBoundedPull(*reader, *query, 0);
  }
  for (Query* query : {dense, word, packed, shortTerm, pulse, conjunction, lookahead, disjunction,
                      nested, nestedFiltered, excluded, filtered, filterOnly, optional, all, prefix,
                      zone, pointArray, pointBits, pointComplement, boosted, twoWay}) {
    SCOPED_TRACE(::testing::Message() << "bulk query=" << (int) query->getKind());
    checkBulk(*reader, *query, true);
  }
  for (Query* query : {numericConjunction, numericUnion, numericNot}) {
    SCOPED_TRACE(::testing::Message() << "numeric bulk "
        << (query == numericConjunction ? "conjunction"
            : query == numericUnion ? "union" : "not"));
    checkBulk(*reader, *query, std::nullopt);
  }
  // Exhaustive numeric conjunctions: dense fills, per-candidate leapfrog,
  // and the sparse route admitted for an enclosing filter. In the pulse
  // case the physical demand still selects sparse numeric verification.
  for (int32_t mode : {0, 1, 2}) {
    using Conj = BooleanQuery::ConjunctionBulkScorer;
    SearchOverridesGuard guard(BooleanQuery::disableExactTermCountForTests,
        Conj::denseThresholdInverseForTests,
        Conj::denseLeapfrogThresholdForTests, Conj::termLeadLeapfrogThresholdForTests);
    BooleanQuery::disableExactTermCountForTests = true;
    Conj::denseThresholdInverseForTests = mode == 2 ? 1 : N;
    Conj::denseLeapfrogThresholdForTests = mode == 1 ? N : 0;
    Conj::termLeadLeapfrogThresholdForTests = mode == 1 ? N : 0;
    for (Query* lead : {word, shortTerm, pulse}) {
      SCOPED_TRACE(::testing::Message() << "numeric FILTER mode=" << mode
          << " lead=" << (lead == word ? "word" : lead == shortTerm ? "short" : "pulse"));
      Query* body = boolean(clauses({lead, packed}), {});
      checkBulk(*reader, *body, true, nullptr, nullptr, false, zone);
    }
  }
  Query* numericExcluded = boolean(clauses({word}), {}, clauses({pointArray}), clauses({zone}));
  checkBulk(*reader, *numericExcluded, true);
  checkBulk(*reader, *sparse, false);
  checkBulk(*reader, *scan, false);
  checkBulk(*reader, *exists, false);
  checkBulk(*reader, *constant, false);
  {
    // Ordinary unscored normalization drops score-only optionals. Keep
    // them here to exercise the reset contracts of the MandOpt products.
    SearchOverridesGuard guard(BooleanQuery::disableUnscoredOptionalDropForTests);
    BooleanQuery::disableUnscoredOptionalDropForTests = true;
    checkPull(*reader, *optional, N);
    checkPull(*reader, *optional, 0);
    checkBulk(*reader, *optional, true);
  }
  checkBulk(*reader, *dense, true, nullptr, word);
  checkBulk(*reader, *dense, true, nullptr, scan);
  checkBulk(*reader, *dense, true, nullptr, scan, true);
  checkBulk(*reader, *disjunction, true, nullptr, dense);
  checkBulk(*reader, *conjunction, true, nullptr, dense, true);

  Query* exactConjunction = boolean(clauses({word, packed}), {}, {}, clauses({dense}));
  checkBulk(*reader, *exactConjunction, true);
  {
    SearchOverridesGuard guard(BooleanQuery::disableExactTermCountForTests);
    BooleanQuery::disableExactTermCountForTests = true;
    checkBulk(*reader, *filtered, true);
    checkBulk(*reader, *exactConjunction, true);
  }
  // A dominant term engages COUNT's union identity. A supplied domain
  // selects its fallback, whose state must also survive a new epoch.
  Query* dominantUnion = boolean({}, clauses({dense, shortTerm, pulse}));
  checkBulk(*reader, *dominantUnion, true);
  checkBulk(*reader, *dominantUnion, true, nullptr, word);
  {
    Products product(*reader, *dominantUnion);
    auto plan = product.supplier->planBulk(Use::COUNT_WINDOWS, {});
    ASSERT_EQ(Answer::YES, plan.supportsReposition());
    auto* bulk = product.supplier->buildBulk(product.pool, plan);
    Products filter(*reader, *word);
    auto* filterPlan = filter.pullPlan(N);
    auto scorers = filter.pool.make_span<Query::Scorer*>(1);
    scorers[0] = filterPlan->build(filter.pool);
    ASSERT_TRUE(bulk->attachWindowFilter(
        filter.pool.make<WindowFilter>(filter.pool, scorers)));
    // Fresh-product parity alone cannot catch a shortcut that ignores a
    // filter in both products. This union is all docs, so the filter is the
    // independent membership oracle, before and after a reset.
    Products oracle(*reader, *word);
    auto* oracleScorer = oracle.pullPlan(N)->build(oracle.pool);
    int64_t want = (int64_t) pullDocs(*oracleScorer, 0, N, false).size();
    EXPECT_EQ(want, bulkCount(*bulk, nullptr, 0, N));
    bulk->reposition(0);
    EXPECT_EQ(want, bulkCount(*bulk, nullptr, 0, N));
  }

  std::vector<int32_t> domainDocs;
  std::vector<uint64_t> domainWords(FixedBitSet::sizeInWords(N));
  for (int32_t doc = 0; doc < N; doc += 11) {
    domainDocs.push_back(doc);
    domainWords[(size_t) doc >> 6] |= 1ULL << (doc & 63);
  }
  ArrDocSet array(std::move(domainDocs));
  BitDocSet bits(FixedBitSet(domainWords.data(), N));
  for (Query* query : {dominantUnion, disjunction, all, filterOnly}) {
    checkChangingDomains(*reader, *query, &array, &bits);
  }
  {
    SearchOverridesGuard statsGuard(SkipStats::enabled);
    SkipStats::enabled = true;
    int64_t before = SkipStats::filteredDisjBatchPostingsFeedEngagements;
    Query* sparseFiltered = boolean({}, clauses({word, packed}), {}, clauses({shortTerm}), 1);
    for (Use use : {Use::MATCH_WINDOWS, Use::COUNT_WINDOWS}) {
      Products reused(*reader, *sparseFiltered);
      auto plan = reused.supplier->planBulk(use, {});
      ASSERT_EQ(Answer::YES, plan.supportsReposition());
      auto* bulk = reused.supplier->buildBulk(reused.pool, plan);
      for (int epoch = 0; epoch < 2; epoch++) {
        bulk->reposition(0);
        int32_t min = 0;
        for (DocSet* domain : {(DocSet*)nullptr, (DocSet*)&array,
                              (DocSet*)&bits, (DocSet*)nullptr}) {
          int32_t max = min == 39 ? N : min + 13;
          Products fresh(*reader, *sparseFiltered);
          auto freshPlan = fresh.supplier->planBulk(use, {});
          auto* expected = fresh.supplier->buildBulk(fresh.pool, freshPlan);
          if (use == Use::MATCH_WINDOWS) {
            EXPECT_EQ(bulkDocs(*expected, domain, min, max),
                      bulkDocs(*bulk, domain, min, max));
          } else {
            EXPECT_EQ(bulkCount(*expected, domain, min, max),
                      bulkCount(*bulk, domain, min, max));
          }
          min = max;
        }
      }
    }
    EXPECT_GT(SkipStats::filteredDisjBatchPostingsFeedEngagements, before);
  }
  for (DocSet* domain : std::array<DocSet*, 2>{&array, &bits}) {
    checkBulk(*reader, *dense, true, domain);
    checkBulk(*reader, *disjunction, true, domain);
    checkBulk(*reader, *dominantUnion, true, domain);
    checkBulk(*reader, *all, true, domain);
    MemPool pool;
    QueryPrep::DocSetSupplier supplier(domain, reader->segments()[0]);
    auto* plan = supplier.resolve(pool, Query::PlanContext::fromLeadCost(N));
    ASSERT_TRUE(plan->supportsReset());
    auto* pull = plan->build(pool);
    auto bulkPlan = supplier.planBulk(Use::MATCH_WINDOWS, {});
    ASSERT_EQ(Answer::YES, bulkPlan.supportsReposition());
    auto* bulk = supplier.buildBulk(pool, bulkPlan);
    for (auto [min, max] : windows(N)) {
      QueryPrep::DocSetScorer freshPull(domain, N);
      pull->reset();
      EXPECT_EQ(pullDocs(freshPull, min, max, false), pullDocs(*pull, min, max, false));
      freshPull.reset();
      pull->reset();
      std::vector<uint64_t> want((size_t) (max - min + 63) / 64);
      auto actual = want;
      freshPull.fillWindowBits(want, min, max);
      pull->fillWindowBits(actual, min, max);
      EXPECT_EQ(want, actual);
      MemPool freshPool;
      DocSetBulkScorer freshBulk(freshPool, domain, N);
      bulk->reposition(min);
      EXPECT_EQ(bulkDocs(freshBulk, nullptr, min, max), bulkDocs(*bulk, nullptr, min, max));
    }
  }

  std::array<std::string_view, 2> phraseTerms{"qdense", "qword"};
  std::array<int32_t, 2> positions{0, 1};
  PhraseQuery phrase("body_w", phraseTerms, positions);
  PhraseQuery sloppy("body_w", phraseTerms, positions, 3);
  Query* mixedMust = boolean(clauses({dense, &phrase}), {});
  Query* mixedShould = boolean({}, clauses({packed, &sloppy}));
  Query* mixedNot = boolean(clauses({dense}), {}, clauses({&phrase}));
  // Check the executing phrase arms before later traversals can populate a
  // prohibited membership cache and legitimately resolve to materialized docs.
  for (Query* query : std::array<Query*, 5>{&phrase, &sloppy, mixedMust, mixedShould, mixedNot}) {
    Products product(*reader, *query);
    ASSERT_NE(nullptr, product.supplier);
    EXPECT_FALSE(product.pullPlan(N)->supportsReset());
    EXPECT_EQ(Answer::NO, product.supplier->planBulk(Use::MATCH_WINDOWS, {}).supportsReposition());
  }
  // Scored bit fills must retain exact cursor state. In particular a sparse
  // predicate must not leave an unverified doc at the next window's start.
  for (Query* query : {zone, sparse, scan, multi, all, prefix, pointBits}) {
    for (int64_t leadCost : {int64_t(0), int64_t(N)}) {
      Products reused(*reader, *query, Query::NEED_SCORES);
      auto* scorer = reused.pullPlan(leadCost, Use::PULL)->build(reused.pool);
      std::vector<int32_t> actual;
      for (int32_t start = 0; start < 257; start += 13) {
        int32_t end = std::min(257, start + 13);
        uint64_t bits = 0;
        scorer->fillScoredWindowBits(std::span(&bits, 1), start, end);
        while (bits != 0) {
          actual.push_back(start + (int32_t)std::countr_zero(bits));
          bits &= bits - 1;
        }
      }
      if (scorer->docId() < 257) scorer->advance(257);
      for (int32_t doc = scorer->docId(); doc < N; doc = scorer->next()) {
        actual.push_back(doc);
      }
      Products fresh(*reader, *query, Query::NEED_SCORES);
      EXPECT_EQ(pullDocs(*fresh.pullPlan(leadCost, Use::PULL)->build(fresh.pool),
                         0, N, false), actual);
    }
  }
  // Membership drains retain ownership after a scored prefix. In particular,
  // a conjunction may have driven a phrase's postings enums externally.
  Query* wrappedPhraseConj = queries.make<ConstantScoreQuery>(mixedMust);
  Query* wrappedTerm = queries.make<ConstantScoreQuery>(word);
  Query* constantUnion = boolean({}, clauses({wrappedPhraseConj, wrappedTerm}));
  Query* wrappedPrivatePhrase = queries.make<ConstantScoreQuery>(mixedShould);
  for (Query* query : {mixedMust, mixedShould, mixedNot, wrappedPhraseConj,
                      wrappedPrivatePhrase, constantUnion}) {
    Products reused(*reader, *query, Query::NEED_SCORES);
    auto* scorer = reused.pullPlan(N, Use::PULL)->build(reused.pool);
    scorer->advance(17);
    scorer->prepareMembership();
    constexpr int32_t begin = 257, end = 2 * DocsEnumMeta::L1_DOCS + 1;
    Products fresh(*reader, *query, Query::NEED_SCORES);
    auto expected = pullDocs(*fresh.pullPlan(N, Use::PULL)->build(fresh.pool), begin, end, false);
    std::vector<uint64_t> actual((end - begin + 63) / 64);
    scorer->fillWindowBits(actual, begin, end);
    std::vector<int32_t> docs;
    for (int32_t doc = begin; doc < end; doc++) {
      int32_t bit = doc - begin;
      if ((actual[(size_t)bit >> 6] >> (bit & 63)) & 1) docs.push_back(doc);
    }
    EXPECT_EQ(expected, docs);
  }
  {
    SearchOverridesGuard statsGuard(SkipStats::enabled);
    SkipStats::enabled = true;
    int64_t intersectionsBefore = SkipStats::maxScoreRequiredIntersectionWindows;
    Products reused(*reader, *constantUnion, Query::NEED_SCORES | Query::ALLOW_PRUNING);
    auto plan = reused.supplier->planBulk(Use::SCORED_WINDOWS, {});
    ASSERT_EQ(Answer::YES, plan.available);
    auto* bulk = reused.supplier->buildBulk(reused.pool, plan);
    // All matching docs belong to both clauses. A floor requiring both
    // engages the scored bit intersection, followed by another scored window.
    std::vector<int32_t> scored;
    int32_t cursor = 0;
    constexpr int32_t stop = 2 * DocsEnumMeta::L1_DOCS;
    while (cursor < stop) {
      ScoreWindow out;
      int32_t next = bulk->scoreNextWindow(out, nullptr, cursor, stop, 1.5f);
      scored.insert(scored.end(), out.docs.begin(), out.docs.begin() + out.size);
      if (next == PostingsReader::END || next >= stop) break;
      ASSERT_GT(next, cursor);
      cursor = next;
    }
    EXPECT_GT(SkipStats::maxScoreRequiredIntersectionWindows, intersectionsBefore);
    Products expected(*reader, *word);
    EXPECT_EQ(pullDocs(*expected.pullPlan(N)->build(expected.pool), 0, stop, false), scored);
    Products drain(*reader, *constantUnion, Query::NEED_SCORES);
    auto drainPlan = drain.supplier->planBulk(Use::SCORED_WINDOWS, {});
    ASSERT_EQ(Answer::YES, drainPlan.available);
    auto* draining = drain.supplier->buildBulk(drain.pool, drainPlan);
    ScoreWindow out;
    int32_t resume = draining->scoreNextWindow(out, nullptr, 0, stop,
                                              std::numeric_limits<float>::lowest());
    ASSERT_NE(PostingsReader::END, resume);
    draining->prepareMembership();
    Products rest(*reader, *word);
    auto remaining = pullDocs(*rest.pullPlan(N)->build(rest.pool), resume, N, false);
    EXPECT_EQ((int64_t)remaining.size(), bulkCount(*draining, nullptr, resume, N));
  }
#ifndef NDEBUG
  EXPECT_GT(VerificationBounds::observations, 0);
#endif

  for (Query* query : {dense, disjunction, all, exists, constant, prefix, zone, pointArray}) {
    for (int32_t flags : {Query::NEED_SCORES, Query::NEED_SCORES | Query::ALLOW_PRUNING}) {
      Products scored(*reader, *query, flags);
      EXPECT_FALSE(scored.pullPlan(N, Use::PULL)->supportsReset());
      // Membership requests must also reject scored weights. Without
      // pruning, prefix expansion reaches the eager bulk arm's score guard.
      for (Use use : {Use::MATCH_WINDOWS, Use::COUNT_WINDOWS, Use::SCORED_WINDOWS}) {
        auto plan = scored.supplier->planBulk(use, {});
        if (flags == Query::NEED_SCORES
            && (query == prefix || query == zone || query == pointArray)) {
          EXPECT_EQ(Answer::YES, plan.available);
        }
        EXPECT_EQ(Answer::NO, plan.supportsReposition());
      }
    }
  }
  for (Query* query : {dense, conjunction, disjunction, filtered, filterOnly, optional,
                      nestedFiltered, all, exists, constant, prefix, zone, pointArray}) {
    Products unscored(*reader, *query);
    for (Use use : {Use::PULL, Use::SCORED_WINDOWS, Use::EXACT_CANDIDATE_SCORING}) {
      EXPECT_EQ(Answer::NO, unscored.supplier->planBulk(use, {}).supportsReposition());
    }
  }
  for (auto mode : {Query::PlanContext::MultiTermScorerMode::FORCE_WINDOWED,
                    Query::PlanContext::MultiTermScorerMode::FORCE_HEAP}) {
    Products lazy(*reader, *prefix, Query::NEED_SCORES | Query::ALLOW_PRUNING);
    auto context = lazy.supplier->makePlanContext(Query::Demand::fromLeadCost(N));
    context.multiTermScorerModeForTests = mode;
    EXPECT_FALSE(lazy.supplier->resolve(lazy.pool, context)->supportsReset());
  }

  for (std::string_view field : {"body_w", "tags"}) {
    auto* fieldReader = reader->segments()[0].postingsReader().fieldInfo(field);
    ASSERT_TRUE((fieldReader != nullptr));
    const auto& info = *fieldReader;
    TermsEnum terms(queries, reader->segments()[0].postingsReader(), info);
    for (std::string_view name : {"qdense", "qword", "qpacked", "qshort", "qpulse"}) {
      SCOPED_TRACE(::testing::Message() << field << ':' << name);
      ASSERT_TRUE(terms.seek(name));
      auto state = terms.postingsState();
      if (name == "qpacked") {
        EXPECT_GT(state.packedBlockCount, DocsEnumMeta::L1_PERIOD);
      }
      if (name == "qpulse") {
        EXPECT_EQ(state.docsStart, state.docsEnd);
      }
      checkEnum<DocsOnlyEnum>(state, N);
      checkEnum<DocsFreqEnum>(state, N);
    }
  }
}
