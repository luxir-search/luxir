// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include "test/CollectionHelper.h"
#include "test/LocalReq.h"
#include "test/LuxirTest.h"
#include "test/QueryBuild.h"
#include "test/TestUtils.h"
#include "test/SchemaBuilder.h"
#include "luxir/query/BooleanQuery.h"
#include "luxir/query/CombinedFieldQuery.h"
#include "luxir/query/ConstantScoreQuery.h"
#include "luxir/query/DisMaxQuery.h"
#include "luxir/query/ForcePrepareQuery.h"
#include "luxir/query/FuzzyQuery.h"
#include "luxir/query/PrefixQuery.h"
#include "luxir/query/MinScoreQuery.h"
#include "luxir/query/ParseContext.h"
#include "luxir/query/PhraseQuery.h"
#include "luxir/query/QueryPruning.h"
#include "luxir/query/RescoreQuery.h"
#include "luxir/query/ProtobufQueryParser.h"
#include "luxir/query/ReadDeclarer.h"
#include "luxir/query/TermQuery.h"
#include "luxir/search/ProtobufSearchParser.h"
#include "luxir/search/ops/RootOp.h"
#include "luxir/search/ops/TopDocsReq.h"
#include "luxir/server/JsonRequest.h"
#include "luxir/value/ValueExprParser.h"

namespace luxir::test {
namespace {

#ifndef NDEBUG

class ReadDeclarationTest : public LuxirTest {
protected:
  CollectionHelper docs;
  bool previous = Query::verifyReadDeclarationsForTests;

  void SetUp() override {
    LuxirTest::SetUp();
    Query::verifyReadDeclarationsForTests = true;
    ASSERT_TRUE(docs.index(flatdoc("id", "one", "body_w", "alpha beta gamma"),
                           UpdateMessage::COMMIT).success);
  }

  void TearDown() override {
    ReadVerification::suppressForTests(std::nullopt);
    Query::verifyReadDeclarationsForTests = previous;
    LuxirTest::TearDown();
  }

  std::shared_ptr<IndexReader> reader() { return docs.getIndexWriter()->snapshots.readers.getReader(); }

  static uint8_t products(const ReadVerification& reads, const Query& query,
                          const Query::PlanningContext& planning, int32_t flags,
                          std::string_view term) {
    const auto& demands = reads.demands(query, planning, flags);
    auto found = demands.find({&planning, "body_w", term});
    return found == demands.end() ? 0 : found->second.products;
  }
};

TEST_F(ReadDeclarationTest, BooleanModesAndExclusion) {
  auto view = reader();
  MemPool pool;
  Query::Context context(pool, *view);
  auto& planning = context.planningContext();
  TermQuery alpha("body_w", "alpha");
  TermQuery beta("body_w", "beta");
  TermQuery gamma("body_w", "gamma");
  Query* required[] = {&alpha};
  Query* optional[] = {&beta};
  Query* excluded[] = {&gamma};
  BooleanQuery query(required, optional, excluded, {});
  query.validateLogical(planning);
  ReadVerification reads(false);
  reads.root(query, planning, 0);
  reads.root(query, planning, Query::NEED_SCORES);
  EXPECT_EQ(0, products(reads, query, planning, 0, "beta"));
  EXPECT_EQ(TERM_STATS | TERM_DOCS, products(reads, query, planning, 0, "gamma"));
  EXPECT_EQ(TERM_STATS | TERM_DOCS | FIELD_NORMS,
            products(reads, query, planning, Query::NEED_SCORES, "beta"));
  context.createRootWeight(query, 0);
  context.createRootWeight(query, Query::NEED_SCORES);
  EXPECT_GT(reads.countsFor(QueryKind::TERM).lookupsChecked, 0u);
}

TEST_F(ReadDeclarationTest, PhraseFilterAndScoreWrappers) {
  auto view = reader();
  MemPool pool;
  Query::Context context(pool, *view);
  auto& planning = context.planningContext();
  TermQuery alpha("body_w", "alpha", Similarity::TermStats{});
  std::string_view words[] = {"beta", "gamma"};
  int32_t positions[] = {0, 1};
  PhraseQuery phrase("body_w", words, positions);
  Query* filter[] = {&phrase};
  Query* required[] = {&alpha};
  BooleanQuery query(required, {}, {}, filter);
  ConstantScoreQuery constant(&alpha);
  MinScoreQuery minimum(&alpha, 0.1f);
  ValueExprOptions options;
  options.schema = view->schema().get();
  RescoreQuery scored(&alpha, ValueExprParser(options, context.arena()).parse("add(score,1)"));
  RescoreQuery unscored(&alpha, ValueExprParser(options, context.arena()).parse("1"));
  ReadVerification reads(false);
  for (const auto& [root, flags, wantedNorms] : std::initializer_list<std::tuple<Query*, int32_t, bool>>{
           {&query, Query::NEED_SCORES, true},
           {&alpha, 0, false},
           {&constant, Query::NEED_SCORES, false},
           {&minimum, Query::EXCLUSION_WINDOW_FILL, true},
           {&scored, Query::NEED_SCORES, true},
           {&scored, 0, false},
           {&unscored, Query::NEED_SCORES, false}}) {
    root->validateLogical(planning);
    reads.root(*root, planning, flags);
    EXPECT_EQ(TERM_STATS | TERM_DOCS | (wantedNorms ? FIELD_NORMS : 0),
              products(reads, *root, planning, flags, "alpha"));
    context.createRootWeight(*root, flags);
  }
  EXPECT_EQ(TERM_STATS | TERM_DOCS | TERM_POSITIONS,
            products(reads, query, planning, Query::NEED_SCORES, "beta"));
}

TEST_F(ReadDeclarationTest, ScoredMandatoryCacheAndDisMax) {
  auto view = reader();
  ASSERT_TRUE(view->filterCache() && view->filterCache()->enabled());
  MemPool pool;
  Query::Context context(pool, *view);
  auto& planning = context.planningContext();
  TermQuery alpha("body_w", "alpha");
  TermQuery beta("body_w", "beta");
  ConstantScoreQuery constant(&alpha, 2);
  Query* required[] = {&constant, &beta};
  BooleanQuery conjunction(required, {}, {}, {});
  ReadVerification reads(false);
  conjunction.validateLogical(planning);
  reads.root(conjunction, planning, Query::NEED_SCORES);
  context.createRootWeight(conjunction, Query::NEED_SCORES);
  // Alpha's main and cache weights each look up its field and term. Beta
  // has only its scored weight.
  EXPECT_EQ(6u, reads.countsFor(QueryKind::TERM).lookupsChecked);
  Query* disjuncts[] = {&alpha, &beta};
  auto* dismax = DisMaxQuery::make(pool, disjuncts, 0.5f);
  dismax->validateLogical(planning);
  for (int32_t flags : {0, Query::NEED_SCORES}) {
    reads.root(*dismax, planning, flags);
    EXPECT_EQ(TERM_STATS | TERM_DOCS | (flags ? FIELD_NORMS : 0),
              products(reads, *dismax, planning, flags, "alpha"));
    context.createRootWeight(*dismax, flags);
  }
}

TEST_F(ReadDeclarationTest, CombinedFieldWantsLengthsEvenWithoutAnOwnTerm) {
  auto view = reader();
  MemPool pool;
  Query::Context context(pool, *view);
  auto& planning = context.planningContext();
  std::string_view fields[] = {"body_w", "other_w"};
  float weights[] = {1, 1};
  CombinedFieldQuery::Term terms[] = {{0, "alpha"}};
  CombinedFieldQuery query(fields, weights, terms);
  query.validateLogical(planning);
  ReadVerification reads(false);
  for (int32_t flags : {0, Query::NEED_SCORES}) {
    reads.root(query, planning, flags);
    const auto& demands = reads.demands(query, planning, flags);
    EXPECT_EQ(TERM_STATS | TERM_DOCS, products(reads, query, planning, flags, "alpha"));
    auto secondField = demands.find({&planning, "other_w", std::nullopt});
    if (flags) {
      ASSERT_NE(demands.end(), secondField);
      EXPECT_EQ(FIELD_NORMS, secondField->second.products);
      // This fixture has no norms in the second field.
      EXPECT_EQ(1, secondField->second.unusedProducts(0));
    } else {
      EXPECT_EQ(demands.end(), secondField);
    }
    context.createRootWeight(query, flags);
  }
}

TEST_F(ReadDeclarationTest, DeclaredEntryResolvesOnlyOnLookup) {
  auto view = reader();
  MemPool pool;
  TermFactsCache cache(pool, *view);
  auto& field = cache.declareField("body_w");
  cache.declareTerm("body_w", "alpha", TERM_STATS);
  cache.declareTerm("body_w", "alpha", TERM_DOCS);
  cache.declareTerm("body_w", "alphx", TERM_STATS);
  EXPECT_TRUE(field.unresolved);
  EXPECT_TRUE(field.termsEnums.empty());
  EXPECT_TRUE(field.termInfos.at("alpha").unresolved);
  ASSERT_EQ(&field, cache.getCachedFieldInfo("body_w"));
  auto* term = cache.getCachedTerminfo(field, "alpha");
  ASSERT_NE(nullptr, term);
  EXPECT_FALSE(term->unresolved);
  EXPECT_EQ(TERM_STATS | TERM_DOCS, term->requested);
  EXPECT_EQ(TERM_STATS | TERM_DOCS | TERM_POSITIONS, term->available(0));
  EXPECT_EQ(nullptr, cache.getCachedTerminfo(field, "alphx"));
  EXPECT_FALSE(field.termInfos.at("alphx").unresolved);
  EXPECT_EQ(0, field.termInfos.at("alphx").available(0));
  cache.declareTerm("body_w", "alphx", FIELD_NORMS);
  EXPECT_EQ(TERM_STATS | FIELD_NORMS, field.termInfos.at("alphx").requested);
}

TEST_F(ReadDeclarationTest, PhraseMayStopBeforeADeclaredTerm) {
  auto view = reader();
  MemPool pool;
  Query::Context context(pool, *view);
  auto& planning = context.planningContext();
  std::string_view words[] = {"alphx", "beta"};
  int32_t positions[] = {0, 1};
  PhraseQuery phrase("body_w", words, positions);
  phrase.validateLogical(planning);
  ReadVerification reads(false);
  reads.root(phrase, planning, 0);
  context.createRootWeight(phrase, 0);
  const auto& demands = reads.demands(phrase, planning, 0);
  EXPECT_TRUE(demands.at({&planning, "body_w", "alphx"}).lookedUp);
  const auto& later = demands.at({&planning, "body_w", "beta"});
  EXPECT_FALSE(later.lookedUp);
  EXPECT_EQ(3, later.unusedProducts(0));
}

TEST_F(ReadDeclarationTest, EmptyRootDefersProofAttribution) {
  auto view = reader();
  MemPool pool;
  Query::Context context(pool, *view);
  auto& planning = context.planningContext();
  TermQuery absent("body_w", "zz");
  absent.validateLogical(planning);
  std::vector<std::string> notes;
  planning.proofNotes = &notes;
  planning.opName = "preflight";
  ReadVerification reads(false);
  reads.root(absent, planning, Query::NEED_SCORES);
  EXPECT_TRUE(reads.demands(absent, planning, Query::NEED_SCORES).empty());
  EXPECT_TRUE(notes.empty());
  planning.opName = "weight";
  context.createRootWeight(absent, Query::NEED_SCORES);
  context.createRootWeight(absent, Query::NEED_SCORES);
  ASSERT_EQ(1u, notes.size());
  EXPECT_TRUE(notes.front().starts_with("op weight: empty weight for the collection"));
}

TEST_F(ReadDeclarationTest, JoinProjectsIntoChildViewWithReductionFlags) {
  CollectionHelper nested("read_declaration_nested");
  SchemaBuilder schema;
  using Type = api::FieldDef::FieldClass;
  schema.field("items").type = Type::NESTED;
  schema.field("items.text").type = Type::TEXT;
  schema.set(nested.collection());
  CollectionHelper::UpdateBuilder input;
  input.addRaw([](auto& doc, auto& mr) {
    ASSERT_TRUE(api::read_json(doc, R"({"id":"one","items":{"text":"alpha"}})", mr));
  });
  input.commit();
  ASSERT_TRUE(nested.submit(input).success);

  auto req = localReq(nested.getSearchEngine());
  req->reader = nested.getIndexWriter()->snapshots.readers.getReader();
  req->schema = req->reader->schema();
  ParseView view(*req->schema, nullptr, &req->reader, req.get());
  ParseContext parse(req->requestPool, *req->schema, req->arena, CoerceContext{}, "q", nullptr, &view);
  auto& planning = *req->createPlanningContext(*req->reader);
  auto* context = Query::Context::create(&req->arena, planning);
  ReadVerification reads(false);
  for (std::string_view reduction : {"max", "none", "uniform"}) {
    auto child = qb::match(req->mr, "text", "alpha");
    if (reduction == "uniform") child = qb::constantScore(req->mr, child, 2);
    auto wire = qb::toParent(req->mr, "items", child, reduction == "none" ? "none" : "max");
    auto* query = ProtobufQueryParser(parse).parse(wire);
    query->validateLogical(planning);
    reads.root(*query, planning, Query::NEED_SCORES);
    const auto& demands = reads.demands(*query, planning, Query::NEED_SCORES);
    ASSERT_EQ(2u, demands.size());
    for (const auto& [key, demand] : demands) {
      const auto& [declaredPlanning, field, term] = key;
      EXPECT_NE(&planning, declaredPlanning);
      EXPECT_EQ("items", declaredPlanning->topReader.path());
      EXPECT_EQ("items.text", field);
      if (term) {
        EXPECT_EQ("alpha", *term);
        EXPECT_EQ(TERM_STATS | TERM_DOCS | (reduction == "max" ? FIELD_NORMS : 0),
                  demand.products);
      }
    }
    context->createRootWeight(*query, Query::NEED_SCORES);
  }
}

TEST_F(ReadDeclarationTest, ResidentCountOmitsItsDeclarationAndContext) {
  auto req = localReq(docs.getSearchEngine());
  req->reader = reader();
  req->schema = req->reader->schema();
  req->filterUses = std::make_shared<FilterCache::UseRegistry>(req->reader->filterCache(), *req->reader);
  TermQuery alpha("body_w", "alpha");
  TermQuery beta("body_w", "beta");
  Query* required[] = {&alpha, &beta};
  BooleanQuery conjunction(required, {}, {}, {});
  publishWholeMembershipForTests(*req->reader, conjunction);
  auto& top = req->topDocs().countLimit().limit(0);
  top.rawQuery() = qb::boolean(req->mr, {qb::match(req->mr, "body_w", "alpha"),
                                       qb::match(req->mr, "body_w", "beta")});
  auto* root = ProtobufSearchParser(*req).parse();
  auto* op = static_cast<TopDocsReq*>(root->subOps.at("q"));
  req->materialize();
  EXPECT_EQ(nullptr, op->weight);
  EXPECT_EQ(nullptr, op->weightContext);
  EXPECT_FALSE(op->planning.sharedTermFacts->has_value());
}

TEST_F(ReadDeclarationTest, CountAndRankingProjectSeparateModes) {
  auto req = localReq(docs.getSearchEngine());
  req->reader = reader();
  req->schema = req->reader->schema();
  auto& top = req->topDocs().countLimit().limit(1).getScores();
  top.rawQuery() = qb::boolean(req->mr, {qb::match(req->mr, "body_w", "alpha")},
                              {qb::match(req->mr, "body_w", "beta")});
  auto* root = ProtobufSearchParser(*req).parse();
  auto* op = static_cast<TopDocsReq*>(root->subOps.at("q"));
  ReadVerification reads(false);
  req->materialize();
  EXPECT_EQ(TERM_STATS | TERM_DOCS, products(reads, *op->query, op->planning, 0, "alpha"));
  EXPECT_EQ(0, products(reads, *op->query, op->planning, 0, "beta"));
  EXPECT_EQ(TERM_STATS | TERM_DOCS | FIELD_NORMS,
            products(reads, *op->query, op->planning, Query::NEED_SCORES, "beta"));
}

TEST_F(ReadDeclarationTest, MissingSiblingDemandIsCaughtAfterExemptRewrite) {
  auto view = reader();
  MemPool pool;
  Query::Context context(pool, *view);
  auto& planning = context.planningContext();
  FuzzyQuery fuzzy("body_w", "alphx", 1, 0);
  PrefixQuery prefix("body_w", "al");
  TermQuery term("body_w", "alpha");
  std::string_view words[] = {"beta", "gamma"};
  int32_t positions[] = {0, 1};
  PhraseQuery phrase("body_w", words, positions);
  Query* clauses[] = {&fuzzy, &prefix, &term, &phrase};
  BooleanQuery query({}, clauses, {}, {});
  query.validateLogical(planning);
  ReadVerification reads(false);
  ReadVerification::suppressForTests(QueryKind::TERM);
  reads.root(query, planning, Query::NEED_SCORES);
  try {
    context.createRootWeight(query, Query::NEED_SCORES);
    FAIL() << "undeclared term lookup succeeded";
  } catch (const std::logic_error& error) {
    EXPECT_TRUE(std::string_view(error.what()).ends_with("term alpha")) << error.what();
  }
  EXPECT_GT(reads.countsFor(QueryKind::FUZZY).lookupsExempt, 0u);
  EXPECT_GT(reads.countsFor(QueryKind::PREFIX).lookupsExempt, 0u);
}

TEST_F(ReadDeclarationTest, AnotherRootsDeclarationDoesNotCoverLookups) {
  auto req = localReq(docs.getSearchEngine());
  req->reader = reader();
  req->schema = req->reader->schema();
  auto& planning = *req->createPlanningContext(*req->reader);
  auto& context = *Query::Context::create(&req->arena, planning);
  TermQuery first("body_w", "alpha");
  TermQuery term("body_w", "alpha");
  std::string_view words[] = {"beta", "gamma"};
  int32_t positions[] = {0, 1};
  PhraseQuery phrase("body_w", words, positions);
  Query* clauses[] = {&term, &phrase};
  BooleanQuery second(clauses, {}, {}, {});
  first.validateLogical(planning);
  second.validateLogical(planning);
  ReadVerification reads(false);
  reads.root(first, planning, 0);
  context.createRootWeight(first, 0);
  auto* field = (**planning.sharedTermFacts).getCachedFieldInfo("body_w");
  ASSERT_NE(nullptr, field);
  EXPECT_EQ(TERM_STATS | TERM_DOCS, field->termInfos.at("alpha").requested);
  ReadVerification::suppressForTests(QueryKind::TERM);
  reads.root(second, planning, 0);
  try {
    context.createRootWeight(second, 0);
    FAIL() << "undeclared term lookup reused another root's declaration";
  } catch (const std::logic_error& error) {
    EXPECT_TRUE(std::string_view(error.what()).ends_with("term alpha")) << error.what();
  }
}

#endif

} // namespace
} // namespace luxir::test
