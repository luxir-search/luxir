#include <gtest/gtest.h>

#include <limits>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "solux/query/BooleanQuery.h"
#include "solux/reader/SkipStats.h"
#include "test/CollectionHelper.h"
#include "test/LocalReq.h"
#include "test/QueryBuild.h"
#include "test/SoluxTest.h"
#include "test/TestUtils.h"

using namespace solux;
using namespace solux::test;

namespace api = solux::api;

namespace {

struct DisjGroupBulkGuard {
  bool saved = BooleanQuery::ConjunctionBulkScorer::disableDisjGroupBulkForTests;

  explicit DisjGroupBulkGuard(bool disabled) {
    BooleanQuery::ConjunctionBulkScorer::disableDisjGroupBulkForTests = disabled;
  }

  ~DisjGroupBulkGuard() {
    BooleanQuery::ConjunctionBulkScorer::disableDisjGroupBulkForTests = saved;
  }
};

struct SkipStatsGuard {
  bool saved = SkipStats::enabled;

  SkipStatsGuard() {
    SkipStats::enabled = true;
    SkipStats::reset();
  }

  ~SkipStatsGuard() {
    SkipStats::enabled = saved;
    SkipStats::reset();
  }
};

} // namespace

class DisjGroupConjunctionTest : public SoluxTest {
public:
  enum class Shape {
    TWO_GROUPS,
    THREE_GROUPS,
    GROUP_AND_TERM,
    ROUNDING_ORDER,
    ABSENT_TERM,
    FILTER,
    MIN_MATCH_TWO,
    PHRASE_MEMBER
  };

  struct Run {
    int64_t count = 0;
    std::set<std::string> ids;
    std::map<std::string, float> scores;
    int64_t groupCountWindows = 0;
    int64_t groupScoreWindows = 0;
    int64_t bulkFillCalls = 0;
  };

  CollectionHelper helper;
  static constexpr int32_t numDocs = 2 * DocsEnumMeta::L1_DOCS + 257;

  static api::Query termGroup(std::pmr::memory_resource& mr,
                              std::initializer_list<std::string_view> terms,
                              int minMatch = 0) {
    std::vector<api::Query> optional;
    for (auto term : terms) {
      optional.push_back(qb::match(mr, "body_w", term));
    }
    return qb::boolean(mr, {}, optional, {}, {}, minMatch);
  }

  static api::Query queryFor(std::pmr::memory_resource& mr, Shape shape,
                             std::string_view filterTerm) {
    std::vector<api::Query> required;
    std::vector<api::Query> filter;
    switch (shape) {
      case Shape::TWO_GROUPS:
        required = {termGroup(mr, {"a", "b"}),
                    termGroup(mr, {"c", "d"})};
        break;
      case Shape::THREE_GROUPS:
        required = {termGroup(mr, {"a", "b", "t"}),
                    termGroup(mr, {"c", "d"}),
                    termGroup(mr, {"e", "f"})};
        break;
      case Shape::GROUP_AND_TERM:
        required = {termGroup(mr, {"a", "b"}),
                    qb::match(mr, "body_w", "t")};
        break;
      case Shape::ROUNDING_ORDER:
        required = {qb::boost(mr, qb::match(mr, "body_w", "t"), 0x1p24f),
                    termGroup(mr, {"a", "b"})};
        break;
      case Shape::ABSENT_TERM:
        required = {termGroup(mr, {"a", "missing"}),
                    termGroup(mr, {"c", "d"})};
        break;
      case Shape::FILTER:
        required = {termGroup(mr, {"a", "b"}),
                    termGroup(mr, {"c", "d"})};
        filter = {qb::match(mr, "body_w", filterTerm)};
        break;
      case Shape::MIN_MATCH_TWO:
        required = {termGroup(mr, {"a", "b", "c"}, 2),
                    termGroup(mr, {"d", "e"})};
        break;
      case Shape::PHRASE_MEMBER: {
        std::vector<api::Query> optional = {
          qb::match(mr, "body_w", "a"),
          qb::phraseWords(mr, "body_w", {"p", "q"})};
        required = {qb::boolean(mr, {}, optional),
                    termGroup(mr, {"c", "d"})};
        break;
      }
    }
    return qb::boolean(mr, required, {}, {}, filter);
  }

  Run run(Shape shape, bool disabled, bool scored, bool pruning = false) {
    DisjGroupBulkGuard bulkGuard(disabled);
    SkipStatsGuard statsGuard;

    auto req = localReq(helper.getSearchEngine());
    req->collection("main");
    auto& cur = req->topDocs("q");
    if (scored) {
      cur.getScores().limit(pruning ? 10 : numDocs)
          .batchSize(pruning ? 11 : numDocs + 1).fields({"id"});
      if (!pruning) cur.getNumber();
    } else {
      cur.getNumber().limit(0);
    }
    std::string filterTerm = std::string("keep_")
        + (disabled ? "opaque" : "bulk")
        + (scored ? "_score" : "_count")
        + (pruning ? "_topk" : "_all");
    cur.rawQuery() = queryFor(cur.mr(), shape, filterTerm);
    req->execute(false);
    EXPECT_TRUE(req->ok()) << req->errorMsg();

    Run result;
    result.count = req->getMatchCount();
    if (scored && result.count != 0) {
      const api::DocList* docs = req->docList();
      if (docs == nullptr) {
        ADD_FAILURE() << "missing doc list";
      } else {
        const api::Column* ids = docs->columns.find("id");
        const api::Column* scores = docs->columns.find("_score_");
        if (ids == nullptr || scores == nullptr) {
          ADD_FAILURE() << "missing id or score column";
        } else {
          const auto& idValues = std::get<api::ColStr>(ids->kind).v;
          const auto& scoreValues = std::get<api::ColFloat>(scores->kind).v;
          EXPECT_EQ(idValues.size(), scoreValues.size());
          for (size_t i = 0; i < std::min(idValues.size(), scoreValues.size()); i++) {
            std::string id(idValues[i]);
            result.ids.insert(id);
            result.scores.emplace(std::move(id), scoreValues[i]);
          }
        }
      }
    }
    result.groupCountWindows = SkipStats::conjDisjGroupCountWindows;
    result.groupScoreWindows = SkipStats::conjDisjGroupScoreWindows;
    result.bulkFillCalls = SkipStats::countBulkFillCalls;
    return result;
  }

  void SetUp() override {
    SoluxTest::SetUp();
    std::vector<Doc> docs;
    docs.reserve((size_t) numDocs);
    for (int32_t doc = 0; doc < numDocs; doc++) {
      std::string body = "filler";
      if ((doc % 2) == 0) {
        body += " a keep_opaque_count_all keep_bulk_count_all"
                " keep_opaque_score_all keep_bulk_score_all"
                " keep_opaque_score_topk keep_bulk_score_topk";
      }
      if ((doc % 3) == 0) body += " b";
      if ((doc % 4) == 0) body += " c";
      if ((doc % 5) == 0) body += " d";
      if ((doc % 7) == 0) body += " e";
      if ((doc % 11) == 0) body += " f";
      if ((doc % 13) == 0) body += " t";
      if ((doc % 17) == 0) body += " p q";
      if ((doc % 19) == 0) body += " p x q";
      docs.push_back(flatdoc("id", "dg" + std::to_string(doc), "body_w", body));
    }
    helper.indexAll(docs, UpdateMessage::COMMIT);
  }

  void assertRouting() {
    auto reader = helper.getIndexWriter()->getIndexReader();
    MemPool pool;
    Query::Context context(pool, *reader);
    TermQuery a("body_w", "a");
    TermQuery b("body_w", "b");
    TermQuery c("body_w", "c");
    TermQuery d("body_w", "d");
    Query* abTerms[] = {&a, &b};
    Query* cdTerms[] = {&c, &d};
    BooleanQuery ab({}, abTerms, {}, {});
    BooleanQuery cd({}, cdTerms, {}, {});
    auto& segment = context.topReader.segments()[0];

    auto* groupWeight = ab.createWeight(context, Query::NEED_SCORES);
    auto* standaloneSupplier = groupWeight->scorerSupplier(pool, segment);
    ASSERT_NE(standaloneSupplier, nullptr);
    EXPECT_NE(dynamic_cast<BooleanQuery::MaxScoreDisjunctionScorer*>(
                  buildScorerForTests(
                      pool, *standaloneSupplier,
                      std::numeric_limits<int64_t>::max())),
              nullptr);
    auto* drivenSupplier = groupWeight->scorerSupplier(pool, segment);
    ASSERT_NE(drivenSupplier, nullptr);
    auto* driven = buildScorerForTests(pool, *drivenSupplier, 1);
    auto* plain = dynamic_cast<BooleanQuery::DisjunctionScorer*>(driven);
    ASSERT_NE(plain, nullptr);
    EXPECT_EQ(2, plain->flatDisjunctionScorers().size());

    Query* required[] = {&ab, &cd};
    BooleanQuery outer(required, {}, {}, {});
    auto* outerWeight = outer.createWeight(context, Query::NEED_SCORES);
    auto* outerSupplier = outerWeight->scorerSupplier(pool, segment);
    ASSERT_NE(outerSupplier, nullptr);
    EXPECT_NE(dynamic_cast<BooleanQuery::ConjunctionBulkScorer*>(
                  outerSupplier->bulkScorer(pool)),
              nullptr);
  }
};

TEST_F(DisjGroupConjunctionTest, bulkAndOpaquePathsMatch) {
  assertRouting();
  for (Shape shape : {Shape::TWO_GROUPS, Shape::THREE_GROUPS,
                      Shape::GROUP_AND_TERM, Shape::ROUNDING_ORDER,
                      Shape::ABSENT_TERM,
                      Shape::FILTER, Shape::MIN_MATCH_TWO,
                      Shape::PHRASE_MEMBER}) {
    Run opaqueCount = run(shape, true, false);
    Run bulkCount = run(shape, false, false);
    EXPECT_EQ(opaqueCount.count, bulkCount.count) << (int) shape;

    Run opaqueScored = run(shape, true, true);
    Run bulkScored = run(shape, false, true);
    EXPECT_EQ(opaqueScored.count, bulkScored.count) << (int) shape;
    EXPECT_EQ(opaqueScored.ids, bulkScored.ids) << (int) shape;
    EXPECT_EQ(opaqueScored.scores, bulkScored.scores) << (int) shape;

    // Filtered COUNT and scored filter-mask execution both support decomposed
    // direct-term groups. MIN_MATCH_TWO and PHRASE_MEMBER are ineligible for
    // both (non-decomposable disjunction / phrase member -> opaque sparse path).
    bool countEligible = shape != Shape::MIN_MATCH_TWO
        && shape != Shape::PHRASE_MEMBER;
    bool scoreEligible = shape != Shape::MIN_MATCH_TWO
        && shape != Shape::PHRASE_MEMBER;
    EXPECT_EQ(0, opaqueCount.groupCountWindows) << (int) shape;
    EXPECT_EQ(0, opaqueScored.groupScoreWindows) << (int) shape;
    if (countEligible) {
      EXPECT_GT(bulkCount.groupCountWindows, 0) << (int) shape;
      EXPECT_GT(bulkCount.bulkFillCalls, 0) << (int) shape;
    } else {
      EXPECT_EQ(0, bulkCount.groupCountWindows) << (int) shape;
    }
    if (scoreEligible) {
      EXPECT_GT(bulkScored.groupScoreWindows, 0) << (int) shape;
    } else {
      EXPECT_EQ(0, bulkScored.groupScoreWindows) << (int) shape;
    }
  }

  for (Shape shape : {Shape::TWO_GROUPS, Shape::THREE_GROUPS,
                      Shape::GROUP_AND_TERM, Shape::ROUNDING_ORDER,
                      Shape::ABSENT_TERM, Shape::FILTER}) {
    Run opaqueTopK = run(shape, true, true, true);
    Run bulkTopK = run(shape, false, true, true);
    EXPECT_EQ(opaqueTopK.ids, bulkTopK.ids) << (int) shape;
    EXPECT_EQ(opaqueTopK.scores, bulkTopK.scores) << (int) shape;
    EXPECT_GT(bulkTopK.groupScoreWindows, 0) << (int) shape;
  }
}
