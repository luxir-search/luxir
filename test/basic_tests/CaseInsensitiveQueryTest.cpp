// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include <map>

#include "test/LuxirTest.h"
#include "test/CollectionHelper.h"
#include "test/LocalReq.h"
#include "test/QueryBuild.h"
#include "test/TestUtils.h"
#include "luxir/server/JsonRequest.h"
#include "luxir/schema/Schema.h"
#include "luxir/query/QueryBuilder.h"
#include "luxir/search/FilterCache.h"

using namespace luxir;
using namespace luxir::test;

// case_insensitive over fields that keep case:
// name_s is a STRING without a normalizer, body_w a whitespace TEXT field.
class CaseInsensitiveQueryTest : public LuxirTest {
protected:
  CollectionHelper helper{"main"};
  using Ids = std::vector<std::string>;

  void SetUp() override {
    ASSERT_TRUE(helper.indexAll(std::array{
      flatdoc("id", "a", "name_s", "FOO", "body_w", "Error HAPPENED", "title_t", "Error"),
      flatdoc("id", "b", "name_s", "foo", "body_w", "error happened", "title_t", "error"),
      flatdoc("id", "c", "name_s", "Foo", "body_w", "ERROR"),
      flatdoc("id", "d", "name_s", "bar", "body_w", "other"),
      flatdoc("id", "e", "name_s", "\u212a", "body_w", "other"),  // KELVIN SIGN
    }, UpdateMessage::COMMIT).success);
  }

  LocalReqHandle run(std::string_view json) {
    auto req = localReq(helper.getSearchEngine());
    auto kept = api::build::arenaStr(req->mr, json);
    parseQueryRequest(kept, req->rawRequest(), req->mr);
    req->collection("main").execute(false);
    return req;
  }

  static Ids ids(const LocalReq& req) {
    EXPECT_TRUE(req.ok()) << req.errorMsg();
    Ids out;
    for (const auto& doc : req.getDocs()) out.push_back(std::get<std::string>(*find(doc, "id")));
    std::sort(out.begin(), out.end());
    return out;
  }

  Ids hits(std::string_view query) {
    return ids(*run(std::format(R"({{"query":{},"fields":["id"],"limit":-1}})", query)));
  }

  std::map<std::string, float> scores(std::string_view expression) {
    auto req = localReq(helper.getSearchEngine());
    req->topDocs("q").exprQuery(expression).fields({"id"}).limit(-1).getScores();
    req->execute(false);
    EXPECT_TRUE(req->ok()) << req->errorMsg();
    std::map<std::string, float> out;
    if (!req->ok() || req->docList()->row_count == 0) return out;
    const auto& columns = req->docList()->columns;
    const auto& ids = std::get<api::ColStr>(columns.at("id").kind).v;
    const auto& values = std::get<api::ColFloat>(columns.at("_score_").kind).v;
    for (size_t i = 0; i < ids.size(); i++) out.emplace(std::string(ids[i]), values[i]);
    return out;
  }

  static FilterKey key(const Query& query) {
    FilterKeyContext context;
    FilterKeyBuilder out;
    auto scope = query.appendFilterKey(out, context);
    return std::move(out).finishStructural(scope, context);
  }

  Ids expr(std::string_view expression) {
    auto req = localReq(helper.getSearchEngine());
    req->collection("main").topDocs("q").exprQuery(expression).fields({"id"}).limit(-1);
    req->execute(false);
    return ids(*req);
  }
};

TEST_F(CaseInsensitiveQueryTest, stringLiteralsIgnoreCase) {
  for (auto query : {
      R"({"wildcard":{"field":"name_s","pattern":"fO?","case_insensitive":true}})",
      R"({"regex":{"field":"name_s","pattern":"fo+","case_insensitive":true}})",
      R"({"match":{"name_s":"fOO","case_insensitive":true}})",
      R"({"match":{"field":"name_s","val":"FOO","case_insensitive":true}})"}) {
    EXPECT_EQ((Ids{"a", "b", "c"}), hits(query)) << query;
  }
  EXPECT_EQ((Ids{"b"}), hits(R"({"wildcard":{"field":"name_s","pattern":"fo?"}})"));
  EXPECT_EQ((Ids{"c"}), hits(R"({"match":{"name_s":"Foo"}})"));
  EXPECT_EQ((Ids{"a", "b", "c"}), expr("wildcard(f*, field=name_s, case_insensitive=true)"));
  EXPECT_EQ((Ids{"a", "b", "c"}), expr("match(foo, field=name_s, case_insensitive=true)"));
  // Escaped literal: '*' in a case-insensitive Match is not a wildcard.
  EXPECT_TRUE(hits(R"({"match":{"name_s":"f*","case_insensitive":true}})").empty());
  // Simple case folding classes span more than lower/upper.
  EXPECT_EQ((Ids{"e"}), hits(R"({"match":{"name_s":"k","case_insensitive":true}})"));
}

TEST_F(CaseInsensitiveQueryTest, regexClassesExpandCase) {
  EXPECT_EQ((Ids{"b"}), hits(R"({"regex":{"field":"name_s","pattern":"[a-f]oo"}})"));
  EXPECT_EQ((Ids{"a", "b", "c"}),
            hits(R"({"regex":{"field":"name_s","pattern":"[a-f]oo","case_insensitive":true}})"));
  EXPECT_EQ((Ids{"b"}), hits(R"({"regex":{"field":"name_s","pattern":"[^F]oo"}})"));
  EXPECT_TRUE(hits(R"({"regex":{"field":"name_s","pattern":"[^F][oO]+","case_insensitive":true}})").empty());
  EXPECT_EQ((Ids{"e"}), hits(R"({"regex":{"field":"name_s","pattern":"[j-l]","case_insensitive":true}})"));
}

TEST_F(CaseInsensitiveQueryTest, textMatchUsesItsAnalyzer) {
  EXPECT_EQ((Ids{"b"}), hits(R"({"match":{"body_w":"error"}})"));
  EXPECT_EQ((Ids{"a", "b"}), hits(R"({"match":{"title_t":"ERROR"}})"));
  EXPECT_EQ((Ids{"a", "b", "c"}), hits(R"({"match":{"body_w":"error","case_insensitive":true}})"));
  EXPECT_EQ((Ids{"a", "b"}), hits(R"({"match":{"title_t":"ERROR","case_insensitive":true}})"));
  EXPECT_EQ((Ids{"a", "b", "c"}), expr("match(error, field=body_w, case_insensitive=true)"));
}

TEST_F(CaseInsensitiveQueryTest, textOperatorAndMinMatchCountTokens) {
  EXPECT_EQ((Ids{"a", "b", "c"}), hits(R"({"match":{"body_w":"error happened","case_insensitive":true}})"));
  EXPECT_EQ((Ids{"a", "b"}), hits(R"({"match":{"body_w":"error happened","case_insensitive":true,"operator":"and"}})"));
  EXPECT_EQ((Ids{"a", "b"}), hits(R"({"match":{"body_w":"error happened other","case_insensitive":true,"min_match":2}})"));
  // Two alternate spellings in a document still count as just one query token.
  ASSERT_TRUE(helper.index(flatdoc("id", "mixed", "body_w", "Error ERROR"), UpdateMessage::COMMIT).success);
  EXPECT_EQ((Ids{"a", "b"}), hits(R"({"match":{"body_w":"error happened","case_insensitive":true,"min_match":2}})"));
}

TEST_F(CaseInsensitiveQueryTest, foldingKeepsTermQueriesAndScores) {
  MemPool pool;
  auto schema = helper.collection().getSchema();
  QueryBuilder builder(pool, *schema, CoerceContext{});
  for (auto field : {"title_t", "title_un", "title_wl"}) {
    auto* plain = builder.createMatchQuery(field, "ERROR");
    auto* ci = builder.createMatchQuery(field, "ERROR", QueryBuilder::Operator::OR, 0, 1.0f, true);
    EXPECT_EQ(QueryKind::TERM, ci->getKind());
    EXPECT_TRUE(plain->equals(*ci));
    EXPECT_EQ(plain->hash(), ci->hash());
    EXPECT_EQ(key(*plain), key(*ci));
    std::string longWord(300, 'A');
    plain = builder.createMatchQuery(field, longWord);
    ci = builder.createMatchQuery(field, longWord, QueryBuilder::Operator::OR, 0, 1.0f, true);
    EXPECT_EQ(QueryKind::TERM, ci->getKind());
    EXPECT_TRUE(plain->equals(*ci));
  }
  EXPECT_EQ(scores("match(ERROR, field=title_t)"),
            scores("match(ERROR, field=title_t, case_insensitive=true)"));
}

class CaseInsensitiveScoringTest : public CaseInsensitiveQueryTest {
protected:
  void SetUp() override {
    // Separate commits exercise the union and term statistics across segments.
    ASSERT_TRUE(helper.indexAll(std::array{
      flatdoc("id", "a", "word_w", "Foo pad"),
      flatdoc("id", "b", "word_w", "foo pad"),
      flatdoc("id", "c", "word_w", "foo pad"),
    }, UpdateMessage::COMMIT).success);
    ASSERT_TRUE(helper.indexAll(std::array{
      flatdoc("id", "d", "word_w", "Foo foo"),
      flatdoc("id", "e", "word_w", "FOO FOO"),
      flatdoc("id", "f", "word_w", "other other"),
    }, UpdateMessage::COMMIT).success);
  }
};

TEST_F(CaseInsensitiveScoringTest, synonymsSumTfBeforeSaturation) {
  auto actual = scores("match(Foo, field=word_w, case_insensitive=true)");
  ASSERT_EQ(5u, actual.size());
  Similarity sim;
  // Shared df = max(2, 3, 1), field length and avgdl are both 2.
  Similarity::BM25Scorer bm25(1.0f, sim.k1, sim.b, sim.idf(3, 6), 2.0f);
  auto expected = [&](float freq) { return bm25.score(freq, SmallFloat::intToByte4(2)); };
  float alternate = AutomatonQuery::CASE_ALTERNATE_WEIGHT;
  EXPECT_FLOAT_EQ(expected(1.0f), actual.at("a"));
  for (auto id : {"b", "c"}) EXPECT_FLOAT_EQ(expected(alternate), actual.at(id));
  EXPECT_FLOAT_EQ(expected(1.0f + alternate), actual.at("d"));
  EXPECT_FLOAT_EQ(expected(2.0f * alternate), actual.at("e"));
  EXPECT_GT(actual.at("d"), actual.at("a"));
  EXPECT_LT(actual.at("d"), actual.at("a") + actual.at("b"));
  // Preferring the rare FOO spelling still uses the common form's IDF.
  auto upper = scores("match(FOO, field=word_w, case_insensitive=true)");
  ASSERT_EQ(actual.size(), upper.size());
  EXPECT_FLOAT_EQ(expected(2.0f), upper.at("e"));
  EXPECT_FLOAT_EQ(expected(alternate), upper.at("a"));
  auto boosted = scores("match(Foo, field=word_w, case_insensitive=true)^3");
  ASSERT_EQ(actual.size(), boosted.size());
  for (const auto& [id, score] : actual) EXPECT_FLOAT_EQ(3.0f * score, boosted.at(id));
}

TEST_F(CaseInsensitiveScoringTest, sharedStatsAndIdentitySurviveWeightCreation) {
  auto reader = helper.getIndexWriter()->snapshots.readers.getReader();
  ASSERT_GE(reader->segments().size(), 2u);
  MemPool pool;
  QueryBuilder builder(pool, *reader->schema(), CoerceContext{});
  auto* ci = builder.createMatchQuery("word_w", "Foo", QueryBuilder::Operator::OR, 0, 1.0f, true);
  auto* plain = builder.createMatchQuery("word_w", "Foo");
  EXPECT_FALSE(ci->equals(*plain));
  EXPECT_NE(ci->hash(), plain->hash());
  EXPECT_NE(key(*ci), key(*plain));
  auto before = key(*ci);
  Query::Context context(pool, *reader);
  auto* weight = ci->createWeight(context, Query::NEED_SCORES);
  auto* combined = dynamic_cast<CombinedFieldQuery::Weight*>(weight);
  ASSERT_NE(nullptr, combined);
  EXPECT_EQ(3, combined->getTermStats().docFreq);
  EXPECT_EQ(7, combined->getTermStats().totalTermFreq);
  auto* filter = ci->createWeight(context, 0);
  EXPECT_TRUE(filter->isConstantScoring());
  EXPECT_EQ(before, key(*ci));
  // Scored and unscored weights see exactly the same segment-local documents.
  for (auto& segment : reader->segments()) {
    auto* scored = weight->createScorer(pool, segment);
    auto* unscored = filter->createScorer(pool, segment);
    ASSERT_NE(nullptr, scored);
    ASSERT_NE(nullptr, unscored);
    float bound = scored->getMaxScore(PostingsReader::END);
    for (int doc = scored->next(); doc != PostingsReader::END; doc = scored->next()) {
      EXPECT_EQ(doc, unscored->advance(doc));
      EXPECT_GE(bound, scored->score());
    }
    EXPECT_EQ(PostingsReader::END, unscored->next());
  }
}

TEST_F(CaseInsensitiveQueryTest, unicodeTextAndStringScores) {
  ASSERT_TRUE(helper.indexAll(std::array{
    flatdoc("id", "k1", "letter_u", "k", "letter_s", "k"),
    flatdoc("id", "k2", "letter_u", "K", "letter_s", "K"),
    flatdoc("id", "k3", "letter_u", "\u212a", "letter_s", "\u212a"),
  }, UpdateMessage::COMMIT).success);
  for (auto field : {"letter_u", "letter_s"}) {
    auto actual = scores(std::format("match(k, field={}, case_insensitive=true)", field));
    ASSERT_EQ(3u, actual.size());
    Similarity sim;
    Similarity::BM25Scorer bm25(1.0f, sim.k1, sim.b, sim.idf(1, 3), 1.0f);
    // STRING uses its column-backed norms, as in TermQuery: K=1, k=2,
    // Kelvin=3. TEXT has length 1 for every doc. Preference affects tf only.
    bool stringField = std::string_view(field) == "letter_s";
    EXPECT_FLOAT_EQ(bm25.score(1.0f, stringField ? 2 : 1), actual.at("k1"));
    EXPECT_FLOAT_EQ(bm25.score(AutomatonQuery::CASE_ALTERNATE_WEIGHT, 1), actual.at("k2"));
    EXPECT_FLOAT_EQ(bm25.score(AutomatonQuery::CASE_ALTERNATE_WEIGHT, stringField ? 3 : 1), actual.at("k3"));
  }
  EXPECT_EQ((Ids{"k1", "k2", "k3"}), hits(R"({"match":{"letter_u":"k","case_insensitive":true}})"));
}

TEST_F(CaseInsensitiveQueryTest, exactOnlyScoresMatchPlainTermQueries) {
  ASSERT_TRUE(helper.indexAll(std::array{
    flatdoc("id", "Aardvark", "only_w", "Solo pad", "only_s", "Solo",
            "only_ss", std::vector<std::string>{"Aardvark", "Solo"}),
    flatdoc("id", "Solo", "only_w", "Solo", "only_s", "Other",
            "only_ss", std::vector<std::string>{"Solo", "Zebra"}),
    flatdoc("id", "Zebra", "only_s", "Solo"),
  }, UpdateMessage::COMMIT).success);
  // Non-unit ordinals and different first ordinals exercise the non-text
  // norm source, rather than accidentally comparing two unit lengths.
  for (auto field : {"only_w", "only_s", "only_ss", "id"}) {
    auto plain = scores(std::format("match(Solo, field={})", field));
    auto ci = scores(std::format("match(Solo, field={}, case_insensitive=true)", field));
    ASSERT_FALSE(plain.empty()) << field;
    EXPECT_EQ(plain, ci) << field;
  }
}

TEST_F(CaseInsensitiveQueryTest, exactCaseRanksFirst) {
  ASSERT_TRUE(helper.indexAll(std::array{
    flatdoc("id", "http://x/Foo", "rank_w", "Foo"),
    flatdoc("id", "http://x/foo", "rank_w", "foo"),
  }, UpdateMessage::COMMIT).success);
  for (auto expression : {"match(\"http://x/Foo\", field=id, case_insensitive=true)",
                          "match(Foo, field=rank_w, case_insensitive=true)"}) {
    auto actual = scores(expression);
    ASSERT_EQ(2u, actual.size());
    EXPECT_GT(actual.at("http://x/Foo"), actual.at("http://x/foo"));
    auto req = localReq(helper.getSearchEngine());
    req->topDocs("q").exprQuery(expression).fields({"id"}).limit(1);
    req->execute(false);
    EXPECT_EQ((Ids{"http://x/Foo"}), ids(*req));
  }
}

TEST_F(CaseInsensitiveQueryTest, termWeightsSeparateScoringIdentityOnly) {
  std::string_view fields[] = {"body_w"};
  float weights[] = {1.0f};
  CombinedFieldQuery::Term plain[] = {{0, "Foo"}, {0, "foo"}};
  CombinedFieldQuery::Term preferred[] = {{0, "Foo"}, {0, "foo", AutomatonQuery::CASE_ALTERNATE_WEIGHT}};
  CombinedFieldQuery a(fields, weights, plain);
  CombinedFieldQuery b(fields, weights, preferred);
  EXPECT_FALSE(a.equals(b));
  EXPECT_NE(a.hash(), b.hash());
  EXPECT_EQ(key(a), key(b));
}

TEST_F(CaseInsensitiveQueryTest, escapedLiteralKeepsExactCasePreference) {
  ASSERT_TRUE(helper.indexAll(std::array{
    flatdoc("id", "exact", "escaped_w", R"(Foo*?\Tail)", "escaped_s", R"(Foo*?\Tail)"),
    flatdoc("id", "alternate", "escaped_w", R"(foo*?\tail)", "escaped_s", R"(foo*?\tail)"),
  }, UpdateMessage::COMMIT).success);
  for (auto field : {"escaped_w", "escaped_s"}) {
    auto actual = scores(std::format(R"(match("Foo*?\\Tail", field={}, case_insensitive=true))", field));
    ASSERT_EQ(2u, actual.size());
    EXPECT_GT(actual.at("exact"), actual.at("alternate"));
  }
}

TEST_F(CaseInsensitiveQueryTest, scoredExpansionBudgetRejectsWithoutTruncatingFilters) {
  for (int i = 0; i <= (int)AutomatonQuery::MAX_CASE_VARIANTS; i++) {
    std::string term(7, 'a');
    for (size_t bit = 0; bit < term.size(); bit++) {
      if ((i & (1 << bit)) != 0) term[bit] = 'A';
    }
    ASSERT_TRUE(helper.index(flatdoc("id", std::to_string(i), "variants_w", term),
        UpdateMessage::NO_COMMIT).success);
    if (i + 1 == (int)AutomatonQuery::MAX_CASE_VARIANTS) {
      helper.commit();
      EXPECT_EQ(AutomatonQuery::MAX_CASE_VARIANTS,
                scores("match(aaaaaaa, field=variants_w, case_insensitive=true)").size());
    }
  }
  helper.commit();
  auto req = run(R"({"query":{"match":{"variants_w":"aaaaaaa","case_insensitive":true}},"get_scores":true})");
  EXPECT_FALSE(req->ok());
  EXPECT_NE(std::string::npos, req->errorMsg().find("64 indexed case variants"));
  auto filtered = run(R"json({"query":{"all":true},"filter":["match(aaaaaaa, field=variants_w, case_insensitive=true)"],"fields":["id"],"limit":-1})json");
  EXPECT_EQ(AutomatonQuery::MAX_CASE_VARIANTS + 1, ids(*filtered).size());
}

TEST_F(CaseInsensitiveQueryTest, idAndNumericMatch) {
  EXPECT_EQ((Ids{"a"}), hits(R"({"match":{"id":"A","case_insensitive":true}})"));
  helper.index(flatdoc("id", "num", "count_i", 7, "when_dt", "2026-10-21"), UpdateMessage::COMMIT);
  for (auto query : {
      R"({"match":{"field":"count_i","val":7,"case_insensitive":true}})",
      R"({"match":{"field":"count_i","val":"7","case_insensitive":true}})",
      R"({"match":{"field":"when_dt","val":"2026-10-21","case_insensitive":true}})"}) {
    EXPECT_EQ((Ids{"num"}), hits(query));
  }
}

TEST_F(CaseInsensitiveQueryTest, prefixIgnoresCase) {
  EXPECT_EQ((Ids{"a", "b", "c"}),
      hits(R"({"prefix":{"field":"name_s","prefix":"fO","case_insensitive":true}})"));
  EXPECT_EQ((Ids{"b"}), hits(R"({"prefix":{"field":"name_s","prefix":"fo"}})"));
  EXPECT_EQ((Ids{"a", "b", "c"}), expr("prefix(fO, field=name_s, case_insensitive=true)"));
  EXPECT_EQ((Ids{"a", "b", "c"}),
      hits(R"({"prefix":{"field":"body_w","prefix":"err","case_insensitive":true}})"));
  EXPECT_EQ((Ids{"b"}), hits(R"({"prefix":{"field":"body_w","prefix":"err"}})"));
  EXPECT_EQ((Ids{"a"}), hits(R"({"prefix":{"field":"id","prefix":"A","case_insensitive":true}})"));
  EXPECT_EQ((Ids{"e"}), hits(R"({"prefix":{"field":"name_s","prefix":"k","case_insensitive":true}})"));
  EXPECT_EQ((Ids{"a", "b", "c", "d", "e"}),
      hits(R"({"prefix":{"field":"name_s","prefix":"","case_insensitive":true}})"));
}

TEST_F(CaseInsensitiveQueryTest, prefixEscapesLiteralSyntax) {
  ASSERT_TRUE(helper.index(flatdoc("id", "escaped", "name_s", "F*?\\Tail"), UpdateMessage::COMMIT).success);
  EXPECT_EQ((Ids{"escaped"}),
      hits(R"({"prefix":{"field":"name_s","prefix":"f*?\\","case_insensitive":true}})"));
  EXPECT_EQ((Ids{"escaped"}), expr(R"(prefix("f*?\\", field=name_s, case_insensitive=true))"));
}

TEST_F(CaseInsensitiveQueryTest, flagSeparatesQueryIdentity) {
  // Same pattern with and without the flag: neither boolean dedup nor the
  // filter cache may merge them.
  EXPECT_EQ((Ids{"a", "b", "c"}), hits(R"({"boolean":{"optional":[
      {"regex":{"field":"name_s","pattern":"Foo"}},
      {"regex":{"field":"name_s","pattern":"Foo","case_insensitive":true}}]}})"));
  auto req = run(R"json({"query":{"all":true},"limit":0,"ops":{"f":{"query_facet":{"buckets":{
      "exact":"wildcard(Fo*, field=name_s)",
      "ci":"wildcard(Fo*, field=name_s, case_insensitive=true)",
      "again":"wildcard(Fo*, field=name_s)"}}}}})json");
  ASSERT_OK(req);
  const auto& facet = *req->docList()->ops.at("f")->facetResult();
  EXPECT_EQ((std::vector<int64_t>{1, 3, 1}), std::vector<int64_t>(facet.counts.begin(), facet.counts.end()));
}

class CaseInsensitiveMappingTest : public CaseInsensitiveQueryTest {
protected:
  std::shared_ptr<FilterCache> cache;

  void SetUp() override {
    std::pmr::monotonic_buffer_resource arena;
    api::SchemaDef def;
    ASSERT_TRUE(api::read_json(def, R"({"fields":{"id":"id","name_s":"string","word_w":{"type":"text","analyzer":{"tokenizer":"whitespace"}},
      "stem":{"type":"text","analyzer":{"tokenizer":"whitespace","filters":["kstem"]}},"body":{"type":"text",
      "analyzer":{"tokenizer":"whitespace","char_filters":[{"name":"mapping",
        "params":{"mappings":["a => b","b => c","ph => f"]}}]}}}})", arena));
    helper.collection().setSchema(Schema::fromProto(def));
    cache = std::make_shared<FilterCache>(FilterCacheConfig{.minSegmentDocs = 0, .admissionThreshold = 1});
    helper.getIndexWriter()->snapshots.readers.filterCache = cache;
    ASSERT_TRUE(helper.indexAll(std::array{
      flatdoc("id", "a", "body", "apple"), flatdoc("id", "b", "body", "apricot"),
      flatdoc("id", "c", "body", "berry"), flatdoc("id", "d", "body", "BERRY"),
      flatdoc("id", "e", "body", "phone"),
    }, UpdateMessage::COMMIT).success);
  }
};

TEST_F(CaseInsensitiveMappingTest, prefixNormalizesOnce) {
  EXPECT_EQ((Ids{"a", "b", "d"}),
      hits(R"({"prefix":{"field":"body","prefix":"a","case_insensitive":true}})"));
  EXPECT_EQ((Ids{"a", "b", "d"}), expr("prefix(a, field=body, case_insensitive=true)"));
  EXPECT_EQ((Ids{"a", "b", "d"}),
      hits(R"({"wildcard":{"field":"body","pattern":"a*","case_insensitive":true}})"));
  EXPECT_EQ((Ids{"e"}),
      hits(R"({"prefix":{"field":"body","prefix":"ph","case_insensitive":true}})"));
}

TEST_F(CaseInsensitiveMappingTest, literalAndPatternIdentity) {
  MemPool pool;
  auto schema = helper.collection().getSchema();
  QueryBuilder builder(pool, *schema, CoerceContext{});
  auto key = [](const Query& query) {
    FilterKeyContext context;
    FilterKeyBuilder out;
    auto scope = query.appendFilterKey(out, context);
    return std::move(out).finishStructural(scope, context);
  };
  auto distinct = [&](Query* literal, Query* pattern) {
    EXPECT_FALSE(literal->equals(*pattern));
    EXPECT_NE(literal->hash(), pattern->hash());
    EXPECT_NE(key(*literal), key(*pattern));
  };
  distinct(builder.createPrefixQuery("body", "a", true), builder.createWildcardQuery("body", "b*", true));
  distinct(builder.createMatchQuery("name_s", "b", QueryBuilder::Operator::OR, 0, 1.0f, true),
           builder.createWildcardQuery("name_s", "b", true));
}

TEST_F(CaseInsensitiveMappingTest, literalAndPatternDedup) {
  EXPECT_EQ((Ids{"c"}), hits(R"({"wildcard":{"field":"body","pattern":"b*","case_insensitive":true}})"));
  EXPECT_EQ((Ids{"a", "b", "c", "d"}), hits(R"({"boolean":{"optional":[
      {"prefix":{"field":"body","prefix":"a","case_insensitive":true}},
      {"wildcard":{"field":"body","pattern":"b*","case_insensitive":true}}]}})"));
}

TEST_F(CaseInsensitiveMappingTest, literalAndPatternFilterCache) {
  for (int pass = 0; pass < 2; pass++) {
    auto prefix = run(R"json({"query":{"all":true},"filter":["prefix(a, field=body, case_insensitive=true)"],
        "fields":["id"],"limit":-1})json");
    EXPECT_EQ((Ids{"a", "b", "d"}), ids(*prefix));
    auto wildcard = run(R"json({"query":{"all":true},"filter":["wildcard(b*, field=body, case_insensitive=true)"],
        "fields":["id"],"limit":-1})json");
    EXPECT_EQ((Ids{"c"}), ids(*wildcard));
  }
  auto counters = cache->counters();
  EXPECT_GT(counters.hits + counters.readerStableHits, 0u);
}

TEST_F(CaseInsensitiveMappingTest, matchNormalizesOnlyBeforeExpansion) {
  // berry analyzes to cerry; expanding before analysis would also find BERRY.
  EXPECT_EQ((Ids{"c"}), hits(R"({"match":{"body":"berry","case_insensitive":true}})"));
  ASSERT_TRUE(helper.index(flatdoc("id", "upper", "body", "CERRY"), UpdateMessage::COMMIT).success);
  auto actual = scores("match(berry, field=body, case_insensitive=true)");
  ASSERT_EQ(2u, actual.size());
  EXPECT_GT(actual.at("c"), actual.at("upper"));
}

TEST_F(CaseInsensitiveMappingTest, matchFilterCacheSeparatesExactAndCaseInsensitive) {
  ASSERT_TRUE(helper.indexAll(std::array{
    flatdoc("id", "lower", "word_w", "foo"),
    flatdoc("id", "upper", "word_w", "FOO"),
  }, UpdateMessage::COMMIT).success);
  for (int pass = 0; pass < 2; pass++) {
    for (bool ci : {false, true, false}) {
      auto req = run(std::format(R"json({{"query":{{"all":true}},"filter":[
          {{"match":{{"word_w":"foo","case_insensitive":{}}}}}],"fields":["id"],"limit":-1}})json", ci));
      EXPECT_EQ(ci ? (Ids{"lower", "upper"}) : (Ids{"lower"}), ids(*req));
    }
  }
  auto actual = scores("match(foo, field=word_w, case_insensitive=true)");
  ASSERT_EQ(2u, actual.size());
  EXPECT_GT(actual.at("lower"), 0.0f);
  EXPECT_GT(actual.at("lower"), actual.at("upper"));
  auto counters = cache->counters();
  EXPECT_GT(counters.hits + counters.readerStableHits, 0u);
}

TEST_F(CaseInsensitiveMappingTest, expansionUsesAnalyzedStem) {
  ASSERT_TRUE(helper.indexAll(std::array{
    flatdoc("id", "lower", "stem", "ponies"),
    flatdoc("id", "upper", "stem", "PONIES"),
  }, UpdateMessage::COMMIT).success);
  EXPECT_EQ((Ids{"lower"}), hits(R"({"match":{"stem":"ponies","case_insensitive":true}})"));
  EXPECT_EQ((Ids{"upper"}), hits(R"({"match":{"stem":"PONIES","case_insensitive":true}})"));
}
