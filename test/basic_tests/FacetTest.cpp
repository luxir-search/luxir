#include <gtest/gtest.h>
#include <mutex>
#include <thread>
#include <atomic>
#include <cmath>
#include <functional>
#include <format>
#include <map>
#include <numeric>
#include <optional>
#include <variant>
#include <unordered_map>
#include <tbb/task_group.h>
#include <boost/unordered/unordered_flat_map.hpp>
#include "test/SchemaBuilder.h"
#include "test/SoluxTest.h"
#include "test/CollectionHelper.h"
#include "test/LocalReq.h"
#include "test/QueryBuild.h"
#include "test/TestUtils.h"
#include "solux/util/random.h"
#include "solux/util/proto.h"
#include "solux/index/Inverter.h"
#include "solux/index/IndexWriter.h"
#include "solux/reader/SkipStats.h"
#include "solux/search/ops/FacetOp.h"
#include "solux/search/ops/StrFacetOp.h"
#include "solux/util/NumericUtils.h"

using namespace solux;
using namespace solux::test;

class FacetTest : public SoluxTest {
protected:
};

namespace {

struct RangeSchemaField {
  std::string_view name;
  bool points;
  bool multi;
  api::FieldDef::FieldClass fieldClass = api::FieldDef::FieldClass::INT;
};

void setRangeFacetSchema(CollectionHelper& helper,
                         std::span<const RangeSchemaField> fields) {
  SchemaBuilder b;
  for (const auto& fs : fields) {
    auto& f = b.field(fs.name);
    f.type = fs.fieldClass;
    f.index = fs.points ? api::FieldDef::IndexMode::RANGE
                        : api::FieldDef::IndexMode::NONE;
    f.multi = fs.multi;
  }
  b.set(helper.collection());
}

const api::FacetResult& rootFacetResult(const LocalReq& req,
                                        std::string_view name) {
  return *req.responses[0]->proto.ops.at(name)->facetResult();
}

std::vector<std::byte> encodeFacetResult(const LocalReq& req,
                                         std::string_view name) {
  std::vector<std::byte> encoded;
  EXPECT_TRUE(api::encode(rootFacetResult(req, name), encoded));
  return encoded;
}

void expectRangeResult(const api::FacetResult& result,
                       std::span<const std::pair<int64_t, int64_t>> bounds,
                       std::span<const int64_t> counts, int64_t missing) {
  ASSERT_TRUE(result.bucket_ids.has_value());
  const auto& actualBounds = std::get<api::ArrArrInt>(result.bucket_ids->kind).v;
  ASSERT_EQ(bounds.size(), actualBounds.size());
  ASSERT_EQ(counts.size(), result.counts.size());
  for (size_t i = 0; i < bounds.size(); i++) {
    ASSERT_EQ(2u, actualBounds[i].v.size());
    EXPECT_EQ(bounds[i].first, actualBounds[i].v[0]);
    EXPECT_EQ(bounds[i].second, actualBounds[i].v[1]);
    EXPECT_EQ(counts[i], result.counts[i]);
  }
  EXPECT_EQ(missing, result.missing.value_or(-1));
}

int64_t fpEncoded(float value) {
  return (int64_t)floatToSortableInt32(value);
}

int64_t fpEncoded(double value) {
  return doubleToSortableInt64(value);
}

template<typename Outer, typename T>
void expectFloatingRangeResult(const api::FacetResult& result,
                               std::span<const T> fences,
                               std::span<const T> values) {
  ASSERT_TRUE(result.bucket_ids.has_value());
  const auto& bounds = std::get<Outer>(result.bucket_ids->kind).v;
  ASSERT_EQ(fences.size() - 1, bounds.size());
  ASSERT_EQ(bounds.size(), result.counts.size());

  std::vector<int64_t> expected(bounds.size());
  for (T value : values) {
    int64_t encoded = fpEncoded(value);
    for (size_t bucket = 0; bucket < bounds.size(); bucket++) {
      if (encoded >= fpEncoded(fences[bucket])
          && encoded < fpEncoded(fences[bucket + 1])) {
        expected[bucket]++;
        break;
      }
    }
  }
  for (size_t bucket = 0; bucket < bounds.size(); bucket++) {
    ASSERT_EQ(2u, bounds[bucket].v.size());
    EXPECT_EQ(fences[bucket], bounds[bucket].v[0]);
    EXPECT_EQ(fences[bucket + 1], bounds[bucket].v[1]);
    EXPECT_EQ(expected[bucket], result.counts[bucket]);
  }
}

int64_t epoch(std::string_view text) {
  auto value = parseDateToEpochMillis(text);
  EXPECT_TRUE(value.has_value()) << text;
  return value.value_or(0);
}

TimeZone zone(std::string_view name) {
  auto value = resolveTimeZone(name);
  EXPECT_TRUE(value.has_value()) << name;
  return value.value_or(TimeZone::utc());
}

int64_t localLo(std::string_view text, const TimeZone& timeZone,
                int64_t now = 0) {
  auto value = parseDateRange(text, now, timeZone);
  EXPECT_TRUE(value.has_value()) << text << " in " << timeZone.name();
  return value ? value->lo : 0;
}

class PointsRangeFacetTestGuard {
  bool savedDisabled = IntFacetRangeReq::disablePointsRangeFacetForTests;
  bool savedStatsEnabled = SkipStats::enabled;
public:
  PointsRangeFacetTestGuard() { SkipStats::enabled = true; }
  ~PointsRangeFacetTestGuard() {
    IntFacetRangeReq::disablePointsRangeFacetForTests = savedDisabled;
    SkipStats::enabled = savedStatsEnabled;
  }
};

class FacetCounterModeGuard {
  FacetCounterMode saved = forcedFacetCounterMode;
public:
  ~FacetCounterModeGuard() {
    forcedFacetCounterMode = saved;
  }
};

class StrFacetStrategyGuard {
  StrFacetStrategy saved = forcedStrFacetStrategy;
public:
  ~StrFacetStrategyGuard() {
    forcedStrFacetStrategy = saved;
  }
};

} // namespace

TEST_F(FacetTest, spanCounter) {
  constexpr size_t maxOrd = 200000;
  SpanCounter counter(maxOrd);

  counter.increment(3);
  counter.increment(3, 4);
  counter.increment(65536);
  counter.increment(65536, 2);
  counter.increment(196700, 9);
  EXPECT_EQ(5, counter.total(3));
  EXPECT_EQ(3, counter.total(65536));
  EXPECT_EQ(9, counter.total(196700));

  for (int i = 0; i < 70000; i++) {
    counter.increment(70000);
  }
  counter.increment(131100, 70000);
  counter.increment(196699, 65536);
  EXPECT_EQ(70000, counter.total(70000));
  EXPECT_EQ(70000, counter.total(131100));
  EXPECT_EQ(65536, counter.total(196699));
  EXPECT_TRUE(counter.overflow.contains(70000));
  EXPECT_TRUE(counter.overflow.contains(131100));
  EXPECT_TRUE(counter.overflow.contains(196699));
  auto saturated = counter.spans[(size_t)(196699 >> SpanCounter::SPAN_BITS)]
                              .find((uint16_t)(196699 & SpanCounter::SPAN_MASK));
  ASSERT_NE(saturated,
            counter.spans[(size_t)(196699 >> SpanCounter::SPAN_BITS)].end());
  EXPECT_EQ(0, saturated->second);

  SpanCounter a(maxOrd);
  a.increment(1, 10);
  a.increment(70000, 70000);
  a.increment(190000, 3);
  SpanCounter b(maxOrd);
  b.increment(1, 7);
  b.increment(65540, 5);
  b.increment(70000, 66000);
  b.increment(196000, 9);

  a.merge(b);
  EXPECT_EQ(17, a.total(1));
  EXPECT_EQ(5, a.total(65540));
  EXPECT_EQ(136000, a.total(70000));
  EXPECT_EQ(3, a.total(190000));
  EXPECT_EQ(9, a.total(196000));
}

TEST_F(FacetTest, spanCounterModesMatchAuto) {
  CollectionHelper helper;
  helper.clear();
  constexpr int segments = 4;
  constexpr int docsPerSegment = 900;
  constexpr int categoryCardinality = 2600;

  for (int seg = 0; seg < segments; seg++) {
    std::vector<Doc> docs;
    docs.reserve(docsPerSegment);
    for (int i = 0; i < docsPerSegment; i++) {
      std::string id = std::format("span-{}-{}", seg, i);
      std::string category =
          std::format("cat{:04}", (i * 37 + seg * 613) % categoryCardinality);
      if (i == 2) {
        docs.push_back(flatdoc("id", id, "selected_s", "yes"));
      } else {
        docs.push_back(flatdoc("id", id, "category_s", category,
                               "selected_s", i < 3 ? "yes" : "no"));
      }
    }
    ASSERT_TRUE(helper.indexAll(docs, UpdateMessage::COMMIT).success);
  }
  ASSERT_EQ((size_t)segments,
            helper.getIndexWriter()->getIndexReader()->segments().size());

  FacetCounterModeGuard guard;
  auto run = [&](FacetCounterMode mode, int64_t mincount) {
    forcedFacetCounterMode = mode;
    auto req = localReq(soluxNode->getSearchEngine());
    req->collection("main");
    auto& topDocs = req->topDocs();
    topDocs.getNumber(true).matchQuery("selected_s", "yes");
    auto& facet = topDocs.facet("f", "category_s").limit(10).mincount(mincount);
    std::get<api::FieldFacet>(facet.rawOp().kind).missing = true;
    req->execute(true);
    EXPECT_TRUE(req->ok()) << req->errorMsg();
    const auto* docs = req->docList("q");
    EXPECT_NE(nullptr, docs);
    if (docs == nullptr) {
      return std::vector<std::byte>{};
    }
    const auto* result = docs->ops.at("f")->facetResult();
    EXPECT_NE(nullptr, result);
    if (result == nullptr) {
      return std::vector<std::byte>{};
    }
    std::vector<std::byte> encoded;
    EXPECT_TRUE(api::encode(*result, encoded));
    return encoded;
  };

  for (int64_t mincount : {1, 0}) {
    auto expected = run(FacetCounterMode::AUTO, mincount);
    EXPECT_EQ(expected, run(FacetCounterMode::SPAN_GLOBAL, mincount));
    EXPECT_EQ(expected, run(FacetCounterMode::SPAN_LOCAL, mincount));
  }
}

TEST_F(FacetTest, stringFacetStrategiesMatchAcrossDomainSeams) {
  CollectionHelper helper;
  helper.clear();

  // Segment 0 has a deleted value and a single-term field. Segment 1 shifts
  // local string ords in the global dictionary and has distinct multi-values.
  // Segment 2 omits every faceted field.
  ASSERT_TRUE(helper.indexAll(std::array{
      flatdoc("id", "a", "cat_s", "middle", "tags_ss", vecs("x", "y"),
              "single_s", "only", "sel_s", "yes"),
      flatdoc("id", "deleted", "cat_s", "deleted-value",
              "tags_ss", vecs("x", "z"), "single_s", "only",
              "sel_s", "yes"),
  }, UpdateMessage::COMMIT).success);
  ASSERT_TRUE(helper.indexAll(std::array{
      flatdoc("id", "b", "cat_s", "alpha", "tags_ss", vecs("y", "z"),
              "sel_s", "yes"),
      flatdoc("id", "c", "cat_s", "zulu", "sel_s", "no"),
  }, UpdateMessage::COMMIT).success);
  ASSERT_TRUE(helper.index(
      flatdoc("id", "absent", "sel_s", "yes"),
      UpdateMessage::COMMIT).success);
  ASSERT_TRUE(helper.deleteById("deleted", UpdateMessage::COMMIT).success);
  ASSERT_EQ(3u, helper.durableSegmentCount());

  StrFacetStrategyGuard guard;
  auto run = [&](StrFacetStrategy strategy, int domainKind) {
    forcedStrFacetStrategy = strategy;
    auto req = localReq(helper.getSearchEngine());
    req->collection("main");

    auto addFacets = [](auto& cursor) {
      for (std::string_view field : {"cat_s", "tags_ss", "single_s"}) {
        auto& facet = cursor.facet(field, field).limit(-1).mincount(0);
        std::get<api::FieldFacet>(facet.rawOp().kind).missing = true;
      }
    };
    if (domainKind == 0) {
      addFacets(*req);
    } else {
      auto& top = req->topDocs("q");
      top.getNumber(true).matchQuery(
          "sel_s", domainKind == 1 ? "yes" : "does-not-exist");
      addFacets(top);
    }

    req->execute(false);
    EXPECT_TRUE(req->ok()) << req->errorMsg();
    std::vector<std::vector<std::byte>> encoded;
    for (std::string_view field : {"cat_s", "tags_ss", "single_s"}) {
      const api::FacetResult* result;
      if (domainKind == 0) {
        result = req->responses[0]->proto.ops.at(field)->facetResult();
      } else {
        const auto* docs = req->docList("q");
        EXPECT_NE(nullptr, docs);
        result = docs == nullptr ? nullptr : docs->ops.at(field)->facetResult();
      }
      EXPECT_NE(nullptr, result);
      encoded.emplace_back();
      if (result != nullptr) {
        EXPECT_TRUE(api::encode(*result, encoded.back()));
      }
    }
    return encoded;
  };

  for (int domainKind : {0, 1, 2}) {
    auto expected = run(StrFacetStrategy::COLUMN_DOMAIN, domainKind);
    EXPECT_EQ(expected, run(StrFacetStrategy::COLUMN_COMPLEMENT, domainKind));
    EXPECT_EQ(expected, run(StrFacetStrategy::TERM_DRIVEN, domainKind));
    EXPECT_EQ(expected, run(StrFacetStrategy::AUTO, domainKind));
  }
}

TEST_F(FacetTest, stringFacetTopTermsMatchesForcedStrategies) {
  CollectionHelper helper;
  helper.clear();
  ASSERT_TRUE(helper.indexAll(std::array{
      flatdoc("id", "a1", "cat_s", "a", "few_s", "x", "sel_s", "yes"),
      flatdoc("id", "missing", "few_s", "x", "sel_s", "yes"),
  }, UpdateMessage::COMMIT).success);
  ASSERT_TRUE(helper.indexAll(std::array{
      flatdoc("id", "b1", "cat_s", "b", "few_s", "x", "sel_s", "yes"),
      flatdoc("id", "c1", "cat_s", "c", "sel_s", "no"),
  }, UpdateMessage::COMMIT).success);
  ASSERT_TRUE(helper.indexAll(std::array{
      flatdoc("id", "a2", "cat_s", "a", "few_s", "y", "sel_s", "yes"),
      flatdoc("id", "d1", "cat_s", "d", "sel_s", "no"),
      flatdoc("id", "a3", "cat_s", "a", "sel_s", "yes"),
  }, UpdateMessage::COMMIT).success);
  ASSERT_EQ(3u, helper.durableSegmentCount());

  StrFacetStrategyGuard guard;
  auto run = [&](StrFacetStrategy strategy, std::string_view field,
                 int64_t limit, int64_t mincount, bool missing) {
    forcedStrFacetStrategy = strategy;
    auto req = localReq(helper.getSearchEngine());
    req->collection("main");
    auto& facet = req->facet("f", field).limit(limit).mincount(mincount);
    std::get<api::FieldFacet>(facet.rawOp().kind).missing = missing;
    req->execute(false);
    EXPECT_TRUE(req->ok()) << req->errorMsg();
    return encodeFacetResult(*req, "f");
  };
  auto expectMatches = [&](std::string_view field, int64_t limit,
                           int64_t mincount, bool missing) {
    auto expected = run(
        StrFacetStrategy::COLUMN_DOMAIN, field, limit, mincount, missing);
    EXPECT_EQ(expected, run(
        StrFacetStrategy::COLUMN_COMPLEMENT, field, limit, mincount, missing));
    EXPECT_EQ(expected, run(
        StrFacetStrategy::AUTO, field, limit, mincount, missing));
  };

  expectMatches("cat_s", 3, 1, false);
  expectMatches("cat_s", 3, 0, true);
  expectMatches("cat_s", 3, 2, true);
  expectMatches("few_s", 10, 0, true);
  expectMatches("cat_s", -1, 0, true);

  auto runMixedDomain = [&](StrFacetStrategy strategy) {
    forcedStrFacetStrategy = strategy;
    auto req = localReq(helper.getSearchEngine());
    req->collection("main");
    auto& docs = req->topDocs("q");
    docs.getNumber(true).matchQuery("sel_s", "yes");
    auto& facet = docs.facet("f", "cat_s").limit(3).mincount(0);
    std::get<api::FieldFacet>(facet.rawOp().kind).missing = true;
    req->execute(false);
    EXPECT_TRUE(req->ok()) << req->errorMsg();
    const auto* result = req->docList("q")->ops.at("f")->facetResult();
    std::vector<std::byte> encoded;
    EXPECT_TRUE(api::encode(*result, encoded));
    return encoded;
  };
  EXPECT_EQ(runMixedDomain(StrFacetStrategy::COLUMN_DOMAIN),
            runMixedDomain(StrFacetStrategy::AUTO));

  ASSERT_TRUE(helper.deleteById("a1", UpdateMessage::COMMIT).success);
  expectMatches("cat_s", 3, 0, true);
}

TEST_F(FacetTest, spanCounterTopKOverflowShortCircuit) {
  // A value appearing > 65536 times overflows the u16 span slot. With a facet
  // limit <= the number of overflowed values, span emit takes the overflow-first
  // top-K path (skips the span scan). It must pick the right bucket and must NOT
  // double-count (the slot low bits + the overflow multiple, counted once).
  CollectionHelper helper;
  helper.clear();
  constexpr int64_t hot = 66000;  // > 65536 -> exactly one overflowed ord
  std::vector<Doc> docs;
  docs.reserve(hot + 30);
  for (int64_t i = 0; i < hot; i++) {
    docs.push_back(flatdoc("id", std::format("h{}", i), "category_s", "hot"));
  }
  for (int i = 0; i < 30; i++) {
    docs.push_back(flatdoc("id", std::format("c{}", i), "category_s",
                           std::format("cold{:02}", i % 5)));
  }
  ASSERT_TRUE(helper.indexAll(docs, UpdateMessage::COMMIT).success);

  FacetCounterModeGuard guard;
  auto topBucket = [&](FacetCounterMode mode, int64_t limit) {
    forcedFacetCounterMode = mode;
    auto req = localReq(soluxNode->getSearchEngine());
    req->collection("main");
    req->facet("f", "category_s").limit(limit);
    req->execute(true);
    EXPECT_TRUE(req->ok()) << req->errorMsg();
    const auto* result = req->responses[0]->proto.ops.at("f")->facetResult();
    EXPECT_NE(result, nullptr);
    const auto& ids = std::get<solux::api::ColStr>(result->bucket_ids->kind);
    return std::pair<std::string, int64_t>(std::string(ids.v[0]), result->counts[0]);
  };

  // limit 1 <= 1 overflowed ord -> short-circuit fires.
  for (auto mode : {FacetCounterMode::AUTO, FacetCounterMode::SPAN_GLOBAL}) {
    auto [id, cnt] = topBucket(mode, 1);
    EXPECT_EQ("hot", id);
    EXPECT_EQ(hot, cnt);  // a double-count bug would report 2*hot
  }
  // limit 2 > 1 overflowed ord -> full span scan path; top bucket unchanged.
  auto [id, cnt] = topBucket(FacetCounterMode::SPAN_GLOBAL, 2);
  EXPECT_EQ("hot", id);
  EXPECT_EQ(hot, cnt);
}

TEST_F(FacetTest, mergeableStrDataMergeVariants) {
  using Data = StrFacetOp::MergeableStrData;
  using CountVector = Data::CountVector;
  using OrdHash = Data::OrdHash;

  auto makeOrd = [](std::initializer_list<std::pair<const int64_t, int64_t>> vals, int64_t missing) {
    Data data;
    data.counts = OrdHash(vals);
    data.missing_num = missing;
    return data;
  };
  auto makeVec = [](std::initializer_list<int64_t> vals, int64_t missing) {
    Data data;
    data.counts = CountVector(vals);
    data.missing_num = missing;
    return data;
  };
  auto makeSkinny = [](std::initializer_list<std::pair<int64_t, int64_t>> vals, int64_t missing) {
    Data data;
    data.counts.emplace<SkinnyCounter8>(8);
    auto& skinny = std::get<SkinnyCounter8>(data.counts);
    for (auto [ord, count] : vals) {
      skinny.increment(ord, count);
    }
    data.missing_num = missing;
    return data;
  };
  auto asVec = [](const Data& data) {
    std::vector<int64_t> out(8);
    if (auto* ords = std::get_if<OrdHash>(&data.counts)) {
      for (auto [ord, count] : *ords) out[ord] = count;
    } else if (auto* vec = std::get_if<CountVector>(&data.counts)) {
      for (size_t i = 0; i < vec->size(); i++) out[i] = (*vec)[i];
    } else if (auto* skinny = std::get_if<SkinnyCounter8>(&data.counts)) {
      for (size_t i = 0; i < skinny->counts.size(); i++) out[i] = skinny->counts[i];
      for (auto [ord, count] : skinny->overflow) out[ord] += count;
    }
    return out;
  };
  auto expectMerge = [&](Data a, Data b, std::vector<int64_t> expected) {
    auto missing = a.missing_num + b.missing_num;
    auto* result = Data::merge(&a, &b);
    EXPECT_EQ(expected, asVec(*result));
    EXPECT_EQ(missing, result->missing_num);
  };

  {
    Data empty;
    empty.missing_num = 1;
    expectMerge(empty, makeOrd({{2, 3}}, 2), {0, 0, 3, 0, 0, 0, 0, 0});
  }
  {
    Data empty;
    empty.missing_num = 4;
    expectMerge(makeOrd({{1, 5}}, 3), empty, {0, 5, 0, 0, 0, 0, 0, 0});
  }
  expectMerge(makeOrd({{2, 1}}, 5), makeOrd({{2, 4}, {3, 6}}, 6), {0, 0, 5, 6, 0, 0, 0, 0});
  expectMerge(makeOrd({{1, 7}, {2, 1}}, 7), makeOrd({{1, 3}}, 8), {0, 10, 1, 0, 0, 0, 0, 0});
  expectMerge(makeSkinny({{2, 3}}, 9), makeOrd({{2, 4}, {4, 5}}, 10), {0, 0, 7, 0, 5, 0, 0, 0});
  expectMerge(makeVec({1, 0, 2, 0, 0, 0, 0, 0}, 11), makeOrd({{2, 5}, {5, 6}}, 12), {1, 0, 7, 0, 0, 6, 0, 0});
  expectMerge(makeVec({0, 2, 0, 0, 0, 0, 0, 0}, 13), makeSkinny({{1, 5}, {6, 300}}, 14), {0, 7, 0, 0, 0, 0, 300, 0});
  expectMerge(makeVec({1, 2, 0, 0, 0, 0, 0, 0}, 15), makeVec({3, 0, 4, 0, 0, 0, 0, 0}, 16), {4, 2, 4, 0, 0, 0, 0, 0});
}

// ensureRep is the storage-upgrade step calcOrdMap runs when a later segment
// wants a larger counter than the merged-so-far it obtained (move the old
// counts out, rebuild as the larger rep, fold the old back in).  It is the
// trickiest part of the SegmentMergeDriver conversion, and the engine only
// exercises the upgrade under a specific multi-segment completion order (so it
// is covered by RandomFacetTest but not deterministically there).  Test it
// directly.
TEST_F(FacetTest, ensureRepUpgradesAndPreservesCounts) {
  using Data = StrFacetOp::MergeableStrData;
  using Rep = Data::Rep;
  using CountVector = Data::CountVector;
  using OrdHash = Data::OrdHash;

  auto asVec = [](const Data& data) {
    std::vector<int64_t> out(8, 0);
    if (auto* ords = std::get_if<OrdHash>(&data.counts)) {
      for (auto [ord, count] : *ords) out[ord] = count;
    } else if (auto* vec = std::get_if<CountVector>(&data.counts)) {
      for (size_t i = 0; i < vec->size(); i++) out[i] = (*vec)[i];
    } else if (auto* skinny = std::get_if<SkinnyCounter8>(&data.counts)) {
      for (size_t i = 0; i < skinny->counts.size(); i++) out[i] = skinny->counts[i];
      for (auto [ord, count] : skinny->overflow) out[ord] += count;
    }
    return out;
  };
  auto makeMap = [](std::initializer_list<std::pair<const int64_t, int64_t>> vals, int64_t missing) {
    Data data; data.counts = OrdHash(vals); data.missing_num = missing; return data;
  };
  auto makeSkinny = [](std::initializer_list<std::pair<int64_t, int64_t>> vals, int64_t missing) {
    Data data;
    data.counts.emplace<SkinnyCounter8>(8);
    auto& s = std::get<SkinnyCounter8>(data.counts);
    for (auto [ord, count] : vals) s.increment(ord, count);
    data.missing_num = missing;
    return data;
  };

  // Fresh (monostate) builds the requested rep; a fresh vector is sized to globVals.
  { Data d; Data::ensureRep(d, Rep::Vector, 8);
    EXPECT_TRUE(std::holds_alternative<CountVector>(d.counts));
    EXPECT_EQ((size_t)8, std::get<CountVector>(d.counts).size()); }
  { Data d; Data::ensureRep(d, Rep::Hash, 8);
    EXPECT_TRUE(std::holds_alternative<OrdHash>(d.counts)); }
  { Data d; Data::ensureRep(d, Rep::Skinny, 8);
    EXPECT_TRUE(std::holds_alternative<SkinnyCounter8>(d.counts)); }

  // UPGRADE skinny -> vector (incl. an overflowed count): rep changes, counts
  // and missing_num are preserved.
  { Data d = makeSkinny({{2, 3}, {5, 300}}, 7);
    Data::ensureRep(d, Rep::Vector, 8);
    EXPECT_TRUE(std::holds_alternative<CountVector>(d.counts));
    EXPECT_EQ((std::vector<int64_t>{0, 0, 3, 0, 0, 300, 0, 0}), asVec(d));
    EXPECT_EQ(7, d.missing_num); }

  // UPGRADE map -> vector.
  { Data d = makeMap({{1, 5}, {3, 6}}, 4);
    Data::ensureRep(d, Rep::Vector, 8);
    EXPECT_TRUE(std::holds_alternative<CountVector>(d.counts));
    EXPECT_EQ((std::vector<int64_t>{0, 5, 0, 6, 0, 0, 0, 0}), asVec(d));
    EXPECT_EQ(4, d.missing_num); }

  // UPGRADE map -> skinny.
  { Data d = makeMap({{2, 9}, {6, 1}}, 2);
    Data::ensureRep(d, Rep::Skinny, 8);
    EXPECT_TRUE(std::holds_alternative<SkinnyCounter8>(d.counts));
    EXPECT_EQ((std::vector<int64_t>{0, 0, 9, 0, 0, 0, 1, 0}), asVec(d));
    EXPECT_EQ(2, d.missing_num); }

  // NEVER DOWNGRADE: an existing larger rep is kept, counts intact.
  { Data d = makeSkinny({{1, 4}}, 0);  // skinny, ask hash -> keep skinny
    Data::ensureRep(d, Rep::Hash, 8);
    EXPECT_TRUE(std::holds_alternative<SkinnyCounter8>(d.counts));
    EXPECT_EQ((std::vector<int64_t>{0, 4, 0, 0, 0, 0, 0, 0}), asVec(d)); }
  { Data d; d.counts = CountVector({1, 2, 0, 0, 0, 0, 0, 0}); d.missing_num = 3;  // vec, ask skinny -> keep vec
    Data::ensureRep(d, Rep::Skinny, 8);
    EXPECT_TRUE(std::holds_alternative<CountVector>(d.counts));
    EXPECT_EQ((std::vector<int64_t>{1, 2, 0, 0, 0, 0, 0, 0}), asVec(d));
    EXPECT_EQ(3, d.missing_num); }

  // SAME REP: existing storage and its accumulated counts are kept (not cleared).
  { Data d; d.counts = CountVector({5, 0, 7, 0, 0, 0, 0, 0});
    Data::ensureRep(d, Rep::Vector, 8);
    EXPECT_TRUE(std::holds_alternative<CountVector>(d.counts));
    EXPECT_EQ((std::vector<int64_t>{5, 0, 7, 0, 0, 0, 0, 0}), asVec(d)); }
}

TEST_F(FacetTest, emptyIndex) {
  CollectionHelper helper;
  
  // Test all field types that support faceting
  struct FieldTypeTest {
    std::string fieldName;
    std::string description;
    bool isRangeFacet;
  };
  
  std::vector<FieldTypeTest> fieldTypes = {
    {"price_i", "integer field", false},
    {"pricea_is", "multivalued integer field", false},
    {"category_s", "string field", false},
    {"categories_ss", "multivalued string field", false},
    {"description_w", "text field", false},
    {"price_i", "integer range facet", true},
    {"price_is", "multivalued integer range facet", true}
  };
  
  for (const auto& fieldType : fieldTypes) {
    // Create a search request with faceting on the field type
    auto req = localReq(soluxNode->getSearchEngine());
    req->collection("main");

    req->topDocs().getNumber(true).allQuery();

    // Add facet based on field type
    if (fieldType.isRangeFacet) {
      // Range facet for integer
      req->rangeFacet("f_range", fieldType.fieldName).range(0, 100, 10);
    } else {
      // Regular field facet
      auto& facet = req->facet("f", fieldType.fieldName);
      facet.limit(10);
      std::get<solux::api::FieldFacet>(facet.rawOp().kind).missing = true; // Also test missing value handling
    }

    // Execute search on empty index
    req->execute(true);

    // Verify we get a response without crashing
    ASSERT_EQ(1u, req->responses.size()) << "Failed for " << fieldType.description;
    const auto& ops = req->responses[0]->proto.ops;

    // Check the appropriate facet result based on type
    if (fieldType.isRangeFacet) {
      ASSERT_TRUE(ops.contains("f_range")) << "Failed for " << fieldType.description;
      const auto* facetResult = ops.at("f_range")->facetResult();
      ASSERT_NE(facetResult, nullptr) << "Failed for " << fieldType.description;

      const auto& ids = std::get<solux::api::ArrArrInt>(facetResult->bucket_ids->kind);
      ASSERT_EQ(10, (int)ids.v.size()) << "Failed for " << fieldType.description;
      ASSERT_EQ(10, (int)facetResult->counts.size()) << "Failed for " << fieldType.description;
      for (int64_t count : facetResult->counts) EXPECT_EQ(0, count);
    } else {
      ASSERT_TRUE(ops.contains("f")) << "Failed for " << fieldType.description;
      const auto* facetResult = ops.at("f")->facetResult();
      ASSERT_NE(facetResult, nullptr) << "Failed for " << fieldType.description;

      // Check appropriate bucket type based on field type
      if (fieldType.fieldName.contains("_i")) {
        // Integer field
        ASSERT_EQ(0, (int)std::get<solux::api::ColInt>(facetResult->bucket_ids->kind).v.size()) << "Failed for " << fieldType.description;
      } else if (fieldType.fieldName.contains("_s") || fieldType.fieldName.contains("_w")) {
        // String or text field
        ASSERT_EQ(0, (int)std::get<solux::api::ColStr>(facetResult->bucket_ids->kind).v.size()) << "Failed for " << fieldType.description;
      }
      ASSERT_EQ(0, (int)facetResult->counts.size()) << "Failed for " << fieldType.description;

      // Since we requested missing=true, missing count should be 0 for empty index
      ASSERT_EQ(0, facetResult->missing.value_or(0)) << "Failed for " << fieldType.description;
    }
  }
}

TEST_F(FacetTest, pointsRangeFacetMatchesColumnWalk) {
  CollectionHelper helper;
  const std::array fields = {
    RangeSchemaField{"range_is", true, true},
    RangeSchemaField{"extreme_is", true, true}
  };
  setRangeFacetSchema(helper, fields);

  const int64_t i64min = std::numeric_limits<int64_t>::min();
  const int64_t i64max = std::numeric_limits<int64_t>::max();
  std::vector<Doc> docs = {
    flatdoc("id", "a", "range_is", vec_i(-5000, -1000, 0),
            "extreme_is", vec_i(i64min, i64min + 20)),
    flatdoc("id", "b", "range_is", vec_i(1000, 2000),
            "extreme_is", vec_i(0, 10)),
    flatdoc("id", "c", "range_is", vec_i(3000, 7000, 8000),
            "extreme_is", vec_i(i64max - 11, i64max)),
    flatdoc("id", "d", "range_is", vec_i(12000)),
    flatdoc("id", "missing")
  };
  ASSERT_TRUE(helper.indexAll(docs, UpdateMessage::COMMIT).success);

  PointsRangeFacetTestGuard guard;
  auto run = [&](bool disablePoints) {
    IntFacetRangeReq::disablePointsRangeFacetForTests = disablePoints;
    SkipStats::reset();
    auto req = localReq(soluxNode->getSearchEngine());
    req->collection("main");
    auto& all = req->rangeFacet("all", "range_is").range(-1500, 9100, 2700);
    std::get<api::RangeFacet>(all.rawOp().kind).missing = true;
    auto& filtered = req->rangeFacet("filtered", "range_is")
                         .range(-1500, 9100, 2700).mincount(3);
    std::get<api::RangeFacet>(filtered.rawOp().kind).missing = true;
    auto& extreme = req->rangeFacet("extreme", "extreme_is")
                        .range(i64min + 10, i64max - 10, i64max);
    std::get<api::RangeFacet>(extreme.rawOp().kind).missing = true;
    req->execute(false);
    EXPECT_FALSE(hasError(req->responses[0]->proto)) << req->toString();

    const std::array allBounds = {
      std::pair<int64_t, int64_t>{-1500, 1200},
      std::pair<int64_t, int64_t>{1200, 3900},
      std::pair<int64_t, int64_t>{3900, 6600},
      std::pair<int64_t, int64_t>{6600, 9100}
    };
    const std::array<int64_t, 4> allCounts = {3, 2, 0, 2};
    expectRangeResult(rootFacetResult(*req, "all"), allBounds, allCounts, 1);
    const std::array filteredBounds = {
      std::pair<int64_t, int64_t>{-1500, 1200}
    };
    const std::array<int64_t, 1> filteredCounts = {3};
    expectRangeResult(rootFacetResult(*req, "filtered"), filteredBounds,
                      filteredCounts, 1);
    const std::array extremeBounds = {
      std::pair<int64_t, int64_t>{i64min + 10, 9},
      std::pair<int64_t, int64_t>{9, i64max - 10}
    };
    const std::array<int64_t, 2> extremeCounts = {2, 2};
    expectRangeResult(rootFacetResult(*req, "extreme"), extremeBounds,
                      extremeCounts, 2);

    std::array<std::vector<std::byte>, 3> encoded = {
      encodeFacetResult(*req, "all"), encodeFacetResult(*req, "filtered"),
      encodeFacetResult(*req, "extreme")
    };
    return std::pair{std::move(encoded), SkipStats::rangeFacetPointsArms};
  };

  auto [pointsResults, pointsArms] = run(false);
  auto [walkResults, walkArms] = run(true);
  EXPECT_EQ(3, pointsArms);
  EXPECT_EQ(0, walkArms);
  EXPECT_EQ(pointsResults, walkResults);
  // Later tests (TermScorerTest) index into "main" without clearing first.
}

TEST_F(FacetTest, floatingRangeFacetMatchesScanAndColumnWalk) {
  CollectionHelper helper;
  helper.clear();
  const std::array fields = {
    RangeSchemaField{"value_f", true, false, api::FieldDef::FieldClass::FLOAT},
    RangeSchemaField{"value_d", true, false, api::FieldDef::FieldClass::DOUBLE}
  };
  setRangeFacetSchema(helper, fields);

  const float floatEdge = 0.2f;
  const std::array floatValues = {
    -std::numeric_limits<float>::infinity(), -0.0f, 0.0f,
    std::nextafter(floatEdge, -std::numeric_limits<float>::infinity()),
    floatEdge,
    std::nextafter(floatEdge, std::numeric_limits<float>::infinity()),
    0.8f, std::nextafter(1.0f, 0.0f), 1.0f,
    std::numeric_limits<float>::infinity(),
    std::numeric_limits<float>::quiet_NaN()
  };
  const double doubleEdge = 0.25;
  const std::array doubleValues = {
    -std::numeric_limits<double>::infinity(), -0.0, 0.0,
    std::nextafter(doubleEdge, -std::numeric_limits<double>::infinity()),
    doubleEdge,
    std::nextafter(doubleEdge, std::numeric_limits<double>::infinity()),
    0.75, std::nextafter(1.0, 0.0), 1.0,
    std::numeric_limits<double>::infinity(),
    std::numeric_limits<double>::quiet_NaN()
  };
  for (size_t i = 0; i < floatValues.size(); i++) {
    ASSERT_TRUE(helper.index(flatdoc("id", "f" + std::to_string(i),
                                     "value_f", floatValues[i]),
                             UpdateMessage::NO_COMMIT).success);
  }
  for (size_t i = 0; i < doubleValues.size(); i++) {
    ASSERT_TRUE(helper.index(flatdoc("id", "d" + std::to_string(i),
                                     "value_d", doubleValues[i]),
                             i + 1 == doubleValues.size()
                                 ? UpdateMessage::COMMIT
                                 : UpdateMessage::NO_COMMIT).success);
  }

  const std::array<float, 6> floatFences = {
    0.0f, 0.2f, 0.4f, 0.6f, 0.8f, 1.0f
  };
  const std::array<double, 5> doubleFences = {
    0.0, 0.25, 0.5, 0.75, 1.0
  };
  PointsRangeFacetTestGuard guard;
  auto run = [&](bool disablePoints) {
    IntFacetRangeReq::disablePointsRangeFacetForTests = disablePoints;
    SkipStats::reset();
    auto req = localReq(soluxNode->getSearchEngine());
    req->collection("main");
    req->rangeFacet("floats", "value_f").rangeFp(-0.0, 1.0, 0.2);
    req->rangeFacet("doubles", "value_d").rangeFp(-0.0, 1.0, 0.25);
    req->rangeFacet("none", "value_d").rangeFp(0.0, 1.0, 0.25)
        .mincount(100);
    req->execute(false);
    EXPECT_TRUE(req->ok()) << req->errorMsg();

    const auto& floatResult = rootFacetResult(*req, "floats");
    expectFloatingRangeResult<api::ArrArrFloat>(
        floatResult, std::span<const float>(floatFences),
        std::span<const float>(floatValues));
    const auto& floatBounds = std::get<api::ArrArrFloat>(
        floatResult.bucket_ids->kind).v;
    EXPECT_EQ(0, std::bit_cast<uint32_t>(floatBounds.front().v.front()));
    EXPECT_EQ(0, floatResult.counts[2]);

    const auto& doubleResult = rootFacetResult(*req, "doubles");
    expectFloatingRangeResult<api::ArrArrDouble>(
        doubleResult, std::span<const double>(doubleFences),
        std::span<const double>(doubleValues));
    const auto& doubleBounds = std::get<api::ArrArrDouble>(
        doubleResult.bucket_ids->kind).v;
    EXPECT_EQ(0, std::bit_cast<uint64_t>(doubleBounds.front().v.front()));
    EXPECT_EQ(0, doubleResult.counts[2]);

    const auto& none = rootFacetResult(*req, "none");
    EXPECT_TRUE(std::get<api::ArrArrDouble>(none.bucket_ids->kind).v.empty());
    EXPECT_TRUE(none.counts.empty());
    std::array<std::vector<std::byte>, 3> encoded = {
      encodeFacetResult(*req, "floats"), encodeFacetResult(*req, "doubles"),
      encodeFacetResult(*req, "none")
    };
    return std::pair{std::move(encoded), SkipStats::rangeFacetPointsArms};
  };

  auto [pointsResults, pointsArms] = run(false);
  auto [walkResults, walkArms] = run(true);
  EXPECT_EQ(3, pointsArms);
  EXPECT_EQ(0, walkArms);
  EXPECT_EQ(pointsResults, walkResults);
}

TEST_F(FacetTest, pointsRangeFacetFallbacks) {
  PointsRangeFacetTestGuard guard;
  IntFacetRangeReq::disablePointsRangeFacetForTests = false;

  auto indexDocs = [](CollectionHelper& helper) {
    const std::array fields = {
      RangeSchemaField{"range_i", true, false},
      RangeSchemaField{"walk_i", false, false}
    };
    setRangeFacetSchema(helper, fields);
    std::vector<Doc> docs = {
      flatdoc("id", "a", "tag_s", "keep", "range_i", 1000, "walk_i", 1000),
      flatdoc("id", "b", "tag_s", "drop", "range_i", 2000, "walk_i", 2000),
      flatdoc("id", "c", "tag_s", "keep")
    };
    ASSERT_TRUE(helper.indexAll(docs, UpdateMessage::COMMIT).success);
  };
  const std::array bounds = {
    std::pair<int64_t, int64_t>{0, 1000},
    std::pair<int64_t, int64_t>{1000, 2000},
    std::pair<int64_t, int64_t>{2000, 3000}
  };

  {
    CollectionHelper helper;
    indexDocs(helper);
    ASSERT_TRUE(helper.deleteById("b", UpdateMessage::COMMIT).success);
    SkipStats::reset();
    auto req = localReq(soluxNode->getSearchEngine());
    req->collection("main");
    auto& facet = req->rangeFacet("f", "range_i").range(0, 3000, 1000);
    std::get<api::RangeFacet>(facet.rawOp().kind).missing = true;
    req->execute(false);
    EXPECT_EQ(0, SkipStats::rangeFacetPointsArms);
    const std::array<int64_t, 3> counts = {0, 1, 0};
    expectRangeResult(rootFacetResult(*req, "f"), bounds, counts, 1);
  }

  {
    CollectionHelper helper;
    helper.clear();
    indexDocs(helper);
    SkipStats::reset();
    auto req = localReq(soluxNode->getSearchEngine());
    req->collection("main");
    auto& top = req->topDocs("q");
    top.matchQuery("tag_s", "keep");
    auto& facet = top.rangeFacet("f", "range_i").range(0, 3000, 1000);
    std::get<api::RangeFacet>(facet.rawOp().kind).missing = true;
    req->execute(false);
    EXPECT_EQ(0, SkipStats::rangeFacetPointsArms);
    const auto* docs = req->docList("q");
    ASSERT_NE(nullptr, docs);
    const std::array<int64_t, 3> counts = {0, 1, 0};
    expectRangeResult(*docs->ops.at("f")->facetResult(), bounds, counts, 1);
  }

  {
    CollectionHelper helper;
    helper.clear();
    indexDocs(helper);
    SkipStats::reset();
    auto req = localReq(soluxNode->getSearchEngine());
    req->collection("main");
    auto& facet = req->rangeFacet("f", "walk_i").range(0, 3000, 1000);
    std::get<api::RangeFacet>(facet.rawOp().kind).missing = true;
    req->execute(false);
    EXPECT_EQ(0, SkipStats::rangeFacetPointsArms);
    const std::array<int64_t, 3> counts = {0, 1, 1};
    expectRangeResult(rootFacetResult(*req, "f"), bounds, counts, 1);
  }
}

// Historical transition expectations depend on the process tzdb snapshot.
TEST_F(FacetTest, calendarDateRangeDenverDstAndPointsParity) {
  CollectionHelper helper;
  helper.clear();
  const std::array fields = {
    RangeSchemaField{"when_dt", true, false, api::FieldDef::FieldClass::DATE}
  };
  setRangeFacetSchema(helper, fields);

  TimeZone denver = zone("America/Denver");
  std::array<int64_t, 6> fences;
  const std::array dates = {
    "2024-03-09", "2024-03-10", "2024-03-11",
    "2024-03-12", "2024-03-13", "2024-03-14"
  };
  for (size_t i = 0; i < fences.size(); i++) fences[i] = localLo(dates[i], denver);
  EXPECT_EQ(24 * 60 * 60 * 1000LL, fences[1] - fences[0]);
  EXPECT_EQ(23 * 60 * 60 * 1000LL, fences[2] - fences[1]);
  EXPECT_EQ(24 * 60 * 60 * 1000LL, fences[3] - fences[2]);

  const std::array values = {
    fences[0], fences[1] - 1,
    fences[1], fences[1] + 1, fences[2] - 1,
    fences[2], fences[3] - 1,
    fences[3], fences[4] - 1
  };
  for (size_t i = 0; i < values.size(); i++) {
    ASSERT_TRUE(helper.index(
        flatdoc("id", std::to_string(i), "when_dt", values[i]),
        i + 1 == values.size() ? UpdateMessage::COMMIT
                               : UpdateMessage::NO_COMMIT).success);
  }

  PointsRangeFacetTestGuard guard;
  auto run = [&](bool disablePoints) {
    IntFacetRangeReq::disablePointsRangeFacetForTests = disablePoints;
    auto req = localReq(soluxNode->getSearchEngine());
    req->collection("main");
    req->rangeFacet("days", "when_dt").calendarRange(
        "2024-03-09", "2024-03-14", 1,
        api::CalendarGap_::Unit::DAY, "America/Denver");
    req->execute(false);
    EXPECT_TRUE(req->ok()) << req->errorMsg();
    const auto& result = rootFacetResult(*req, "days");
    const std::array expectedCounts = {2LL, 3LL, 2LL, 2LL, 0LL};
    EXPECT_EQ(expectedCounts.size(), result.counts.size());
    std::vector<int64_t> actualCounts(result.counts.begin(), result.counts.end());
    std::vector<int64_t> wantedCounts(expectedCounts.begin(), expectedCounts.end());
    EXPECT_EQ(wantedCounts, actualCounts);
    EXPECT_EQ((int64_t)values.size(),
              std::accumulate(result.counts.begin(), result.counts.end(), 0LL));
    return encodeFacetResult(*req, "days");
  };
  EXPECT_EQ(run(false), run(true));
}

TEST_F(FacetTest, calendarDateRangeTransitionFenceEdges) {
  CollectionHelper helper;
  helper.clear();
  const std::array fields = {
    RangeSchemaField{"when_dt", true, false, api::FieldDef::FieldClass::DATE}
  };
  setRangeFacetSchema(helper, fields);

  int64_t transition = epoch("2024-03-10T09:00:00Z");
  ASSERT_TRUE(helper.index(flatdoc("id", "a", "when_dt", transition - 1),
                           UpdateMessage::NO_COMMIT).success);
  ASSERT_TRUE(helper.index(flatdoc("id", "b", "when_dt", transition),
                           UpdateMessage::NO_COMMIT).success);
  ASSERT_TRUE(helper.index(flatdoc("id", "c", "when_dt", transition + 1),
                           UpdateMessage::COMMIT).success);

  auto req = localReq(soluxNode->getSearchEngine());
  req->collection("main");
  req->rangeFacet("days", "when_dt").calendarRange(
      "2024-03-09T02", "2024-03-11T02", 1,
      api::CalendarGap_::Unit::DAY, "America/Denver");
  req->execute(false);
  ASSERT_OK(req);
  const std::array bounds = {
    std::pair{epoch("2024-03-09T09:00:00Z"), transition},
    std::pair{transition, epoch("2024-03-11T08:00:00Z")}
  };
  const std::array<int64_t, 2> counts = {1, 2};
  expectRangeResult(rootFacetResult(*req, "days"), bounds, counts, -1);
}

TEST_F(FacetTest, dateRangeFacetZoneInheritanceAndOverrides) {
  CollectionHelper helper;
  helper.clear();
  const std::array fields = {
    RangeSchemaField{"when_dt", false, false, api::FieldDef::FieldClass::DATE}
  };
  setRangeFacetSchema(helper, fields);

  auto req = localReq(soluxNode->getSearchEngine());
  req->collection("main").timeZone("America/Denver");
  req->rangeFacet("inherited", "when_dt").calendarRange(
      "2024-03-10", "2024-03-11", 1, api::CalendarGap_::Unit::DAY);
  req->rangeFacet("override", "when_dt").calendarRange(
      "2024-03-10", "2024-03-11", 1, api::CalendarGap_::Unit::DAY,
      "America/New_York");
  req->rangeFacet("utc", "when_dt").calendarRange(
      "2024-03-10", "2024-03-11", 1, api::CalendarGap_::Unit::DAY, "UTC");
  req->execute(false);
  ASSERT_OK(req);

  auto firstBound = [&](std::string_view name) {
    return std::get<api::ArrArrInt>(
        rootFacetResult(*req, name).bucket_ids->kind).v[0].v[0];
  };
  EXPECT_EQ(epoch("2024-03-10T07:00:00Z"), firstBound("inherited"));
  EXPECT_EQ(epoch("2024-03-10T05:00:00Z"), firstBound("override"));
  EXPECT_EQ(epoch("2024-03-10T00:00:00Z"), firstBound("utc"));
}

TEST_F(FacetTest, dateRangeFacetDateMathBounds) {
  CollectionHelper helper;
  helper.clear();
  const std::array fields = {
    RangeSchemaField{"when_dt", false, false, api::FieldDef::FieldClass::DATE}
  };
  setRangeFacetSchema(helper, fields);

  auto req = localReq(soluxNode->getSearchEngine());
  req->collection("main");
  int64_t now = req->dateMathNowEpochMillis;
  req->rangeFacet("days", "when_dt").range(
      "NOW/DAY-30DAYS", "NOW/DAY+1DAY", 86400000);
  req->execute(false);
  ASSERT_OK(req);
  const auto& bounds = std::get<api::ArrArrInt>(
      rootFacetResult(*req, "days").bucket_ids->kind).v;
  ASSERT_EQ(31u, bounds.size());
  EXPECT_EQ(localLo("NOW/DAY-30DAYS", TimeZone::utc(), now), bounds.front().v[0]);
  EXPECT_EQ(localLo("NOW/DAY+1DAY", TimeZone::utc(), now), bounds.back().v[1]);
}

TEST_F(FacetTest, dateRangeFacetSkippedBoundWarns) {
  CollectionHelper helper;
  helper.clear();
  const std::array fields = {
    RangeSchemaField{"when_dt", false, false, api::FieldDef::FieldClass::DATE}
  };
  setRangeFacetSchema(helper, fields);

  auto req = localReq(soluxNode->getSearchEngine());
  req->collection("main");
  auto& cursor = req->rangeFacet("minutes", "when_dt")
                     .range("2024-03-10T02:30", "2024-03-10T04", 60000);
  std::get<api::RangeFacet>(cursor.rawOp().kind).time_zone = "America/Denver";
  req->execute(false);
  ASSERT_OK(req);
  ASSERT_TRUE(req->hasWarning("date_granule_skipped"));
  bool actionable = false;
  for (const auto& warning : req->respWarnings()) {
    if (warning.code == "date_granule_skipped") {
      actionable = warning.message.find("facet 'minutes'") != std::string_view::npos
          && warning.message.find("2024-03-10T02:30") != std::string_view::npos;
    }
  }
  EXPECT_TRUE(actionable);
}

TEST_F(FacetTest, calendarDateRangeFromStartAndFoldProvenance) {
  CollectionHelper helper;
  helper.clear();
  const std::array fields = {
    RangeSchemaField{"when_dt", false, false, api::FieldDef::FieldClass::DATE}
  };
  setRangeFacetSchema(helper, fields);

  auto req = localReq(soluxNode->getSearchEngine());
  req->collection("main");
  req->rangeFacet("month_ends", "when_dt").calendarRange(
      "2024-01-31", "2024-04-01", 1,
      api::CalendarGap_::Unit::MONTH, "UTC");
  req->rangeFacet("havana", "when_dt").calendarRange(
      "2015-01-01T00", "2015-12-01T00", 1,
      api::CalendarGap_::Unit::MONTH, "America/Havana");
  req->rangeFacet("fixed_days", "when_dt").calendarRange(
      "2024-03-09", "2024-03-12", 1,
      api::CalendarGap_::Unit::DAY, "+05:30");
  req->execute(false);
  ASSERT_OK(req);

  const auto& months = std::get<api::ArrArrInt>(
      rootFacetResult(*req, "month_ends").bucket_ids->kind).v;
  ASSERT_EQ(3u, months.size());
  EXPECT_EQ(epoch("2024-02-29T00:00:00Z"), months[0].v[1]);
  EXPECT_EQ(epoch("2024-03-31T00:00:00Z"), months[1].v[1]);
  EXPECT_EQ(epoch("2024-04-01T00:00:00Z"), months[2].v[1]);

  const auto& havana = std::get<api::ArrArrInt>(
      rootFacetResult(*req, "havana").bucket_ids->kind).v;
  ASSERT_EQ(11u, havana.size());
  EXPECT_EQ(epoch("2015-11-01T05:00:00Z"), havana[9].v[1]);

  const auto& fixed = std::get<api::ArrArrInt>(
      rootFacetResult(*req, "fixed_days").bucket_ids->kind).v;
  ASSERT_EQ(3u, fixed.size());
  for (const auto& pair : fixed) EXPECT_EQ(86400000, pair.v[1] - pair.v[0]);
}

// Pacific/Apia skipped 2011-12-30 when it moved across the dateline.
TEST_F(FacetTest, calendarDateRangeApiaSkippedDayParityAndWarning) {
  CollectionHelper helper;
  helper.clear();
  const std::array fields = {
    RangeSchemaField{"when_dt", true, false, api::FieldDef::FieldClass::DATE}
  };
  setRangeFacetSchema(helper, fields);
  const std::array values = {
    epoch("2011-12-29T10:00:00Z"),
    epoch("2011-12-30T10:00:00Z"),
    epoch("2011-12-31T10:00:00Z")
  };
  for (size_t i = 0; i < values.size(); i++) {
    ASSERT_TRUE(helper.index(
        flatdoc("id", std::to_string(i), "when_dt", values[i]),
        i + 1 == values.size() ? UpdateMessage::COMMIT
                               : UpdateMessage::NO_COMMIT).success);
  }

  PointsRangeFacetTestGuard guard;
  auto run = [&](bool disablePoints) {
    IntFacetRangeReq::disablePointsRangeFacetForTests = disablePoints;
    auto req = localReq(soluxNode->getSearchEngine());
    req->collection("main");
    req->rangeFacet("by_day", "when_dt").calendarRange(
        "2011-12-29", "2012-01-02", 1,
        api::CalendarGap_::Unit::DAY, "Pacific/Apia");
    req->execute(false);
    EXPECT_TRUE(req->ok()) << req->errorMsg();
    EXPECT_TRUE(req->hasWarning("calendar_bucket_skipped"));
    bool actionable = false;
    for (const auto& warning : req->respWarnings()) {
      if (warning.code == "calendar_bucket_skipped") {
        EXPECT_NE(warning.message.find("facet 'by_day'"), std::string_view::npos)
            << warning.message;
        EXPECT_NE(warning.message.find("2011-12-30"), std::string_view::npos)
            << warning.message;
        actionable = warning.message.find("facet 'by_day'") != std::string_view::npos
            && warning.message.find("2011-12-30") != std::string_view::npos;
      }
    }
    EXPECT_TRUE(actionable);
    EXPECT_EQ(3u, rootFacetResult(*req, "by_day").counts.size());
    return encodeFacetResult(*req, "by_day");
  };
  EXPECT_EQ(run(false), run(true));
}

TEST_F(FacetTest, rangeFacetValidationAndDegenerateRanges) {
  CollectionHelper helper;
  helper.clear();
  const std::array fields = {
    RangeSchemaField{"number_i", false, false},
    RangeSchemaField{"when_dt", false, false, api::FieldDef::FieldClass::DATE},
    RangeSchemaField{"number_f", false, false, api::FieldDef::FieldClass::FLOAT},
    RangeSchemaField{"number_d", false, false, api::FieldDef::FieldClass::DOUBLE}
  };
  setRangeFacetSchema(helper, fields);

  auto expectError = [&](auto configure, std::string_view text) {
    auto req = localReq(soluxNode->getSearchEngine());
    req->collection("main");
    configure(*req);
    req->execute(false);
    EXPECT_FALSE(req->ok());
    EXPECT_NE(req->errorMsg().find(text), std::string::npos) << req->errorMsg();
  };
  expectError([](LocalReq& req) { req.rangeFacet("f", "number_i"); },
              "start and end are required");
  expectError([](LocalReq& req) {
    auto& cursor = req.rangeFacet("f", "number_i").range(0, 10, 1);
    std::get<api::RangeFacet>(cursor.rawOp().kind).end.reset();
  }, "start and end are required");
  expectError([](LocalReq& req) { req.rangeFacet("f", "number_i").range(4, 4, 1); },
              "start must be less than end");
  expectError([](LocalReq& req) { req.rangeFacet("f", "number_i").range(5, 4, 1); },
              "start must be less than end");
  expectError([](LocalReq& req) {
    auto& cursor = req.rangeFacet("f", "when_dt")
                       .range("2024-03-10T02", "2024-03-10T03", 1);
    std::get<api::RangeFacet>(cursor.rawOp().kind).time_zone = "America/Denver";
  }, "start must be less than end");
  expectError([](LocalReq& req) {
    auto& cursor = req.rangeFacet("f", "number_i").range(0, 10, 1);
    std::get<api::RangeFacet>(cursor.rawOp().kind).time_zone = "UTC";
  }, "only valid for DATE fields");
  expectError([](LocalReq& req) {
    req.rangeFacet("f", "number_i").calendarRange(
        "0", "10", 1, api::CalendarGap_::Unit::DAY);
  }, "only valid for DATE fields");
  expectError([](LocalReq& req) {
    req.rangeFacet("f", "when_dt").calendarRange(
        "2024-01-01", "2024-01-02", 1, api::CalendarGap_::Unit::DAY,
        "No/Such_Zone");
  }, "No/Such_Zone");
  expectError([](LocalReq& req) {
    req.rangeFacet("f", "number_i").range(0, 100001, 1);
  }, "100000 bucket limit");
  expectError([](LocalReq& req) {
    req.rangeFacet("f", "number_i").range(0, 10, 0);
  }, "gap must be > 0");
  expectError([](LocalReq& req) {
    req.rangeFacet("f", "when_dt").calendarRange(
        "2024-01-01", "2024-01-02", 0, api::CalendarGap_::Unit::DAY);
  }, "requires n > 0");
  expectError([](LocalReq& req) {
    req.rangeFacet("f", "when_dt").calendarRange(
        "2024-01-01", "2024-01-02", 1, api::CalendarGap_::Unit::UNKNOWN);
  }, "requires n > 0");
  expectError([](LocalReq& req) {
    req.rangeFacet("f", "number_f").rangeFp(
        std::numeric_limits<double>::quiet_NaN(), 1.0, 0.1);
  }, "range start must be finite");
  expectError([](LocalReq& req) {
    req.rangeFacet("f", "number_d").rangeFp(
        0.0, std::numeric_limits<double>::infinity(), 0.1);
  }, "range end must be finite");
  expectError([](LocalReq& req) {
    req.rangeFacet("f", "number_d").rangeFp(
        0.0, 1.0, std::numeric_limits<double>::quiet_NaN());
  }, "range gap must be finite");
  expectError([](LocalReq& req) {
    req.rangeFacet("f", "number_f").rangeFp(
        0.0, 1.0, std::numeric_limits<double>::infinity());
  }, "range gap must be finite");
  expectError([](LocalReq& req) {
    req.rangeFacet("f", "number_f").rangeFp(0.0, 1.0, -0.0);
  }, "gap must be > 0");
  expectError([](LocalReq& req) {
    req.rangeFacet("f", "number_f").rangeFp(16777216.0, 16777220.0, 1.0);
  }, "fence 1");
  expectError([](LocalReq& req) {
    req.rangeFacet("f", "number_f").rangeFp(0.0, 1e300, 1e299);
  }, "not finite after FLOAT rounding");
  expectError([](LocalReq& req) {
    auto& cursor = req.rangeFacet("f", "number_f").rangeFp(0.0, 1.0, 0.1);
    std::get<api::RangeFacet>(cursor.rawOp().kind).time_zone = "UTC";
  }, "only valid for DATE fields");
  expectError([](LocalReq& req) {
    req.rangeFacet("f", "number_d").calendarRange(
        "0", "1", 1, api::CalendarGap_::Unit::DAY);
  }, "only valid for DATE fields");
}

TEST_F(FacetTest, rangeFacetEmptyDomainAndPartialLastBucket) {
  CollectionHelper helper;
  helper.clear();
  ASSERT_TRUE(helper.index(flatdoc("id", "a", "number_i", 9, "pick_s", "no"),
                           UpdateMessage::COMMIT).success);

  auto req = localReq(soluxNode->getSearchEngine());
  req->collection("main");
  auto& top = req->topDocs("q");
  top.matchQuery("pick_s", "yes");
  top.rangeFacet("f", "number_i").range(0, 10, 4);
  req->execute(false);
  ASSERT_OK(req);
  const auto* docs = req->docList("q");
  ASSERT_NE(nullptr, docs);
  const std::array bounds = {
    std::pair<int64_t, int64_t>{0, 4},
    std::pair<int64_t, int64_t>{4, 8},
    std::pair<int64_t, int64_t>{8, 10}
  };
  const std::array<int64_t, 3> counts = {0, 0, 0};
  expectRangeResult(*docs->ops.at("f")->facetResult(), bounds, counts, -1);
}

TEST_F(FacetTest, emptyIndexNestedFacet) {
  CollectionHelper helper;

  auto req = localReq(soluxNode->getSearchEngine());
  req->collection("main");

  auto& topDocs = req->topDocs();
  topDocs.getNumber(true).allQuery();

  auto& facet = topDocs.facet("f", "category_s");
  facet.limit(10);
  std::get<solux::api::FieldFacet>(facet.rawOp().kind).missing = true;

  req->execute(true);

  ASSERT_OK(req);
  const auto* docs = req->docList("q");
  ASSERT_NE(docs, nullptr) << req->toString();
  ASSERT_EQ(0, docs->found.value_or(0));
  ASSERT_TRUE(docs->ops.contains("f")) << req->toString();
  const auto* facetResult = docs->ops.at("f")->facetResult();
  ASSERT_NE(facetResult, nullptr) << req->toString();
  EXPECT_EQ(0, (int)std::get<solux::api::ColStr>(facetResult->bucket_ids->kind).v.size());
  EXPECT_EQ(0, (int)facetResult->counts.size());
  EXPECT_EQ(0, facetResult->missing.value_or(0));
}

TEST_F(FacetTest, singleSegment) {
  CollectionHelper helper;
  
  // Add some documents with integer and string fields using dynamic field naming
  std::vector<std::string> colors = {"red", "blue", "green", "red", "blue"};
  for (int i = 0; i < 5; i++) {
    helper.index(flatdoc("id", std::to_string(i), 
                        "price_i", i * 10,
                        "color_s", colors[i]), UpdateMessage::NO_COMMIT);
  }
  helper.commit();
  
  // Test integer faceting
  {
    auto req = localReq(soluxNode->getSearchEngine());
    req->collection("main");

    req->topDocs().getNumber(true).allQuery();
    req->facet("f", "price_i").limit(10);

    req->execute(true);

    ASSERT_EQ(1u, req->responses.size());
    const auto* facetResult = req->responses[0]->proto.ops.at("f")->facetResult();
    ASSERT_NE(facetResult, nullptr);
    const auto& bucketIds = std::get<solux::api::ColInt>(facetResult->bucket_ids->kind);

    // Should have 5 buckets (0, 10, 20, 30, 40)
    ASSERT_EQ(5, (int)bucketIds.v.size());
    ASSERT_EQ(5, (int)facetResult->counts.size());

    // Each bucket should have count of 1
    for (int i = 0; i < 5; i++) {
      EXPECT_EQ(i * 10, bucketIds.v[i]);
      EXPECT_EQ(1, facetResult->counts[i]);
    }
  }

  // Test string faceting
  {
    auto req = localReq(soluxNode->getSearchEngine());
    req->collection("main");

    req->topDocs().getNumber(true).allQuery();
    req->facet("f_str", "color_s").limit(10);

    req->execute(true);

    ASSERT_EQ(1u, req->responses.size());
    const auto* facetResult = req->responses[0]->proto.ops.at("f_str")->facetResult();
    ASSERT_NE(facetResult, nullptr);
    const auto& bucketIds = std::get<solux::api::ColStr>(facetResult->bucket_ids->kind);

    // Should have 3 unique colors
    ASSERT_EQ(3, (int)bucketIds.v.size());
    ASSERT_EQ(3, (int)facetResult->counts.size());

    // Check the counts for each color
    // Note: facets are typically sorted by count desc, then by value
    // We expect: red(2), blue(2), green(1)
    boost::unordered_flat_map<std::string, int> expectedCounts = {
      {"red", 2},
      {"blue", 2},
      {"green", 1}
    };

    for (int i = 0; i < (int)bucketIds.v.size(); i++) {
      std::string color = std::string(bucketIds.v[i]);
      EXPECT_TRUE(expectedCounts.count(color) > 0) << "Unexpected color: " << color;
      EXPECT_EQ(expectedCounts[color], facetResult->counts[i]) << "Wrong count for color: " << color;
    }
  }
}

TEST_F(FacetTest, multipleSegments) {
  CollectionHelper helper;
  
  // Add documents and commit multiple times to create multiple segments
  for (int seg = 0; seg < 3; seg++) {
    for (int i = 0; i < 3; i++) {
      helper.index(flatdoc("id", std::to_string(seg * 100 + i), "price_i", seg * 10 + i), UpdateMessage::NO_COMMIT);
    }
    helper.commit();
  }
  
  // Create search request with faceting
  auto req = localReq(soluxNode->getSearchEngine());
  req->collection("main");

  req->topDocs().getNumber(true).allQuery();
  req->facet("f", "price_i").limit(20);

  req->execute(true);

  ASSERT_EQ(1u, req->responses.size());
  const auto* facetResult = req->responses[0]->proto.ops.at("f")->facetResult();
  ASSERT_NE(facetResult, nullptr);

  // Should have 9 unique values: 0,1,2,10,11,12,20,21,22
  ASSERT_EQ(9, (int)std::get<solux::api::ColInt>(facetResult->bucket_ids->kind).v.size());
  ASSERT_EQ(9, (int)facetResult->counts.size());

  // Each value should have count of 1
  for (int i = 0; i < 9; i++) {
    EXPECT_EQ(1, facetResult->counts[i]);
  }
}

// A match-all TopDocs answers from its domain instead of iterating a scorer,
// so the hit count and the facet domain must come out the same at every limit
// - including limit 0, where there is no top-K collection to fall back on -
// and must exclude deleted docs, which live only in the domain.
TEST_F(FacetTest, matchAllCountsFromDomain) {
  CollectionHelper helper;
  std::vector<std::string> colors = {"red", "blue", "green"};
  for (int seg = 0; seg < 3; seg++) {
    for (int i = 0; i < 4; i++) {
      helper.index(flatdoc("id", std::to_string(seg * 100 + i),
                           "color_s", colors[(size_t)(i % 3)]),
                   UpdateMessage::NO_COMMIT);
    }
    helper.commit();
  }
  // 12 indexed as red 6 / blue 3 / green 3; deleting two reds and one blue
  // leaves 9 live docs with a distinct count per color.
  helper.deleteByIds({{"0", "1", "100"}}, UpdateMessage::COMMIT);

  boost::unordered_flat_map<std::string, int64_t> expected = {
    {"red", 4}, {"blue", 2}, {"green", 3}
  };

  for (int64_t limit : {0, 2, 100}) {
    for (bool para : {false, true}) {
      auto req = localReq(soluxNode->getSearchEngine());
      req->collection("main");
      req->topDocs().allQuery().getNumber(true).limit(limit);
      req->facet("f", "color_s").limit(10);
      req->execute(para);

      const auto* docs = req->responses[0]->proto.ops.at("q")->docList();
      auto why = std::format("limit={} para={}", limit, para);
      ASSERT_EQ(9, docs->found.value_or(-1)) << why;
      ASSERT_EQ(std::min(limit, (int64_t)9), (int64_t)docs->row_count) << why;

      const auto* facetResult = req->responses[0]->proto.ops.at("f")->facetResult();
      const auto& bucketIds = std::get<solux::api::ColStr>(facetResult->bucket_ids->kind);
      ASSERT_EQ(3u, bucketIds.v.size()) << why;
      for (size_t i = 0; i < bucketIds.v.size(); i++) {
        EXPECT_EQ(expected[std::string(bucketIds.v[i])], facetResult->counts[i])
          << why << " color=" << bucketIds.v[i];
      }
    }
  }
}

TEST_F(FacetTest, flatOrdMapDeltaFrames) {
  CollectionHelper helper;
  for (int seg = 0; seg < 2; seg++) {
    for (int i = seg; i < 300; i += 2) {
      for (int repeat = 0; repeat < 3; repeat++) {
        helper.index(flatdoc("id", std::format("{}-{}", i, repeat),
                             "cat_s", std::format("term{:04}", i)),
                     UpdateMessage::NO_COMMIT);
      }
    }
    helper.commit();
  }

  auto req = localReq(soluxNode->getSearchEngine());
  req->collection("main");
  req->topDocs().getNumber(true).allQuery();
  req->facet("f", "cat_s").limit(-1);
  req->execute(true);

  ASSERT_OK(req);
  const auto& result = rootFacetResult(*req, "f");
  const auto& bucketIds = std::get<api::ColStr>(result.bucket_ids->kind).v;
  ASSERT_EQ(300u, bucketIds.size());
  ASSERT_EQ(300u, result.counts.size());
  for (int i = 0; i < 300; i++) {
    EXPECT_EQ(std::format("term{:04}", i), bucketIds[i]);
    EXPECT_EQ(3, result.counts[i]);
  }
}

TEST_F(FacetTest, fullTextFacetSegmentMissingField) {
  CollectionHelper helper;

  helper.index(flatdoc("id", "a", "body_w", "alpha beta"), UpdateMessage::NO_COMMIT);
  helper.index(flatdoc("id", "b", "body_w", "alpha"), UpdateMessage::COMMIT);
  helper.index(flatdoc("id", "c", "other_s", "x"), UpdateMessage::NO_COMMIT);
  helper.index(flatdoc("id", "d", "other_s", "y"), UpdateMessage::COMMIT);

  auto req = localReq(soluxNode->getSearchEngine());
  req->collection("main");

  req->topDocs().getNumber(true).allQuery();

  auto& facet = req->facet("f", "body_w");
  facet.limit(-1);
  std::get<solux::api::FieldFacet>(facet.rawOp().kind).missing = true;

  req->execute(true);

  ASSERT_OK(req);
  ASSERT_TRUE(req->responses[0]->proto.ops.contains("f")) << req->toString();
  const auto* facetResult = req->responses[0]->proto.ops.at("f")->facetResult();
  ASSERT_NE(facetResult, nullptr) << req->toString();
  const auto& bucketIds = std::get<solux::api::ColStr>(facetResult->bucket_ids->kind);
  ASSERT_EQ(2, (int)bucketIds.v.size());
  ASSERT_EQ(2, (int)facetResult->counts.size());
  EXPECT_EQ("alpha", bucketIds.v[0]);
  EXPECT_EQ(2, facetResult->counts[0]);
  EXPECT_EQ("beta", bucketIds.v[1]);
  EXPECT_EQ(1, facetResult->counts[1]);
  EXPECT_EQ(2, facetResult->missing.value_or(0));
}

TEST_F(FacetTest, fullTextFacetNestedSparseArrayDomain) {
  CollectionHelper helper;

  for (int i = 0; i < 100; i++) {
    std::string id = std::to_string(i);
    if (i == 3) {
      helper.index(flatdoc("id", id, "pick_w", "yes", "body_w", "apple red"), UpdateMessage::NO_COMMIT);
    } else if (i == 17) {
      helper.index(flatdoc("id", id, "pick_w", "yes", "body_w", "red cherry"), UpdateMessage::NO_COMMIT);
    } else if (i == 88) {
      helper.index(flatdoc("id", id, "pick_w", "yes", "body_w", "blue apple"), UpdateMessage::NO_COMMIT);
    } else {
      helper.index(flatdoc("id", id, "body_w", "noise filler"), UpdateMessage::NO_COMMIT);
    }
  }
  helper.commit();

  auto req = localReq(soluxNode->getSearchEngine());
  req->collection("main");

  auto& topDocs = req->topDocs();
  topDocs.getNumber(true).matchQuery("pick_w", "yes");

  topDocs.facet("f", "body_w").limit(-1);

  req->execute(true);

  ASSERT_OK(req);
  const auto* docs = req->docList("q");
  ASSERT_NE(docs, nullptr) << req->toString();
  ASSERT_EQ(3, docs->found.value_or(0));
  ASSERT_TRUE(docs->ops.contains("f")) << req->toString();
  const auto* facetResult = docs->ops.at("f")->facetResult();
  ASSERT_NE(facetResult, nullptr) << req->toString();
  const auto& bucketIds = std::get<solux::api::ColStr>(facetResult->bucket_ids->kind);
  ASSERT_EQ(4, (int)bucketIds.v.size());
  ASSERT_EQ(4, (int)facetResult->counts.size());
  EXPECT_EQ("apple", bucketIds.v[0]);
  EXPECT_EQ(2, facetResult->counts[0]);
  EXPECT_EQ("red", bucketIds.v[1]);
  EXPECT_EQ(2, facetResult->counts[1]);
  EXPECT_EQ("blue", bucketIds.v[2]);
  EXPECT_EQ(1, facetResult->counts[2]);
  EXPECT_EQ("cherry", bucketIds.v[3]);
  EXPECT_EQ(1, facetResult->counts[3]);
}

TEST_F(FacetTest, vectorOptimization) {
  CollectionHelper helper;
  
  // Add documents with a small range of values to trigger vector optimization
  for (int i = 0; i < 100; i++) {
    helper.index(flatdoc("id", std::to_string(i), "score_i", i % 20), UpdateMessage::NO_COMMIT);
  }
  helper.commit();
  
  // Create search request with faceting
  auto req = localReq(soluxNode->getSearchEngine());
  req->collection("main");

  req->topDocs().getNumber(true).allQuery();
  req->facet("f", "score_i").limit(30);

  req->execute(true);

  ASSERT_EQ(1u, req->responses.size());
  const auto* facetResult = req->responses[0]->proto.ops.at("f")->facetResult();
  ASSERT_NE(facetResult, nullptr);
  const auto& bucketIds = std::get<solux::api::ColInt>(facetResult->bucket_ids->kind);

  // Should have 20 unique values (0-19)
  ASSERT_EQ(20, (int)bucketIds.v.size());
  ASSERT_EQ(20, (int)facetResult->counts.size());

  // Each value should have count of 5 (100 docs / 20 values)
  for (int i = 0; i < 20; i++) {
    EXPECT_EQ(i, bucketIds.v[i]);
    EXPECT_EQ(5, facetResult->counts[i]);
  }
}

// Facet sorted by an inline sub-op (avg) with a finite limit.  This exercises
// FacetReq::init()'s "!sorts.empty()" branch, which moves the sort-field sub-op
// into inlineSubOps.  The existing SearchEngineTest coverage uses limit==-1,
// which inlines via a different branch and so masks regressions in this one.
TEST_F(FacetTest, sortBySubOp) {
  CollectionHelper helper;
  // 2 segments.  Per-category foo_i avg differs from per-category count so that
  // an avg-ascending sort produces a different bucket order than count-desc.
  //   a: count 3, avg 100
  //   b: count 1, avg 1
  //   c: count 2, avg 50
  helper.index(flatdoc("cat_s", "a", "foo_i", 100), UpdateMessage::NO_COMMIT);
  helper.index(flatdoc("cat_s", "b", "foo_i", 1), UpdateMessage::NO_COMMIT);
  helper.index(flatdoc("cat_s", "c", "foo_i", 50), UpdateMessage::COMMIT);
  helper.index(flatdoc("cat_s", "a", "foo_i", 100), UpdateMessage::NO_COMMIT);
  helper.index(flatdoc("cat_s", "a", "foo_i", 100), UpdateMessage::NO_COMMIT);
  helper.index(flatdoc("cat_s", "c", "foo_i", 50), UpdateMessage::COMMIT);

  // Builds a facet on cat_s with an inline avg(foo_i) sub-op, sorted by that
  // sub-op, and returns the request handle (kept alive by the caller).
  auto runFacet = [&](int64_t limit, qb::SortDir dir) {
    auto req = localReq(soluxNode->getSearchEngine());
    req->collection("main");

    req->topDocs().getNumber(true).allQuery();

    auto& facet = req->facet("f", "cat_s");
    facet.limit(limit);
    facet.avg("avgsub", "foo_i");
    qb::sort(facet, "avgsub", dir);

    req->execute(true);
    return req;
  };

  // Ascending avg, all buckets.  Order is b(1), c(50), a(100) -- which differs
  // from count-desc (a, c, b), so this confirms we sorted by the sub-op.
  {
    auto req = runFacet(10, qb::ASC);
    const auto* facet = req->responses[0]->proto.ops.at("f")->facetResult();
    ASSERT_NE(facet, nullptr);
    const auto& bucketIds = std::get<solux::api::ColStr>(facet->bucket_ids->kind);
    ASSERT_EQ(3, (int)bucketIds.v.size());
    EXPECT_EQ("b", bucketIds.v[0]);
    EXPECT_EQ("c", bucketIds.v[1]);
    EXPECT_EQ("a", bucketIds.v[2]);
    EXPECT_EQ(1, facet->counts[0]);
    EXPECT_EQ(2, facet->counts[1]);
    EXPECT_EQ(3, facet->counts[2]);
    const auto& avg = std::get<solux::api::ArrDouble>(facet->ops.at("avgsub")->kind);
    ASSERT_EQ(3, (int)avg.v.size());
    EXPECT_EQ(1, avg.v[0]);
    EXPECT_EQ(50, avg.v[1]);
    EXPECT_EQ(100, avg.v[2]);
  }

  // Descending avg with a finite limit smaller than the bucket count: keep the
  // top 2 by avg -> a(100), c(50).
  {
    auto req = runFacet(2, qb::DESC);
    const auto* facet = req->responses[0]->proto.ops.at("f")->facetResult();
    ASSERT_NE(facet, nullptr);
    const auto& bucketIds = std::get<solux::api::ColStr>(facet->bucket_ids->kind);
    ASSERT_EQ(2, (int)bucketIds.v.size());
    EXPECT_EQ("a", bucketIds.v[0]);
    EXPECT_EQ("c", bucketIds.v[1]);
    const auto& avg = std::get<solux::api::ArrDouble>(facet->ops.at("avgsub")->kind);
    ASSERT_EQ(2, (int)avg.v.size());
    EXPECT_EQ(100, avg.v[0]);
    EXPECT_EQ(50, avg.v[1]);
  }
}

// min/max sub-ops on a string facet across 2 segments, including a bucket
// whose docs carry no value for the stats field (reports NaN).  Runs both the
// deferred per-bucket path (finite limit, no sort) and the inline path
// (limit -1); results must agree.
TEST_F(FacetTest, minMaxSubOps) {
  CollectionHelper helper;
  helper.clear();
  // 2 segments; bucket 'a' spans both so min/max must merge across segments.
  //   a: {10, 30, -5} -> min -5, max 30
  //   b: {7, 7}       -> min 7, max 7
  //   c: {42}         -> min 42, max 42 (second segment only)
  //   d: no foo_i     -> NaN
  helper.index(flatdoc("cat_s", "a", "foo_i", 10), UpdateMessage::NO_COMMIT);
  helper.index(flatdoc("cat_s", "a", "foo_i", 30), UpdateMessage::NO_COMMIT);
  helper.index(flatdoc("cat_s", "b", "foo_i", 7), UpdateMessage::NO_COMMIT);
  helper.index(flatdoc("cat_s", "d", "other_i", 1), UpdateMessage::COMMIT);
  helper.index(flatdoc("cat_s", "a", "foo_i", -5), UpdateMessage::NO_COMMIT);
  helper.index(flatdoc("cat_s", "b", "foo_i", 7), UpdateMessage::NO_COMMIT);
  helper.index(flatdoc("cat_s", "c", "foo_i", 42), UpdateMessage::NO_COMMIT);
  helper.index(flatdoc("cat_s", "d", "other_i", 1), UpdateMessage::COMMIT);

  // Default count-desc sort with bucket-id tiebreak: a(3), b(2), d(2), c(1).
  for (int64_t limit : {10, -1}) {  // 10 = deferred sub-op path, -1 = inline path
    auto req = localReq(soluxNode->getSearchEngine());
    req->collection("main");
    req->topDocs().getNumber(true).allQuery();
    auto& facet = req->facet("f", "cat_s");
    facet.limit(limit);
    facet.min("mn", "foo_i");
    facet.max("mx", "foo_i");
    req->execute(true);

    ASSERT_EQ(1u, req->responses.size()) << req->toString();
    const auto* facetResult = req->responses[0]->proto.ops.at("f")->facetResult();
    ASSERT_NE(facetResult, nullptr);
    const auto& bucketIds = std::get<solux::api::ColStr>(facetResult->bucket_ids->kind);
    ASSERT_EQ(4, (int)bucketIds.v.size()) << "limit=" << limit;
    EXPECT_EQ("a", bucketIds.v[0]);
    EXPECT_EQ("b", bucketIds.v[1]);
    EXPECT_EQ("d", bucketIds.v[2]);
    EXPECT_EQ("c", bucketIds.v[3]);
    const auto& mn = std::get<solux::api::ArrDouble>(facetResult->ops.at("mn")->kind);
    const auto& mx = std::get<solux::api::ArrDouble>(facetResult->ops.at("mx")->kind);
    ASSERT_EQ(4, (int)mn.v.size()) << "limit=" << limit;
    ASSERT_EQ(4, (int)mx.v.size()) << "limit=" << limit;
    EXPECT_EQ(-5, mn.v[0]);
    EXPECT_EQ(30, mx.v[0]);
    EXPECT_EQ(7, mn.v[1]);
    EXPECT_EQ(7, mx.v[1]);
    EXPECT_TRUE(std::isnan(mn.v[2])) << "limit=" << limit;
    EXPECT_TRUE(std::isnan(mx.v[2])) << "limit=" << limit;
    EXPECT_EQ(42, mn.v[3]);
    EXPECT_EQ(42, mx.v[3]);
  }
}

// Facet sorted by a min sub-op, with a bucket that has no values for the stats
// field: empty buckets order below every non-empty bucket (first in ASC, last
// in DESC) so NaN never reaches the sort comparator.
TEST_F(FacetTest, sortByMinSubOpWithEmptyBucket) {
  CollectionHelper helper;
  helper.clear();
  //   a: min 100 (count 2)
  //   b: min 1   (count 1)
  //   c: min 50  (count 2)
  //   d: no foo_i (count 2) -> empty
  helper.index(flatdoc("cat_s", "a", "foo_i", 100), UpdateMessage::NO_COMMIT);
  helper.index(flatdoc("cat_s", "b", "foo_i", 1), UpdateMessage::NO_COMMIT);
  helper.index(flatdoc("cat_s", "c", "foo_i", 75), UpdateMessage::COMMIT);
  helper.index(flatdoc("cat_s", "a", "foo_i", 200), UpdateMessage::NO_COMMIT);
  helper.index(flatdoc("cat_s", "c", "foo_i", 50), UpdateMessage::NO_COMMIT);
  helper.index(flatdoc("cat_s", "d", "other_i", 1), UpdateMessage::NO_COMMIT);
  helper.index(flatdoc("cat_s", "d", "other_i", 1), UpdateMessage::COMMIT);

  auto runFacet = [&](int64_t limit, qb::SortDir dir) {
    auto req = localReq(soluxNode->getSearchEngine());
    req->collection("main");
    req->topDocs().getNumber(true).allQuery();
    auto& facet = req->facet("f", "cat_s");
    facet.limit(limit);
    facet.min("mn", "foo_i");
    qb::sort(facet, "mn", dir);
    req->execute(true);
    return req;
  };

  // Ascending: the empty bucket first, then b(1), c(50), a(100).
  {
    auto req = runFacet(10, qb::ASC);
    const auto* facet = req->responses[0]->proto.ops.at("f")->facetResult();
    ASSERT_NE(facet, nullptr);
    const auto& bucketIds = std::get<solux::api::ColStr>(facet->bucket_ids->kind);
    ASSERT_EQ(4, (int)bucketIds.v.size());
    EXPECT_EQ("d", bucketIds.v[0]);
    EXPECT_EQ("b", bucketIds.v[1]);
    EXPECT_EQ("c", bucketIds.v[2]);
    EXPECT_EQ("a", bucketIds.v[3]);
    const auto& mn = std::get<solux::api::ArrDouble>(facet->ops.at("mn")->kind);
    ASSERT_EQ(4, (int)mn.v.size());
    EXPECT_TRUE(std::isnan(mn.v[0]));
    EXPECT_EQ(1, mn.v[1]);
    EXPECT_EQ(50, mn.v[2]);
    EXPECT_EQ(100, mn.v[3]);
  }

  // Descending with a limit that cuts the (last-sorted) empty bucket off.
  {
    auto req = runFacet(3, qb::DESC);
    const auto* facet = req->responses[0]->proto.ops.at("f")->facetResult();
    ASSERT_NE(facet, nullptr);
    const auto& bucketIds = std::get<solux::api::ColStr>(facet->bucket_ids->kind);
    ASSERT_EQ(3, (int)bucketIds.v.size());
    EXPECT_EQ("a", bucketIds.v[0]);
    EXPECT_EQ("c", bucketIds.v[1]);
    EXPECT_EQ("b", bucketIds.v[2]);
    const auto& mn = std::get<solux::api::ArrDouble>(facet->ops.at("mn")->kind);
    ASSERT_EQ(3, (int)mn.v.size());
    EXPECT_EQ(100, mn.v[0]);
    EXPECT_EQ(50, mn.v[1]);
    EXPECT_EQ(1, mn.v[2]);
  }
}

TEST_F(FacetTest, limitMinusOneInlinesMultipleAvgSubOps) {
  CollectionHelper helper;

  const int totalDocs = 6000;
  auto categoryName = [](int i) {
    return "cat" + std::to_string(100000 + i);
  };

  std::vector<Doc> firstSegment;
  std::vector<Doc> secondSegment;
  firstSegment.reserve(totalDocs / 2);
  secondSegment.reserve(totalDocs / 2);
  for (int i = 0; i < totalDocs; i++) {
    auto doc = flatdoc("cat_s", categoryName(i),
                       "score_i", (int64_t)i,
                       "bonus_i", (int64_t)(totalDocs - i));
    if (i < totalDocs / 2) {
      firstSegment.push_back(std::move(doc));
    } else {
      secondSegment.push_back(std::move(doc));
    }
  }
  helper.indexAll(firstSegment, UpdateMessage::COMMIT);
  helper.indexAll(secondSegment, UpdateMessage::COMMIT);

  auto req = localReq(soluxNode->getSearchEngine());
  req->collection("main");

  req->topDocs().getNumber(true).allQuery();

  auto& facet = req->facet("f", "cat_s");
  facet.limit(-1);
  facet.avg("avg_score", "score_i");
  facet.avg("avg_bonus", "bonus_i");

  req->execute(true);

  ASSERT_OK(req);
  const auto* facetResult = req->responses[0]->proto.ops.at("f")->facetResult();
  ASSERT_NE(facetResult, nullptr) << req->toString();
  const auto& bucketIds = std::get<solux::api::ColStr>(facetResult->bucket_ids->kind);
  ASSERT_EQ(totalDocs, (int)bucketIds.v.size());
  ASSERT_EQ(totalDocs, (int)facetResult->counts.size());
  const auto& avgScoreResult = std::get<solux::api::ArrDouble>(facetResult->ops.at("avg_score")->kind);
  const auto& avgBonusResult = std::get<solux::api::ArrDouble>(facetResult->ops.at("avg_bonus")->kind);
  ASSERT_EQ(totalDocs, (int)avgScoreResult.v.size());
  ASSERT_EQ(totalDocs, (int)avgBonusResult.v.size());

  std::vector<int> checkIndexes = {0, 10, 11, 2999, 3000, totalDocs - 1};
  for (int idx : checkIndexes) {
    EXPECT_EQ(categoryName(idx), bucketIds.v[idx]);
    EXPECT_EQ(1, facetResult->counts[idx]);
    EXPECT_DOUBLE_EQ((double)idx, avgScoreResult.v[idx]);
    EXPECT_DOUBLE_EQ((double)(totalDocs - idx), avgBonusResult.v[idx]);
  }
}

TEST_F(FacetTest, unsupportedFacetOptionsRejected) {
  CollectionHelper helper;
  helper.index(flatdoc("cat_s", "a", "foo_i", 1, "body_w", "alpha"), UpdateMessage::NO_COMMIT);
  helper.index(flatdoc("cat_s", "b", "foo_i", 2, "body_w", "beta"), UpdateMessage::COMMIT);

  struct Case {
    std::string name;
    std::function<void(LocalReq&)> configure;
    std::string expectSubstr; // a phrase the clear error message must contain
  };

  std::vector<Case> cases = {
    {"int_subop", [](LocalReq& req) {
      req.facet("f", "foo_i").avg("avg", "foo_i");
    }, "not yet supported for int field facets"},
    {"int_sort", [](LocalReq& req) {
      auto& facet = req.facet("f", "foo_i");
      qb::sort(facet, "avg", qb::ASC);
    }, "not yet supported for int field facets"},
    {"int_mincount_zero", [](LocalReq& req) {
      req.facet("f", "foo_i").mincount(0);
    }, "not supported for int field facets"},
    {"text_subop", [](LocalReq& req) {
      req.facet("f", "body_w").avg("avg", "foo_i");
    }, "not yet supported for text field facets"},
    {"range_sort", [](LocalReq& req) {
      auto& facet = req.rangeFacet("f", "foo_i");
      facet.range(0, 10, 1);
      qb::sort(facet, "avg", qb::ASC);
    }, "not yet supported for range facets"},
    {"range_mincount_negative", [](LocalReq& req) {
      req.rangeFacet("f", "foo_i").range(0, 10, 1).mincount(-1);
    }, "mincount must be >= 0"},
    {"string_unknown_sort", [](LocalReq& req) {
      auto& facet = req.facet("f", "cat_s");
      qb::sort(facet, "not_a_subop", qb::ASC);
    }, "unknown sort field"},
    {"string_two_sorts", [](LocalReq& req) {
      auto& facet = req.facet("f", "cat_s");
      qb::sort(facet, "first", qb::ASC);
      qb::sort(facet, "second", qb::DESC);
    }, "multiple sort fields"}
  };

  for (const auto& testCase : cases) {
    auto req = localReq(soluxNode->getSearchEngine());
    req->collection("main");
    testCase.configure(*req);

    req->execute(true);

    ASSERT_EQ(1u, req->responses.size()) << testCase.name;
    const std::string error(req->responses[0]->proto.error);
    EXPECT_TRUE(hasError(req->responses[0]->proto)) << testCase.name << "\n" << req->toString();
    // Lock in the clear-message contract: the facet name and the specific
    // unsupported-option phrase must both appear.
    EXPECT_NE(error.find("'f'"), std::string::npos) << testCase.name << ": '" << error << "'";
    EXPECT_NE(error.find(testCase.expectSubstr), std::string::npos) << testCase.name << ": '" << error << "'";
  }
}

// Finding 1: a prepare-requiring query (force_prepare) on an empty index must
// still emit its nested ops. Pre-fix, doPrepareDomain's segnum<0 branch only
// called doneCollecting() and dropped the nested facet/avg.
TEST_F(FacetTest, emptyIndexForcePrepareNestedOps) {
  CollectionHelper helper;

  auto req = localReq(soluxNode->getSearchEngine());
  req->testForcePrepare = true;  // wraps the root: force_prepare(all)
  req->collection("main");

  auto& topDocs = req->topDocs();
  topDocs.getNumber(true);
  topDocs.rawQuery() = qb::all();

  topDocs.facet("f", "category_s").limit(10);

  topDocs.avg("a", "price_i");

  req->execute(true);

  ASSERT_OK(req);
  const auto* docs = req->docList("q");
  ASSERT_NE(docs, nullptr) << req->toString();
  ASSERT_EQ(0, docs->found.value_or(0));
  // Both nested calculator families must still emit on an empty prepared index.
  ASSERT_TRUE(docs->ops.contains("f")) << req->toString();
  const auto* fFacet = docs->ops.at("f")->facetResult();
  ASSERT_NE(fFacet, nullptr) << req->toString();
  EXPECT_EQ(0, (int)std::get<solux::api::ColStr>(fFacet->bucket_ids->kind).v.size());
  ASSERT_TRUE(docs->ops.contains("a")) << req->toString();
  EXPECT_TRUE(std::isnan(std::get<double>(docs->ops.at("a")->kind)));
}

TEST_F(FacetTest, stringFacetMincountZeroShowsAllValues) {
  CollectionHelper helper;
  const int aDocs = 700;
  std::vector<Doc> docs;
  for (int i = 0; i < aDocs; i++) {
    docs.push_back(flatdoc("id", std::to_string(i), "cat_s", "a", "sel_s", "yes"));
  }
  for (int i = 0; i < 5; i++) {
    docs.push_back(flatdoc("id", std::to_string(1000 + i), "cat_s", "b", "sel_s", "no"));
  }
  helper.indexAll(docs, UpdateMessage::COMMIT);

  auto req = localReq(soluxNode->getSearchEngine());
  req->collection("main");

  auto& topDocs = req->topDocs();
  topDocs.getNumber(true).matchQuery("sel_s", "yes");

  topDocs.facet("f", "cat_s").limit(-1).mincount(0);

  req->execute(true);

  ASSERT_OK(req);
  const auto* qDocs = req->docList("q");
  ASSERT_NE(qDocs, nullptr) << req->toString();
  const auto* facetResult = qDocs->ops.at("f")->facetResult();
  ASSERT_NE(facetResult, nullptr) << req->toString();
  const auto& bucketIds = std::get<solux::api::ColStr>(facetResult->bucket_ids->kind);
  ASSERT_EQ(2, (int)bucketIds.v.size()) << req->toString();
  ASSERT_EQ(2, (int)facetResult->counts.size());
  EXPECT_EQ("a", bucketIds.v[0]);
  EXPECT_EQ(aDocs, facetResult->counts[0]);
  EXPECT_EQ("b", bucketIds.v[1]);
  EXPECT_EQ(0, facetResult->counts[1]);
}

TEST_F(FacetTest, stringFacetMincountZeroPadsZerosByValue) {
  CollectionHelper helper;
  helper.index(flatdoc("id", "1", "cat_s", "x", "sel_s", "yes"), UpdateMessage::NO_COMMIT);
  helper.index(flatdoc("id", "2", "cat_s", "x", "sel_s", "yes"), UpdateMessage::NO_COMMIT);
  helper.index(flatdoc("id", "3", "cat_s", "y", "sel_s", "no"), UpdateMessage::NO_COMMIT);
  helper.index(flatdoc("id", "4", "cat_s", "z", "sel_s", "no"), UpdateMessage::COMMIT);

  auto req = localReq(soluxNode->getSearchEngine());
  req->collection("main");

  auto& topDocs = req->topDocs();
  topDocs.getNumber(true).matchQuery("sel_s", "yes");

  topDocs.facet("f", "cat_s").limit(-1).mincount(0);

  req->execute(true);

  ASSERT_OK(req);
  const auto* docs = req->docList("q");
  ASSERT_NE(docs, nullptr) << req->toString();
  const auto* facetResult = docs->ops.at("f")->facetResult();
  ASSERT_NE(facetResult, nullptr) << req->toString();
  const auto& bucketIds = std::get<solux::api::ColStr>(facetResult->bucket_ids->kind);
  ASSERT_EQ(3, (int)bucketIds.v.size()) << req->toString();
  ASSERT_EQ(3, (int)facetResult->counts.size());
  EXPECT_EQ("x", bucketIds.v[0]);
  EXPECT_EQ(2, facetResult->counts[0]);
  EXPECT_EQ("y", bucketIds.v[1]);
  EXPECT_EQ(0, facetResult->counts[1]);
  EXPECT_EQ("z", bucketIds.v[2]);
  EXPECT_EQ(0, facetResult->counts[2]);
}

TEST_F(FacetTest, stringFacetMincountZeroPadsToFiniteLimit) {
  CollectionHelper helper;
  helper.index(flatdoc("id", "1", "cat_s", "a", "sel_s", "yes"), UpdateMessage::NO_COMMIT);
  helper.index(flatdoc("id", "2", "cat_s", "a", "sel_s", "yes"), UpdateMessage::NO_COMMIT);
  helper.index(flatdoc("id", "3", "cat_s", "b", "sel_s", "yes"), UpdateMessage::NO_COMMIT);
  helper.index(flatdoc("id", "4", "cat_s", "c", "sel_s", "no"), UpdateMessage::NO_COMMIT);
  helper.index(flatdoc("id", "5", "cat_s", "d", "sel_s", "no"), UpdateMessage::NO_COMMIT);
  helper.index(flatdoc("id", "6", "cat_s", "e", "sel_s", "no"), UpdateMessage::COMMIT);

  auto req = localReq(soluxNode->getSearchEngine());
  req->collection("main");

  auto& topDocs = req->topDocs();
  topDocs.getNumber(true).matchQuery("sel_s", "yes");

  topDocs.facet("f", "cat_s").limit(4).mincount(0);

  req->execute(true);

  ASSERT_OK(req);
  const auto* docs = req->docList("q");
  ASSERT_NE(docs, nullptr) << req->toString();
  const auto* facetResult = docs->ops.at("f")->facetResult();
  ASSERT_NE(facetResult, nullptr) << req->toString();
  const auto& bucketIds = std::get<solux::api::ColStr>(facetResult->bucket_ids->kind);
  ASSERT_EQ(4, (int)bucketIds.v.size()) << req->toString();
  ASSERT_EQ(4, (int)facetResult->counts.size());
  EXPECT_EQ("a", bucketIds.v[0]);
  EXPECT_EQ(2, facetResult->counts[0]);
  EXPECT_EQ("b", bucketIds.v[1]);
  EXPECT_EQ(1, facetResult->counts[1]);
  EXPECT_EQ("c", bucketIds.v[2]);
  EXPECT_EQ(0, facetResult->counts[2]);
  EXPECT_EQ("d", bucketIds.v[3]);
  EXPECT_EQ(0, facetResult->counts[3]);
}

TEST_F(FacetTest, fullTextFacetMincountZeroShowsOutOfDomainTerms) {
  CollectionHelper helper;
  helper.index(flatdoc("id", "1", "sel_s", "yes", "body_w", "alpha beta"), UpdateMessage::NO_COMMIT);
  helper.index(flatdoc("id", "2", "sel_s", "yes", "body_w", "alpha"), UpdateMessage::NO_COMMIT);
  helper.index(flatdoc("id", "3", "sel_s", "no", "body_w", "gamma"), UpdateMessage::NO_COMMIT);
  helper.index(flatdoc("id", "4", "sel_s", "no", "body_w", "delta"), UpdateMessage::COMMIT);

  auto req = localReq(soluxNode->getSearchEngine());
  req->collection("main");

  auto& topDocs = req->topDocs();
  topDocs.getNumber(true).matchQuery("sel_s", "yes");

  topDocs.facet("f", "body_w").limit(-1).mincount(0);

  req->execute(true);

  ASSERT_OK(req);
  const auto* docs = req->docList("q");
  ASSERT_NE(docs, nullptr) << req->toString();
  const auto* facetResult = docs->ops.at("f")->facetResult();
  ASSERT_NE(facetResult, nullptr) << req->toString();
  const auto& bucketIds = std::get<solux::api::ColStr>(facetResult->bucket_ids->kind);
  ASSERT_EQ(4, (int)bucketIds.v.size()) << req->toString();
  ASSERT_EQ(4, (int)facetResult->counts.size());
  EXPECT_EQ("alpha", bucketIds.v[0]);
  EXPECT_EQ(2, facetResult->counts[0]);
  EXPECT_EQ("beta", bucketIds.v[1]);
  EXPECT_EQ(1, facetResult->counts[1]);
  EXPECT_EQ("delta", bucketIds.v[2]);
  EXPECT_EQ(0, facetResult->counts[2]);
  EXPECT_EQ("gamma", bucketIds.v[3]);
  EXPECT_EQ(0, facetResult->counts[3]);
}

// Finding 3: avg op nested under a selective query (sparse ArrDocSet domain).
// Locks in correct sparse-domain handling after dropping the BitDocSet C-cast.
TEST_F(FacetTest, avgNestedSparseArrayDomain) {
  CollectionHelper helper;
  for (int i = 0; i < 100; i++) {
    std::string id = std::to_string(i);
    if (i == 5) {
      helper.index(flatdoc("id", id, "pick_s", "yes", "val_i", 10), UpdateMessage::NO_COMMIT);
    } else if (i == 50) {
      helper.index(flatdoc("id", id, "pick_s", "yes", "val_i", 20), UpdateMessage::NO_COMMIT);
    } else if (i == 95) {
      helper.index(flatdoc("id", id, "pick_s", "yes", "val_i", 30), UpdateMessage::NO_COMMIT);
    } else {
      helper.index(flatdoc("id", id, "val_i", 999), UpdateMessage::NO_COMMIT);
    }
  }
  helper.commit();

  auto req = localReq(soluxNode->getSearchEngine());
  req->collection("main");

  auto& topDocs = req->topDocs();
  topDocs.getNumber(true).matchQuery("pick_s", "yes");

  topDocs.avg("a", "val_i");

  req->execute(true);

  ASSERT_OK(req);
  const auto* docs = req->docList("q");
  ASSERT_NE(docs, nullptr) << req->toString();
  ASSERT_EQ(3, docs->found.value_or(0));
  // avg over the 3 in-domain docs: (10+20+30)/3 = 20
  EXPECT_DOUBLE_EQ(20.0, std::get<double>(docs->ops.at("a")->kind));
}

// Finding 4: full-text facet per-doc missing. A segment that has the field but
// where some in-domain docs lack any token must count those as missing.
// Pre-fix, missing was only counted when the whole segment lacked the field.
TEST_F(FacetTest, fullTextFacetMixedPresenceMissing) {
  CollectionHelper helper;
  // single segment, mixed presence: 2 docs with body_w, 2 without.
  helper.index(flatdoc("id", "1", "body_w", "alpha beta"), UpdateMessage::NO_COMMIT);
  helper.index(flatdoc("id", "2", "body_w", "alpha"), UpdateMessage::NO_COMMIT);
  helper.index(flatdoc("id", "3", "other_s", "x"), UpdateMessage::NO_COMMIT);
  helper.index(flatdoc("id", "4", "other_s", "y"), UpdateMessage::COMMIT);

  auto req = localReq(soluxNode->getSearchEngine());
  req->collection("main");

  auto& topDocs = req->topDocs();
  topDocs.getNumber(true).allQuery();

  auto& facet = topDocs.facet("f", "body_w");
  facet.limit(-1);
  std::get<solux::api::FieldFacet>(facet.rawOp().kind).missing = true;

  req->execute(true);

  ASSERT_OK(req);
  const auto* docs = req->docList("q");
  ASSERT_NE(docs, nullptr) << req->toString();
  const auto* facetResult = docs->ops.at("f")->facetResult();
  ASSERT_NE(facetResult, nullptr) << req->toString();
  const auto& bucketIds = std::get<solux::api::ColStr>(facetResult->bucket_ids->kind);
  ASSERT_EQ(2, (int)bucketIds.v.size());
  EXPECT_EQ("alpha", bucketIds.v[0]);
  EXPECT_EQ(2, facetResult->counts[0]);
  EXPECT_EQ("beta", bucketIds.v[1]);
  EXPECT_EQ(1, facetResult->counts[1]);
  // docs 3 and 4 have no body_w token -> missing = 2 (pre-fix counted 0).
  EXPECT_EQ(2, facetResult->missing.value_or(0));
}

// Full-text facet missing under a selective query: the domain is a sparse
// ArrDocSet, so missing is computed by intersecting the indexed docs-with-value
// set with the domain. Verifies out-of-domain docs that have the field are
// excluded from both the buckets and the have-field count.
TEST_F(FacetTest, fullTextFacetSparseDomainMissing) {
  CollectionHelper helper;
  for (int i = 0; i < 100; i++) {
    std::string id = std::to_string(i);
    if (i == 5) {
      helper.index(flatdoc("id", id, "pick_s", "yes", "body_w", "alpha"), UpdateMessage::NO_COMMIT);
    } else if (i == 50) {
      helper.index(flatdoc("id", id, "pick_s", "yes", "body_w", "beta"), UpdateMessage::NO_COMMIT);
    } else if (i == 95) {
      helper.index(flatdoc("id", id, "pick_s", "yes"), UpdateMessage::NO_COMMIT); // in domain, no body_w
    } else if (i == 10 || i == 20 || i == 30) {
      helper.index(flatdoc("id", id, "body_w", "gamma"), UpdateMessage::NO_COMMIT); // has body_w, not in domain
    } else {
      helper.index(flatdoc("id", id, "other_s", "z"), UpdateMessage::NO_COMMIT);
    }
  }
  helper.commit();

  auto req = localReq(soluxNode->getSearchEngine());
  req->collection("main");

  auto& topDocs = req->topDocs();
  topDocs.getNumber(true).matchQuery("pick_s", "yes");

  auto& facet = topDocs.facet("f", "body_w");
  facet.limit(-1);
  std::get<solux::api::FieldFacet>(facet.rawOp().kind).missing = true;

  req->execute(true);

  ASSERT_OK(req);
  const auto* docs = req->docList("q");
  ASSERT_NE(docs, nullptr) << req->toString();
  ASSERT_EQ(3, docs->found.value_or(0));
  const auto* facetResult = docs->ops.at("f")->facetResult();
  ASSERT_NE(facetResult, nullptr) << req->toString();
  const auto& bucketIds = std::get<solux::api::ColStr>(facetResult->bucket_ids->kind);
  // Only in-domain docs contribute: alpha (doc 5) and beta (doc 50). The gamma
  // docs have body_w but are out of domain, so they appear in neither the
  // buckets nor the have-field count.
  ASSERT_EQ(2, (int)bucketIds.v.size());
  EXPECT_EQ("alpha", bucketIds.v[0]);
  EXPECT_EQ("beta", bucketIds.v[1]);
  EXPECT_EQ(1, facetResult->counts[0]);
  EXPECT_EQ(1, facetResult->counts[1]);
  // domain = {5, 50, 95}; doc 95 has no body_w -> missing = 1.
  EXPECT_EQ(1, facetResult->missing.value_or(0));
}

// Diagnostic: facet avg() sub-op must average only over the query domain, not
// all docs with the bucket value. Two segments; out-of-domain docs carry wildly
// different avgval so a domain leak is obvious.
TEST_F(FacetTest, facetAvgRespectsSelectiveDomain) {
  CollectionHelper helper;
  helper.index(flatdoc("id", "A", "cat_s", "x", "sel_s", "yes", "avgval_i", 10), UpdateMessage::NO_COMMIT);
  helper.index(flatdoc("id", "B", "cat_s", "x", "sel_s", "no",  "avgval_i", 1000), UpdateMessage::NO_COMMIT);
  helper.index(flatdoc("id", "C", "cat_s", "y", "sel_s", "yes", "avgval_i", 20), UpdateMessage::NO_COMMIT);
  helper.index(flatdoc("id", "G", "cat_s", "z", "sel_s", "yes", "avgval_i", 100), UpdateMessage::COMMIT);
  helper.index(flatdoc("id", "D", "cat_s", "x", "sel_s", "yes", "avgval_i", 30), UpdateMessage::NO_COMMIT);
  helper.index(flatdoc("id", "E", "cat_s", "y", "sel_s", "no",  "avgval_i", 2000), UpdateMessage::NO_COMMIT);
  helper.index(flatdoc("id", "F", "cat_s", "y", "sel_s", "yes", "avgval_i", 40), UpdateMessage::NO_COMMIT);
  // z also appears in out-of-domain docs across this segment -> z's in-domain
  // bucket is just {G}; the avg must be 100, not polluted by H/I.
  helper.index(flatdoc("id", "H", "cat_s", "z", "sel_s", "no",  "avgval_i", 200), UpdateMessage::NO_COMMIT);
  helper.index(flatdoc("id", "I", "cat_s", "z", "sel_s", "no",  "avgval_i", 300), UpdateMessage::COMMIT);

  // domain = {A,C,D,F,G}; x={A,D} avg 20; y={C,F} avg 30; z={G} avg 100 (count 1).
  for (int64_t limit : {(int64_t)10, (int64_t)-1}) {
    auto req = localReq(soluxNode->getSearchEngine());
    req->collection("main");
    auto& topDocs = req->topDocs();
    topDocs.getNumber(true).matchQuery("sel_s", "yes");
    auto& facet = topDocs.facet("f", "cat_s");
    facet.limit(limit);
    facet.avg("av", "avgval_i");
    req->execute(true);

    ASSERT_FALSE(hasError(req->responses[0]->proto)) << req->toString();
    const auto* docs = req->docList("q");
    ASSERT_NE(docs, nullptr) << req->toString();
    const auto* f = docs->ops.at("f")->facetResult();
    ASSERT_NE(f, nullptr) << req->toString();
    const auto& bucketIds = std::get<solux::api::ColStr>(f->bucket_ids->kind);
    const auto& avgArr = std::get<solux::api::ArrDouble>(f->ops.at("av")->kind);
    ASSERT_EQ(3, (int)bucketIds.v.size()) << "limit=" << limit << "\n" << req->toString();
    std::map<std::string, double> got;
    for (int i = 0; i < (int)bucketIds.v.size(); i++)
      got[std::string(bucketIds.v[i])] = avgArr.v[i];
    EXPECT_DOUBLE_EQ(20.0, got["x"]) << "limit=" << limit << "\n" << req->toString();
    EXPECT_DOUBLE_EQ(30.0, got["y"]) << "limit=" << limit << "\n" << req->toString();
    EXPECT_DOUBLE_EQ(100.0, got["z"]) << "limit=" << limit << "\n" << req->toString();
  }
}

// A facet sub-op (avg) must not be polluted by a segment that lacks the facet
// field: the bucket has no docs there. Regression for doSubops passing a null
// (== all-docs) domain when the field/value is absent in a segment.
TEST_F(FacetTest, facetAvgFieldAbsentInSegment) {
  CollectionHelper helper;
  // seg1 has cat_s; seg2 has NO cat_s at all (only avgval).
  helper.index(flatdoc("id", "A", "cat_s", "x", "sel_s", "yes", "avgval_i", 10), UpdateMessage::NO_COMMIT);
  helper.index(flatdoc("id", "B", "cat_s", "x", "sel_s", "yes", "avgval_i", 20), UpdateMessage::COMMIT);
  helper.index(flatdoc("id", "C", "sel_s", "yes", "avgval_i", 1000), UpdateMessage::NO_COMMIT);
  helper.index(flatdoc("id", "D", "sel_s", "yes", "avgval_i", 2000), UpdateMessage::COMMIT);

  auto req = localReq(soluxNode->getSearchEngine());
  req->collection("main");
  auto& topDocs = req->topDocs();
  topDocs.getNumber(true).matchQuery("sel_s", "yes");
  auto& facet = topDocs.facet("f", "cat_s");
  facet.limit(10);  // finite -> post-hoc sub-op path
  facet.avg("av", "avgval_i");
  req->execute(true);

  ASSERT_FALSE(hasError(req->responses[0]->proto)) << req->toString();
  const auto* docs = req->docList("q");
  ASSERT_NE(docs, nullptr) << req->toString();
  const auto* f = docs->ops.at("f")->facetResult();
  ASSERT_NE(f, nullptr) << req->toString();
  const auto& bucketIds = std::get<solux::api::ColStr>(f->bucket_ids->kind);
  ASSERT_EQ(1, (int)bucketIds.v.size()) << req->toString();
  EXPECT_EQ("x", bucketIds.v[0]);
  EXPECT_EQ(2, f->counts[0]);
  // bucket x = {A,B}; avg = (10+20)/2 = 15. C,D lack cat_s and must NOT leak in.
  EXPECT_DOUBLE_EQ(15.0, std::get<solux::api::ArrDouble>(f->ops.at("av")->kind).v[0]) << req->toString();
}

TEST_F(FacetTest, bucketPreparedTopDocsRetainsSegmentDomains) {
  CollectionHelper helper;
  helper.indexAll(std::array{
    flatdoc("id", "a", "cat_s", "x"),
    flatdoc("id", "b"),
    flatdoc("id", "c"),
  }, UpdateMessage::COMMIT);
  helper.indexAll(std::array{
    flatdoc("id", "d"),
    flatdoc("id", "e"),
    flatdoc("id", "f", "cat_s", "x"),
  }, UpdateMessage::COMMIT);
  ASSERT_EQ(2u, helper.getIndexWriter()->getIndexReader()->segments().size());

  auto req = localReq(soluxNode->getSearchEngine());
  req->testForcePrepare = true;
  auto& topDocs = req->collection("main").topDocs("q").allQuery().limit(0);
  auto& facet = topDocs.facet("f", "cat_s").limit(10);
  facet.topDocs("bucket_docs").allQuery().fields({"id"}).getNumber().limit(-1);
  req->execute();

  ASSERT_OK(req);
  const auto* outerDocs = req->docList("q");
  ASSERT_NE(nullptr, outerDocs);
  const auto* result = outerDocs->ops.at("f")->facetResult();
  ASSERT_NE(nullptr, result);
  ASSERT_EQ(1u, result->counts.size());
  EXPECT_EQ(2, result->counts[0]);

  const auto* bucketDocs = result->ops.at("bucket_docs")->docList();
  ASSERT_NE(nullptr, bucketDocs);
  EXPECT_EQ(2, bucketDocs->found.value_or(-1));
  const auto& ids = std::get<api::ColStr>(
      bucketDocs->columns.at("id").kind).v;
  ASSERT_EQ(2u, ids.size());
  EXPECT_EQ("a", ids[0]);
  EXPECT_EQ("f", ids[1]);
}

// Nested facet: a string facet under a string facet, returning a per-parent-
// bucket sub-facet (ops[name].arr.v[i].facet parallel to bucket_ids).
TEST_F(FacetTest, nestedStringFacet) {
  CollectionHelper helper;
  helper.index(flatdoc("id", "1", "cat_s", "x", "sub_s", "p"), UpdateMessage::NO_COMMIT);
  helper.index(flatdoc("id", "2", "cat_s", "x", "sub_s", "q"), UpdateMessage::NO_COMMIT);
  helper.index(flatdoc("id", "3", "cat_s", "y", "sub_s", "p"), UpdateMessage::COMMIT);

  auto req = localReq(soluxNode->getSearchEngine());
  req->collection("main");
  auto& topDocs = req->topDocs();
  topDocs.allQuery();
  auto& facet = topDocs.facet("f", "cat_s");
  facet.limit(-1);
  facet.facet("sf", "sub_s").limit(-1);
  req->execute(true);

  ASSERT_FALSE(hasError(req->responses[0]->proto)) << req->toString();
  const auto* docs = req->docList("q");
  ASSERT_NE(docs, nullptr) << req->toString();
  const auto* f = docs->ops.at("f")->facetResult();
  ASSERT_NE(f, nullptr) << req->toString();
  const auto& fIds = std::get<solux::api::ColStr>(f->bucket_ids->kind);
  ASSERT_EQ(2, (int)fIds.v.size()) << req->toString();
  EXPECT_EQ("x", fIds.v[0]);  // x(2), y(1): count desc
  EXPECT_EQ("y", fIds.v[1]);
  const auto& sfArr = std::get<solux::api::ArrVal>(f->ops.at("sf")->kind);
  ASSERT_EQ(2, (int)sfArr.v.size()) << req->toString();  // one sub-facet per parent bucket
  // bucket x = docs {1,2} -> sub_s {p:1, q:1}
  const auto& sfx = std::get<solux::api::FacetResult>(sfArr.v[0].kind);
  const auto& sfxIds = std::get<solux::api::ColStr>(sfx.bucket_ids->kind);
  ASSERT_EQ(2, (int)sfxIds.v.size()) << req->toString();
  EXPECT_EQ("p", sfxIds.v[0]);
  EXPECT_EQ("q", sfxIds.v[1]);
  EXPECT_EQ(1, sfx.counts[0]);
  EXPECT_EQ(1, sfx.counts[1]);
  // bucket y = doc {3} -> sub_s {p:1}
  const auto& sfy = std::get<solux::api::FacetResult>(sfArr.v[1].kind);
  const auto& sfyIds = std::get<solux::api::ColStr>(sfy.bucket_ids->kind);
  ASSERT_EQ(1, (int)sfyIds.v.size()) << req->toString();
  EXPECT_EQ("p", sfyIds.v[0]);
  EXPECT_EQ(1, sfy.counts[0]);
}

//
// Comprehensive random faceting test class
//
class RandomFacetTest : public SoluxTest {
protected:
  static constexpr int MERGE_FACTOR = 10;
  static constexpr int NUM_FIELDS = 8;  // we do a linear search on field names in the document, so keep this small.
  static constexpr int PERCENT_PARA = 80;  // percent of the time we do parallel faceting on a single request

  // Field definitions with various characteristics
  struct FieldDef {
    std::string name;
    bool isInt;
    bool isText = false;  // _w text field (single token; modeled like _s)
    bool multiValued;
    int numUniqueValues;
    int maxValuesPerDoc;
    int sparsityPercent;  // 0 = always missing, 100 = always present
  };
  
  // No need for FacetRequest struct anymore - we read the built request directly.

  // The request is built via the OpCursor fluent API; the model reads it back through the
  // non-owning concrete request view (LocalReq::proto, a solux::api::SearchRequest).
  using ReqOps = solux::test::OpsMap;  // map_view<string_view, indirect_view<SearchOp>>
  using RespOps = solux::api::map_view<std::string_view, ::hpp_proto::indirect_view<solux::api::Val>>;

  // Plain expected-result mirror of the engine's solux::api facet/doclist output. The model
  // computes these from the built request; verify compares the (non-owning, arena-backed)
  // engine output against them. (The api result types are spans into an arena, so a plain
  // owning mirror is simpler than rebuilding owning api structs.)
  struct ExpFacet {
    bool isRange = false;
    bool intBuckets = false;                              // field facet: int vs string buckets
    std::vector<int64_t> intIds;                          // field facet int buckets
    std::vector<std::string> strIds;                      // field facet string buckets
    std::vector<std::pair<int64_t, int64_t>> rangePairs;  // range facet [lo, hi]
    std::vector<int64_t> counts;
    std::optional<int64_t> missing;
    std::map<std::string, std::vector<double>> avgOps;        // avg op name -> per-bucket avg
    std::map<std::string, std::vector<ExpFacet>> subFacetOps; // sub-facet op name -> per-bucket
  };
  struct ExpVal {
    bool isDocList = false;
    ExpFacet facet;                     // when !isDocList
    int64_t matches = 0;                // when isDocList
    std::map<std::string, ExpVal> ops;  // when isDocList (nested ops)
  };

  // Model to track documents and calculate facets dynamically
  class Model {
    struct ValueRange {
      size_t begin = 0;
      size_t end = 0;
    };

    static constexpr size_t BUILTIN_FIELDS = 3;
    static constexpr size_t MODEL_FIELDS = NUM_FIELDS + BUILTIN_FIELDS;
    std::vector<std::array<ValueRange, MODEL_FIELDS>> valueRanges;
    std::array<std::string, MODEL_FIELDS> fieldNames;
    boost::unordered_flat_map<std::string_view, size_t> fieldOrdinals;

    size_t fieldOrdinal(std::string_view name) const {
      auto it = fieldOrdinals.find(name);
      return it == fieldOrdinals.end() ? MODEL_FIELDS : it->second;
    }

    std::span<const NameVal> values(size_t docIdx, size_t ordinal) const {
      if (ordinal >= MODEL_FIELDS) return {};
      const auto& range = valueRanges[docIdx][ordinal];
      const auto& doc = docs[docIdx];
      return std::span<const NameVal>(doc).subspan(range.begin, range.end - range.begin);
    }

    const FieldVal* findOne(size_t docIdx, size_t ordinal) const {
      auto vals = values(docIdx, ordinal);
      return vals.empty() ? nullptr : &vals.front().val;
    }

  public:
    std::vector<Doc> docs;
    std::vector<char> deleted;  // parallel to docs; deleted docs are not live

    explicit Model(const std::vector<FieldDef>& fields) {
      fieldNames[0] = "id";
      fieldNames[1] = "avgval_i";
      fieldNames[2] = "avgval2_i";
      for (size_t i = 0; i < fields.size(); i++) {
        fieldNames[i + BUILTIN_FIELDS] = fields[i].name;
      }
      fieldOrdinals.reserve(MODEL_FIELDS);
      for (size_t i = 0; i < fieldNames.size(); i++) {
        fieldOrdinals.emplace(fieldNames[i], i);
      }
    }

    void addDoc(Doc doc) {
      docs.push_back(std::move(doc));
      deleted.push_back(0);
      auto& ranges = valueRanges.emplace_back();
      const auto& stored = docs.back();
      for (size_t i = 0; i < stored.size(); i++) {
        size_t ordinal = fieldOrdinal(stored[i].name);
        ASSERT_LT(ordinal, ranges.size());
        auto& range = ranges[ordinal];
        if (range.begin == range.end) {
          range.begin = i;
        } else {
          ASSERT_EQ(range.end, i) << "model field values must be contiguous";
        }
        range.end = i + 1;
      }
    }

    void markDeleted(size_t i) { deleted[i] = 1; }
    bool isDeleted(size_t i) const { return i < deleted.size() && deleted[i]; }
    const FieldVal* findOne(size_t docIdx, std::string_view name) const {
      return findOne(docIdx, fieldOrdinal(name));
    }

    std::vector<size_t> allDocIndexes() const {
      std::vector<size_t> out;
      out.reserve(docs.size());
      for (size_t i = 0; i < docs.size(); i++) {
        if (!isDeleted(i)) out.push_back(i);  // root domain = live docs
      }
      return out;
    }

    std::vector<size_t> matchingDocIndexes(const solux::api::Query& query) const {
      std::vector<size_t> out;
      out.reserve(docs.size());
      if (std::holds_alternative<bool>(query.kind) && std::get<bool>(query.kind)) {
        return allDocIndexes();
      }
      if (!std::holds_alternative<solux::api::Match>(query.kind)) {
        return allDocIndexes();
      }

      const auto& match = std::get<solux::api::Match>(query.kind);
      const auto& matchVal = *match.val;
      size_t ordinal = fieldOrdinal(match.field);
      for (size_t i = 0; i < docs.size(); i++) {
        if (isDeleted(i)) continue;
        auto vals = values(i, ordinal);
        if (std::holds_alternative<std::string_view>(matchVal.kind)) {
          std::string_view expected = std::get<std::string_view>(matchVal.kind);
          for (const auto& nv : vals) {
            if (auto* strVal = std::get_if<std::string>(&nv.val); strVal && *strVal == expected) {
              out.push_back(i);
              break;
            }
          }
        } else if (std::holds_alternative<int64_t>(matchVal.kind)) {
          int64_t expected = std::get<int64_t>(matchVal.kind);
          for (const auto& nv : vals) {
            if (auto* intVal = std::get_if<int64_t>(&nv.val); intVal && *intVal == expected) {
              out.push_back(i);
              break;
            }
          }
        }
      }
      return out;
    }

    static void findAll(const Doc& doc, std::string_view name, std::vector<const FieldVal*>& out) {
      out.clear();
      for (const auto& nv : doc) {
        if (nv.name == name) {
          out.push_back(&nv.val);
        }
      }
    }

    // Dump model state for debugging
    void dumpModel(const std::string& field, const solux::api::Query& query) const {
      LOG_ERROR("=== Model Dump for field '{}' ===", field);
      LOG_ERROR("Total docs: {}", docs.size());
      
      // Count matching docs and field values
      boost::unordered_flat_map<std::string, int> valueCounts;
      boost::unordered_flat_map<int64_t, int> intValueCounts;
      int matchingDocs = 0;
      int docsWithField = 0;
      int missingField = 0;
      std::vector<const FieldVal*> vals;
      for (const auto& doc : docs) {
        bool matches = matchesQuery(doc, query);
        if (!matches) continue;
      
        matchingDocs++;
        findAll(doc, field, vals);
        if (!vals.empty()) {
          docsWithField++;
          for (auto* val : vals) {
            if (auto* strVal = std::get_if<std::string>(val)) {
              valueCounts[*strVal]++;
            } else if (auto* intVal = std::get_if<int64_t>(val)) {
              intValueCounts[*intVal]++;
            }
          }
        } else {
          missingField++;
        }
      }
      
      std::string queryStr;
      if (std::holds_alternative<solux::api::Match>(query.kind)) {
        const auto& qm = std::get<solux::api::Match>(query.kind);
        const auto& qv = *qm.val;
        queryStr = "match(" + std::string(qm.field) + "=" +
          (std::holds_alternative<std::string_view>(qv.kind) ? std::string(std::get<std::string_view>(qv.kind)) :
           std::to_string(std::get<int64_t>(qv.kind))) + ")";
      } else {
        queryStr = "all";
      }
      LOG_ERROR("Query: {}", queryStr);
      LOG_ERROR("Matching docs: {}", matchingDocs);
      LOG_ERROR("Docs with field '{}': {}", field, docsWithField);
      LOG_ERROR("Missing field '{}': {}", field, missingField);
      
      if (!valueCounts.empty()) {
        LOG_ERROR("String value distribution:");
        std::vector<std::pair<std::string, int>> sortedValues(valueCounts.begin(), valueCounts.end());
        std::sort(sortedValues.begin(), sortedValues.end());
        for (const auto& [val, count] : sortedValues) {
          LOG_ERROR("  '{}': {}", val, count);
        }
      }
      
      if (!intValueCounts.empty()) {
        LOG_ERROR("Int value distribution:");
        std::vector<std::pair<int64_t, int>> sortedIntValues(intValueCounts.begin(), intValueCounts.end());
        std::sort(sortedIntValues.begin(), sortedIntValues.end());
        for (const auto& [val, count] : sortedIntValues) {
          LOG_ERROR("  {}: {}", val, count);
        }
      }
      
      LOG_ERROR("=== End Model Dump ===");
    }
    
    // Check if a document matches a query
    bool matchesQuery(const Doc& doc, const solux::api::Query& query) const {
      if (std::holds_alternative<bool>(query.kind) && std::get<bool>(query.kind)) {
        return true;
      }

      if (std::holds_alternative<solux::api::Match>(query.kind)) {
        const auto& match = std::get<solux::api::Match>(query.kind);
        const auto& matchVal = *match.val;
        std::vector<const FieldVal*> vals;
        findAll(doc, match.field, vals);
        if (vals.empty()) return false;
        if (std::holds_alternative<std::string_view>(matchVal.kind)) {
          for (auto* val : vals) {
            if (auto* strVal = std::get_if<std::string>(val)) {
              if (*strVal == std::get<std::string_view>(matchVal.kind)) return true;
            }
          }
        } else if (std::holds_alternative<int64_t>(matchVal.kind)) {
          for (auto* val : vals) {
            if (auto* intVal = std::get_if<int64_t>(val)) {
              if (*intVal == std::get<int64_t>(matchVal.kind)) return true;
            }
          }
        }
        return false;
      }

      // TODO: Add support for range, boolean queries
      return true;  // Default to matching for unsupported query types
    }
    // Apply sorting and limits to facet results
    template<typename T>
    std::vector<std::pair<T, int64_t>> sortAndLimitFacets(
        const boost::unordered_flat_map<T, int64_t>& counts,
        int limit,
        int64_t minCount) const {
      
      std::vector<std::pair<T, int64_t>> result;
      for (const auto& [val, count] : counts) {
        if (count >= minCount) {
          result.push_back({val, count});
        }
      }
      
      // Sort by count desc, then value asc
      std::sort(result.begin(), result.end(), [](const auto& a, const auto& b) {
        if (a.second != b.second) return a.second > b.second;
        return a.first < b.first;
      });
      
      if (limit >= 0 && result.size() > static_cast<size_t>(limit)) {
        result.resize(limit);
      }
      
      return result;
    }
    
    // Process all operations in an ops map recursively
    void processOps(const ReqOps& requestOps, std::map<std::string, ExpVal>& responseOps) const {

      auto allDocs = allDocIndexes();
      for (const auto& [opName, searchOpPtr] : requestOps) {
        const auto& searchOp = *searchOpPtr;
        if (std::holds_alternative<solux::api::FieldFacet>(searchOp.kind)) {
          // Process field facet - facets at root level operate on all documents
          ExpVal v; v.facet = calculateFieldFacet(std::get<solux::api::FieldFacet>(searchOp.kind), allDocs);
          responseOps[std::string(opName)] = std::move(v);
        } else if (std::holds_alternative<solux::api::RangeFacet>(searchOp.kind)) {
          ExpVal v; v.facet = calculateRangeFacet(std::get<solux::api::RangeFacet>(searchOp.kind), allDocs);
          responseOps[std::string(opName)] = std::move(v);
        } else if (std::holds_alternative<solux::api::TopDocs>(searchOp.kind)) {
          // Process top docs query (which may have nested ops)
          const auto& topDocs = std::get<solux::api::TopDocs>(searchOp.kind);

          // The query in top_docs defines the domain for nested ops
          solux::api::Query effectiveQuery;
          if (topDocs.query.has_value()) effectiveQuery = *topDocs.query;
          if (effectiveQuery.kind.index() == 0) {
            effectiveQuery.kind = true;  // Default to all if no query specified
          }

          auto matchingDocs = matchingDocIndexes(effectiveQuery);
          ExpVal v; v.isDocList = true; v.matches = (int64_t)matchingDocs.size();

          // Process any nested operations under this query with the query as their domain
          if (!topDocs.ops.empty()) {
            processOpsWithDomain(topDocs.ops, v.ops, matchingDocs);
          }

          responseOps[std::string(opName)] = std::move(v);
        }
        // Add other operation types as needed
      }
    }

    // Process operations with a specific domain query (for nested ops)
    void processOpsWithDomain(const ReqOps& requestOps, std::map<std::string, ExpVal>& responseOps,
                               const std::vector<size_t>& domainDocs) const {

      for (const auto& [opName, searchOpPtr] : requestOps) {
        const auto& searchOp = *searchOpPtr;
        if (std::holds_alternative<solux::api::FieldFacet>(searchOp.kind)) {
          // Nested facet uses the domain query from its parent
          ExpVal v; v.facet = calculateFieldFacet(std::get<solux::api::FieldFacet>(searchOp.kind), domainDocs);
          responseOps[std::string(opName)] = std::move(v);
        } else if (std::holds_alternative<solux::api::RangeFacet>(searchOp.kind)) {
          ExpVal v; v.facet = calculateRangeFacet(std::get<solux::api::RangeFacet>(searchOp.kind), domainDocs);
          responseOps[std::string(opName)] = std::move(v);
        } else if (std::holds_alternative<solux::api::TopDocs>(searchOp.kind)) {
          // Nested top_docs would combine its query with the domain query
          // This is more complex and would need proper query combination logic
          // For now, just using the nested query
          const auto& topDocs = std::get<solux::api::TopDocs>(searchOp.kind);
          ExpVal v; v.isDocList = true;

          if (topDocs.query.has_value()) {
            auto matchingDocs = matchingDocIndexes(*topDocs.query);
            v.matches = (int64_t)matchingDocs.size();
            if (!topDocs.ops.empty()) {
              processOpsWithDomain(topDocs.ops, v.ops, matchingDocs);
            }
          } else {
            v.matches = (int64_t)domainDocs.size();
            if (!topDocs.ops.empty()) {
              processOpsWithDomain(topDocs.ops, v.ops, domainDocs);
            }
          }

          responseOps[std::string(opName)] = std::move(v);
        }
      }
    }
          
    // Calculate expected facet results for a single field facet
    ExpFacet calculateFieldFacet(const solux::api::FieldFacet& facetOp,
                                 const std::vector<size_t>& domainDocs) const {
      ExpFacet out;

      std::string fieldName(facetOp.field);
      size_t fieldOrd = fieldOrdinal(fieldName);
      int64_t limit = facetOp.limit.value_or(0);
      bool hasMin = facetOp.mincount.has_value();
      int64_t mincount = hasMin ? std::max<int64_t>(*facetOp.mincount, 1) : 1;
      bool includeMissing = facetOp.missing;

      // Determine field type from field name convention
      bool isIntField = fieldName.ends_with("_i") || fieldName.ends_with("_is");
      bool showZeros = !isIntField && hasMin && *facetOp.mincount == 0;

      // avg() sub-ops (string/id parent only; the parser rejects sub-ops on
      // int/range/text). There can be several; at most one is the sort key.
      struct AvgOpDef {
        std::string name;
        size_t fieldOrd;
      };
      std::vector<AvgOpDef> avgOps;
      for (const auto& [opName, subPtr] : facetOp.ops) {
        const auto& sub = *subPtr;
        if (std::holds_alternative<solux::api::GenOp>(sub.kind)) {
          const auto& genOp = std::get<solux::api::GenOp>(sub.kind);
          if (genOp.name == "avg" || genOp.name == "average") {
            avgOps.push_back({
                std::string(opName),
                fieldOrdinal(std::get<std::string_view>(genOp.args[0].kind))
            });
          }
        }
      }
      bool hasAvg = !avgOps.empty();
      bool hasSubFacet = false;
      for (const auto& [opName, subPtr] : facetOp.ops) {
        unused(opName);
        if (std::holds_alternative<solux::api::FieldFacet>(subPtr->kind)) {
          hasSubFacet = true;
          break;
        }
      }
      int sortAvgIdx = -1;  // index into avgOps of the sort key, or -1
      if (!facetOp.sorts.empty()) {
        for (size_t k = 0; k < avgOps.size(); k++)
          if (avgOps[k].name == facetOp.sorts[0].expr) { sortAvgIdx = (int)k; break; }
      }
      bool avgDesc = sortAvgIdx >= 0 && facetOp.sorts[0].dir == solux::api::SortSpec_::SortDir::DESC;

      // Count values for documents matching the domain query
      boost::unordered_flat_map<int64_t, int64_t> intCounts;
      boost::unordered_flat_map<std::string, int64_t> strCounts;
      boost::unordered_flat_map<std::string, std::vector<int64_t>> strAvgSums; // bucket -> sum per avgOp
      boost::unordered_flat_map<std::string, std::vector<size_t>> strBucketDocs;
      int64_t missingCount = 0;

      if (showZeros) {
        for (size_t docIdx = 0; docIdx < docs.size(); docIdx++) {
          for (const auto& nv : values(docIdx, fieldOrd)) {
            if (auto* strVal = std::get_if<std::string>(&nv.val)) {
              strCounts.try_emplace(*strVal, 0);
            }
          }
        }
      }

      for (auto docIdx : domainDocs) {
        boost::container::small_vector<int64_t, 2> avs(avgOps.size(), 0);
        for (size_t k = 0; k < avgOps.size(); k++)
          if (auto* p = findOne(docIdx, avgOps[k].fieldOrd))
            if (auto* iv = std::get_if<int64_t>(p)) avs[k] = *iv;
        bool hasField = false;
        for (const auto& nv : values(docIdx, fieldOrd)) {
          if (isIntField) {
            if (auto* intVal = std::get_if<int64_t>(&nv.val)) {
              intCounts[*intVal]++;
              hasField = true;
            }
          } else {
            if (auto* strVal = std::get_if<std::string>(&nv.val)) {
              strCounts[*strVal]++;
              if (hasSubFacet) {
                auto& bucketDocs = strBucketDocs[*strVal];
                if (bucketDocs.empty() || bucketDocs.back() != docIdx) {
                  bucketDocs.push_back(docIdx);
                }
              }
              if (hasAvg) {
                auto& sums = strAvgSums[*strVal];
                if (sums.empty()) sums.resize(avgOps.size(), 0);
                for (size_t k = 0; k < avgOps.size(); k++) sums[k] += avs[k];
              }
              hasField = true;
            }
          }
        }
        if (!hasField) {
          missingCount++;
        }
      }

      // Apply mincount, sort, and limit, then populate the expected result
      if (isIntField) {
        out.intBuckets = true;
        auto sorted = sortAndLimitFacets(intCounts, limit, mincount);
        for (const auto& [val, count] : sorted) {
          out.intIds.push_back(val);
          out.counts.push_back(count);
        }
      } else if (hasAvg) {
        // avg fields (avgval_i / avgval2_i) are always present, so every bucket
        // has count values per avgOp and avg = sum/count (no empty-bucket
        // 0.0-vs-NaN). One avgOp may be the sort key; the rest are annotations.
        struct B { std::string val; int64_t count; std::vector<int64_t> sums; };
        std::vector<B> buckets;
        for (const auto& [val, count] : strCounts) {
          if (count >= mincount) buckets.push_back({val, count, strAvgSums.at(val)});
        }
        if (sortAvgIdx >= 0) {
          std::sort(buckets.begin(), buckets.end(), [&](const B& a, const B& b) {
            double aa = (double)a.sums[sortAvgIdx] / (double)a.count;
            double ba = (double)b.sums[sortAvgIdx] / (double)b.count;
            if (aa != ba) return avgDesc ? aa > ba : aa < ba;
            return a.val < b.val; // tie-break by bucket value asc (matches facetResult2)
          });
        } else {
          std::sort(buckets.begin(), buckets.end(), [](const B& a, const B& b) {
            if (a.count != b.count) return a.count > b.count;
            return a.val < b.val;
          });
        }
        if (limit >= 0 && (int64_t)buckets.size() > limit) buckets.resize(limit);
        // Only emit sub-op results when there are buckets (the engine's post-hoc
        // path creates no ops entry for an empty facet). One vector per avgOp;
        // the push_back below creates each entry on the first bucket.
        for (const auto& b : buckets) {
          out.strIds.push_back(b.val);
          out.counts.push_back(b.count);
          for (size_t k = 0; k < avgOps.size(); k++)
            out.avgOps[avgOps[k].name].push_back((double)b.sums[k] / (double)b.count);
        }
      } else {
        auto sorted = sortAndLimitFacets(strCounts, limit, showZeros ? 0 : mincount);
        for (const auto& [val, count] : sorted) {
          out.strIds.push_back(val);
          out.counts.push_back(count);
        }
      }

      // Set missing count if requested
      if (includeMissing) {
        out.missing = missingCount;
      }

      // Nested sub-facets (string/id parent only; the engine supports facet
      // sub-ops there). One sub-facet per RETURNED bucket, parallel to
      // bucket_ids: ops[name].arr.v[i].facet over that bucket's sub-domain.
      if (!isIntField) {
        const std::vector<std::string>& parentVals = out.strIds;
        if (parentVals.empty()) return out;  // engine emits no sub-op for an empty parent
        for (const auto& [opName, subPtr] : facetOp.ops) {
          const auto& sub = *subPtr;
          if (!std::holds_alternative<solux::api::FieldFacet>(sub.kind)) continue;
          auto& arr = out.subFacetOps[std::string(opName)];
          for (const auto& bval : parentVals) {
            auto bucketIt = strBucketDocs.find(bval);
            if (bucketIt == strBucketDocs.end()) {
              ADD_FAILURE() << "missing model documents for facet bucket " << bval;
              arr.emplace_back();
              continue;
            }
            arr.push_back(calculateFieldFacet(
                std::get<solux::api::FieldFacet>(sub.kind), bucketIt->second));
          }
        }
      }

      return out;
    }

    // Expected result for an int range facet. Buckets are dense and in
    // bucket order; out-of-range values are dropped (not missing);
    // missing = domain docs with no int value for the field.
    ExpFacet calculateRangeFacet(const solux::api::RangeFacet& rf,
                                 const std::vector<size_t>& domainDocs) const {
      ExpFacet out;
      out.isRange = true;
      std::string fieldName(rf.field);
      size_t fieldOrd = fieldOrdinal(fieldName);
      int64_t start = std::get<int64_t>((*rf.start).kind);
      int64_t end = std::get<int64_t>((*rf.end).kind);
      int64_t gap = std::get<int64_t>(
          std::get<solux::api::Val>(rf.gap_kind).kind);
      int64_t minCount = rf.mincount.value_or(0);

      int64_t bucketCount = (end - start) / gap + ((end - start) % gap != 0);
      std::vector<int64_t> bucketCounts((size_t)bucketCount, 0);
      int64_t missingCount = 0;
      for (auto docIdx : domainDocs) {
        bool hasField = false;
        for (const auto& nv : values(docIdx, fieldOrd)) {
          if (auto* iv = std::get_if<int64_t>(&nv.val)) {
            hasField = true;
            int64_t v = *iv;
            if (v < start || v >= end) continue;  // out of range -> dropped
            bucketCounts[(size_t)((v - start) / gap)]++;
          }
        }
        if (!hasField) missingCount++;
      }
      for (int64_t k = 0; k < bucketCount; k++) {
        int64_t count = bucketCounts[(size_t)k];
        if (count < minCount) continue;
        out.rangePairs.push_back({start + k * gap, std::min(start + (k + 1) * gap, end)});
        out.counts.push_back(count);
      }
      if (rf.missing) out.missing = missingCount;
      return out;
    }
  };

  // Build index with random data using parallel segment construction
  void buildRandomIndex(CollectionHelper& helper, Model& model, Rng& rng,
                       const std::vector<FieldDef>& fields,
                       int maxSegments = MERGE_FACTOR-1, int maxDocsPerSegment = 100) {
    helper.clear();

    SchemaBuilder b;
    for (const auto& field : fields) {
      if (!field.isInt) continue;
      auto& def = b.field(field.name);
      def.type = api::FieldDef::FieldClass::INT;
      def.index = api::FieldDef::IndexMode::RANGE;
      def.multi = field.multiValued;
    }
    b.set(helper.collection());
    
    // Random number of segments
    int numSegments = rng.rint(1, std::min(maxSegments, MERGE_FACTOR));
    

    
    // Get index writer for parallel segment building
    auto iw = helper.getIndexWriter();
    
    // Pre-obtain inverters for parallel processing
    std::vector<Inverter*> inverters;
    inverters.reserve(numSegments);
    for (int i = 0; i < numSegments; i++) {
      inverters.push_back(&iw->obtainInverter());
    }
    
    // Track documents for the model
    std::vector<std::vector<Doc>> segmentDocs(numSegments);
    
    // Get a single seed for all segments (don't call rng() inside parallel tasks)
    uint64_t baseSeed = rng();
    
    // Build segments in parallel using TBB
    tbb::task_group tg;
    for (int segNum = 0; segNum < numSegments; segNum++) {
      tg.run([&, segNum, baseSeed]() {
        Rng segRng(baseSeed + segNum);  // Deterministic seed per segment

        // Pre-calculate which fields should exist in all documents in this segment (5% chance per field)
        // and which fields should not exist at all in this segment (5% chance per field)
        boost::container::small_vector<uint8_t,8> fieldExists(fields.size());
        for (size_t i = 0; i < fields.size(); i++) {
          fieldExists[i] = segRng.rint(100);
        }

        Inverter& inverter = *inverters[segNum];

        // Random number of documents per segment
        int segDocCount = segRng.rint(1, maxDocsPerSegment);
        
        // get IndexHandlers for the fields that exist in this segment
        auto* idHandler = &inverter.getIndexHandler("id");
        // Dedicated avg-targets: single-valued ints, indexed for EVERY doc in
        // every segment so an avg() sub-op never sees an empty bucket (which
        // would make the inline path report 0.0 but the post-hoc path NaN, and
        // make sort-by-avg ill-defined). Two of them, to test multiple
        // simultaneous avg sub-ops on different fields.
        auto* avgHandler = &inverter.getIndexHandler("avgval_i");
        auto* avgHandler2 = &inverter.getIndexHandler("avgval2_i");
        boost::container::small_vector<Inverter::IndexHandler*,8> handlers(fields.size());
        for (size_t i = 0; i < fields.size(); i++) {
          if (fieldExists[i] < 5) {
            continue; // Field does not exist in this segment
          }
          handlers[i] = &inverter.getIndexHandler(fields[i].name);
        }
        
        for (int docIdx = 0; docIdx < segDocCount; docIdx++) {
          Doc doc;
          int64_t docId = segNum * 10000 + docIdx;
          doc.push_back({"id", std::to_string(docId)});
          
          inverter.startDoc();
          idHandler->index(inverter, std::to_string(docId));

          // Always-present avg targets (see avgHandler above).
          int64_t avgv = segRng.rint(1000);
          avgHandler->index(inverter, avgv);
          doc.push_back({"avgval_i", avgv});
          int64_t avgv2 = segRng.rint(1000);
          avgHandler2->index(inverter, avgv2);
          doc.push_back({"avgval2_i", avgv2});

          // Add random field values
          for (size_t fieldIdx = 0; fieldIdx < fields.size(); fieldIdx++) {
            const auto& field = fields[fieldIdx];

            if (fieldExists[fieldIdx] < 5) {
              continue;
            }

            // most of the time do a normal sparsity check
            if (fieldExists[fieldIdx] < 95 && !(segRng.rint(100) < field.sparsityPercent)) {
              continue;
            }

            if (field.isInt) {
              if (field.multiValued) {
                int count = segRng.rint(1, field.maxValuesPerDoc + 1);
                auto vals = sampleDistinctInts(segRng, field.numUniqueValues, count);
                handlers[fieldIdx]->index(inverter, std::span<const int64_t>(vals.data(), vals.size()));
                for (auto val : vals) {
                  doc.push_back({field.name, val});
                }
              } else {
                int64_t val = segRng.rint(field.numUniqueValues);
                handlers[fieldIdx]->index(inverter, val);
                doc.push_back({field.name, val});
              }
            }
            else {
              if (field.multiValued) {
                int count = segRng.rint(1, field.maxValuesPerDoc + 1);
                auto ords = sampleDistinctInts(segRng, field.numUniqueValues, count);
                std::vector<std::string> vals;
                std::vector<std::string_view> views;
                vals.reserve(ords.size());
                views.reserve(ords.size());
                for (auto ord : ords) {
                  vals.push_back("v" + std::to_string(ord));
                }
                for (auto& val : vals) {
                  views.push_back(val);
                }
                handlers[fieldIdx]->index(inverter, std::span<std::string_view>(views.data(), views.size()));
                for (auto& val : vals) {
                  doc.push_back({field.name, val});
                }
              } else {
                std::string val = "v" + std::to_string(segRng.rint(field.numUniqueValues));
                handlers[fieldIdx]->index(inverter, val);
                doc.push_back({field.name, val});
              }
            }

          }
          
          inverter.finishDoc();
          segmentDocs[segNum].push_back(std::move(doc));
        }
        
        iw->releaseInverter(inverter, true);  // Request immediate flush
      });
    }
    tg.wait();
    
    // Add all documents to the model
    for (auto& segDocs : segmentDocs) {
      for (auto& doc : segDocs) {
        model.addDoc(std::move(doc));
      }
    }
    
    helper.commit();
  }
  
  static std::vector<int64_t> sampleDistinctInts(Rng& rng, int max, int count) {
    std::vector<int64_t> vals;
    vals.reserve(count);
    while ((int)vals.size() < count) {
      int64_t val = rng.rint(max);
      bool exists = false;
      for (auto existing : vals) {
        if (existing == val) {
          exists = true;
          break;
        }
      }
      if (!exists) {
        vals.push_back(val);
      }
    }
    return vals;
  }

  // Fill a query: 80% a match on a present-enough string field, else match-all.
  // Multi-valued string fields are fine - matchesQuery matches if ANY value
  // equals, mirroring the engine.
  static void makeRandomQuery(Rng& rng, OpCursor& cur, const std::vector<FieldDef>& fields) {
    if (rng.rint(100) < 80 && !fields.empty()) {
      std::vector<int> candidateFields;
      for (int idx = 0; idx < (int)fields.size(); idx++) {
        if (!fields[idx].isInt && !fields[idx].isText && fields[idx].sparsityPercent >= 50) {
          candidateFields.push_back(idx);  // match on plain string fields only
        }
      }
      if (!candidateFields.empty()) {
        const auto& qf = fields[candidateFields[rng.rint((int)candidateFields.size())]];
        cur.matchQuery(qf.name, "v" + std::to_string(rng.rint(qf.numUniqueValues)));
        return;
      }
    }
    cur.allQuery();
  }

  // Random int range facet (start may be negative, end may exceed the value
  // range, gap may not divide evenly -> exercises out-of-range and partial
  // buckets). Float/double remain unsupported in this stage.
  // `cur` is the RangeFacet cursor (its field is already set).
  static void generateRandomRangeFacet(Rng& rng, OpCursor& cur, const FieldDef& field) {
    int n = field.numUniqueValues;
    int64_t start = (int64_t)rng.rint(std::max(3, n / 2 + 3)) - 2;  // [-2, ...)
    int64_t end = start + 1 + rng.rint(n + 4);                      // > start
    int64_t gap = 1 + rng.rint(std::max(1, n / 3));                 // >= 1
    cur.range(start, end, gap);
    int mc = rng.rint(3);
    if (mc != 0) cur.mincount(mc - 1);
    std::get<solux::api::RangeFacet>(cur.rawOp().kind).missing = rng.rbool();
  }

  // Add a facet op for `field` under `parent` as `name`: an int field becomes a
  // range facet ~40% of the time, else a field facet. `parent` is a LocalReq
  // (root) or an OpCursor (a top_docs query); both expose facet()/rangeFacet().
  template <class Parent>
  static void addFacetOp(Rng& rng, Parent& parent,
                         const std::string& name, const FieldDef& field,
                         const std::vector<FieldDef>& allFields) {
    if (field.isInt && rng.rint(100) < 40) {
      generateRandomRangeFacet(rng, parent.rangeFacet(name, field.name), field);
    } else {
      generateRandomFacet(rng, parent.facet(name, field.name), field, allFields);
    }
  }

  // Generate a random facet configuration onto the FieldFacet cursor (its field
  // is already set). depth>0 is a sub-facet (no further sub-ops, and no
  // mincount=0, to bound the space).
  static void generateRandomFacet(Rng& rng, OpCursor& cur, const FieldDef& field,
                                  const std::vector<FieldDef>& allFields, int depth = 0) {
    // Random limit - avoid problematic edge cases for now
    int limitChoice = rng.rint(5);
    if (limitChoice == 0) {
      cur.limit(-1);  // No limit
    } else if (limitChoice == 1) {
      cur.limit(1);  // Exactly 1 result
    } else if (limitChoice == 2) {
      cur.limit(5);  // Small limit
    } else if (limitChoice == 3) {
      cur.limit(20);  // Medium limit
    } else {
      cur.limit(rng.rint(1, 50));  // Random limit
    }
    auto& ff = std::get<solux::api::FieldFacet>(cur.rawOp().kind);
    // Random mincount. Leaving it unset is distinct from explicit 0.
    int mincountChoice = rng.rint(field.isInt ? 4 : 5);
    if (mincountChoice == 0) {
      // unset: parser maps this to minCount=-1, effective min 1
    } else if (!field.isInt && depth == 0 && mincountChoice == 1) {
      cur.mincount(0);  // string/id facets support zero-count buckets (top-level only)
    } else if (mincountChoice == 2) {
      cur.mincount(2);
    } else if (mincountChoice == 3) {
      cur.mincount(5);
    } else {
      cur.mincount(1 + rng.rint(3));
    }

    // Random missing
    ff.missing = rng.rbool();

    // Sub-ops only on string/id facets (parser rejects them on int/range/text)
    // and only at the top level (bounds nesting). May attach several at once:
    // 1-2 avgs (distinct always-present int fields) and/or a sub-facet.
    if (depth == 0 && !field.isInt && !field.isText) {
      bool wantAvg = rng.rint(100) < 45;
      bool wantSubFacet = rng.rint(100) < 25;
      if ((wantAvg || wantSubFacet) && ff.mincount.has_value() && *ff.mincount == 0) {
        cur.mincount(1);  // sub-ops + mincount=0 is an untested combo; avoid it
      }
      if (wantAvg) {
        cur.avg("av", "avgval_i");
        bool twoAvgs = rng.rbool();
        if (twoAvgs) cur.avg("av2", "avgval2_i");
        if (rng.rint(100) < 50) {  // sort by one of the avgs (at most one sort field)
          std::string_view sortField = (twoAvgs && rng.rbool()) ? "av2" : "av";
          qb::sort(cur, sortField, rng.rbool() ? qb::DESC : qb::ASC);
        }
      }
      if (wantSubFacet) {
        std::vector<int> strFields;
        for (int i = 0; i < (int)allFields.size(); i++)
          if (!allFields[i].isInt && !allFields[i].isText) strFields.push_back(i);  // string sub-facets only
        if (!strFields.empty()) {
          const auto& sf = allFields[strFields[rng.rint((int)strFields.size())]];
          generateRandomFacet(rng, cur.facet("sf", sf.name), sf, allFields, depth + 1);
        }
      }
    }
  }

public:
  void runRandomTest(int numIndexes = 20, int requestsPerIndex = 20, 
                     int maxSegments = MERGE_FACTOR-1, int maxDocsPerSegment = 250) {
    
    for (int iteration = 0; iteration < numIndexes; iteration++) {
      CollectionHelper helper;
      
      // Generate random field definitions for this iteration
      std::vector<FieldDef> fields;
      for (int i = 0; i < NUM_FIELDS; i++) {
        FieldDef field;
        int kind = i % 5;
        if (kind == 0) {
          field.name = "field" + std::to_string(i) + "_i";
          field.isInt = true;
          field.multiValued = false;
        } else if (kind == 1) {
          field.name = "field" + std::to_string(i) + "_s";
          field.isInt = false;
          field.multiValued = false;
        } else if (kind == 2) {
          field.name = "field" + std::to_string(i) + "_is";
          field.isInt = true;
          field.multiValued = true;
        } else if (kind == 3) {
          field.name = "field" + std::to_string(i) + "_ss";
          field.isInt = false;
          field.multiValued = true;
        } else {
          // text field, single token per doc (modeled like a single-valued _s).
          field.name = "field" + std::to_string(i) + "_w";
          field.isInt = false;
          field.isText = true;
          field.multiValued = false;
        }
        field.maxValuesPerDoc = field.multiValued ? 2 + rng.rint(3) : 1;
        int cardClass = i % 3;
        if (cardClass == 0) {
          field.numUniqueValues = 3 + rng.rint(8);
          field.sparsityPercent = 70 + rng.rint(30);
        } else if (cardClass == 1) {
          field.numUniqueValues = 20 + rng.rint(80);
          field.sparsityPercent = 35 + rng.rint(60);
        } else {
          field.numUniqueValues = 300 + rng.rint(1200);
          field.sparsityPercent = 20 + rng.rint(50);
        }
        field.numUniqueValues = std::max(field.numUniqueValues, field.maxValuesPerDoc);
        fields.push_back(field);
      }

      Model model(fields);
      
      // Build index with parallel segment construction
      buildRandomIndex(helper, model, rng, fields, maxSegments, maxDocsPerSegment);

      // On ~half the indexes, delete a random subset of docs so facet domains
      // are live-filtered (non-null liveDocs at root, intersected under queries).
      if (rng.rint(100) < 50) {
        std::vector<std::string> toDelete;
        for (size_t d = 0; d < model.docs.size(); d++) {
          if (rng.rint(100) < 15) {
            if (auto* id = model.findOne(d, "id"))
              if (auto* s = std::get_if<std::string>(id)) {
                toDelete.push_back(*s);
                model.markDeleted(d);
              }
          }
        }
        if (!toDelete.empty()) helper.deleteByIds(toDelete, UpdateMessage::COMMIT);
      }

      // Run multiple random facet tests on this index in parallel
      int numParallelTests = requestsPerIndex;
      
      // Run tests in parallel using TBB
      tbb::parallel_for(tbb::blocked_range<int>(0, numParallelTests),
        [&](const tbb::blocked_range<int>& range) {
          // Create a local RNG for this thread with deterministic seed
          Rng localRng(iteration * 1000000 + range.begin());
          
          for (int testNum = range.begin(); testNum != range.end(); ++testNum) {
            // Advance RNG to ensure different seed for each test
            localRng(); 
            
            auto req = localReq(soluxNode->getSearchEngine());
            req->collection("main");

            // Issue several top_docs queries (each its own domain) per request and
            // facet a random subset of fields under each, so the SAME field is often
            // faceted under several different domains in one request.  This amortizes
            // the expensive index build over many cheap facet computations and
            // stresses the concurrent merge/completion paths.
            int numQueries = 1 + localRng.rint(3);  // 1..3
            for (int qi = 0; qi < numQueries; qi++) {
              auto& topDocs = req->topDocs("q" + std::to_string(qi));
              topDocs.getNumber(true);
              makeRandomQuery(localRng, topDocs, fields);
              for (size_t f = 0; f < fields.size(); f++) {
                if (localRng.rint(100) < 70) {
                  addFacetOp(localRng, topDocs, "f" + std::to_string(f), fields[f], fields);
                }
              }
            }
            // Occasionally also facet at the root (whole-index / livedocs domain).
            if (localRng.rint(100) < 20) {
              for (size_t f = 0; f < fields.size(); f++) {
                if (localRng.rint(100) < 50) {
                  addFacetOp(localRng, *req, "rf" + std::to_string(f), fields[f], fields);
                }
              }
            }

            // Execute the request
            bool para = (localRng.rint(100) < PERCENT_PARA);
            req->execute(para);

            ASSERT_EQ(1u, req->responses.size());

            // Verify facet results
            const auto& response = req->responses[0]->proto;

            // Calculate expected results for all operations (read back from the
            // built request view) into a plain expected-result map.
            std::map<std::string, ExpVal> expected;
            model.processOps(req->proto.ops, expected);

            // Compare one facet result (buckets + counts + missing) against the model.
            std::function<void(const std::string&, const solux::api::FieldFacet&,
                               const solux::api::FacetResult&, const ExpFacet&)> verifyOneFacet =
              [&](const std::string& ctx, const solux::api::FieldFacet& ff,
                  const solux::api::FacetResult& actual, const ExpFacet& expected) {
              if (expected.intBuckets) {
                ASSERT_TRUE(actual.bucket_ids && std::holds_alternative<solux::api::ColInt>(actual.bucket_ids->kind)) << "expected int buckets: " << ctx << "\n" << req->toString();
                const auto& a = std::get<solux::api::ColInt>(actual.bucket_ids->kind);
                ASSERT_EQ(a.v.size(), expected.intIds.size()) << "bucket count: " << ctx << "\n" << req->toString();
                for (int i = 0; i < (int)a.v.size(); i++) EXPECT_EQ(a.v[i], expected.intIds[i]) << "bucket " << i << " value: " << ctx;
              } else {
                ASSERT_TRUE(actual.bucket_ids && std::holds_alternative<solux::api::ColStr>(actual.bucket_ids->kind)) << "expected string buckets: " << ctx << "\n" << req->toString();
                const auto& a = std::get<solux::api::ColStr>(actual.bucket_ids->kind);
                ASSERT_EQ(a.v.size(), expected.strIds.size()) << "bucket count: " << ctx << "\n" << req->toString();
                for (int i = 0; i < (int)a.v.size(); i++) EXPECT_EQ(a.v[i], expected.strIds[i]) << "bucket " << i << " value: " << ctx;
              }
              ASSERT_EQ(actual.counts.size(), expected.counts.size()) << "counts size: " << ctx;
              for (int i = 0; i < (int)expected.counts.size(); i++)
                EXPECT_EQ(actual.counts[i], expected.counts[i]) << "count " << i << ": " << ctx;
              if (ff.missing) { EXPECT_EQ(actual.missing.value_or(0), expected.missing.value_or(0)) << "missing: " << ctx; }
              // avg() sub-op result: arr_d with one entry per returned bucket.
              for (const auto& [opName, subPtr] : ff.ops) {
                const auto& sub = *subPtr;
                if (std::holds_alternative<solux::api::GenOp>(sub.kind)) {
                  const auto& genOp = std::get<solux::api::GenOp>(sub.kind);
                  if (genOp.name != "avg" && genOp.name != "average") continue;
                  // Empty facets emit no sub-op result (model omits it too).
                  if (!expected.avgOps.contains(std::string(opName))) continue;
                  ASSERT_TRUE(actual.ops.contains(opName)) << "actual avg missing: " << ctx << "\n" << req->toString();
                  const auto& aArr = std::get<solux::api::ArrDouble>(actual.ops.at(opName)->kind);
                  const auto& eArr = expected.avgOps.at(std::string(opName));
                  ASSERT_EQ(aArr.v.size(), eArr.size()) << "avg arr size: " << ctx << "\n" << req->toString();
                  for (int i = 0; i < (int)eArr.size(); i++)
                    EXPECT_DOUBLE_EQ(aArr.v[i], eArr[i]) << "avg[" << i << "]: " << ctx;
                }
              }
              // Nested sub-facet results: ops[name].arr.v[i].facet, one per bucket.
              for (const auto& [opName, subPtr] : ff.ops) {
                const auto& sub = *subPtr;
                if (!std::holds_alternative<solux::api::FieldFacet>(sub.kind)) continue;
                if (!expected.subFacetOps.contains(std::string(opName))) continue;  // empty parent -> no sub-op
                ASSERT_TRUE(actual.ops.contains(opName)) << "actual sub-facet missing: " << ctx << "\n" << req->toString();
                const auto& aArr = std::get<solux::api::ArrVal>(actual.ops.at(opName)->kind);
                const auto& eArr = expected.subFacetOps.at(std::string(opName));
                ASSERT_EQ(aArr.v.size(), eArr.size()) << "sub-facet arr size: " << ctx << "\n" << req->toString();
                for (int i = 0; i < (int)eArr.size(); i++)
                  verifyOneFacet(ctx + "/" + std::string(opName) + "[" + std::to_string(i) + "]",
                                 std::get<solux::api::FieldFacet>(sub.kind),
                                 std::get<solux::api::FacetResult>(aArr.v[i].kind),
                                 eArr[i]);
              }
            };

            // Walk the request ops and verify every facet at its path: root facets,
            // facets under each top_docs query, and (recursively) deeper nesting.
            std::function<void(const ReqOps&, const RespOps&, const std::map<std::string, ExpVal>&, const std::string&)> verifyOps =
              [&](const ReqOps& reqOps, const RespOps& actualOps, const std::map<std::string, ExpVal>& expectedOps, const std::string& path) {
                for (const auto& [name, opPtr] : reqOps) {
                  const auto& op = *opPtr;
                  std::string nameStr(name);
                  if (std::holds_alternative<solux::api::FieldFacet>(op.kind)) {
                    std::string ctx = path + nameStr + " (field " + std::string(std::get<solux::api::FieldFacet>(op.kind).field) + ")";
                    ASSERT_TRUE(expectedOps.contains(nameStr) && !expectedOps.at(nameStr).isDocList) << "expected facet missing: " << ctx;
                    ASSERT_TRUE(actualOps.contains(name) && actualOps.at(name)->facetResult())
                      << "actual facet missing: " << ctx << "\n" << req->toString();
                    verifyOneFacet(ctx, std::get<solux::api::FieldFacet>(op.kind),
                                   *actualOps.at(name)->facetResult(),
                                   expectedOps.at(nameStr).facet);
                  } else if (std::holds_alternative<solux::api::RangeFacet>(op.kind)) {
                    std::string ctx = path + nameStr + " (range " + std::string(std::get<solux::api::RangeFacet>(op.kind).field) + ")";
                    ASSERT_TRUE(expectedOps.contains(nameStr) && !expectedOps.at(nameStr).isDocList) << "expected range missing: " << ctx;
                    ASSERT_TRUE(actualOps.contains(name) && actualOps.at(name)->facetResult())
                      << "actual range missing: " << ctx << "\n" << req->toString();
                    const auto& af = *actualOps.at(name)->facetResult();
                    const auto& ef = expectedOps.at(nameStr).facet;
                    const auto& ab = std::get<solux::api::ArrArrInt>(af.bucket_ids->kind);
                    ASSERT_EQ(ab.v.size(), ef.rangePairs.size()) << "range bucket count: " << ctx << "\n" << req->toString();
                    for (int i = 0; i < (int)ef.rangePairs.size(); i++) {
                      ASSERT_EQ(2, (int)ab.v[i].v.size()) << ctx;
                      EXPECT_EQ(ef.rangePairs[i].first, ab.v[i].v[0]) << "range[" << i << "] lo: " << ctx;
                      EXPECT_EQ(ef.rangePairs[i].second, ab.v[i].v[1]) << "range[" << i << "] hi: " << ctx;
                    }
                    ASSERT_EQ(af.counts.size(), ef.counts.size()) << "range counts size: " << ctx;
                    for (int i = 0; i < (int)ef.counts.size(); i++)
                      EXPECT_EQ(af.counts[i], ef.counts[i]) << "range count " << i << ": " << ctx;
                    if (std::get<solux::api::RangeFacet>(op.kind).missing) { EXPECT_EQ(af.missing.value_or(0), ef.missing.value_or(0)) << "range missing: " << ctx; }
                  } else if (std::holds_alternative<solux::api::TopDocs>(op.kind) && !std::get<solux::api::TopDocs>(op.kind).ops.empty()) {
                    ASSERT_TRUE(actualOps.contains(name) && actualOps.at(name)->docList()) << "actual docs missing: " << path + nameStr << "\n" << req->toString();
                    ASSERT_TRUE(expectedOps.contains(nameStr) && expectedOps.at(nameStr).isDocList) << "expected docs missing: " << path + nameStr;
                    verifyOps(std::get<solux::api::TopDocs>(op.kind).ops,
                              actualOps.at(name)->docList()->ops,
                              expectedOps.at(nameStr).ops, path + nameStr + "/");
                  }
                }
              };
            verifyOps(req->proto.ops, response.ops, expected, "");
          }  // end of for loop in lambda
        });  // end of parallel_for

    }  // end of iteration loop
  }  // end of runRandomTest
};


TEST_F(RandomFacetTest, randomFaceting) {
  // Index build dominates cost, so amortize it: fewer indexes, many more
  // requests, each issuing several queries x a facet per field (many facet
  // computations, the same field faceted under multiple domains). Indexes and
  // requests are independent work dimensions, so scale each by sqrt(effort).
  StrFacetStrategyGuard guard;
  for (StrFacetStrategy strategy : {
      StrFacetStrategy::AUTO,
      StrFacetStrategy::COLUMN_DOMAIN,
      StrFacetStrategy::COLUMN_COMPLEMENT,
      StrFacetStrategy::TERM_DRIVEN}) {
    forcedStrFacetStrategy = strategy;
    runRandomTest(
        (int)scaleTestDimension(2, 2),
        (int)scaleTestDimension(100, 2));
  }
}
